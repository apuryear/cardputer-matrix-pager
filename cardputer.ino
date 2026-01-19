/*
 * M5Stack Cardputer Matrix Pager Client
 * Target: ESP32-S3 (M5Stack Cardputer)
 * * Features:
 * - Connects to Matrix via HTTPS
 * - Polling Sync for new messages
 * - Send messages via Keyboard
 * - Simple text-wrapping UI
 * * Dependencies:
 * - M5Cardputer
 * - M5Unified
 * - ArduinoJson (v6 or v7)
 * - HTTPClient
 * - WiFiClientSecure
 */

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// =============================================================
// CONFIGURATION
// =============================================================

// Wi-Fi Credentials
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASS     = "YOUR_WIFI_PASSWORD";

// Matrix Configuration
// Note: HOMESERVER_URL must include protocol and port if non-standard, no trailing slash.
// Example: "https://matrix.org" or "https://my-server.com:8448"
const char* HOMESERVER_URL = "https://matrix.org"; 

// The internal Room ID (starts with !), NOT the alias (starts with #)
const char* ROOM_ID        = "!your_room_id:homeserver.com"; 

// Your Access Token (Keep this secret)
const char* ACCESS_TOKEN   = "syt_YOUR_ACCESS_TOKEN_HERE";

// Settings
const unsigned long SYNC_INTERVAL_MS = 5000; // Poll every 5 seconds
const int MAX_HISTORY = 10;                  // Max messages to keep in RAM
const int MAX_MSG_LEN = 120;                 // Max chars per message to save memory

// =============================================================
// GLOBALS & STATE
// =============================================================

// Network
WiFiClientSecure secureClient;
HTTPClient http;

// Matrix State
String nextBatchToken = "";
unsigned long lastSyncTime = 0;

// UI State
struct MatrixMessage {
  char sender[32];
  char body[MAX_MSG_LEN];
};

MatrixMessage messageBuffer[MAX_HISTORY];
int messageCount = 0; // Number of messages currently stored

// Input State
bool isInputMode = false;
String inputString = "";

// Display Constants
const int TEXT_SIZE = 1;
const int LINE_HEIGHT = 16; // Height for size 2 text approx
const int SCREEN_WIDTH = 240;
const int SCREEN_HEIGHT = 135;
const int HEADER_HEIGHT = 20;

// =============================================================
// HELPER FUNCTIONS
// =============================================================

// Draw the header bar (Status line)
void drawHeader(const char* status) {
  M5Cardputer.Display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, BLUE);
  M5Cardputer.Display.setTextColor(WHITE, BLUE);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(5, 5);
  M5Cardputer.Display.print("Matrix Pager");
  
  // Right aligned status
  int w = M5Cardputer.Display.textWidth(status);
  M5Cardputer.Display.setCursor(SCREEN_WIDTH - w - 5, 5);
  M5Cardputer.Display.print(status);
}

// Add a message to the circular(ish) buffer
void addMessage(const char* sender, const char* body) {
  Serial.printf("Adding message: sender='%s', body='%s'\n", sender, body);
  // If full, shift everything up
  if (messageCount >= MAX_HISTORY) {
    Serial.println("Message buffer full, shifting messages up.");
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      messageBuffer[i] = messageBuffer[i+1];
    }
    messageCount = MAX_HISTORY - 1;
  }
  
  // Clean sender ID (strip @ and domain for brevity if desired, keeping full for now)
  strncpy(messageBuffer[messageCount].sender, sender, 31);
  messageBuffer[messageCount].sender[31] = '\0';
  
  strncpy(messageBuffer[messageCount].body, body, MAX_MSG_LEN - 1);
  messageBuffer[messageCount].body[MAX_MSG_LEN - 1] = '\0';
  
  messageCount++;
  Serial.printf("Message added. Total messages: %d\n", messageCount);
}

