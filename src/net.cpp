#include "net.h"
#include "app_state.h"
#include "radio_tools.h"
#include "sw3518.h"

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef MQTT_BASE
#define MQTT_BASE "geek/sw3518"
#endif
#ifndef MQTT_DISCOVERY_PREFIX
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#endif

static constexpr bool kWifiOn = (WIFI_SSID[0] != '\0');
static constexpr bool kMqttOn = kWifiOn && (MQTT_HOST[0] != '\0');

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static WebServer web(80);
static bool mqttDiscoverySent = false;
static char mqttDevId[24];
static char mqttAvailTopic[48];

bool netWifiConfigured() { return kWifiOn; }
bool netWebStarted() { return app.webStarted; }

bool netWifiUp() { return kWifiOn && WiFi.status() == WL_CONNECTED; }

int8_t netRssi() { return netWifiUp() ? (int8_t)WiFi.RSSI() : (int8_t)-127; }

int netWifiBars() {
  if (!netWifiUp()) return 0;
  const int rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

void netIpText(char* out, size_t n) {
  if (!kWifiOn) {
    snprintf(out, n, "no-wifi");
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else {
    snprintf(out, n, "wifi...");
  }
}

bool netMqttOk() { return kMqttOn && mqtt.connected(); }

bool netWebActive(uint32_t now) {
  return app.lastWebHitMs != 0 && (now - app.lastWebHitMs) < kWebActiveMs;
}

void netNoteWebHit() { app.lastWebHitMs = millis(); }

static void mqttBuildIds() {
  if (!kMqttOn) return;
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttDevId, sizeof(mqttDevId), "sw3518geek_%04x", (unsigned)(mac & 0xFFFF));
  snprintf(mqttAvailTopic, sizeof(mqttAvailTopic), "%s/status", MQTT_BASE);
}

static void publishHaDiscovery() {
  if (!kMqttOn) return;
  char topic[128];
  char payload[768];
  char state[64];

  auto discSensor = [&](const char* objectId, const char* name, const char* leaf, const char* unit,
                        const char* deviceClass, const char* stateClass) {
    snprintf(state, sizeof(state), "%s/%s", MQTT_BASE, leaf);
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", MQTT_DISCOVERY_PREFIX, mqttDevId,
             objectId);
    if (deviceClass && deviceClass[0]) {
      snprintf(payload, sizeof(payload),
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
               "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"unit_of_meas\":\"%s\","
               "\"dev_cla\":\"%s\",\"stat_cla\":\"%s\","
               "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
               "\"mf\":\"DIY\"}}",
               name, mqttDevId, objectId, state, mqttAvailTopic, unit, deviceClass, stateClass,
               mqttDevId);
    } else {
      snprintf(payload, sizeof(payload),
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
               "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"unit_of_meas\":\"%s\","
               "\"stat_cla\":\"%s\","
               "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
               "\"mf\":\"DIY\"}}",
               name, mqttDevId, objectId, state, mqttAvailTopic, unit, stateClass, mqttDevId);
    }
    mqtt.publish(topic, payload, true);
  };

  auto discText = [&](const char* objectId, const char* name, const char* leaf) {
    snprintf(state, sizeof(state), "%s/%s", MQTT_BASE, leaf);
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", MQTT_DISCOVERY_PREFIX, mqttDevId,
             objectId);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
             "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
             "\"mf\":\"DIY\"}}",
             name, mqttDevId, objectId, state, mqttAvailTopic, mqttDevId);
    mqtt.publish(topic, payload, true);
  };

  auto discBinary = [&](const char* objectId, const char* name, const char* leaf) {
    snprintf(state, sizeof(state), "%s/%s", MQTT_BASE, leaf);
    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s/config", MQTT_DISCOVERY_PREFIX, mqttDevId,
             objectId);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
             "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"power\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
             "\"mf\":\"DIY\"}}",
             name, mqttDevId, objectId, state, mqttAvailTopic, mqttDevId);
    mqtt.publish(topic, payload, true);
  };

  discSensor("vin", "Input voltage", "vin", "V", "voltage", "measurement");
  discSensor("vout", "Output voltage", "vout", "V", "voltage", "measurement");
  discSensor("i_c", "USB-C current", "i_c", "A", "current", "measurement");
  discSensor("i_a", "USB-A current", "i_a", "A", "current", "measurement");
  discSensor("power", "Total power", "power", "W", "power", "measurement");
  discSensor("power_c", "USB-C power", "power_c", "W", "power", "measurement");
  discSensor("power_a", "USB-A power", "power_a", "W", "power", "measurement");
  // Resettable session energy is not an HA Energy-panel total.
  discSensor("session_wh", "Session energy", "session_wh", "Wh", "", "measurement");
  discSensor("session_mwh", "Session energy mWh", "session_mwh", "mWh", "", "measurement");
  discSensor("session_peak_w", "Session peak power", "session_peak_w", "W", "power", "measurement");
  discText("protocol", "Charge protocol", "protocol");
  discBinary("charging", "Charging", "charging");
  Serial.println("HA MQTT discovery published");
}

