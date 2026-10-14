// Dendrometer Logger - Adafruit Feather ESP32 V2
// Hardware: Adafruit Feather ESP32 V2 + Adalogger FeatherWing (SD + PCF8523 RTC) + ADS1115

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <WiFi.h>
#include <RTClib.h>
#include <Adafruit_ADS1X15.h>
#include <ESP_Google_Sheet_Client.h>

#include "credentials.h"

// ============================================================
//  USER CONFIGURATION
// ============================================================

// WiFi, Google service account, and spreadsheet settings live in credentials.h

// ============================================================
//  INTERVALS
// ============================================================

#define SAMPLE_INTERVAL_MS   20000UL
#define LOG_INTERVAL_MS      60000UL
#define STATUS_INTERVAL_MS    5000UL

// ============================================================
//  HARDWARE CONFIG (FEATHER ESP32 V2 PIN MAPPINGS)
// ============================================================

const int SD_CS_PIN = 33; // Feather Header pin "10" = GPIO 33 on ESP32 Feather V2
const int LED_PIN   = 13;

#define SD_SCK  5
#define SD_MOSI 19
#define SD_MISO 21

const long UTC_OFFSET_SECONDS = -7L * 3600L;

// ============================================================
//  OBJECTS
// ============================================================

RTC_PCF8523      rtc;
Adafruit_ADS1115 ads_sensor;

SdFat     sdCard;
SPIClass  sdSPI(VSPI);

// ============================================================
//  GLOBALS
// ============================================================

float sumS1 = 0, sumS2 = 0, sumS3 = 0, sumS4 = 0;
int   sampleCount = 0;
float latestS1 = 0, latestS2 = 0, latestS3 = 0, latestS4 = 0;

String cachedI2C = "scanning...";
bool   i2cFound[128] = { false };

unsigned long lastSampleTime = 0;
unsigned long lastLogTime    = 0;
unsigned long lastStatusTime = 0;

bool wifiConnected = false;
bool gsheetsReady  = false;
bool sdAvailable   = false;

// ============================================================
//  TOKEN STATUS CALLBACK
// ============================================================

void tokenStatusCallback(TokenInfo info) {
  Serial.print("[GSheets] Token: ");
  Serial.print(GSheet.getTokenType(info).c_str());
  Serial.print(" / ");
  Serial.println(GSheet.getTokenStatus(info).c_str());
  if (info.status == token_status_error) {
    Serial.print("[GSheets] ERROR: ");
    Serial.println(GSheet.getTokenError(info).c_str());
    gsheetsReady = false;
  }
}

// ============================================================
//  I2C BUS SCAN
// ============================================================

void scanI2C() {
  Serial.println("[I2C] Scanning...");
  cachedI2C = "";
  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      i2cFound[addr] = true;
      Serial.print("[I2C]   0x"); Serial.print(addr, HEX);
      if (addr == 0x48) Serial.print("  <- ADS1115");
      if (addr == 0x68) Serial.print("  <- PCF8523 RTC");
      Serial.println();

      char buf[16];
      snprintf(buf, sizeof(buf), "0x%02X ", addr);
      cachedI2C += String(buf);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("[I2C]   None found - check wiring");
  } else {
    Serial.print("[I2C] "); Serial.print(found); Serial.println(" device(s) found");
  }
}

// ============================================================
//  SENSOR READING
// ============================================================

float readChannel(uint8_t channel) {
  float val = ads_sensor.readADC_SingleEnded(channel);
  if (val < 10) {
    Serial.print("[ADS] WARNING: Ch"); Serial.print(channel);
    Serial.println(" near 0 - sensor disconnected?");
  }
  return val;
}

// ============================================================
//  SERIAL STATUS LINE
// ============================================================

void printStatus() {
  DateTime now_disp = rtc.now();

  Serial.print("[STATUS] ");
  char timebuf[16];
  snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", now_disp.hour(), now_disp.minute(), now_disp.second());
  Serial.print(timebuf);

  Serial.print("  S1:"); Serial.print(latestS1, 0);
  Serial.print(" S2:");  Serial.print(latestS2, 0);
  Serial.print(" S3:");  Serial.print(latestS3, 0);
  Serial.print(" S4:");  Serial.print(latestS4, 0);

  Serial.print("  Smpl ");
  Serial.print(sampleCount);
  Serial.print("/");
  Serial.print((int)(LOG_INTERVAL_MS / SAMPLE_INTERVAL_MS));

  Serial.print("  Wi:");   Serial.print(wifiConnected ? "OK" : "X");
  Serial.print(" SD:");    Serial.print(sdAvailable  ? "OK" : "X");
  Serial.print(" GS:");    Serial.print(gsheetsReady ? "OK" : "X");
  Serial.print("  I2C: "); Serial.println(cachedI2C);
}

