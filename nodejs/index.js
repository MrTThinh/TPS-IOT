const mqtt  = require("mqtt");
const admin = require("firebase-admin");
const axios = require("axios");

/* ================= FIREBASE ================= */
const serviceAccount = require("./serviceAccountKey.json");

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
  databaseURL: ""
});

const db = admin.database();

/* ================= MQTT ================= */
const MQTT_BROKER = "";
const MQTT_USER   = "";
const MQTT_PASS   = "";
const MQTT_TOPIC  = "esp32/health";

const mqttClient = mqtt.connect(MQTT_BROKER, {
  username: MQTT_USER,
  password: MQTT_PASS
});

/* ================= THINGSPEAK ================= */
const THINGSPEAK_KEY = "";
let lastThingSpeakSend = 0;

/* ================= MQTT EVENTS ================= */
mqttClient.on("connect", () => {
  console.log("✅ MQTT connected");
  mqttClient.subscribe(MQTT_TOPIC);
});

mqttClient.on("message", async (topic, message) => {
  try {
    const payload = JSON.parse(message.toString());

    /* ===== CHUẨN HÓA DỮ LIỆU ===== */
    const data = {
      hrData:   Math.round(Number(payload.heart_rate) * 10) / 10, // 1 số thập phân
      spo2Data: Number(payload.SpO2),
      tempData: Math.round(Number(payload.temp) * 10) / 10,
      humidity: Math.round(Number(payload.humidity) * 10) / 10
    };

    console.log("📥 Data:", data);

    /* ===== FIREBASE: GHI RA ROOT ===== */
    await db.ref("/").update(data);

    /* ===== THINGSPEAK: 15 GIÂY / LẦN ===== */
    const now = Date.now();
    if (now - lastThingSpeakSend > 16000) {
      lastThingSpeakSend = now;

      const url =
        `https://api.thingspeak.com/update?api_key=${THINGSPEAK_KEY}` +
        `&field1=${data.hrData}` +
        `&field2=${data.spo2Data}` +
        `&field3=${data.tempData}` +
        `&field4=${data.humidity}`;

      await axios.get(url);
      console.log("📊 Sent to ThingSpeak");
    }

  } catch (err) {
    console.error("❌ Error:", err.message);
  }
});
