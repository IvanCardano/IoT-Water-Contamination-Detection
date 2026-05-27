/*
  ============================================================
  IoT-Assisted Detection and Prediction of Nanoparticle
  Contamination in Water Bodies Using Multi-Sensor Systems
  ============================================================

  Sensors:
    - DS18B20 Temperature  → GPIO4
    - Turbidity            → GPIO34
    - TDS                  → GPIO32
    - pH                   → GPIO35

  Cloud:
    - Firebase RTDB
    - Blynk

  Blynk Virtual Pins:
    V0 → Temperature
    V1 → pH
    V2 → Turbidity
    V3 → TDS
    V4 → Status
    V5 → Risk Score
    V6 → Prediction
    V7 → Future Risk
*/

// ============================================================
// LIBRARIES
// ============================================================

#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Firebase_ESP_Client.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ============================================================
// BLYNK
// ============================================================

#define BLYNK_TEMPLATE_ID   "TMPL60kB5WjSd"
#define BLYNK_TEMPLATE_NAME "NanoWater"
#define BLYNK_AUTH_TOKEN    "L88JwZUQDgP6ozS7-ycyoeMvVtOa20mq"

#include <BlynkSimpleEsp32.h>

// ============================================================
// WIFI
// ============================================================

#define WIFI_SSID     "WOBETIE"
#define WIFI_PASSWORD "shinmahwa30"

// ============================================================
// FIREBASE
// ============================================================

#define FIREBASE_API_KEY       "AIzaSyDFd_SelPSdlyI-4BusrBhtJ7Q9NnQc2bE"
#define FIREBASE_DATABASE_URL  "https://water-quality-iot-6e91d-default-rtdb.firebaseio.com/"

#define FIREBASE_USER_EMAIL    "johnivancardano@gmail.com"
#define FIREBASE_USER_PASSWORD "formyowngrowth<3"

// ============================================================
// PINS
// ============================================================

#define ONE_WIRE_BUS  4
#define TURBIDITY_PIN 34
#define TDS_PIN       32
#define PH_PIN        35

// ============================================================
// CALIBRATION
// ============================================================

// Temperature calibration
float tempOffset = -0.7;

// Turbidity calibration
float V_clean    = 3.2;
float V_dirty    = 1.5;

float NTU_clean  = 0.5;
float NTU_dirty  = 1000;

// pH calibration
float neutralVoltage = 2.25;

// ============================================================
// SMOOTHING
// ============================================================

const int turbSamples = 20;
const int phSamples   = 30;
const int tdsSamples  = 20;

// ============================================================
// TIMING
// ============================================================

unsigned long lastSensorRead   = 0;
unsigned long lastFirebasePush = 0;

const unsigned long SENSOR_INTERVAL   = 2000;
const unsigned long FIREBASE_INTERVAL = 5000;

// ============================================================
// OBJECTS
// ============================================================

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

BlynkTimer timer;

// ============================================================
// GLOBAL VALUES
// ============================================================

float g_temperature = 0;
float g_turbidity   = 0;
float g_tds         = 0;
float g_pH          = 0;

String g_status = "Unknown";

int g_riskScore = 0;

// Prediction
String g_prediction = "Stable";

int g_futureRisk = 0;

float previousRisk = 0;

// ============================================================
// RISK FUNCTIONS
// ============================================================

int scoreTurbidity(float ntu) {

  if (ntu <= 1.0)   return 0;
  if (ntu <= 4.0)   return (int)map(ntu, 1, 4, 1, 15);
  if (ntu <= 50.0)  return (int)map(ntu, 4, 50, 15, 50);
  if (ntu <= 300.0) return (int)map(ntu, 50, 300, 50, 80);

  return constrain((int)map(ntu, 300, 3000, 80, 100), 80, 100);
}

int scorePH(float ph) {

  if (ph >= 6.5 && ph <= 8.5) return 0;

  if (ph >= 6.0 && ph < 6.5)
    return (int)map(ph * 10, 60, 65, 20, 1);

  if (ph > 8.5 && ph <= 9.0)
    return (int)map(ph * 10, 85, 90, 1, 20);

  if (ph >= 5.0 && ph < 6.0)
    return (int)map(ph * 10, 50, 60, 60, 20);

  if (ph > 9.0 && ph <= 10.0)
    return (int)map(ph * 10, 90, 100, 20, 60);

  if (ph >= 4.0 && ph < 5.0)
    return (int)map(ph * 10, 40, 50, 85, 60);

  if (ph > 10.0 && ph <= 11.0)
    return (int)map(ph * 10, 100, 110, 60, 85);

  return 100;
}