// ============================================================
//  SYNC SYSTEM CLOCK FROM RTC
// ============================================================

void syncSystemClockFromRTC() {
  if (!rtc.initialized() || rtc.lostPower()) {
    Serial.println("[RTC] WARNING: RTC not set or lost power - clock may be wrong");
    return;
  }
  DateTime now = rtc.now();
  time_t epochTime = now.unixtime();
  time_t utcEpoch  = epochTime - UTC_OFFSET_SECONDS;

  struct timeval tv = { .tv_sec = utcEpoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  GSheet.setSystemTime(utcEpoch);

  Serial.print("[RTC] System clock synced. UTC epoch = ");
  Serial.println((long)utcEpoch);
}

// ============================================================
//  GSHEETS AUTH
// ============================================================

void beginGSheets() {
  if (strlen(CLIENT_EMAIL) == 0 || strlen(PROJECT_ID) == 0) return;
  Serial.println("[GSheets] Starting auth...");
  GSheet.setTokenCallback(tokenStatusCallback);
  GSheet.setPrerefreshSeconds(10 * 60);
  GSheet.begin(CLIENT_EMAIL, PROJECT_ID, PRIVATE_KEY);
  gsheetsReady = true;
  Serial.println("[GSheets] Auth initiated");
}

// ============================================================
//  WIFI
// ============================================================

bool connectWiFi() {
  if (strlen(WIFI_SSID) == 0) { wifiConnected = false; return false; }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    if (!gsheetsReady) {
      Serial.println("[WiFi] Already connected - re-initiating GSheets auth");
      syncSystemClockFromRTC();
      beginGSheets();
    }
    return true;
  }

  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! IP: "); Serial.println(WiFi.localIP());
    Serial.print("[WiFi] RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
    wifiConnected = true;
    syncSystemClockFromRTC();
    beginGSheets();
    return true;
  }
  Serial.print("[WiFi] FAILED. Code: "); Serial.println(WiFi.status());
  wifiConnected = false;
  gsheetsReady  = false;
  return false;
}

// ============================================================
//  INTERNET CHECK
// ============================================================

bool checkInternetConnection() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  client.setTimeout(3000);
  if (!client.connect("8.8.8.8", 53)) { client.stop(); return false; }
  client.stop();
  return true;
}

// ============================================================
//  SD CARD INIT (SdFat Direct)
// ============================================================

bool sdPermanentlyFailed = false;

bool initSD() {
  if (sdPermanentlyFailed) return false;
  Serial.println("[SD] Initializing...");

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS_PIN);

  SdSpiConfig spiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(12), &sdSPI);

  if (!sdCard.begin(spiConfig)) {
    Serial.println("[SD] FAILED - check FeatherWing seating and SD card");
    sdPermanentlyFailed = true;
    return false;
  }

  uint32_t cardSizeMB = sdCard.card()->sectorCount() / 2048;
  Serial.print("[SD] Card size: ");
  Serial.print(cardSizeMB); Serial.println(" MB");

  if (!sdCard.exists("/LDS.csv")) {
    FsFile hdr = sdCard.open("/LDS.csv", O_WRITE | O_CREAT | O_APPEND);
    if (hdr) {
      hdr.println("Timestamp,Sensor1_raw,Sensor2_raw,Sensor3_raw,Sensor4_raw");
      hdr.flush();
      hdr.close();
      Serial.println("[SD] Header written");
    } else {
      Serial.println("[SD] ERROR: could not create LDS.csv");
      return false;
    }
  } else {
    Serial.println("[SD] LDS.csv exists - appending");
  }
  return true;
}

// ============================================================
//  SD CARD WRITE
// ============================================================

bool writeToSD(const char *timestamp, float s1, float s2, float s3, float s4) {
  if (!sdAvailable) {
    sdAvailable = initSD();
    if (!sdAvailable) return false;
  }

  FsFile f = sdCard.open("/LDS.csv", O_WRITE | O_CREAT | O_APPEND);
  if (!f) {
    sdAvailable = initSD();
    if (!sdAvailable) return false;
    f = sdCard.open("/LDS.csv", O_WRITE | O_CREAT | O_APPEND);
    if (!f) return false;
  }

  f.print(timestamp); f.print(",");
  f.print(s1, 1);     f.print(",");
  f.print(s2, 1);     f.print(",");
  f.print(s3, 1);     f.print(",");
  f.println(s4, 1);
  f.flush();
  f.close();

  Serial.println("[SD] Write SUCCESS");
  return true;
}

