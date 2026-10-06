#include "net.h"
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include "app_state.h"
#include "features.h"
#include "radio_tools.h"
#include "ui.h"

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
#define MQTT_BASE "geek/totp"
#endif

static const bool kWifiOn = WIFI_SSID[0] != 0;
static const bool kMqttOn = kWifiOn && MQTT_HOST[0] != 0;

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static WebServer web(80);

static char mqttDevId[24] = "";
static char mqttAvailTopic[64] = "";

static void mqttBuildIds() {
  if (mqttDevId[0]) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(mqttDevId, sizeof(mqttDevId), "geek_%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(mqttAvailTopic, sizeof(mqttAvailTopic), "%s/status", MQTT_BASE);
}

void netPublish() {}

bool netWifiUp() { return app.wifiEnabled && WiFi.status() == WL_CONNECTED; }
bool netMqttOk() { return app.wifiEnabled && kMqttOn && mqtt.connected(); }

void netNoteWebHit() {
  app.lastWebHitMs = millis();
  app.ipShowUntilMs = millis() + kWebActiveMs;
}

static void handleRoot() {
  netNoteWebHit();
  web.send(200, "text/html", "GEEK is in TOTP mode.");
}

static void handleApi() {
  netNoteWebHit();
  web.send(200, "application/json", "{}");
}

static void handleRadio() {
  netNoteWebHit();
  char body[1600];
  char apJson[768];
  RadioTools::jsonStatus(apJson, sizeof(apJson));
  snprintf(body, sizeof(body),
           "<!doctype html><html lang=\"en\"><head><meta charset=utf-8>"
           "<meta http-equiv=refresh content=3>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>GEEK Radio</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{font-size:1.2rem;color:#0ff}a{color:#0ff}.card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px;margin:.6rem 0} pre{white-space:pre-wrap;color:#aaa;font-size:.85rem}"
           "</style></head><body>"
           "<h1>Radio</h1><p>"
           "<a href=/>root</a> - <a href=/help>help</a></p>"
           "<div class=card><pre>%s</pre></div>"
           "<p style=color:#aaa>AP list from Wi-Fi beacon scan (own RF view).</p>"
           "</body></html>",
           apJson);
  web.send(200, "text/html", body);
}

static void handleHelp() {
  netNoteWebHit();
  web.send(200, "text/html", "Help page placeholder");
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
    return;
  }
  mqtt.publish(mqttAvailTopic, "online", true);
  Serial.println("MQTT connected");
}

void netSetup() {
  if (!kWifiOn) {
    app.wifiEnabled = false;
    Serial.println("WiFi/MQTT disabled");
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