int scoreTDS(float tds) {

  if (tds <= 300)  return 0;
  if (tds <= 600)  return (int)map(tds, 300, 600, 5, 25);
  if (tds <= 900)  return (int)map(tds, 600, 900, 25, 50);
  if (tds <= 1200) return (int)map(tds, 900, 1200, 50, 75);

  return constrain((int)map(tds, 1200, 9999, 75, 100), 75, 100);
}

int scoreTemperature(float temp) {

  if (temp >= 10 && temp <= 25) return 0;

  if (temp > 25 && temp <= 30)
    return (int)map(temp, 25, 30, 1, 20);

  if (temp >= 5 && temp < 10)
    return (int)map(temp, 5, 10, 20, 1);

  if (temp > 30 && temp <= 40)
    return (int)map(temp, 30, 40, 20, 60);

  if (temp >= 0 && temp < 5)
    return (int)map(temp, 0, 5, 60, 20);

  return 100;
}

// ============================================================
// RISK CALCULATION
// ============================================================

void calculateRisk() {

  int sTurb = scoreTurbidity(g_turbidity);
  int sPH   = scorePH(g_pH);
  int sTDS  = scoreTDS(g_tds);
  int sTemp = scoreTemperature(g_temperature);

  g_riskScore =
      (int)(sTurb * 0.35 +
            sPH   * 0.30 +
            sTDS  * 0.25 +
            sTemp * 0.10);

  g_riskScore = constrain(g_riskScore, 0, 100);

  // contamination boosting
  if (sTurb >= 80 || sPH >= 80 || sTDS >= 80)
    g_riskScore = max(g_riskScore, 65);

  if (sTurb >= 50 || sPH >= 50 || sTDS >= 50)
    g_riskScore = max(g_riskScore, 40);

  // status
  if      (g_riskScore <= 15) g_status = "Safe";
  else if (g_riskScore <= 35) g_status = "Low Risk";
  else if (g_riskScore <= 55) g_status = "Mild Contamination";
  else if (g_riskScore <= 75) g_status = "High Contamination";
  else                        g_status = "Severely Contaminated";
}

// ============================================================
// PREDICTION
// ============================================================

void calculatePrediction() {

  float delta = g_riskScore - previousRisk;

  // future risk prediction
  g_futureRisk = g_riskScore + (delta * 3);

  g_futureRisk = constrain(g_futureRisk, 0, 100);

  // prediction text
  if (delta >= 15) {

    g_prediction = "Rapid Contamination Rise";

  }
  else if (delta >= 5) {

    g_prediction = "Increasing Contamination";

  }
  else if (delta <= -15) {

    g_prediction = "Rapid Improvement";

  }
  else if (delta <= -5) {

    g_prediction = "Improving Water Quality";

  }
  else {

    g_prediction = "Stable";
  }

  previousRisk = g_riskScore;
}

// ============================================================
// SENSOR FUNCTIONS
// ============================================================

float readTemperature() {

  tempSensor.requestTemperatures();

  float raw = tempSensor.getTempCByIndex(0);

  if (raw == DEVICE_DISCONNECTED_C) {

    Serial.println("[TEMP] Sensor Error");

    return g_temperature;
  }

  return raw + tempOffset;
}

float readTurbidity() {

  long sum = 0;

  for (int i = 0; i < turbSamples; i++) {

    sum += analogRead(TURBIDITY_PIN);

    delay(5);
  }

  float rawAvg = sum / (float)turbSamples;

  float voltage = (rawAvg / 4095.0) * 3.3;

  float ntu =
      NTU_clean +
      (voltage - V_clean) *
      (NTU_dirty - NTU_clean) /
      (V_dirty - V_clean);

  if (ntu < 0)
    ntu = 0;

  return ntu;
}

float readTDS() {

  analogRead(TDS_PIN);
  delay(10);

  long total = 0;

  for (int i = 0; i < tdsSamples; i++) {

    total += analogRead(TDS_PIN);

    delay(10);
  }

  float avgRaw = total / (float)tdsSamples;

  float voltage = (avgRaw / 4095.0) * 3.3;

  float tds = (1250.0 * voltage) - 850.0;

  if (tds < 0)
    tds = 0;

  return tds;
}