// Render the message history list
void drawMessages() {
  if (isInputMode) return; // Don't redraw history over input UI

  Serial.println("Drawing messages to display.");
  M5Cardputer.Display.fillRect(0, HEADER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_HEIGHT, BLACK);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setTextSize(1); // Standard readable size

  int y = HEADER_HEIGHT + 2;
  
  // Draw from newest at bottom? No, let's draw standard list top-down
  // To make it look like a pager, we usually want latest at the bottom.
  // Let's draw the last N messages that fit.
  
  for (int i = 0; i < messageCount; i++) {
    if (y >= SCREEN_HEIGHT) {
      Serial.println("Screen is full, stopping message draw.");
      break;
    }
    
    // Sender in Green
    M5Cardputer.Display.setTextColor(GREEN, BLACK);
    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print(messageBuffer[i].sender);
    M5Cardputer.Display.print(": ");
    
    // Calculate cursor after sender
    int x = M5Cardputer.Display.getCursorX();
    
    // Body in White
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    
    // Simple word wrap simulation
    String body = String(messageBuffer[i].body);
    int remainingSpace = SCREEN_WIDTH - x;
    
    // Just print it, let M5GFX handle wrapping if set, 
    // but M5Cardputer library usually needs explicit cursor management for clean UI.
    // For simplicity in this constrained script, we print.
    M5Cardputer.Display.print(body);
    
    y += 18; // Move to next line (approximate)
    // Add extra spacing if the text wrapped (rough estimation)
    if (body.length() > 35) y += 10; 
  }
}

// Render the input box overlay
void drawInputUI() {
  int inputHeight = 40;
  int inputY = SCREEN_HEIGHT - inputHeight;
  
  // Background box
  M5Cardputer.Display.fillRect(0, inputY, SCREEN_WIDTH, inputHeight, 0x333333); // Dark Grey
  M5Cardputer.Display.drawRect(0, inputY, SCREEN_WIDTH, inputHeight, ORANGE);
  
  M5Cardputer.Display.setCursor(5, inputY + 5);
  M5Cardputer.Display.setTextColor(ORANGE, 0x333333);
  M5Cardputer.Display.print("Compose: ");
  
  M5Cardputer.Display.setTextColor(WHITE, 0x333333);
  M5Cardputer.Display.print(inputString);
  
  // Cursor
  if (millis() % 1000 < 500) {
    M5Cardputer.Display.print("_");
  }
}

// =============================================================
// MATRIX API FUNCTIONS
// =============================================================

// Send a message
bool sendMessage(String text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendMessage failed: WiFi not connected.");
    return false;
  }

  drawHeader("Sending...");
  Serial.println("Sending message: " + text);
  
  // Generate a TxnID based on time
  String txnId = String(millis());
  
  String url = String(HOMESERVER_URL) + "/_matrix/client/v3/rooms/" + String(ROOM_ID) + "/send/m.room.message/" + txnId;
  Serial.println("Request URL: " + url);
  
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(ACCESS_TOKEN));
  
  // Create JSON payload
  // Minimal: {"msgtype":"m.text", "body":"hello"}
  StaticJsonDocument<256> doc;
  doc["msgtype"] = "m.text";
  doc["body"] = text;
  
  String requestBody;
  serializeJson(doc, requestBody);
  Serial.println("Request Body: " + requestBody);
  
  int httpResponseCode = http.PUT(requestBody);
  http.end();
  
  Serial.println("Send Response Code: " + String(httpResponseCode));
  if (httpResponseCode == 200) {
    drawHeader("Sent!");
    Serial.println("Message sent successfully.");
    delay(500);
    return true;
  } else {
    drawHeader("Send Fail");
    Serial.println("Failed to send message.");
    delay(1000);
    return false;
  }
}