// ============================================================
//  GOOGLE SHEETS UPLOAD
// ============================================================

bool uploadToGSheets(const char *timestamp, float s1, float s2, float s3, float s4) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[GSheets] SKIP - WiFi not connected");
    return false;
  }
  if (!gsheetsReady) {
    Serial.println("[GSheets] SKIP - auth not initiated");
    return false;
  }

  if (!GSheet.ready()) {
    Serial.println("[GSheets] Token not ready - waiting up to 10s...");
    unsigned long tw = millis();
    while (!GSheet.ready() && millis() - tw < 10000) { delay(200); }
    if (!GSheet.ready()) {
      Serial.println("[GSheets] SKIP - token still not ready after 10s");
      return false;
    }
    Serial.println("[GSheets] Token ready");
  }

  FirebaseJson response, valueRange;
  valueRange.add("majorDimension", "ROWS");
  valueRange.set("values/[0]/[0]", timestamp);
  valueRange.set("values/[0]/[1]", s1);
  valueRange.set("values/[0]/[2]", s2);
  valueRange.set("values/[0]/[3]", s3);
  valueRange.set("values/[0]/[4]", s4);

  unsigned long t_upload = millis();
  bool ok = GSheet.values.append(&response, spreadsheetId, SHEET_RANGE, &valueRange);
  unsigned long elapsed = millis() - t_upload;
  valueRange.clear();

  Serial.print("[GSheets] Upload took "); Serial.print(elapsed); Serial.println(" ms");
  if (ok) {
    Serial.println("[GSheets] Upload SUCCESS");
  } else {
    Serial.print("[GSheets] FAILED: "); Serial.println(GSheet.errorReason());
  }
  return ok;
}

// ============================================================
//  STATUS BLINK
// ============================================================

void statusBlink(bool sdOk, bool gsOk) {
  int n = (sdOk && gsOk) ? 2 : (sdOk || gsOk) ? 1 : 0;
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(200);
  }
  if (n == 0) {
    for (int i = 0; i < 4; i++) {
      digitalWrite(LED_PIN, HIGH); delay(50);
      digitalWrite(LED_PIN, LOW);  delay(50);
    }
  }
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  unsigned long serialWait = millis();
  while (!Serial && millis() - serialWait < 5000) { delay(10); }
  delay(500);

  Serial.println("\n========================================");
  Serial.println("  Dendrometer Logger - Feather ESP32 V2");
  Serial.println("========================================");

  Wire.begin();
  pinMode(LED_PIN, OUTPUT);
  scanI2C();

  Serial.println("[RTC] Initializing PCF8523...");
  if (!rtc.begin()) {
    Serial.println("[RTC] ERROR: PCF8523 not found - check FeatherWing seating");
  } else {
    if (!rtc.initialized() || rtc.lostPower()) {
      Serial.println("[RTC] WARNING: RTC not set or lost power");
    } else {
      DateTime now = rtc.now();
      Serial.print("[RTC] Time: ");
      Serial.print(now.day());   Serial.print("/");
      Serial.print(now.month()); Serial.print("/");
      Serial.print(now.year());  Serial.print("  ");
      Serial.print(now.hour());  Serial.print(":");
      if (now.minute() < 10) Serial.print("0");
      Serial.print(now.minute()); Serial.print(":");
      if (now.second() < 10) Serial.print("0");
      Serial.println(now.second());
    }
    rtc.start();
  }

  Serial.println("[ADS] Initializing ADS1115 at 0x48...");
  if (!ads_sensor.begin(0x48)) {
    Serial.println("[ADS] ERROR - check ADDR->GND, SDA/SCL, VCC->3.3V");
  } else {
    ads_sensor.setGain(GAIN_ONE);
    Serial.println("[ADS] Ready (GAIN_ONE, +/-4.096V)");
    for (int ch = 0; ch < 4; ch++) {
      int16_t raw = ads_sensor.readADC_SingleEnded(ch);
      Serial.print("[ADS]   Ch"); Serial.print(ch);
      Serial.print(" = "); Serial.print(raw);
      Serial.print(" (~"); Serial.print(raw * 0.000125, 3); Serial.println(" V)");
    }
  }

  sdAvailable = initSD();
  if (!sdAvailable) Serial.println("[SD] Logging disabled until card reachable");

  syncSystemClockFromRTC();

  if (strlen(WIFI_SSID) > 0) connectWiFi();
  else Serial.println("[WiFi] SSID empty - skipping");

  lastSampleTime = millis();
  lastLogTime    = millis();
  lastStatusTime = millis();

  Serial.print("Sample interval : "); Serial.print(SAMPLE_INTERVAL_MS / 1000); Serial.println(" s");
  Serial.print("Log interval    : "); Serial.print(LOG_INTERVAL_MS    / 1000); Serial.println(" s");
  Serial.print("Samples per log : "); Serial.println(LOG_INTERVAL_MS / SAMPLE_INTERVAL_MS);
  Serial.println("========================================");
  Serial.println("Setup complete.");
  Serial.print("SD      : "); Serial.println(sdAvailable  ? "ENABLED" : "DISABLED");
  Serial.print("GSheets : "); Serial.println(gsheetsReady ? "ENABLED" : "DISABLED");
  Serial.println("========================================\n");
}

