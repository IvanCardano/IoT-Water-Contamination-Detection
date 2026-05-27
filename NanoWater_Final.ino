// ═══════════════════════════════════════════════════════════════════════════
//  NanoWater — IoT Water Quality Monitor with On-Device ML Prediction
//  Microcontroller : ESP32
//  Sensors         : DS18B20 (Temp), PH-4502C (pH), Generic TDS
//  Cloud           : Firebase Realtime DB + Blynk
//  ML              : On-device weighted Z-score anomaly scoring
// ═══════════════════════════════════════════════════════════════════════════

#define BLYNK_TEMPLATE_ID   "TMPL60kB5WjSd"
#define BLYNK_TEMPLATE_NAME "NanoWater"
#define BLYNK_AUTH_TOKEN    "L88JwZUQDgP6ozS7-ycyoeMvVtOa20mq"

// ── MUST come before any Firebase / SSL include ────────────────────────────
// Tells the ESP32 Firebase client to skip certificate verification.
// Required when the board's clock isn't NTP-synced or no root CA is loaded.
// This is standard practice for ESP32 + Firebase on local/school networks.
#define FIREBASE_CLIENT_INSECURE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <BlynkSimpleEsp32.h>
#include <FirebaseESP32.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>    // fabsf(), expf()

// ── WIFI ──────────────────────────────────────────────────────────────────
char ssid[] = "MPC Students";
char pass[] = "Qu4lity3xperts@mPc2025.sM2";

// ── FIREBASE ──────────────────────────────────────────────────────────────
// Host must NOT have https:// prefix — the library adds it internally.
// Wrong host format is one of the top causes of SSL "connection refused".
#define FIREBASE_HOST "nanowater-53e56-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "uz7LdZGHPoNIGJeJN4eXlvaQie017nu3FNyyd8sE"

FirebaseData   firebaseData;
FirebaseAuth   auth;
FirebaseConfig config;

// ── BLYNK VIRTUAL PINS ────────────────────────────────────────────────────
//  V0 = Temperature  (Double, °C,   0 – 50)
//  V1 = pH Level     (Double, pH,   0 – 14)
//  V2 = TDS          (Double, ppm,  0 – 9999)   ← was V3; V2 freed by turbidity removal
//  V3 = Status       (String)                   ← was V4
//  V4 = Risk Score   (Integer, %,   0 – 100)    ← was V5

// ── SENSOR PINS ───────────────────────────────────────────────────────────
#define PH_PIN       35
#define TDS_PIN      32
#define ONE_WIRE_BUS  4

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// ── LIVE READINGS ─────────────────────────────────────────────────────────
float  temperature = 0.0;
float  pHValue     = 0.0;
float  tdsValue    = 0.0;
String statusText  = "INITIALIZING";
int    riskScore   = 0;   // Rule-based  (0 / 25 / 50 / 75)
int    mlRisk      = 0;   // ML anomaly  (0 – 100)

// ═══════════════════════════════════════════════════════════════════════════
//  ML — ON-DEVICE WEIGHTED Z-SCORE ANOMALY DETECTOR
//
//  3 active sensors: pH, TDS, Temperature  (turbidity removed)
//  Weights redistributed to sum to 1.0:
//    pH   35%  — most direct contamination indicator
//    TDS  40%  — nanoparticle / dissolved solids proxy (highest weight)
//    Temp 25%  — biological growth threshold
//
//  Per-parameter formula:
//    z  = |reading − μ| / σ
//    p  = 1 − exp(−z²/2)     ← Gaussian anomaly probability (0→1)
//    contribution = weight × p × 100
//  mlRisk = Σ contributions, clamped 0–100
//
//  Baselines: WHO Drinking-water Quality Guidelines 4th ed.
//             EPA Secondary Drinking Water Standards (40 CFR §143)
// ═══════════════════════════════════════════════════════════════════════════
struct SensorParam { float mu; float sigma; float weight; };

