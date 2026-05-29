#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include "HX711.h"

// --- Піни компонентів ---
#define DT_PIN 4 //d6
#define SCK_PIN 5 //d5
#define BUTTON_PIN 0 //d3
#define GREEN_LED 16 //d8
#define TRIG_PIN 14 //d4
#define ECHO_PIN 12 //d7
#define STEP_PIN 13 //d1
#define DIR_PIN 15 //d2
#define RED_LED 2 //d0

// --- Налаштування MQTT топіків ---
const char* topic_status = "lp_feeder/feeder_1/status";
const char* topic_command = "lp_feeder/feeder_1/command";

// --- Глобальні об'єкти ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);
HX711 scale;

// --- Змінні конфігурації ---
String wifi_ssid, wifi_pass, mqtt_server;
int mqtt_port;
float scale_factor;

// --- Таймери та прапорці ---
unsigned long lastSensorRead = 0;
const long sensorInterval = 10000; 
volatile bool buttonPressed = false;
bool mqttFeedTriggered = false;
int mqttFeedAmount = 0;

void IRAM_ATTR handleButtonPress() {
  buttonPressed = true;
}

// Завантаження налаштувань з LittleFS
bool loadConfig() {
  if (!LittleFS.begin()) return false;
  if (!LittleFS.exists("/config.json")) return false;

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return false;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  if (error) return false;

  wifi_ssid = doc["wifi_ssid"].as<String>();
  wifi_pass = doc["wifi_pass"].as<String>();
  mqtt_server = doc["mqtt_server"].as<String>();
  mqtt_port = doc["mqtt_port"] | 1883;
  scale_factor = doc["scale_factor"] | 420.0;
  return true;
}

// Обробка вхідних команд від сервера Node.js
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  deserializeJson(doc, payload, length);
  
  if (doc["action"] == "feed") {
    mqttFeedAmount = doc["amount"] | 50;
    mqttFeedTriggered = true; 
  }
}

void setupWiFi() {
  digitalWrite(GREEN_LED, LOW);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(RED_LED, !digitalRead(RED_LED));
    delay(500);
  }
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, HIGH);
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    if (mqttClient.connect("WemosFeederClient")) {
      mqttClient.subscribe(topic_command);
    } else {
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);

  if (!loadConfig()) {
    Serial.println("Config error!");
    while(1) { digitalWrite(RED_LED, HIGH); delay(100); digitalWrite(RED_LED, LOW); delay(100); }
  }

  scale.begin(DT_PIN, SCK_PIN);
  scale.set_scale(scale_factor);
  scale.tare();

  setupWiFi();
  mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) setupWiFi();
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  unsigned long currentMillis = millis();

  // Опитування сенсорів кожні 10 сек
  if (currentMillis - lastSensorRead >= sensorInterval) {
    lastSensorRead = currentMillis;
    
    // Вага
    long currentWeight = scale.get_units(3);
    if (currentWeight < -50) {
      // Миску знято
      JsonDocument doc;
      doc["weight"] = 0;
      doc["feed_level"] = 0;
      doc["status"] = "removed";
      char buffer[128];
      serializeJson(doc, buffer);
      mqttClient.publish(topic_status, buffer);
    } else {
      // Рівень корму
      digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
      digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
      digitalWrite(TRIG_PIN, LOW);
      long duration = pulseIn(ECHO_PIN, HIGH, 30000);
      float distance = (duration == 0) ? 30 : (duration * 0.034 / 2);
      int feedPercent = map(constrain(distance, 5, 30), 5, 30, 100, 0);

      JsonDocument doc;
      doc["weight"] = currentWeight < 0 ? 0 : currentWeight;
      doc["feed_level"] = feedPercent;
      doc["status"] = "online";
      char buffer[128];
      serializeJson(doc, buffer);
      mqttClient.publish(topic_status, buffer);
    }
  }

  // Обробка ручного годування (кнопка)
  if (buttonPressed) {
    buttonPressed = false;
    feed(50);
  }

  // Обробка команди з сайту через MQTT
  if (mqttFeedTriggered) {
    mqttFeedTriggered = false;
    feed(mqttFeedAmount);
  }
}

void feed(int targetWeight) {
  long startWeight = scale.get_units(3);
  if (startWeight > 150) { // Захист від переповнення
    mqttClient.publish("lp_feeder/feeder_1/error", "{\"error\":\"bowl_full\"}");
    return;
  }
  
  digitalWrite(DIR_PIN, HIGH);
  unsigned long startTime = millis();
  long currentWeight = startWeight;

  while (currentWeight < (startWeight + targetWeight)) {
    if (millis() - startTime > 20000) { // Таймаут 20 сек
      mqttClient.publish("lp_feeder/feeder_1/error", "{\"error\":\"jammed\"}");
      break;
    }
    digitalWrite(STEP_PIN, HIGH); delayMicroseconds(800);
    digitalWrite(STEP_PIN, LOW); delayMicroseconds(800);
    currentWeight = scale.get_units(1);
  }
  
  mqttClient.publish("lp_feeder/feeder_1/success", "{\"msg\":\"done\"}");
}