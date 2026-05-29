#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include "HX711.h"

#define DT_PIN 4      // d6
#define SCK_PIN 5     // d5
#define BUTTON_PIN 0  // d3
#define GREEN_LED 16  // d8
#define TRIG_PIN 14   // d4
#define ECHO_PIN 12   // d7
#define STEP_PIN 13   // d1
#define DIR_PIN 15    // d2
#define RED_LED 2     // d0

const char* topic_status = "lp_feeder/feeder_1/status";
const char* topic_command = "lp_feeder/feeder_1/command";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
ESP8266WebServer server(80);
HX711 scale;

String wifi_ssid, wifi_pass, mqtt_server;
int mqtt_port;
float scale_factor;

unsigned long lastSensorRead = 0;
const long sensorInterval = 10000; 
volatile bool buttonPressed = false;
bool mqttFeedTriggered = false;
int mqttFeedAmount = 0;

unsigned long lastMqttReconnect = 0;
unsigned long lastWifiBlink = 0;

bool isFeeding = false;
long feedTargetWeight = 0;
long feedStartWeight = 0;
unsigned long feedStartTime = 0;
unsigned long lastStepTime = 0;
bool stepState = LOW;

long globalWeight = 0;
int globalFeedPercent = 0;

void IRAM_ATTR handleButtonPress() {
  buttonPressed = true;
}

void logToFile(String msg) {
  bool needHeader = !LittleFS.exists("/log.csv");
  File logFile = LittleFS.open("/log.csv", "a");
  
  if (logFile) {
    if (needHeader) {
      logFile.println("Time (ms),Action"); 
    }
    logFile.println(String(millis()) + "," + msg);
    logFile.close();
    Serial.println("[LOG CSV] " + msg);
  } else {
    Serial.println("[Error] Could not open /log.csv for writing!");
  }
}
bool loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("ERROR LittleFS not initialized!");
    return false;
  }
  if (!LittleFS.exists("/config.json")) {
    Serial.println("ERROR File /config.json is missing!");
    return false;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return false;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  if (error) {
    Serial.println("ERROR JSON parsing error!");
    return false;
  }

  wifi_ssid = doc["wifi_ssid"].as<String>();
  wifi_pass = doc["wifi_pass"].as<String>();
  mqtt_server = doc["mqtt_server"].as<String>();
  mqtt_port = doc["mqtt_port"] | 1883;
  scale_factor = doc["scale_factor"] | 420.0;
  
  Serial.println("SYSTEM Config successfully loaded.");
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("MQTT Command received.!");
  JsonDocument doc;
  deserializeJson(doc, payload, length);
  
  if (doc["action"] == "feed") {
    mqttFeedAmount = doc["amount"] | 50;
    mqttFeedTriggered = true; 
    logToFile("MQTT command: issue " + String(mqttFeedAmount) + "г");
  }
}