static void ensureMqtt() {
  if (!kMqttOn) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  mqttBuildIds();
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  bool ok = false;
  if (MQTT_USER[0]) {
    ok = mqtt.connect(mqttDevId, MQTT_USER, MQTT_PASSWORD, mqttAvailTopic, 1, true, "offline");
  } else {
    ok = mqtt.connect(mqttDevId, mqttAvailTopic, 1, true, "offline");
  }
  if (!ok) {
    Serial.println("MQTT connect failed");
    mqttDiscoverySent = false;
    return;
  }
  mqtt.publish(mqttAvailTopic, "online", true);
  mqttDiscoverySent = false;
  Serial.println("MQTT connected");
}

void netPublish() {
  if (!kMqttOn || !mqtt.connected()) return;

  if (!mqttDiscoverySent) {
    publishHaDiscovery();
    mqttDiscoverySent = true;
  }

  char topic[64], val[48];
  auto pub = [&](const char* leaf, const char* v) {
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_BASE, leaf);
    mqtt.publish(topic, v, true);
  };

  snprintf(val, sizeof(val), "%.3f", app.snap.vin_mv / 1000.0f);
  pub("vin", val);
  snprintf(val, sizeof(val), "%.3f", app.snap.vout_mv / 1000.0f);
  pub("vout", val);
  snprintf(val, sizeof(val), "%.3f", app.snap.ic_ma / 1000.0f);
  pub("i_c", val);
  snprintf(val, sizeof(val), "%.3f", app.snap.ia_ma / 1000.0f);
  pub("i_a", val);
  snprintf(val, sizeof(val), "%.3f", app.snap.power_total_w);
  pub("power", val);
  snprintf(val, sizeof(val), "%.3f", app.snap.power_c_w);
  pub("power_c", val);
  snprintf(val, sizeof(val), "%.3f", app.snap.power_a_w);
  pub("power_a", val);
  pub("protocol", SW3518::protocolName(app.snap.protocol));
  snprintf(val, sizeof(val), "%.1f", app.session.mwh);
  pub("session_mwh", val);
  snprintf(val, sizeof(val), "%.4f", app.session.mwh / 1000.0f);
  pub("session_wh", val);
  snprintf(val, sizeof(val), "%.2f", app.session.peakW);
  pub("session_peak_w", val);
  const bool charging = app.snap.ia_ma > Session::kLoadMa || app.snap.ic_ma > Session::kLoadMa;
  pub("charging", charging ? "ON" : "OFF");
  mqtt.publish(mqttAvailTopic, "online", true);
}

