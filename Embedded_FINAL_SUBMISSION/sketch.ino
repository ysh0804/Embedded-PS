#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// I2C LCD Configuration
#define I2C_ADDR    0x27
#define LCD_COLUMNS 16
#define LCD_LINES   2
LiquidCrystal_I2C lcd(I2C_ADDR, LCD_COLUMNS, LCD_LINES);

// User Database
struct UserRole {
  unsigned long pinCode;
  const char* roleName;
};

const int TOTAL_USERS = 4;
UserRole userDatabase[TOTAL_USERS] = {
  {6969, "G-STUDENT"},
  {8834, "PARTICIPANT"},
  {6767, "JUDGES"},
  {1060, "FACULTY"}
};

// Finite State Machine (FSM) States
typedef enum {
  STATE_INPUT,
  STATE_FEEDBACK,
  STATE_LEDOFF,
  STATE_LOCKOUT,
  STATE_EMERGENCY
} systemState;

systemState currentState = STATE_INPUT;

// Global System Variables
unsigned long input = 0;
uint8_t d = 0;
unsigned long previousMillis = 0;
int count = 0;
unsigned long lockStartTime = 0;
volatile bool emer = false; // Flag for emergency interrupt

// Keypad Matrix Setup
const uint8_t ROWS = 4;
const uint8_t COLS = 4;
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
uint8_t colPins[COLS] = { 1, 0, 3, 2 };
uint8_t rowPins[ROWS] = { 4, 5, 6, 7 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Network & NTP Time Configuration
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = ""; 
const char* SERVER_URL = "http://httpbin.org/post";     
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET   = 5 * 3600 + 1800; // IST timezone
const int   DST_OFFSET   = 0;               

// Offline Logging Queue (Max 10 entries)
#define QUEUE_SIZE 10

struct LogEntry {
  unsigned long timestamp; 
  char role[16];
  bool granted;
  bool timeSynced; 
};

LogEntry offlineQueue[QUEUE_SIZE];
int queueCount = 0;          

// Connection & Sync Trackers
bool wifiWasConnected = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000;   
bool timeSynced = false;                          
unsigned long lastTimeSyncAttempt = 0;
const unsigned long TIME_SYNC_RETRY_INTERVAL = 10000;  
unsigned long lastFlushAttempt = 0;
const unsigned long FLUSH_INTERVAL = 5000;       

// Interrupt Debounce
volatile unsigned long lastEmergencyISRTime = 0;
const unsigned long EMERGENCY_DEBOUNCE_MS = 300;

// Emergency Button ISR
void IRAM_ATTR emergency() {
  unsigned long now = millis();
  if (now - lastEmergencyISRTime > EMERGENCY_DEBOUNCE_MS) {
    emer = true;
    lastEmergencyISRTime = now;
  }
}

// Check for real-world time sync
void attemptTimeSync() {
  if (millis() - lastTimeSyncAttempt < TIME_SYNC_RETRY_INTERVAL) return;
  lastTimeSyncAttempt = millis();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 500)) {  
    timeSynced = true;
    Serial.println("[TIME] NTP sync confirmed");
  } else {
    timeSynced = false;
    Serial.println("[TIME] NTP sync not yet available");
  }
}

// Get current timestamp for logs
bool getCurrentTimestamp(unsigned long &ts) {
  time_t now;
  time(&now);
  ts = (unsigned long)now;
  return timeSynced;
}

// Add log to offline queue (FIFO buffer)
void enqueueLog(LogEntry entry) {
  if (queueCount < QUEUE_SIZE) {
    offlineQueue[queueCount] = entry;
    queueCount++;
    Serial.printf("[QUEUE] Buffered log (%d/%d)\n", queueCount, QUEUE_SIZE);
  } else {
    Serial.println("[QUEUE] Full - dropping oldest entry");
    for (int i = 1; i < QUEUE_SIZE; i++) {
      offlineQueue[i - 1] = offlineQueue[i];
    }
    offlineQueue[QUEUE_SIZE - 1] = entry;
  }
}