// Sync messages
void syncMatrix() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("syncMatrix failed: WiFi not connected.");
    return;
  }
  
  drawHeader("Syncing...");
  Serial.println("Starting Matrix sync...");
  
  String url = String(HOMESERVER_URL) + "/_matrix/client/v3/sync";
  url += "?timeout=0"; // Return immediately for this implementation to keep UI responsive
  
  // If we have a token, use it to get only new messages
  if (nextBatchToken.length() > 0) {
    url += "&since=" + nextBatchToken;
    Serial.println("Syncing with since token: " + nextBatchToken);
  } else {
    Serial.println("Performing initial sync (no since token).");
  }
  
  http.begin(secureClient, url);
  http.addHeader("Authorization", "Bearer " + String(ACCESS_TOKEN));
  
  int httpCode = http.GET();
  Serial.println("Sync Response Code: " + String(httpCode));
  
  if (httpCode == 200) {
    // Stream result to ArduinoJson
    // The response can be large, so we use a Filter to only keep what we need
    // to save memory.
    
    // Filter Definition
    StaticJsonDocument<200> filter;
    filter["next_batch"] = true;
    filter["rooms"]["join"][ROOM_ID]["timeline"]["events"][0]["type"] = true;
    filter["rooms"]["join"][ROOM_ID]["timeline"]["events"][0]["sender"] = true;
    filter["rooms"]["join"][ROOM_ID]["timeline"]["events"][0]["content"]["body"] = true;
    filter["rooms"]["join"][ROOM_ID]["timeline"]["events"][0]["content"]["msgtype"] = true;

    // Dynamic document for the response. 
    // ESP32-S3 has enough RAM for a reasonable chunk, but be careful.
    // 20KB should cover a standard sync with a few text messages.
    DynamicJsonDocument doc(20000); 
    
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    
    if (!error) {
      // 1. Update Batch Token
      const char* nb = doc["next_batch"];
      if (nb) {
        nextBatchToken = String(nb);
        Serial.println("New batch token received: " + nextBatchToken);
      }
      
      // 2. Process Events
      JsonArray events = doc["rooms"]["join"][ROOM_ID]["timeline"]["events"];
      Serial.printf("Found %d events in timeline.\n", events.size());
      
      bool newMsg = false;
      for (JsonObject v : events) {
        const char* type = v["type"];
        const char* msgtype = v["content"]["msgtype"];
        
        // Check if it is a text message
        if (type && strcmp(type, "m.room.message") == 0 && 
            msgtype && strcmp(msgtype, "m.text") == 0) {
              
            const char* sender = v["sender"];
            const char* body = v["content"]["body"];
            
            if (sender && body) {
              addMessage(sender, body);
              newMsg = true;
            }
        }
      }
      
      if (newMsg) {
        Serial.println("New messages found, redrawing display.");
        drawHeader("New Msg");
        drawMessages();
      } else {
        Serial.println("No new messages.");
        drawHeader("Connected");
      }
      
    } else {
      drawHeader("JSON Err");
      Serial.print("Deserialize failed: ");
      Serial.println(error.c_str());
    }
  } else {
    String err = "Http Err: " + String(httpCode);
    drawHeader(err.c_str());
    Serial.println("HTTP GET failed with code: " + String(httpCode));
  }
  
  http.end();
}

// =============================================================
// SETUP
// =============================================================

void setup() {
  // Initialize Cardputer
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  
  Serial.begin(115200);
  Serial.println("M5Stack Cardputer Matrix Pager Client");

  // Display Setup
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextSize(TEXT_SIZE);
  
  // Connect WiFi
  drawHeader("WiFi Connecting...");
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    M5Cardputer.Display.print(".");
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  // Configure SSL
  // INSECURE: We skip root cert validation for memory/maintenance simplicity
  secureClient.setInsecure();
  Serial.println("SSL certificate validation is insecurely skipped.");
  
  drawHeader("WiFi Connected");
  delay(1000);
  
  // Initial Sync
  drawHeader("Initial Sync...");
  syncMatrix();
  drawMessages();
}

// =============================================================
// LOOP
// =============================================================

void loop() {
  M5Cardputer.update();
  
  // 1. Handle Keyboard Input
  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
      
      for (auto i : status.word) {
        // If user types, enter input mode
        if (!isInputMode) {
          isInputMode = true;
          inputString = "";
          Serial.println("Entering input mode.");
          drawInputUI();
        }
        
        inputString += i;
        drawInputUI();
      }
      
      if (status.del) {
         if (inputString.length() > 0) {
           inputString.remove(inputString.length() - 1);
           drawInputUI();
         }
      }
      
      if (status.enter) {
        if (isInputMode && inputString.length() > 0) {
          Serial.println("Enter key pressed, sending message.");
          if (sendMessage(inputString)) {
            // Success
            isInputMode = false;
            inputString = "";
            M5Cardputer.Display.fillRect(0, SCREEN_HEIGHT - 40, SCREEN_WIDTH, 40, BLACK); // clear input area
            drawMessages(); // Redraw history
            // Trigger immediate sync to see our own message
            Serial.println("Message sent, forcing sync.");
            syncMatrix();
          }
        }
      }
    }
  }
  
  // Handle ESC key (btn G0 on Cardputer usually acts as escape/function)
  if (M5Cardputer.BtnA.wasClicked()) { // 'BtnA' is often the G0 button
      if (isInputMode) {
        isInputMode = false;
        inputString = "";
        Serial.println("Exiting input mode.");
        M5Cardputer.Display.fillRect(0, SCREEN_HEIGHT - 40, SCREEN_WIDTH, 40, BLACK);
        drawMessages();
      }
  }

  // 2. Periodic Sync
  // Only sync if not typing to prevent UI lag while inputting
  if (!isInputMode && (millis() - lastSyncTime > SYNC_INTERVAL_MS)) {
    lastSyncTime = millis();
    syncMatrix();
  }
  
  delay(10); // Small yield
}
