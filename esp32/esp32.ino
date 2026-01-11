#include <WiFi.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

/* ===== ISRG Root X1 – CA Let's Encrypt ===== */
static const char ISRG_Root_X1[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----

-----END CERTIFICATE-----
)PEM";

/* =================== CONFIG =================== */
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";

const char* MQTT_HOST = "";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "";
const char* MQTT_PASSW = "";

#define DEVICE_ID "ESP32_MAX30100"
#define SDA_PIN 21
#define SCL_PIN 22
#define MAX30100_ADDR 0x57
#define MAX_REINIT_RETRY 3
#define MAX_BAD_READS    6

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

#define DHTPIN 4
#define DHTTYPE DHT11

// ===== LED ALERT (GPIO25) =====
// Nối: 3V3 -> R(220-330) -> Anode LED ; Cathode LED -> GPIO25
// => ACTIVE LOW: LOW sáng, HIGH tắt
#define LED_ALERT_PIN 25
#define LED_ACTIVE_LOW 1

// Ngưỡng cảnh báo (tự chỉnh)
#define HR_MAX    120.0   // bpm: nhịp tim quá cao
#define SPO2_MIN   92.0   // %  : SpO2 thấp
#define TEMP_MAX  38.0    // °C : nhiệt độ cao
#define HUM_MAX   80.0    // %  : độ ẩm cao

/* =================== OBJECTS =================== */
WiFiClientSecure net;
PubSubClient mqtt(net);
PulseOximeter pox;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);

/* =================== VARIABLES =================== */
unsigned long lastSensorOK = 0;
uint8_t badReadCount = 0;
unsigned long lastReport = 0, lastCheck = 0;
bool tlsConfiguredStrict = false;

/* =================== HELPER FUNCTIONS =================== */
void onBeatDetected(){ Serial.println("Beat"); }