// Remove oldest log from queue
void dequeueFront() {
  for (int i = 1; i < queueCount; i++) {
    offlineQueue[i - 1] = offlineQueue[i];
  }
  queueCount--;
}

// Send JSON log payload to HTTP server
bool sendLogToServer(const LogEntry &entry) {
  if (WiFi.status() != WL_CONNECTED) return false;

  StaticJsonDocument<200> doc;
  doc["timestamp"]    = entry.timestamp;
  doc["role"]          = entry.role;
  doc["granted"]       = entry.granted;
  doc["time_synced"]   = entry.timeSynced;   

  char payload[200];
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(4000);
  http.setConnectTimeout(4000);

  int httpCode = http.POST(payload);
  http.end();

  if (httpCode > 0 && httpCode < 400) {
    Serial.printf("[SYNC] Sent OK (HTTP %d): %s\n", httpCode, payload);
    return true;
  }
  Serial.printf("[SYNC] Send failed (HTTP %d)\n", httpCode);
  return false;
}

// Print access attempt to Serial Monitor
void printAccessLog(const char* role, bool granted, unsigned long timestamp, bool synced) {
  Serial.println("========================================");
  Serial.println(" ACCESS EVENT");
  Serial.println("========================================");
  Serial.print(" Role/Position : ");
  Serial.println(role);

  if (synced) {
    time_t t = (time_t)timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    char dateStr[16], timeStr[16];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);   
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);   
    Serial.print(" Date          : ");
    Serial.println(dateStr);
    Serial.print(" Time          : ");
    Serial.println(timeStr);
  } else {
    Serial.println(" Date/Time     : NOT SYNCED");
    Serial.printf(" Device Uptime : %ds since boot\n", millis() / 1000);
  }

  Serial.print(" Status        : ");
  Serial.println(granted ? "ACCESS GRANTED" : "ACCESS DENIED");
  Serial.print(" Wi-Fi         : ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Offline (will queue)");
}

// Handle complete logging flow (print + send/queue)
void logAccessEvent(const char* role, bool granted) {
  LogEntry entry;
  entry.timeSynced = getCurrentTimestamp(entry.timestamp);
  strncpy(entry.role, role, sizeof(entry.role) - 1);
  entry.role[sizeof(entry.role) - 1] = '\0';
  entry.granted = granted;

  printAccessLog(entry.role, entry.granted, entry.timestamp, entry.timeSynced);

  if (WiFi.status() == WL_CONNECTED && !sendLogToServer(entry)) {
    enqueueLog(entry); // Server failed
  } else if (WiFi.status() != WL_CONNECTED) {
    enqueueLog(entry); // Offline
  }
}

// Push pending offline logs to server
void handleQueueFlush() {
  if (queueCount == 0 || WiFi.status() != WL_CONNECTED || millis() - lastFlushAttempt < FLUSH_INTERVAL) return;
  lastFlushAttempt = millis();

  Serial.printf("[SYNC] Attempting flush, %d queued...\n", queueCount);
  if (sendLogToServer(offlineQueue[0])) {
    dequeueFront();
    Serial.printf("[SYNC] Flushed one entry, %d remaining\n", queueCount);
  } else {
    Serial.println("[SYNC] Flush attempt failed");
  }
}

// Reconnect WiFi if dropped
void maintainWiFi() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (!isConnected) {
    if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL) {
      lastReconnectAttempt = millis();
      Serial.println("[WIFI] Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    wifiWasConnected = false;
    return;
  }

  // Trigger NTP sync only right after reconnecting
  if (isConnected && !wifiWasConnected) {
    Serial.println("[WIFI] Connection established");
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER); 
    lastTimeSyncAttempt = 0;   
  }
  wifiWasConnected = true;
}

void setup() {
  Serial.begin(115200);

  // Initialize displays and pins
  Wire.begin(18, 19);
  lcd.init();
  lcd.backlight();
  pinMode(8, OUTPUT);
  pinMode(10, OUTPUT);
  digitalWrite(8, LOW);
  digitalWrite(10, LOW);

  // Setup emergency button interrupt
  pinMode(9, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(9), emergency, FALLING);

  lcd.print("ENTER THE CODE");

  // Initial blocking WiFi connection attempt
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected: " + WiFi.localIP().toString());
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    wifiWasConnected = true;
  } else {
    Serial.println("\n[WIFI] Offline at boot");
    wifiWasConnected = false;
  }
}