float readPH() {

  analogRead(PH_PIN);

  delay(10);

  long total = 0;

  for (int i = 0; i < phSamples; i++) {

    total += analogRead(PH_PIN);

    delay(10);
  }

  float avgADC = total / (float)phSamples;

  float voltage = avgADC * (3.3 / 4095.0);

  float ph =
      7.0 +
      ((neutralVoltage - voltage) * 6.2);

  ph = constrain(ph, 0.0, 14.0);

  return ph;
}

// ============================================================
// BLYNK
// ============================================================

void sendToBlynk() {

  Blynk.virtualWrite(V0, g_temperature);

  Blynk.virtualWrite(V1, g_pH);

  Blynk.virtualWrite(V2, g_turbidity);

  Blynk.virtualWrite(V3, g_tds);

  Blynk.virtualWrite(V4, g_status);

  Blynk.virtualWrite(V5, g_riskScore);

  Blynk.virtualWrite(V6, g_prediction);

  Blynk.virtualWrite(V7, g_futureRisk);
}

// ============================================================
// FIREBASE
// ============================================================

void pushToFirebase() {

  if (!Firebase.ready())
    return;

  String path = "/readings/" + String(millis());

  FirebaseJson json;

  json.set("temperature", g_temperature);
  json.set("pH", g_pH);
  json.set("turbidity", g_turbidity);
  json.set("tds", g_tds);

  json.set("status", g_status);

  json.set("riskScore", g_riskScore);

  json.set("prediction", g_prediction);

  json.set("futureRisk", g_futureRisk);

  json.set("timestamp", (int)(millis() / 1000));

  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {

    Serial.println("[Firebase] Data pushed");

  }
  else {

    Serial.print("[Firebase] Error: ");

    Serial.println(fbdo.errorReason());
  }

  FirebaseJson latest;

  latest.set("temperature", g_temperature);
  latest.set("pH", g_pH);
  latest.set("turbidity", g_turbidity);
  latest.set("tds", g_tds);

  latest.set("status", g_status);

  latest.set("riskScore", g_riskScore);

  latest.set("prediction", g_prediction);

  latest.set("futureRisk", g_futureRisk);

  Firebase.RTDB.setJSON(&fbdo, "/latest", &latest);
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  analogReadResolution(12);

  // improve ADC range stability
  analogSetAttenuation(ADC_11db);

  // DS18B20
  tempSensor.begin();

  Serial.println("[TEMP] DS18B20 Initialized");

  // WiFi
  Serial.print("[WiFi] Connecting");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println("[WiFi] Connected");

  // Firebase
  config.api_key = FIREBASE_API_KEY;

  config.database_url = FIREBASE_DATABASE_URL;

  auth.user.email = FIREBASE_USER_EMAIL;

  auth.user.password = FIREBASE_USER_PASSWORD;

  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);

  Firebase.reconnectWiFi(true);

  Serial.println("[Firebase] Initialized");

  // Blynk
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD);

  timer.setInterval(2000L, sendToBlynk);

  Serial.println("[Blynk] Connected");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  Blynk.run();

  timer.run();

  unsigned long now = millis();

  // sensor reading
  if (now - lastSensorRead >= SENSOR_INTERVAL) {

    lastSensorRead = now;

    g_temperature = readTemperature();

    g_turbidity = readTurbidity();

    g_tds = readTDS();

    g_pH = readPH();

    calculateRisk();

    calculatePrediction();

    // optional event notification
    if (g_riskScore >= 80) {

      Blynk.logEvent(
        "high_risk",
        "Severe contamination detected!"
      );
    }

    Serial.println("========================================");

    Serial.printf("[Temp]        %.2f °C\n", g_temperature);

    Serial.printf("[pH]          %.2f\n", g_pH);

    Serial.printf("[Turbidity]   %.1f NTU\n", g_turbidity);

    Serial.printf("[TDS]         %.1f ppm\n", g_tds);

    Serial.printf("[Status]      %s\n", g_status.c_str());

    Serial.printf("[RiskScore]   %d%%\n", g_riskScore);

    Serial.printf("[Prediction]  %s\n", g_prediction.c_str());

    Serial.printf("[FutureRisk]  %d%%\n", g_futureRisk);

    Serial.println("========================================");
  }

  // Firebase push
  if (now - lastFirebasePush >= FIREBASE_INTERVAL) {

    lastFirebasePush = now;

    pushToFirebase();
  }
}
