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
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ThingSpeak.h>

const unsigned long READ_MILLIS = 5000UL;
const unsigned long POST_MILLIS = 300000UL;
const unsigned long RUNNING_MILLIS = 60000UL;
const unsigned long NEXT_RUN = 1200000UL;
int RAN = false;

const char* thingSpeakserver = "api.thingspeak.com";
const int FAN_PIN = D1;
const char* host = "kegeratorfan";

ESP8266WebServer httpServer(80);
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(D4);
DallasTemperature keggeratorSensor(&oneWire);
unsigned long lastReadingTime = 0, lastPostTime = 0, lastRunCheck = 0, lastRunTime = 0;
float currentReading;
float desiredTemperature = 42.0;
unsigned int runningCount = 0;
unsigned int waitCount = 0;
char tsAPIKey[40];
char tsChannel[6];
String proxyAPIKey = "";

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

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
    html += "<br>API Key: ";
    html += String(tsAPIKey);
    html += "<br>Channel: ";
    html += String(tsChannel);
    html += "~<br></body></html>";
    Serial.println("Done serving up HTML...");
    httpServer.send(200, "text/html", html);
  });
  WiFiManager wifiManager;
  WiFi.hostname(String(host));
  String holder = "";
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          holder = json["ThingSpeakWriteKey"].asString();
          strcpy(tsChannel, json["ThingSpeakChannel"]);
          Serial.println(holder);
          Serial.println(tsChannel);
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  holder.toCharArray(tsAPIKey, 40);
  Serial.println(tsAPIKey);
  WiFiManagerParameter custom_thingspeak_api_key("key", "API key", tsAPIKey, 40);
  WiFiManagerParameter custom_thingspeak_channel("ThingSpeak Channel", "Channel Number", tsChannel, 8);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_thingspeak_api_key);
  wifiManager.addParameter(&custom_thingspeak_channel);

  wifiManager.setConfigPortalTimeout(90);
  if (!wifiManager.startConfigPortal(host)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["ThingSpeakWriteKey"] = (String)custom_thingspeak_api_key.getValue();
    json["ThingSpeakChannel"] = (String)custom_thingspeak_channel.getValue();

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }
  Serial.print("Api Key: ");
  Serial.println(tsAPIKey);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
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
  WiFi.mode(WIFI_STA);
  httpServer.begin();
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  lastPostTime = millis() + POST_MILLIS;
  lastRunTime = millis() + RUNNING_MILLIS;
  strcpy(tsAPIKey, custom_thingspeak_api_key.getValue());
  proxyAPIKey = (String)tsAPIKey;
  Serial.println(tsAPIKey);
  Serial.println(proxyAPIKey);
  Serial.println(custom_thingspeak_api_key.getValue());
  strcpy(tsChannel, custom_thingspeak_channel.getValue());
  Serial.println(tsChannel);
}

void loop() {
  if ((long) (millis() - lastReadingTime) >= 0) {
    lastReadingTime += READ_MILLIS;
    keggeratorSensor.requestTemperatures(); // Send the command to get temperatures
    currentReading = getReading(keggeratorSensor);
    if ((long)(millis() - lastPostTime) >= 0) {
      if (RAN) {
        postData(currentReading, 1);
      } else {
        postData(currentReading, 0);
      }
      RAN =false;
      lastPostTime += POST_MILLIS;
    }
    if (millis() - lastRunTime > 0 ) {
      if ( (currentReading > desiredTemperature) & !RAN) {
        digitalWrite(FAN_PIN, HIGH);
        lastRunTime = millis() + RUNNING_MILLIS;
        RAN = true;
        Serial.println("Running");
      } else {
        digitalWrite(FAN_PIN, LOW);
        lastRunTime = millis() + NEXT_RUN;
        Serial.println("Stopping");
      }
    }
    Serial.print(".");
  }
  ArduinoOTA.handle();
  httpServer.handleClient();
}
void postData(float reading, float runTime) {
  WiFiClient client;
  ThingSpeak.begin(client);  // Initialize ThingSpeak
  Serial.println("About to post!");
  Serial.print("Channel: ");
  Serial.println(atoi(tsChannel));
  Serial.print("API Key: ");
  proxyAPIKey.toCharArray(tsAPIKey, 17);
  Serial.println(tsAPIKey);
  ThingSpeak.setField(1, reading);
  ThingSpeak.setField(2, runTime);
  int x = ThingSpeak.writeFields(atoi(tsChannel), tsAPIKey);
  Serial.println(x);
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
