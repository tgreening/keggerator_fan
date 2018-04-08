#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <Wire.h>

const unsigned long READ_MILLIS = 5000UL;
const unsigned long POST_MILLIS = 300000UL;
const int RUN_COUNT = 2;
const int WAIT_COUNT = 3;
const String apiKey = "QBU7IXR9SLBQIJV5";
const char* thingSpeakserver = "api.thingspeak.com";
const int FAN_PIN = D1;
const char* host = "kegerator_fan";

ESP8266WebServer httpServer(80);
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(D4);
DallasTemperature keggeratorSensor(&oneWire);
unsigned long lastReadingTime = 0, lastPostTime = 0;
float currentReading;
float desiredTemperature = 42.0;
unsigned int runningCount = 0;
unsigned int waitCount = 0;

void setup() {
  Serial.begin(115200);
  keggeratorSensor.requestTemperatures(); // Send the command to get temperatures
  httpServer.on("/", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    Serial.println("Serving up HTML...");
    String html = "<html><body><b>Hi There!</b><br>";
    html += "Current Reading: ";
    html += currentReading;
    html += "<br>Runing Count: ";
    html += runningCount;
    html += "<br>Wait Time: ";
    html += waitCount;
    html += "<br></body></html>";
    Serial.println("Done serving up HTML...");
    httpServer.send(200, "text/html", html);
  });
  WiFiManager wifiManager;
  wifiManager.autoConnect(host);
  // Hostname defaults to esp8266-[ChipID]
  WiFi.hostname(host);
  if (!MDNS.begin(host)) {
    Serial.println("Error setting up MDNS responder!");
  }
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  httpServer.begin();
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  lastPostTime = millis() + POST_MILLIS;
}

void loop() {

  if ((long) (millis() - lastReadingTime) >= 0) {
    lastReadingTime += READ_MILLIS;
    keggeratorSensor.requestTemperatures(); // Send the command to get temperatures
    currentReading = getReading(keggeratorSensor);
    if ((long)(millis() - lastPostTime) >= 0) {
      if (currentReading > desiredTemperature && runningCount < RUN_COUNT && waitCount == 0) {
        digitalWrite(FAN_PIN, HIGH);
        runningCount++;
        postData(currentReading, 1);
        waitCount = 0;
      } else {
        digitalWrite(FAN_PIN, LOW);
        if (waitCount < WAIT_COUNT) {
          waitCount++;
        } else {
          waitCount = 0;
        }
        postData(currentReading, 0);
        runningCount = 0;
      }
      Serial.println("Updating checks...");
      lastPostTime += POST_MILLIS;
    }
  }
  ArduinoOTA.handle();
  httpServer.handleClient();
}

void postData(float reading, float runTime) {
  String data = apiKey + "&field1=";
  data += reading;
  data += "&field2=";
  data += runTime;
  data += "\r\n\r\n";
  postToThingSpeak(data);
}

void postToThingSpeak(String data) {
  WiFiClient client;
  if (client.connect(thingSpeakserver, 80)) { // "184.106.153.149" or api.thingspeak.com

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + apiKey + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(data.length());
    client.print("\n\n");
    client.print(data);
  }
}

float getReading(DallasTemperature sensor) {
  int retryCount = 0;
  float celsius = sensor.getTempCByIndex(0);
  while ((celsius == 85 || celsius == -85) && retryCount < 10) {
    celsius = sensor.getTempCByIndex(0);
    retryCount++;
    if (retryCount != 10) {
      delay(retryCount * 1000);
    }
  }
  if (retryCount == 10) {
    ESP.restart();
  }
  return (celsius * 9 / 5) + 32;
}
