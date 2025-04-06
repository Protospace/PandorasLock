#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoOTA.h>
#include <secrets.h>

SoftwareSerial rfidSerial(D2, D1); // D2: RX, D1: TX

const int relayPin = D8;
const int greenLed = D3; // Green LED (Success)
const int redLed = D0;   // Red LED (Failure)
const int buttonD4 = D4; // Momentary Switch
const int buttonD7 = D7; // Momentary Switch
const int sensorPinD6 = D6; // Door Sensor
const int monitorPinD5 = D5; // Case Status Sensor

const String apiBaseURL = "https://api.my.protospace.ca/lockout/";

byte buffer[10]; 
bool isProcessingCard = false;
bool isBusy = false;
String lastCardID;
bool caseTaken = false;

void setup() {
  Serial.begin(9600);
  rfidSerial.begin(9600);

  pinMode(relayPin, OUTPUT);
  pinMode(greenLed, OUTPUT);
  pinMode(redLed, OUTPUT);
  pinMode(buttonD4, INPUT_PULLUP);
  pinMode(buttonD7, INPUT_PULLUP);
  pinMode(sensorPinD6, INPUT_PULLUP);
  pinMode(monitorPinD5, INPUT);

  digitalWrite(relayPin, LOW);
  digitalWrite(greenLed, LOW);
  digitalWrite(redLed, LOW);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // OTA Setup
  ArduinoOTA.setHostname("RFID_Scanner");
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Update Start...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Update Complete!");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\n", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle(); // Handle OTA updates

  static bool readingData = false;
  static int dataIndex = 0;

  while (rfidSerial.available() > 0) {
    byte incomingByte = rfidSerial.read();

    // Start reading new card data
    if (incomingByte == 2) { 
      Serial.println("[SYSTEM] Start reading card...");
      readingData = true;
      dataIndex = 0;
      continue;
    }

    // End of card data read
    if (incomingByte == 3) { 
      Serial.println("[SYSTEM] Card read complete...");
      readingData = false;
      if (dataIndex == 10) {
        processCardData();
      }
      break;
    }

    // Store data into buffer if within limits and reading has started
    if (readingData && dataIndex < 10) {
      buffer[dataIndex++] = incomingByte;
    }
  }
}

void processCardData() {
  isProcessingCard = true;
  isBusy = true;
  Serial.println("[SYSTEM] Processing Card...");

  String cardID = extractCardID(buffer);

  // Case is checked out and a new card is scanned
  if (caseTaken && cardID != lastCardID) {
    Serial.println("[SYSTEM] Case Taken - New Card Scanned! Flashing both LEDs...");
    flashBothLeds(3); // Flash both LEDs simultaneously
    isProcessingCard = false;
    isBusy = false;
    rfidSerial.begin(9600);
    return;
  }

  lastCardID = cardID;

  if (authorizeCard(cardID)) {
    Serial.println("[SYSTEM] Access granted - Activating relay...");
    flashLed(greenLed, 3);
    digitalWrite(relayPin, HIGH);
    delay(5000);
    digitalWrite(relayPin, LOW);

    if (digitalRead(sensorPinD6) == HIGH) {
      Serial.println("[SYSTEM] Door Open - Waiting to close...");

      unsigned long doorOpenStart = millis(); // Start tracking open time

      while (digitalRead(sensorPinD6) == HIGH) {
        delay(100);
        
        // If door open time exceeds 30 sec, process case status
        if (millis() - doorOpenStart >= 30000) {
          Serial.println("[SYSTEM] Door Open Timeout! Processing case status...");
          break;
        }
      }

      Serial.println("[SYSTEM] Door Closed - Checking case status...");

      if (digitalRead(monitorPinD5) == LOW) {
        Serial.println("[SYSTEM] Case In Place");
        caseTaken = false;
        sendCaseReturnedPost(cardID);
      } else {
        Serial.println("[SYSTEM] Case Taken - Flashing both LEDs!");
        caseTaken = true;
        flashBothLeds(3);
        sendCaseStatusPost(cardID);
      }
    }
  } else {
    Serial.println("[SYSTEM] Access Denied");
    flashLed(redLed, 3);
  }

  delay(1000);
  isProcessingCard = false;
  isBusy = false;
  Serial.println("[SYSTEM] Returning to Idle State...");
  rfidSerial.begin(9600);
}

void flashLed(int ledPin, int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(300);
    digitalWrite(ledPin, LOW);
    delay(300);
  }
}

void flashBothLeds(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(greenLed, HIGH);
    digitalWrite(redLed, HIGH);
    delay(300);
    digitalWrite(greenLed, LOW);
    digitalWrite(redLed, LOW);
    delay(300);
  }
}

void sendCaseStatusPost(String cardID) {
  String apiURL = "https://api.my.protospace.ca/stats/raptorx1/scanner3d/";
  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  HTTPClient https;
  
  https.begin(wifiClient, apiURL);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "card_number=" + cardID + "&status=IN_USE";

  int result = https.POST(postData);
  https.end();

  Serial.print("[SYSTEM] Case Checked Out - POST Response: ");
  Serial.println(result);
}

void sendCaseReturnedPost(String cardID) {
  String apiURL = "https://api.my.protospace.ca/stats/raptorx1/scanner3d/";
  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  HTTPClient https;
  
  https.begin(wifiClient, apiURL);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "card_number=" + cardID + "&status=FREE";

  int result = https.POST(postData);
  https.end();

  Serial.print("[SYSTEM] Case Returned - POST Response: ");
  Serial.println(result);
}

String extractCardID(byte* data) {
  char id[11] = {0};
  for (int i = 0; i < 10; i++) {
    id[i] = data[i];
  }
  return String(id);
}

bool authorizeCard(String cardID) {
  String apiURL = apiBaseURL + cardID + "/authorize/";
  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  HTTPClient https;
  https.begin(wifiClient, apiURL);

  String postData = "cert=scanner";
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int result = https.POST(postData);
  https.end();

  return result == HTTP_CODE_OK;
}