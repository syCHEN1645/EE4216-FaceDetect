/*******************************************************************************
 * ESP32 Gateway with PIR Sensor + ThingSpeak & Telegram Integration
 * 
 * FEATURES:
 * - PIR motion sensor triggers ESP32-S3-EYE via TCP command
 * - Receives face detection data from ESP32-S3-EYE
 * - Uploads detection history to ThingSpeak
 * 
 * HARDWARE:
 * - ESP32 board (any variant)
 * - HC-SR501 PIR sensor connected to GPIO 4
 * 
 * CONNECTIONS:
 * PIR Sensor:
 *   VCC → 5V
 *   GND → GND
 *   OUT → GPIO 4
 ******************************************************************************/

// Include Relevant Libraries 
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ============================================================================
// 1. NETWORK CONFIGURATION
// ============================================================================

// WiFi - Connect to Hotspot 
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD"; 

// Static IP: fix ESP32 IP Address so that we do not have to change it in ESP_EYE's tcp_send()  
#define USE_STATIC_IP true
IPAddress STATIC_IP(172, 20, 10, 14);     // pick one outside DHCP pool
IPAddress STATIC_GW(172,20,10,1);
IPAddress STATIC_MASK(255,255,255,0);
IPAddress STATIC_DNS(8,8,8,8);           // or your router

// Set ESP32 as TCP Server [Receive detection data from ESP_EYE]
#define TCP_PORT 5500
WiFiServer tcpServer(TCP_PORT);

// PIR Sensor Initialisation [ESP-EYE Trigger]
#define PIR_PIN 4
#define PIR_TRIGGER_COOLDOWN 10000  // 10 seconds between triggers

// ThingSpeak Database Initialisation
#define TS_HOST "api.thingspeak.com"
#define TS_WRITE_KEY "[REDACTED]"
#define TS_MIN_INTERVAL_MS 16000  // 16 seconds (ThingSpeak rate limit)

// Telegram Notifications Initialisation 
#define TELEGRAM_ENABLE     1           // set 0 to disable
const char* TELEGRAM_BOT_TOKEN = "[REDACTED]";
const char* TELEGRAM_CHAT_ID   = "2112746975";     // your user/chat id
WiFiClientSecure tgClient;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, tgClient);

// Telegram: Rate-limit notifications
unsigned long lastTgNotify = 0;
const unsigned long TG_MIN_INTERVAL_MS = 15000;   // 15s


// ============================================================================
// 2. GLOBAL VARIABLES
// ============================================================================

// Forward declarations
void processDetectionData(const String& data);
void handleClient(WiFiClient& client);

// PIR State
volatile bool pirMotionDetected = false;
volatile uint32_t pirDetectionCount = 0;
unsigned long lastPirTrigger = 0;
bool pirEnabled = true;

// Statistics
unsigned long totalDetections = 0;
unsigned long successfulUploads = 0;
unsigned long failedUploads = 0;
unsigned long lastUploadTime = 0;
String lastDetectedPerson = "none";
float lastConfidence = 0.0;

// System status 
bool systemReady = false;


// ============================================================================
// 3a. UTILITY FUNCTIONS: PIR INTERRUPT SERVICE ROUTINE 
// ============================================================================

// checks if intervals between last trigger and current trigger is at least 
// 10 seconds before calling interrupt 
void IRAM_ATTR pirISR() {
  unsigned long now = millis();
  if (now - lastPirTrigger > PIR_TRIGGER_COOLDOWN) {
    pirMotionDetected = true;
    pirDetectionCount++;
    lastPirTrigger = now;
  }
}

// ============================================================================
// 3b. UTILITY FUNCTIONS: URL ENCODING FOR THINGSPEAK 
// ============================================================================