// STATE: Wait for 4-digit PIN entry
void readKey() {
  char key = keypad.getKey();

  // Reset if inactive for 7 seconds
  if (d > 0 && (millis() - previousMillis > 7000)) {
    d = 0; input = 0;
    lcd.clear(); lcd.print("ENTER THE CODE");
    return;
  }

  if (key) {
    previousMillis = millis();

    // Process number input
    if (key >= '0' && key <= '9') {
      input = (input * 10) + (key - '0');
      d++;
      lcd.clear();
      lcd.print(input);

      if (d == 4) {
        lcd.clear();
        lcd.print("Checking...");
        currentState = STATE_FEEDBACK;
        return;
      }
    } 
    // Handle backspace (*)
    else if (key == '*' && d > 0) {
      input = input / 10;
      d--;
      lcd.clear();
      if (d == 0) lcd.print("ENTER THE CODE");
      else lcd.print(input);
    }
  }
}

// STATE: Validate PIN
void check() {
  bool match = false;
  int mI = 0;

  for (int i = 0; i < TOTAL_USERS; i++) {
    if (input == userDatabase[i].pinCode) {
      match = true;
      mI = i;
      break;
    }
  }

  lcd.clear();

  if (match) {
    lcd.print("WELCOME:");
    lcd.setCursor(0, 1);
    lcd.print(userDatabase[mI].roleName);
    count = 0; 
    digitalWrite(8, HIGH); // Activate relay/LED
    digitalWrite(10, HIGH);
    logAccessEvent(userDatabase[mI].roleName, true);   
  } else {
    count++; 
    lcd.print("ACCESS DENIED");
    lcd.setCursor(0, 1);
    lcd.print("RETRY!!");
    logAccessEvent("UNKNOWN", false);                  
  }
  
  currentState = STATE_LEDOFF;
}

// STATE: Wait 3 seconds, then reset or lockout
void ledOFF() {
  if (millis() - previousMillis >= 3000) {
    digitalWrite(8, LOW); 
    digitalWrite(10, LOW);
    input = 0; d = 0;
    previousMillis = millis(); 

    if (count >= 3) {
      lockStartTime = millis();
      currentState = STATE_LOCKOUT;
      lcd.clear();
      lcd.print("SYSTEM LOCKED");
      lcd.setCursor(0, 1);
      lcd.print("Wait 15 Seconds");
    } else {
      currentState = STATE_INPUT; 
      lcd.clear();
      lcd.print("ENTER THE CODE");
    }
  }
}

// STATE: System locked due to failures
void lockOut() {
  if (millis() - lockStartTime >= 15000) {
    count = 0; 
    currentState = STATE_INPUT;
    lcd.clear();
    lcd.print("ENTER THE CODE");
  }
}

// STATE: Hardware interrupt 
void ghusjaa() {
  emer = false; 
  lcd.clear();
  lcd.print("EMERGENCY ACCESS");
  lcd.setCursor(0, 1);
  lcd.print("OVERRIDE ACTIVE");

  digitalWrite(8, HIGH); 
  digitalWrite(10, HIGH);
  logAccessEvent("EMERGENCY", true);  

  currentState = STATE_LEDOFF; 
}

void loop() {
  // 1. Check Interrupts
  if (emer == true) currentState = STATE_EMERGENCY;

  // 2. Background Tasks
  maintainWiFi();      
  attemptTimeSync();     
  handleQueueFlush();   

  // 3. FSM 
  switch (currentState) {
    case STATE_INPUT:   
      readKey();
       break;
    case STATE_FEEDBACK:
      check();
       break;
    case STATE_LEDOFF:
        ledOFF();
         break;
    case STATE_LOCKOUT:
       lockOut();
        break;
    case STATE_EMERGENCY:
       ghusjaa();
        break;
  }
}