const SensorParam ML_PARAMS[3] = {
  //  mu       sigma    weight
  {  7.0f,   0.75f,   0.35f },   // pH       ideal 7.0, ±0.75 mild   (35%)
  { 250.0f, 150.0f,   0.40f },   // TDS ppm  ideal 250, ±150 mild    (40%)
  { 25.0f,   5.0f,    0.25f },   // Temp °C  ideal 25,  ±5 mild      (25%)
};

int computeMLRisk(float ph, float tds, float temp) {
  float readings[3] = { ph, tds, temp };
  float total = 0.0f;
  for (int i = 0; i < 3; i++) {
    float z = fabsf(readings[i] - ML_PARAMS[i].mu) / ML_PARAMS[i].sigma;
    float p = 1.0f - expf(-(z * z) / 2.0f);
    total += ML_PARAMS[i].weight * p * 100.0f;
  }
  return (int)constrain(total, 0.0f, 100.0f);
}

String mlStatusLabel(int risk) {
  if (risk < 15) return "SAFE";
  if (risk < 35) return "LOW RISK";
  if (risk < 60) return "MODERATE RISK";
  if (risk < 80) return "HIGH RISK";
  return "CRITICAL";
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  tempSensor.begin();

  // ── WiFi ────────────────────────────────────────────────────────────────
  WiFi.begin(ssid, pass);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK  IP: " + WiFi.localIP().toString());

  // ── Blynk ───────────────────────────────────────────────────────────────
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  // ── Firebase ────────────────────────────────────────────────────────────
  // FIX 1: setInsecure() on the internal SSL client so the ESP32 doesn't
  //         reject Firebase's certificate (fixes "Failed to initialize SSL layer").
  // FIX 2: host without https:// prefix (fixes "connection refused").
  config.database_url               = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  // Apply insecure SSL before Firebase.begin() so the client is configured first
  firebaseData.getWiFiClient().setInsecure();

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Raise the HTTP response buffer — prevents partial-read SSL write errors
  Firebase.setResponseSize(firebaseData, 1024);

  Serial.print("Connecting Firebase");
  unsigned long t0 = millis();
  while (!Firebase.ready() && millis() - t0 < 12000) { delay(500); Serial.print("."); }
  Serial.println(Firebase.ready() ? "\nFirebase OK" : "\nFirebase timeout — will retry");
}

// ═══════════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════════
void loop() {
  Blynk.run();

  readTemperature();
  readPH();
  readTDS();
  evaluateStatus();
  mlRisk = computeMLRisk(pHValue, tdsValue, temperature);

  printData();
  uploadFirebase();
  uploadBlynk();

  delay(2000);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SENSORS
// ═══════════════════════════════════════════════════════════════════════════

// DS18B20 — digital; no ADC noise
void readTemperature() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t <= -100.0) {
    Serial.println("[TEMP] Sensor error — check 4.7k pull-up on GPIO4");
    temperature = 25.0;
  } else {
    temperature = t;
  }
}

// PH-4502C — linear calibration: slope 0.18 V/pH, midpoint 2.5 V @ pH 7
// Calibrate by trimming the on-board potentiometer in a pH 7 buffer solution.
void readPH() {
  long total = 0;
  for (int i = 0; i < 10; i++) { total += analogRead(PH_PIN); delay(10); }
  float voltage = (total / 10.0) * (3.3 / 4095.0);
  pHValue = constrain(7.0 + ((2.5 - voltage) / 0.18), 0.0, 14.0);
}

// Generic TDS — polynomial regression + temperature compensation
// fmaxf() avoids the max(float,double) compile error
void readTDS() {
  long total = 0;
  for (int i = 0; i < 10; i++) { total += analogRead(TDS_PIN); delay(10); }
  float voltage     = (total / 10.0) * (3.3 / 4095.0);
  float compCoeff   = 1.0 + 0.02 * (temperature - 25.0);
  float compVoltage = voltage / compCoeff;
  float raw = (133.42f * (float)pow(compVoltage, 3)
             - 255.86f * (float)pow(compVoltage, 2)
             + 857.39f * compVoltage) * 0.5f;
  tdsValue = fmaxf(0.0f, raw);
}

