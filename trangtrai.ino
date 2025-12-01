#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DHT.h"

// WiFi
const char* ssid = "HAUI  Staff";
const char* password = "00000000";
const char* STATUS_URL = "https://api-quan-ly-trang-trai.onrender.com/api/status";

// GPIO
const int ledVangPin = 2;    // LED vàng
const int fanPin     = 14;   // Quạt 1 (auto)
const int fan2Pin    = 25;   // Quạt 2 (server)
const int ledXanhPin = 27;   // LED xanh
const int pumpPin    = 26;   // Bơm
const int ldrPin     = 34;   // ADC ánh sáng

// DHT22
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// TLS
WiFiClientSecure client;

// Biến chia sẻ
volatile float gTemp = NAN;
volatile float gHum  = NAN;
volatile int   gLdr  = 0;

volatile bool gFanAuto = false;
volatile bool gPumpAuto = false;
volatile bool gLedXanhAuto = false;
volatile bool gLedVangFromServer = false;
volatile bool gFan2FromServer = false;

// Mutex bảo vệ biến dùng chung
SemaphoreHandle_t dataMutex;

// Khai báo task
void TaskWiFi(void* pv);
void TaskSensors(void* pv);
void TaskControl(void* pv);
void TaskSyncServer(void* pv);

void setup() {
  Serial.begin(115200);

  pinMode(ledVangPin, OUTPUT);
  pinMode(fanPin, OUTPUT);
  pinMode(fan2Pin, OUTPUT);
  pinMode(ledXanhPin, OUTPUT);
  pinMode(pumpPin, OUTPUT);

  // Trạng thái ban đầu
  digitalWrite(fanPin, LOW);
  digitalWrite(fan2Pin, LOW);
  digitalWrite(pumpPin, LOW);
  digitalWrite(ledXanhPin, LOW);
  digitalWrite(ledVangPin, LOW);

  dht.begin();

  // TLS bỏ xác thực để gọi HTTPS Render
  client.setInsecure();

  // Tạo mutex
  dataMutex = xSemaphoreCreateMutex();

  // Tạo các task FreeRTOS
  xTaskCreatePinnedToCore(TaskWiFi,      "WiFi",      4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(TaskSensors,   "Sensors",   4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskControl,   "Control",   4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskSyncServer,"SyncServer",6144, NULL, 1, NULL, 0);
}

void loop() {
  // Không dùng loop; mọi thứ chạy trong FreeRTOS tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// Task: đảm bảo WiFi luôn kết nối
void TaskWiFi(void* pv) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Đang kết nối WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi đã kết nối");

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ Mất WiFi, đang reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// Task: đọc cảm biến DHT22 và ánh sáng
void TaskSensors(void* pv) {
  for (;;) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int ldr = analogRead(ldrPin); // 0–4095

    if (!isnan(t) && !isnan(h)) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        gTemp = t;
        gHum  = h;
        gLdr  = ldr;
        xSemaphoreGive(dataMutex);
      }
      Serial.printf("🌡️ %.1f°C | 💧 %.1f%% | ☀️ %d\n", t, h, ldr);
    } else {
      Serial.println("❌ Lỗi đọc DHT22");
    }

    vTaskDelay(pdMS_TO_TICKS(3000)); // đọc mỗi 3s
  }
}

// Task: điều khiển tự động quạt1, bơm, LED xanh theo ngưỡng
void TaskControl(void* pv) {
  const int LDR_THRESHOLD = 2000; // ánh sáng yếu
  for (;;) {
    float t, h;
    int ldr;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      t = gTemp; h = gHum; ldr = gLdr;
      xSemaphoreGive(dataMutex);
    }

    // Quạt1: bật nếu temp > 30°C
    bool fanOn = (!isnan(t) && t > 30.0f);
    digitalWrite(fanPin, fanOn ? HIGH : LOW);
    gFanAuto = fanOn;

    // Bơm: bật nếu hum < 80%
    bool pumpOn = (!isnan(h) && h < 80.0f);
    digitalWrite(pumpPin, pumpOn ? HIGH : LOW);
    gPumpAuto = pumpOn;

    // LED xanh: bật nếu ánh sáng yếu
    bool ledXanhOn = (ldr < LDR_THRESHOLD);
    digitalWrite(ledXanhPin, ledXanhOn ? HIGH : LOW);
    gLedXanhAuto = ledXanhOn;

    vTaskDelay(pdMS_TO_TICKS(500)); // phản ứng nhanh
  }
}

// Task: đồng bộ LED vàng, quạt2, bơm từ server (API /api/status)
void TaskSyncServer(void* pv) {
  HTTPClient https;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      https.begin(client, STATUS_URL);
      https.setTimeout(10000); // 10s

      int code = https.GET();
      if (code > 0) {
        String payload = https.getString();
        DynamicJsonDocument doc(2048);
        auto err = deserializeJson(doc, payload);
        if (!err) {
          bool fan2State = doc["devices"]["fan"]  | false;
          bool ledState  = doc["devices"]["led"]  | false;
          bool pumpState = doc["devices"]["pump"] | false;

          int temp = doc["sensor"]["temp"] | 0;
          int hum  = doc["sensor"]["hum"]  | 0;
          int ldr  = doc["sensor"]["ldr"]  | 0;

          // LED vàng từ server
          gLedVangFromServer = ledState;
          digitalWrite(ledVangPin, ledState ? HIGH : LOW);

          // Quạt2 từ server
          gFan2FromServer = fan2State;
          digitalWrite(fan2Pin, fan2State ? HIGH : LOW);

          // Bơm từ server
          digitalWrite(pumpPin, pumpState ? HIGH : LOW);

          // In trạng thái
          Serial.printf("🌀 FAN2: %s | 🟡 LED: %s | 💧 PUMP: %s\n",
                        fan2State ? "ON" : "OFF",
                        ledState ? "ON" : "OFF",
                        pumpState ? "ON" : "OFF");

          Serial.printf("🌡️ %d°C | 💧 %d%% | ☀️ %d (server)\n", temp, hum, ldr);
        } else {
          Serial.println("❌ Lỗi parse JSON từ server");
        }
      } else {
        Serial.printf("❌ GET status lỗi: %s\n", https.errorToString(code).c_str());
      }
      https.end();
    } else {
      Serial.println("⚠️ Chưa có WiFi để sync server");
    }

    vTaskDelay(pdMS_TO_TICKS(2000)); // poll mỗi 2s
  }
}