static void handleRoot() {
  netNoteWebHit();
  char page[1200];
  const bool charging = app.snap.ia_ma > Session::kLoadMa || app.snap.ic_ma > Session::kLoadMa;
  snprintf(page, sizeof(page),
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv=refresh content=2>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>SW3518 GEEK</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{font-size:1.2rem;color:#0ff} .g{color:#8f8} .card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px;margin:.6rem 0} b{color:#fff}</style></head><body>"
           "<h1>SW3518 GEEK</h1>"
           "<div class=card><b>%.2f W</b> total &nbsp; %s<br>"
           "in %.2f V &nbsp; out %.2f V<br>"
           "USB-C %.2f A / %.2f W<br>"
           "USB-A %.2f A / %.2f W<br>"
           "protocol %s<br>"
           "session %.0f mWh (%.3f Wh) &nbsp; peak %.1f W</div>"
           "<div class=\"card g\">MQTT %s &nbsp; Wi-Fi %s (%d dBm)</div>"
           "<p><a href=/radio style=color:#0ff>radio</a> - <a href=/help style=color:#0ff>help</a></p>"
           "<p style=color:#666>Auto-refresh 2s - icon on device lights while you are here.</p>"
           "</body></html>",
           app.snap.power_total_w, charging ? "CHARGING" : "IDLE", app.snap.vin_mv / 1000.0f,
           app.snap.vout_mv / 1000.0f, app.snap.ic_ma / 1000.0f, app.snap.power_c_w,
           app.snap.ia_ma / 1000.0f, app.snap.power_a_w, SW3518::protocolName(app.snap.protocol),
           app.session.mwh, app.session.mwh / 1000.0f, app.session.peakW,
           netMqttOk() ? "up" : "down", netWifiUp() ? "up" : "down",
           netWifiUp() ? (int)WiFi.RSSI() : 0);
  web.send(200, "text/html", page);
}

static void handleApi() {
  netNoteWebHit();
  char json[384];
  snprintf(json, sizeof(json),
           "{\"vin\":%.3f,\"vout\":%.3f,\"i_c\":%.3f,\"i_a\":%.3f,\"power\":%.3f,"
           "\"power_c\":%.3f,\"power_a\":%.3f,\"protocol\":\"%s\","
           "\"session_mwh\":%.1f,\"session_wh\":%.4f,\"peak_w\":%.2f}",
           app.snap.vin_mv / 1000.0f, app.snap.vout_mv / 1000.0f, app.snap.ic_ma / 1000.0f,
           app.snap.ia_ma / 1000.0f, app.snap.power_total_w, app.snap.power_c_w, app.snap.power_a_w,
           SW3518::protocolName(app.snap.protocol), app.session.mwh, app.session.mwh / 1000.0f,
           app.session.peakW);
  web.send(200, "application/json", json);
}

static void handleRadio() {
  netNoteWebHit();
  char body[1600];
  char apJson[768];
  RadioTools::jsonStatus(apJson, sizeof(apJson));
  const char* modeName = (app.mode == Mode::Radio) ? "radio" : "charger";
  snprintf(body, sizeof(body),
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv=refresh content=3>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>GEEK Radio</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{font-size:1.2rem;color:#0ff}a{color:#0ff}.card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px;margin:.6rem 0} pre{white-space:pre-wrap;color:#aaa;font-size:.85rem}"
           "</style></head><body>"
           "<h1>Radio</h1><p>Device mode: <b>%s</b> - "
           "<a href=/>charger</a> - <a href=/help>help</a></p>"
           "<div class=card><pre>%s</pre></div>"
           "<p style=color:#666>AP list from Wi-Fi beacon scan (own RF view).</p>"
           "</body></html>",
           modeName, apJson);
  web.send(200, "text/html", body);
}

static void handleHelp() {
  netNoteWebHit();
  web.send(200, "text/html",
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>GEEK Help</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{color:#0ff}li{margin:.35rem 0}a{color:#0ff}.card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px}</style></head><body>"
           "<h1>BOOT controls</h1><div class=card><ul>"
           "<li><b>Short</b> - next page (charger zoom / radio pages)</li>"
           "<li><b>Double</b> - Session history (charger) or previous radio page</li>"
           "<li><b>Triple</b> - cycle Charger / Radio / HID</li>"
           "<li><b>Long</b> - clear session (charger) or rescan (radio)</li>"
           "</ul></div>"
           "<p><a href=/>charger</a> - <a href=/radio>radio</a> - <a href=/api>api</a></p>"
           "</body></html>");
}

static void setupWeb() {
  if (!kWifiOn || app.webStarted) return;
  web.on("/", handleRoot);
  web.on("/api", handleApi);
  web.on("/radio", handleRadio);
  web.on("/help", handleHelp);
  web.onNotFound([]() {
    netNoteWebHit();
    web.send(404, "text/plain", "not found");
  });
  web.begin();
  app.webStarted = true;
  Serial.println("Web server :80");
}

void netSetup() {
  if (!kWifiOn) {
    app.wifiEnabled = false;
    Serial.println("WiFi/MQTT disabled (empty WIFI_SSID or no secrets.h)");
    return;
  }
  app.wifiEnabled = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi connecting to %s\n", WIFI_SSID);
  if (kMqttOn) mqtt.setServer(MQTT_HOST, MQTT_PORT);
}

void netTick() {
  if (!app.wifiEnabled) return;
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    if (lastWifi != WL_CONNECTED) {
      Serial.printf("WiFi IP %s\n", WiFi.localIP().toString().c_str());
    }
    setupWeb();
    web.handleClient();
  }
  lastWifi = st;
  ensureMqtt();
  if (kMqttOn) mqtt.loop();
}