// ═══════════════════════════════════════════════════════════════════════════
//  RULE-BASED STATUS & RISK SCORE
//  Parameter | Safe range  | Standard
//  pH        | 6.5 – 8.5  | WHO Drinking Water Guidelines
//  TDS       | ≤ 500 ppm  | EPA Secondary Standard
//  Temp      | ≤ 35 °C    | Biological threshold
//
//  Each violation adds 34 pts (3 sensors × 34 ≈ 100 max)
//  Score  | Status
//  0      | SAFE
//  34     | MILD CONTAMINATION
//  68     | MODERATE CONTAMINATION
//  100    | SEVERE CONTAMINATION
// ═══════════════════════════════════════════════════════════════════════════
void evaluateStatus() {
  int score = 0;
  if (pHValue < 6.5 || pHValue > 8.5) score += 34;
  if (tdsValue > 500)                  score += 34;
  if (temperature > 35.0)             score += 34;
  riskScore = min(score, 100);

  if      (score == 0)  statusText = "SAFE";
  else if (score <= 34) statusText = "MILD CONTAMINATION";
  else if (score <= 68) statusText = "MODERATE CONTAMINATION";
  else                  statusText = "SEVERE CONTAMINATION";
}

// ═══════════════════════════════════════════════════════════════════════════
//  SERIAL DEBUG
// ═══════════════════════════════════════════════════════════════════════════
void printData() {
  Serial.println("══════════════════════════════════════");
  Serial.printf("  Temperature  : %.2f C\n",    temperature);
  Serial.printf("  pH           : %.2f\n",       pHValue);
  Serial.printf("  TDS          : %.1f ppm\n",   tdsValue);
  Serial.printf("  Status       : %s\n",          statusText.c_str());
  Serial.printf("  Risk (rules) : %d%%\n",        riskScore);
  Serial.printf("  Risk (ML)    : %d%% [%s]\n",  mlRisk, mlStatusLabel(mlRisk).c_str());
  Serial.println("══════════════════════════════════════");
}

// ═══════════════════════════════════════════════════════════════════════════
//  FIREBASE UPLOAD — 2 round-trips via updateNode() JSON batching
// ═══════════════════════════════════════════════════════════════════════════
void uploadFirebase() {
  if (!Firebase.ready()) {
    Serial.println("[FB] Not ready — skipping");
    return;
  }

  FirebaseJson json;
  json.set("temperature", temperature);
  json.set("ph",          pHValue);
  json.set("tds",         tdsValue);
  json.set("status",      statusText);
  json.set("riskScore",   riskScore);
  json.set("mlRisk",      mlRisk);
  json.set("mlStatus",    mlStatusLabel(mlRisk));

  // ① Latest snapshot (overwritten every loop)
  if (!Firebase.updateNode(firebaseData, "/NanoWater", json)) {
    Serial.println("[FB] Snapshot error: " + firebaseData.errorReason());
    return;
  }

  // ② Append to history log (keyed by uptime ms)
  String histPath = "/history/" + String(millis());
  if (!Firebase.updateNode(firebaseData, histPath, json)) {
    Serial.println("[FB] History error: " + firebaseData.errorReason());
  } else {
    Serial.println("[FB] Upload OK");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  BLYNK UPLOAD
//  V0 Temp | V1 pH | V2 TDS | V3 Status | V4 Risk Score
// ═══════════════════════════════════════════════════════════════════════════
void uploadBlynk() {
  Blynk.virtualWrite(V0, temperature);
  Blynk.virtualWrite(V1, pHValue);
  Blynk.virtualWrite(V2, tdsValue);
  Blynk.virtualWrite(V3, statusText);
  Blynk.virtualWrite(V4, riskScore);
}
