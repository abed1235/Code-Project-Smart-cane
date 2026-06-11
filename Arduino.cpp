// ============================================================
// Arduino Mega
// ============================================================
#include "Arduino.h"
#include "DFRobotDFPlayerMini.h"

// ---- Pins ----
const int VIB_PIN  = 13;
const int TRIG_PIN = 11;
const int ECHO_PIN = 12;

// ---- Distance Thresholds ----
const int CLOSE_DISTANCE = 25;  // 0-25cm = fast vibration
const int FAR_DISTANCE   = 50;  // 25-50cm = slow vibration

// ---- DFPlayer ----
DFRobotDFPlayerMini myDFPlayer;
bool dfPlayerOnline  = false;

// ---- Audio Cooldown ----
String        lastPlayedLabel = "";
unsigned long lastPlayedTime  = 0;
const unsigned long AUDIO_COOLDOWN = 3000;

// ---- Camera Watchdog ----
unsigned long lastCamTime     = 0;
bool          camConnected    = false;
unsigned long lastStatusPrint = 0;
const unsigned long CAM_TIMEOUT     = 10000;  // 10 seconds timeout
const unsigned long STATUS_INTERVAL =  5000;

// ---- Vibration Motor (non-blocking) ----
bool          motorRunning       = false;
unsigned long motorStartTime     = 0;
bool          motorInCooldown    = false;
unsigned long motorCooldownStart = 0;
unsigned long motorOnDuration    = 0;
unsigned long motorOffDuration   = 0;
bool          motorPhaseOn       = true;

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial1.begin(9600);   // DFPlayer
    Serial3.begin(115200); // ESP8266 (camera data)

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(VIB_PIN,  OUTPUT);
    digitalWrite(VIB_PIN, LOW);

    Serial.println("============================================");
    Serial.println("  MEGA BRAIN — SYSTEM BOOT");
    Serial.println("============================================");

    delay(2000);

    // Initialize DFPlayer
    if (!myDFPlayer.begin(Serial1)) {
        Serial.println("[DFPlayer] ERROR: Check SD card + wiring.");
        dfPlayerOnline = false;
    } else {
        dfPlayerOnline = true;
        Serial.println("[DFPlayer] ONLINE");
        myDFPlayer.volume(25);
        delay(20);
        myDFPlayer.playLargeFolder(1, 1);
        Serial.println("[DFPlayer] Playing startup track 1");
    }

    Serial.println("[Ultrasonic] ONLINE");
    Serial.println("[Camera] Waiting for ESP8266 data on Serial3...");
    Serial.println("============================================");
    Serial.println("System Ready.");
    Serial.println("Expected camera labels: PERSON, CHAIR, TABLE");
    Serial.println("============================================");

    lastCamTime     = millis();
    lastStatusPrint = millis();
}

// ============================================================
// HELPERS
// ============================================================
void playTrack(int track) {
    if (!dfPlayerOnline) {
        Serial.println("[DFPlayer] ERROR: Not online.");
        return;
    }
    
    Serial.print("[DFPlayer] Playing track ");
    Serial.println(track);
    myDFPlayer.playLargeFolder(1, track);
    delay(50);  // Give DFPlayer time to process
}

// Improved ultrasonic reading with filtering
int getFilteredDistance() {
    const int numReadings = 3;
    int readings[numReadings];
    int validCount = 0;
    
    for (int i = 0; i < numReadings; i++) {
        digitalWrite(TRIG_PIN, LOW);
        delayMicroseconds(2);
        digitalWrite(TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);
        
        long duration = pulseIn(ECHO_PIN, HIGH, 30000);
        
        if (duration > 0) {
            int dist = duration * 0.034 / 2;
            if (dist >= 2 && dist <= FAR_DISTANCE + 10) {
                readings[validCount++] = dist;
            }
        }
        delay(20);
    }
    
    if (validCount < 2) return -1;  // Not enough valid readings
    
    // Return average
    int sum = 0;
    for (int i = 0; i < validCount; i++) {
        sum += readings[i];
    }
    return sum / validCount;
}

