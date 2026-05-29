#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <LittleFS.h>

// WiFi Configuration
const char* ssid = "TP-LINK_5A92";
const char* password = "16392748";

// Corrected Pins for Ultrasonic Sensor
#define TRIG_PIN D4  // Wired to D4 (GPIO2)
#define ECHO_PIN D7  // Wired to D7 (GPIO13)

ESP8266WebServer server(80);

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed. Formatting...");
    LittleFS.format();
    LittleFS.begin();
  }

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 10) { 
    delay(500); 
    Serial.print("."); 
    attempt++; 
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed. Starting Access Point...");
    WiFi.softAP("SmartFeeder", "12345678");
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("\nWiFi Connected!");
    Serial.print("Station IP Address: ");
    Serial.println(WiFi.localIP());
  }

  // Static route for HTML page
  server.serveStatic("/", LittleFS, "/index.html");
  
  // Dynamic route for CSV with No-Cache Headers to fix the buttons issue
  server.on("/log.csv", HTTP_GET, []() {
    File file = LittleFS.open("/log.csv", "r");
    if (!file) {
      server.send(200, "text/csv", ""); 
      return;
    }
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.streamFile(file, "text/csv");
    file.close();
  });
  
  // Clear log route (truncates file to 0 bytes)
  server.on("/clear_log", HTTP_GET, []() {
    File logFile = LittleFS.open("/log.csv", "w"); 
    if (logFile) logFile.close(); 
    server.send(200, "text/plain", "Cleared");
    Serial.println("Logs cleared by user.");
  });
  
  server.begin();
  Serial.println("HTTP Server Started.");
}

void loop() {
  server.handleClient();
  
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 3000) { 
    lastCheck = millis();

    // Ultrasonic Measurement
    digitalWrite(TRIG_PIN, LOW); 
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); 
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
    float distance = (duration == 0) ? 30 : (duration * 0.034 / 2);
    
    // Constraint values for mapping (5cm to 30cm)
    int percent = map(constrain((int)distance, 5, 30), 5, 30, 100, 0);

    // Save data to CSV (Time, Weight, Percent)
    File logFile = LittleFS.open("/log.csv", "a");
    if (logFile) {
      logFile.println(String(millis()) + ",0," + String(percent)); 
      logFile.close();
      Serial.println("Logged data: Distance=" + String(distance) + "cm, Level=" + String(percent) + "%");
    } else {
      Serial.println("Failed to open log file");
    }
  }
}