void setAlertLed(bool on){
  if(LED_ACTIVE_LOW){
    digitalWrite(LED_ALERT_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(LED_ALERT_PIN, on ? HIGH : LOW);
  }
}

bool syncTimeUntilOk(uint32_t timeout_ms = 60000) {
  configTime(7*3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm t{};
  uint32_t start = millis();
  Serial.print("NTP");
  while (millis() - start < timeout_ms) {
    if (getLocalTime(&t, 2000) && (t.tm_year + 1900) >= 2020) {
      Serial.printf(" -> %04d-%02d-%02d %02d:%02d:%02d\n",
                    t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec);
      return true;
    }
    Serial.print('.');
  }
  Serial.println("\nNTP FAIL");
  return false;
}

void printSSLError(){
  char buf[256]; memset(buf,0,sizeof(buf));
  int err = net.lastError(buf, sizeof(buf)-1);
  Serial.printf("TLS lastError=%d : %s\n", err, buf);
}

/* =================== WIFI =================== */
void setup_wifi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  for(int i=0;i<60 && WiFi.status()!=WL_CONNECTED;i++){
    delay(250); Serial.print('.');
  }
  Serial.println(WiFi.status()==WL_CONNECTED ? " WIFI OK" : " FAIL");
}

/* =================== MQTT =================== */
void reconnect(){
  while(!mqtt.connected() && WiFi.status()==WL_CONNECTED){
    String cid = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.print("MQTT...");
    if(mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASSW)){
      Serial.println("connected");
      mqtt.subscribe("esp32/client");
    } else {
      Serial.printf("rc=%d\n", mqtt.state());
      printSSLError();
      delay(3000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int len){
  String s; s.reserve(len);
  for(unsigned int i=0;i<len;i++) s+=(char)payload[i];
  Serial.printf("<< [%s] %s\n", topic, s.c_str());
}

/* =================== MAX30100 =================== */
bool probeMAX30100() {
  Wire.beginTransmission(MAX30100_ADDR);
  uint8_t err = Wire.endTransmission();
  if (err == 0) { Serial.println("MAX30100 found at 0x57"); return true; }
  Serial.printf("MAX30100 not found (I2C err=%u)\n", err);
  return false;
}

bool initMAX30100(uint8_t retries = MAX_REINIT_RETRY) {
  for (uint8_t i=0; i<retries; i++) {
    if (!probeMAX30100()) { delay(200); continue; }
    if (pox.begin()) {
      pox.setIRLedCurrent(MAX30100_LED_CURR_27_1MA);
      pox.setOnBeatDetectedCallback(onBeatDetected);
      badReadCount = 0; lastSensorOK = millis();
      Serial.println("MAX30100 init OK");
      display.clearDisplay();
      display.setCursor(0,0);
      display.println("MAX30100 OK");
      display.display();
      return true;
    }
    Serial.println("pox.begin() failed, retry...");
    delay(200);
  }
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("MAX30100 ERROR");
  display.display();
  return false;
}

void ensureSensorAlive(float hr, float spo2) {
  bool ok = isfinite(hr) && isfinite(spo2) && hr>0 && spo2>0 && spo2<=100;
  if (ok) { badReadCount = 0; lastSensorOK = millis(); return; }
  if (++badReadCount >= MAX_BAD_READS) {
    Serial.println("Re-init MAX30100 (bad reads)");
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Reinit MAX30100");
    display.display();
    initMAX30100();
  }
}

/* =================== MQTT PUBLISH =================== */
void publishJSON(float hr, float spo2, float temp, float hum){
  DynamicJsonDocument doc(256);
  doc["deviceId"]  = DEVICE_ID;
  doc["heart_rate"]= hr;
  doc["SpO2"]      = spo2;
  doc["temp"]      = temp;
  doc["humidity"]  = hum;
  doc["sensor_ok"] = (badReadCount==0);

  time_t nowSec = time(nullptr);
  uint64_t ts_ms = (nowSec > 1000) ? (uint64_t)nowSec * 1000ULL : (uint64_t)millis();
  doc["ts"] = ts_ms;

  String s; serializeJson(doc, s);
  mqtt.publish("esp32/health", s.c_str(), true);
}

/* =================== TLS =================== */
void enableTLS_Strict(){
  net.setCACert(ISRG_Root_X1);
  net.setHandshakeTimeout(30);
  tlsConfiguredStrict = true;
  Serial.println("TLS: strict CA mode");
}

void enableTLS_Insecure(){
  net.setInsecure();
  tlsConfiguredStrict = false;
  Serial.println("TLS: INSECURE mode (test only!)");
}

/* =================== SETUP =================== */
void setup(){
  Serial.begin(9600);
  Wire.begin(SDA_PIN, SCL_PIN);

  // LED alert
  pinMode(LED_ALERT_PIN, OUTPUT);
  setAlertLed(false);

  // OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)){
    Serial.println("OLED init failed");
    while(true);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("IoT Health Monitor");
  display.display();

  dht.begin();
  setup_wifi();
  syncTimeUntilOk();
  enableTLS_Strict();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(callback);

  if(!initMAX30100()){
    Serial.println("MAX30100 unavailable, continue for MQTT test.");
  }

  Serial.println("Setup done.");
}

/* =================== LOOP =================== */
void loop(){
  // Giữ kết nối
  if(millis()-lastCheck>4000){
    if(WiFi.status()!=WL_CONNECTED) setup_wifi();
    if(!mqtt.connected() && WiFi.status()==WL_CONNECTED){
      String cid = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      Serial.print("MQTT...");
      if(mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASSW)){
        Serial.println("connected");
        mqtt.subscribe("esp32/client");
      } else {
        int st = mqtt.state();
        Serial.printf("rc=%d\n", st);
        printSSLError();
        char buf[128]; int e = net.lastError(buf, sizeof(buf)-1);
        if (e == -8576 && tlsConfiguredStrict) {
          Serial.println("Detected X509 PEM issue -> enabling INSECURE TLS for test.");
          enableTLS_Insecure();
          mqtt.setServer(MQTT_HOST, MQTT_PORT);
        }
      }
    }
    lastCheck = millis();
  }

  if(mqtt.connected()) mqtt.loop();

  // Cập nhật cảm biến
  pox.update();

  // Gửi/hiển thị mỗi 1s
  if(millis()-lastReport>1000){
    float hr = pox.getHeartRate();
    float sp = pox.getSpO2();
    ensureSensorAlive(hr, sp);

    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();

    // ===== LED ALERT LOGIC =====
    bool alarm = false;
    if(isfinite(hr)   && hr > HR_MAX)     alarm = true;
    if(isfinite(sp)   && sp < SPO2_MIN)   alarm = true; // SpO2 thấp
    if(isfinite(temp) && temp > TEMP_MAX) alarm = true;
    if(isfinite(hum)  && hum > HUM_MAX)   alarm = true;

    // Nếu MAX30100 đang đọc lỗi nhiều lần thì cũng cảnh báo
    if(badReadCount > 0) alarm = true;

    setAlertLed(alarm);

    // OLED hiển thị
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Device HealthMonitor");

    display.printf("HR: ");
    if(isfinite(hr)) display.printf("%.1f bpm\n", hr);
    else display.println("-- bpm");

    display.printf("SpO2: ");
    if(isfinite(sp)) display.printf("%.1f %%\n", sp);
    else display.println("-- %");

    display.printf("Temp: ");
    if(isfinite(temp)) display.printf("%.1f C\n", temp);
    else display.println("-- C");

    display.printf("Hum: ");
    if(isfinite(hum)) display.printf("%.1f %%\n", hum);
    else display.println("-- %");

    // Hiển thị trạng thái LED trên OLED (tùy thích)
    display.printf("ALARM: %s\n", alarm ? "ON" : "OFF");

    display.display();

    if(mqtt.connected()) publishJSON(hr, sp, temp, hum);

    lastReport = millis();
  }
}