// ============================================================
// LOOP
// ============================================================
void loop() {

    // ----------------------------------------------------------
    // BLOCK 1: Camera Messages → DFPlayer Audio ONLY
    // FIXED: Better parsing and debugging
    // ----------------------------------------------------------
    while (Serial3.available()) {
        String msg = Serial3.readStringUntil('\n');
        msg.trim();
        msg.toUpperCase();
        
        // Also check for carriage return
        if (msg.endsWith("\r")) {
            msg = msg.substring(0, msg.length() - 1);
        }

        if (msg.length() == 0) continue;

        // Update camera watchdog
        lastCamTime = millis();
        
        if (!camConnected) {
            Serial.println("\n>>> CAM LINK: CONNECTED <<<");
            Serial.println(">>> Receiving data from ESP8266 <<<");
            camConnected = true;
        }

        // Debug: Show raw received data
        Serial.print("[CAM] Raw: '");
        Serial.print(msg);
        Serial.println("'");

        // Ignore heartbeat messages
        if (msg == "ALIVE" || msg == "ESP8266_READY" || msg == "READY") {
            Serial.println("[CAM] Heartbeat received");
            continue;
        }

        // Process detected objects
        int trackNum = 0;
        String detectedObject = "";
        
        if (msg.indexOf("PERSON") != -1) {
            trackNum = 2;
            detectedObject = "PERSON";
        } else if (msg.indexOf("CHAIR") != -1) {
            trackNum = 3;
            detectedObject = "CHAIR";
        } else if (msg.indexOf("TABLE") != -1) {
            trackNum = 4;
            detectedObject = "TABLE";
        }

        if (trackNum > 0) {
            Serial.print("[CAM] Detected: ");
            Serial.println(detectedObject);
            
            bool sameLabel  = (detectedObject == lastPlayedLabel);
            bool cooldownOk = (millis() - lastPlayedTime >= AUDIO_COOLDOWN);

            if (!sameLabel || cooldownOk) {
                playTrack(trackNum);
                lastPlayedLabel = detectedObject;
                lastPlayedTime  = millis();
            } else {
                Serial.println("[DFPlayer] Cooldown — skipping duplicate");
            }
        } else if (msg.length() > 0 && msg != "ALIVE" && msg != "ESP8266_READY") {
            Serial.print("[CAM] Unknown label: ");
            Serial.println(msg);
        }
    }

    // ----------------------------------------------------------
    // BLOCK 2: Camera Watchdog with better messaging
    // ----------------------------------------------------------
    unsigned long now = millis();

    if (now - lastCamTime > CAM_TIMEOUT) {
        if (camConnected) {
            Serial.println("\n>>> CAM LINK: LOST <<<");
            Serial.println(">>> Waiting for ESP8266 data... <<<");
            camConnected = false;
        } else if (now - lastStatusPrint > STATUS_INTERVAL) {
            Serial.println("[CAM] Searching for ESP8266 connection...");
            Serial.println("[CAM] Make sure ESP8266 is sending data on Serial3");
            lastStatusPrint = now;
        }
    } else {
        // Optional: periodic status when connected but no objects
        if (camConnected && (now - lastStatusPrint > STATUS_INTERVAL)) {
            Serial.println("[CAM] Connected - waiting for object detection...");
            lastStatusPrint = now;
        }
    }

    // ----------------------------------------------------------
    // BLOCK 3: Ultrasonic → Distance-Based Vibration
    // ----------------------------------------------------------
    if (!motorRunning && !motorInCooldown) {
        int dist = getFilteredDistance();

        if (dist >= 2 && dist <= CLOSE_DISTANCE) {
            // Zone 1: 2-25cm → FAST vibration (100ms on/off)
            motorOnDuration  = 100;
            motorOffDuration = 100;
            Serial.print("[ULTRASONIC] CLOSE: ");
            Serial.print(dist);
            Serial.println(" cm → Fast vibration");

            digitalWrite(VIB_PIN, HIGH);
            motorRunning   = true;
            motorPhaseOn   = true;
            motorStartTime = millis();

        } else if (dist > CLOSE_DISTANCE && dist <= FAR_DISTANCE) {
            // Zone 2: 25-50cm → SLOW vibration (500ms on/off)
            motorOnDuration  = 500;
            motorOffDuration = 500;
            Serial.print("[ULTRASONIC] FAR: ");
            Serial.print(dist);
            Serial.println(" cm → Slow vibration");

            digitalWrite(VIB_PIN, HIGH);
            motorRunning   = true;
            motorPhaseOn   = true;
            motorStartTime = millis();
        }
    }

    // Handle vibration ON/OFF phases (non-blocking)
    if (motorRunning && motorPhaseOn) {
        if (millis() - motorStartTime >= motorOnDuration) {
            digitalWrite(VIB_PIN, LOW);
            motorPhaseOn   = false;
            motorStartTime = millis();
        }
    }

    if (motorRunning && !motorPhaseOn) {
        if (millis() - motorStartTime >= motorOffDuration) {
            motorRunning       = false;
            motorInCooldown    = true;
            motorCooldownStart = millis();
        }
    }

    // Short cooldown between vibration cycles
    if (motorInCooldown && millis() - motorCooldownStart >= 100) {
        motorInCooldown = false;
    }
    
    // Small delay to prevent overwhelming the system
    delay(10);
}