// converts normal string into an url-safe encoded form to be appended at end of url 
String urlEncode(const String &s) {
  String encoded;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      char hex[4];
      sprintf(hex, "%%%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

// ============================================================================
// 3c. UTILITY FUNCTIONS: Telegram Alert Handler  
// ============================================================================
bool sendTelegramAlert(const String& person, float similarity, int status, int pirCount) {
  if (!TELEGRAM_ENABLE) return false;

  // Enforce rate limit
  unsigned long now = millis();
  if (now - lastTgNotify < TG_MIN_INTERVAL_MS) return false;

  // For quick start: skip cert validation
  tgClient.setInsecure();

  // build message to be sent over telegram 
  String msg;
  msg += "ESP32 Gateway Alert\n";
  msg += "• Status: ";          msg += (status==1 ? "Recognized" : "Unknown"); msg += "\n";
  msg += "• Person: ";          msg += person;                                 msg += "\n";
  msg += "• Similarity: ";      msg += String(similarity * 100, 1);            msg += "%\n";
  msg += "• PIR Triggers: ";    msg += String(pirCount);                        msg += "\n";
  msg += "• TS Channel: ";      msg += "(see your ThingSpeak graphs)";

  bool ok = bot.sendMessage(TELEGRAM_CHAT_ID, msg, "Markdown");
  if (ok) lastTgNotify = now;
  return ok;
}

// ============================================================================
// 4. Detection Data Upload Function: THINGSPEAK UPLOAD
// ============================================================================

bool uploadToThingSpeak(const String& jsonData) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ThingSpeak] ✗ WiFi not connected");
    return false;
  }

  // Rate limiting as ThingSpeak only allows free users to update data every 16 seconds 
  unsigned long now = millis();
  if (lastUploadTime > 0 && (now - lastUploadTime) < TS_MIN_INTERVAL_MS) {
    unsigned long wait = TS_MIN_INTERVAL_MS - (now - lastUploadTime);
    Serial.printf("[ThingSpeak] Rate limit: waiting %lu ms\n", wait);
    delay(wait);
  }

  // Parse JSON
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonData);
  
  // if JSON string has an error 
  if (error) {
    Serial.print("[ThingSpeak] ✗ JSON parse error: ");
    Serial.println(error.c_str());
    failedUploads++;
    return false;
  }

  // Extract data
  int event_status = doc["status"] | 0;
  int person_id = doc["id"] | 0;
  float similarity = doc["similarity"] | 0.0;
  String event_type = doc["event"] | String("UNKNOWN");

  // Update last detection
  lastDetectedPerson = String(person_id);
  lastConfidence = similarity;

  // Build ThingSpeak POST data
  String postData = "api_key=" + String(TS_WRITE_KEY);
  postData += "&field1=" + String(event_status);
  postData += "&field2=" + String(person_id);
  postData += "&field3=" + String(similarity, 3);
  postData += "&status=" + urlEncode(event_type);

  String url = "https://" + String(TS_HOST) + "/update";

  Serial.println("\n[ThingSpeak] ═══════════════════════════");
  Serial.println("[ThingSpeak] Uploading detection...");
  Serial.println("[ThingSpeak] Person ID: " + String(person_id));
  Serial.println("[ThingSpeak] Similarity: " + String(similarity));

  HTTPClient http;
  WiFiClient client; 

  http.begin(client, "http://api.thingspeak.com/update");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST(postData);
  String response = http.getString();
  http.end();

  // ThingSpeak returns the entry ID as a positive integer on success, or "0" if it failed.
  if (httpCode == 200) { //&& response != "0") {
    Serial.println("[ThingSpeak] ✓ Upload successful! Entry ID: " + response);
    //lastTS = millis();
    return true;
  } else {
    Serial.print("[ThingSpeak] ✗ HTTP "); Serial.println(httpCode);
    Serial.println("[ThingSpeak] Response: " + response);
    Serial.println("[ThingSpeak] Note: '0' usually means rate limit or bad API key/fields.");
    return false;
  }
}

// ============================================================================
// 5. Detection Data Processing Function: PROCESS DETECTION DATA
// ============================================================================