// ============================================================
//  LOOP
// ============================================================

void loop() {
  if (gsheetsReady) GSheet.ready();

  unsigned long now = millis();

  if (now - lastStatusTime >= STATUS_INTERVAL_MS) {
    lastStatusTime = now;
    printStatus();
  }

  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    float r1 = readChannel(0);
    float r2 = readChannel(1);
    float r3 = readChannel(2);
    float r4 = readChannel(3);
    latestS1 = r1; latestS2 = r2; latestS3 = r3; latestS4 = r4;
    sumS1 += r1; sumS2 += r2; sumS3 += r3; sumS4 += r4;
    sampleCount++;

    Serial.println("\n---------- SAMPLE ----------");
    Serial.print("[UPTIME] "); Serial.print(now / 1000); Serial.println(" s");
    Serial.print("[ADS] S1="); Serial.print(r1, 1);
    Serial.print(" S2=");      Serial.print(r2, 1);
    Serial.print(" S3=");      Serial.print(r3, 1);
    Serial.print(" S4=");      Serial.println(r4, 1);
    Serial.print("[SAMPLE] "); Serial.print(sampleCount);
    Serial.print(" / ");       Serial.println(LOG_INTERVAL_MS / SAMPLE_INTERVAL_MS);
  }

  if (now - lastLogTime >= LOG_INTERVAL_MS && sampleCount > 0) {
    lastLogTime = now;
    float s1 = sumS1 / sampleCount;
    float s2 = sumS2 / sampleCount;
    float s3 = sumS3 / sampleCount;
    float s4 = sumS4 / sampleCount;
    sumS1 = sumS2 = sumS3 = sumS4 = 0;
    sampleCount = 0;

    Serial.println("\n========== LOG CYCLE ==========");
    Serial.print("[HEAP] "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes free");

    DateTime now_rtc = rtc.now();
    char ts[32];
    snprintf(ts, sizeof(ts), "%02d/%02d/%02d %02d:%02d:%02d",
             now_rtc.day(), now_rtc.month(), now_rtc.year() % 100,
             now_rtc.hour(), now_rtc.minute(), now_rtc.second());
    Serial.print("[RTC] "); Serial.println(ts);

    Serial.print("[LOG] S1="); Serial.print(s1, 1);
    Serial.print(" S2="); Serial.print(s2, 1);
    Serial.print(" S3="); Serial.print(s3, 1);
    Serial.print(" S4="); Serial.println(s4, 1);

    bool sdOk = writeToSD(ts, s1, s2, s3, s4);
    bool gsOk = false;

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
    } else {
      wifiConnected = false;
      gsheetsReady  = false;
      connectWiFi();
    }

    if (wifiConnected && checkInternetConnection()) {
      gsOk = uploadToGSheets(ts, s1, s2, s3, s4);
      if (!gsOk) {
        Serial.println("[GSheets] Retrying after reconnect...");
        gsheetsReady = false;
        connectWiFi();
        if (wifiConnected && checkInternetConnection()) {
          gsOk = uploadToGSheets(ts, s1, s2, s3, s4);
        }
      }
    }

    Serial.println("---------- SUMMARY ----------");
    Serial.print("  SD      : "); Serial.println(sdOk ? "OK" : "FAIL");
    Serial.print("  GSheets : "); Serial.println(gsOk ? "OK" : "FAIL");
    Serial.println("==============================\n");

    statusBlink(sdOk, gsOk);
  }
}