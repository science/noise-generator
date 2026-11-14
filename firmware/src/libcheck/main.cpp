// libcheck — compile-and-link smoke test for the shipping library set.
//
// It is NEVER meant to be flashed. It exists so ./scripts/test.sh proves, on
// every run, that:
//   - the pinned libraries (ESP32-A2DP, arduino-mqtt, ArduinoJson) still
//     build against the pinned Arduino core;
//   - Bluetooth Classic A2DP source + Wi-Fi + MQTT together still fit one
//     ~1.9 MB OTA slot of min_spiffs.csv (watch the "Flash:" line).
// The real application arrives in Phase 1+ as env `app`.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BluetoothA2DPSource.h>
#include <MQTT.h>
#include <WiFi.h>

static BluetoothA2DPSource a2dp;
static WiFiClient net;
static MQTTClient mqtt(512);

static int32_t fillFrames(Frame* frames, int32_t count) {
    for (int32_t i = 0; i < count; ++i) frames[i] = Frame(0, 0);  // silence
    return count;
}

void setup() {
    pinMode(21, OUTPUT);
    digitalWrite(21, LOW);  // speaker amp off, as everywhere
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    mqtt.begin("127.0.0.1", net);
    JsonDocument doc;
    doc["libcheck"] = true;
    serializeJson(doc, Serial);
    a2dp.set_data_callback_in_frames(fillFrames);
    a2dp.start("libcheck-never-flashed");
}

void loop() { delay(1000); }