void setupWebServer() {
  server.serveStatic("/", LittleFS, "/index.html");
  server.serveStatic("/log.csv", LittleFS, "/log.csv");
  
  server.on("/api/data", HTTP_GET, []() {
    JsonDocument doc;
    doc["weight"] = globalWeight;
    doc["feed_level"] = globalFeedPercent;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
  
  server.on("/clear_log", HTTP_GET, []() {
    LittleFS.remove("/log.csv");
    logToFile("Logs cleared");
    server.send(200, "text/plain", "Logs cleared");
  });

  server.begin();
  Serial.println("SYSTEM Web server started.");
}

void startFeed(int targetWeight) {
  if (isFeeding) {
    Serial.println("WARNING Feeding already in progress, command ignored.");
    return;
  }
  
  feedStartWeight = scale.get_units(3);
  if (feedStartWeight > 150) { 
    mqttClient.publish("lp_feeder/feeder_1/error", "{\"error\":\"bowl_full\"}");
    logToFile("Error Bowl is already full (" + String(feedStartWeight) + "г)");
    return;
  }
  
  logToFile("Start of feeding (Goal: +" + String(targetWeight) + "г)");
  
  digitalWrite(DIR_PIN, HIGH);
  isFeeding = true;
  feedTargetWeight = targetWeight;
  feedStartTime = millis();
  lastStepTime = micros();
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n--- System start ---");
  
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);

  if (!loadConfig()) {
    while(1) { 
      digitalWrite(RED_LED, !digitalRead(RED_LED)); 
      delay(100); 
    }
  }

  scale.begin(DT_PIN, SCK_PIN);
  scale.set_scale(scale_factor);
  scale.tare();
  Serial.println("SENSOR Scales calibrated.");

  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  Serial.println("[WIFI] Connect to " + wifi_ssid + "...");

  setupWebServer();
  
  mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
  mqttClient.setCallback(mqttCallback);
  
  logToFile("System dowloaded successfully");
}

void loop() {
  unsigned long currentMillis = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (currentMillis - lastWifiBlink >= 500) {
      lastWifiBlink = currentMillis;
      digitalWrite(RED_LED, !digitalRead(RED_LED));
      digitalWrite(GREEN_LED, LOW);
    }
  } else {
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, HIGH);
  }

  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    if (currentMillis - lastMqttReconnect >= 5000) { 
      lastMqttReconnect = currentMillis;
      Serial.println("[MQTT] Connection try...");
      if (mqttClient.connect("WemosFeederClient")) {
        Serial.println("MQTT Connected!");
        mqttClient.subscribe(topic_command);
        logToFile("MQTT connected");
      }
    }
  }
  
  if (mqttClient.connected()) {
    mqttClient.loop();
  }
  
  server.handleClient();

 
  if (isFeeding) {
    if (currentMillis - feedStartTime > 20000) { 
      isFeeding = false;
      mqttClient.publish("lp_feeder/feeder_1/error", "{\"error\":\"jammed\"}");
      logToFile("Error: Motor timeout");
    } else {
      unsigned long currentMicros = micros();
      if (currentMicros - lastStepTime >= 800) {
        lastStepTime = currentMicros;
        stepState = !stepState;
        digitalWrite(STEP_PIN, stepState);
      }
      
      if (scale.is_ready()) {
        long currentWeight = scale.get_units();
        if (currentWeight >= (feedStartWeight + feedTargetWeight)) {
          isFeeding = false;
          mqttClient.publish("lp_feeder/feeder_1/success", "{\"msg\":\"done\"}");
          logToFile("Feed was successfully dispensed. Weight: " + String(currentWeight) + "г");
        }
      }
    }
  }

   if (currentMillis - lastSensorRead >= sensorInterval) {
    lastSensorRead = currentMillis;
    
    if (scale.is_ready()) {
      long currentWeight = scale.get_units(3);
      globalWeight = currentWeight < 0 ? 0 : currentWeight;
      
      if (currentWeight < -50) {
        JsonDocument doc;
        doc["weight"] = 0;
        doc["feed_level"] = 0;
        doc["status"] = "removed";
        char buffer[128];
        serializeJson(doc, buffer);
        mqttClient.publish(topic_status, buffer);
        Serial.println("SENSOR Bowl removed!");
      } else {
        digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
        digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);
        
        long duration = pulseIn(ECHO_PIN, HIGH, 30000);
        float distance = (duration == 0) ? 30 : (duration * 0.034 / 2);
        globalFeedPercent = map(constrain(distance, 5, 30), 5, 30, 100, 0);

        JsonDocument doc;
        doc["weight"] = globalWeight;
        doc["feed_level"] = globalFeedPercent;
        doc["status"] = "online";
        char buffer[128];
        serializeJson(doc, buffer);
        mqttClient.publish(topic_status, buffer);
      }
    }
  }
  if (buttonPressed) {
    buttonPressed = false;
    Serial.println("BUTTON Manual feeding");
    startFeed(50);
  }

  if (mqttFeedTriggered) {
    mqttFeedTriggered = false;
    startFeed(mqttFeedAmount);
  }
}