// this function is called by handleTCPClient() after data has been received by the ESP32 
void processDetectionData(const String& data) {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     FACE DETECTION RECEIVED            ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  totalDetections++;
  Serial.print("Test for JSON string");
  Serial.println(data);

  // Parse JSON to display info
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, data);

  Serial.print("Test2 for JSON string");
  Serial.println(data);

  if (!error) {
    String event_type = doc["event"] | String("UNKNOWN");
    int status = doc["status"] | 0;
    int person_id = doc["id"] | 0;
    float similarity = doc["similarity"] | 0.0;

    Serial.println("┌────────────────────────────────────────");
    Serial.print("│ Event:       ");
    Serial.println(event_type);
    Serial.print("│ Status:      ");
    Serial.println(status == 1 ? "Recognized" : "Unknown");
    Serial.print("│ Person ID:   ");
    Serial.println(person_id);
    Serial.print("│ Similarity:  ");
    Serial.print(similarity * 100);
    Serial.println("%");
    Serial.println("└────────────────────────────────────────");
  } else {
    Serial.println("[ERROR] Failed to parse JSON");
    Serial.println("Raw data: " + data);
  }

  // Upload to ThingSpeak 
  bool uploaded = uploadToThingSpeak(data);
  if (uploaded) {
    // Placeholder for (TELEGRAM) notifications if needed
    // Use already-parsed values if you have them; otherwise re-extract:
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, data) == DeserializationError::Ok) {
      String person = doc["person_id"].is<const char*>() 
                        ? String(doc["person_id"].as<const char*>())
                        : String( doc["id"] | -1 );
      float similarity = doc["similarity"] | doc["confidence"] | 0.0;
      int status = doc["status"] | doc["face_status"] | 0;

      // After successfully uploading to ThingSpeak, upload to Telegram
      sendTelegramAlert(person, similarity, status, pirDetectionCount);    
      // }
    }
  }
}

// ============================================================================
// 6. Detection Data Retrieval Function: HANDLE TCP CLIENT
// ============================================================================

// this function receives RECOGNISE data from ESP-EYE 
// AND sends msg to ESP-EYE if motion sensor has been triggered 
void handleTCPClient(WiFiClient& client) {
  Serial.println("\n[TCP] ┌─ Client connected");
  Serial.print("[TCP] │  IP: ");
  Serial.println(client.remoteIP());

  String receivedData = "";

  while (client.connected()) {
    if (client.available()) {
      String response = client.readStringUntil('\r');
      Serial.println("RECEIVED STRING: " + response); 
      processDetectionData(response);  // this function uploads data to ThingSpeak 
    }

    // Check for PIR motion while connected
    if (pirMotionDetected && pirEnabled) {
      pirMotionDetected = false;
      
      Serial.println("\n[PIR] ╔════════════════════════════════════╗");
      Serial.print("[PIR] ║   MOTION DETECTED! (#");
      Serial.print(pirDetectionCount);
      Serial.println(")         ║");
      Serial.println("[PIR] ╚════════════════════════════════════╝");
      Serial.println("[PIR] Sending trigger to ESP32-S3-EYE...");
      
      // Send trigger command to ESP32-S3-EYE
      client.print("1\r");
      Serial.println("[PIR] ✓ Trigger sent");
    }
    delay(10);
  }
}


// ============================================================================
// 7. SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 GATEWAY WITH PIR + THINGSPEAK  ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  // Initialize PIR and attach interrupt 
  pinMode(PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);
  Serial.println("[PIR] ✓ Sensor initialized on GPIO " + String(PIR_PIN));

  // Connect to WiFi using Static IP 
  WiFi.mode(WIFI_STA);
#if USE_STATIC_IP
  if (!WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS)) {
    Serial.println("[WiFi] ✗ Static IP config failed – falling back to DHCP");
  } else {
    Serial.print("[WiFi] Using static IP: "); Serial.println(STATIC_IP);
  }
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting to " + String(WIFI_SSID));

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {  // try waiting for at least 15 seconds 
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  // WiFi Connected 
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] ✓ Connected!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] ✗ Connection failed!");
    delay(5000);
    ESP.restart();
  }

  // Start TCP server
  tcpServer.begin();
  Serial.println("\n[TCP] Server started");
  Serial.print("[TCP] Listening on: ");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(TCP_PORT);

  Serial.println("\n[ThingSpeak] Write Key: " + String(TS_WRITE_KEY));

  systemReady = true;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  ✓ GATEWAY READY                       ║");
  Serial.println("║  PIR will trigger ESP32-S3-EYE         ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ============================================================================
// 8. MAIN LOOP
// ============================================================================

void loop() {
  // Handle TCP connections
  WiFiClient client = tcpServer.available();
  if (client) {   // if connected, call handleTCPClient to receive data 
    handleTCPClient(client);
  }

  // WiFi health check
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] ✗ Connection lost! Reconnecting...");
      WiFi.reconnect();
    }
  }
  delay(10);
}
