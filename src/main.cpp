// agri-display-atom — bring-up firmware
//
// Milestone: prove the whole signal path end to end.
//   1. HDMI output    — draw a test pattern on the 1280x720 screen
//   2. WiFi           — provision via WiFiManager captive portal (no creds in code)
//   3. MQTT subscribe  — connect to the agriha broker, subscribe agriha/#
//   4. live view       — show every received topic/payload on the HDMI screen + Serial
//
// Rendering: draw into a full-screen off-screen canvas in PSRAM, then pushSprite()
// once per frame (double-buffered = no flicker). MQTT receipt only appends to a ring
// buffer; the screen is redrawn on a fixed cadence so a burst of retained messages
// can never outrun the drawing.

#include <M5AtomDisplay.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

// ---- display / off-screen canvas -------------------------------------------
M5AtomDisplay display(1280, 720);
M5Canvas      canvas(&display);       // full-screen PSRAM back-buffer
LovyanGFX*    g        = &display;    // draw target (canvas if allocated, else direct)
bool          useCanvas = false;
static int    SCR_W = 1280;
static int    SCR_H = 720;

static uint32_t C_BG, C_ORANGE, C_ORANGE_HI, C_AMBER, C_TEXT, C_DIM, C_OK, C_CRIT;

static void present() { if (useCanvas) canvas.pushSprite(0, 0); }

// ---- network ----------------------------------------------------------------
WiFiClient   net;
PubSubClient mqtt(net);

static char     mqttHost[48] = "pi4.local";
static uint16_t mqttPort     = 1883;
static const char* MDNS_NAME = "agri-display-01";

// ---- on-screen log ring -----------------------------------------------------
static constexpr int LOG_MAX = 28;
static String   logLines[LOG_MAX];
static int      logCount = 0;
static uint32_t msgTotal = 0;

static void logPush(const String& s) {
  if (logCount < LOG_MAX) {
    logLines[logCount++] = s;
  } else {
    for (int i = 1; i < LOG_MAX; i++) logLines[i - 1] = logLines[i];
    logLines[LOG_MAX - 1] = s;
  }
}

// ---- drawing (all into `g`) -------------------------------------------------
static void drawHeader(const char* status, uint32_t statusColor) {
  g->fillRect(0, 0, SCR_W, 52, C_BG);
  g->fillRect(0, 0, 8, 52, C_ORANGE);
  g->setTextDatum(textdatum_t::top_left);
  g->setTextColor(C_TEXT, C_BG);
  g->setTextSize(3);
  g->drawString("AGRIHA DISPLAY // BRING-UP", 24, 8);
  g->setTextSize(2);
  g->setTextColor(C_ORANGE_HI, C_BG);
  g->drawString("M5Stack Atom Display  1280x720 HDMI", 24, 34);
  g->setTextDatum(textdatum_t::top_right);
  g->setTextColor(statusColor, C_BG);
  g->setTextSize(2);
  g->drawString(status, SCR_W - 24, 18);
  g->drawFastHLine(0, 52, SCR_W, C_ORANGE);
}

static void drawTestPattern() {
  g->fillScreen(C_BG);
  uint32_t bars[8] = {
    g->color888(255, 255, 255), g->color888(235, 102, 8),
    g->color888(245, 166, 35),  g->color888(79, 194, 122),
    g->color888(60, 140, 220),  g->color888(200, 60, 60),
    g->color888(120, 120, 120), g->color888(30, 30, 30)
  };
  int bw = SCR_W / 8;
  for (int i = 0; i < 8; i++) g->fillRect(i * bw, 60, bw, 90, bars[i]);
  g->setTextDatum(textdatum_t::top_left);
  g->setTextColor(C_TEXT, C_BG);
  g->setTextSize(6);
  g->drawString("AGRIHA DISPLAY", 40, 190);
  g->setTextSize(3);
  g->setTextColor(C_ORANGE_HI, C_BG);
  g->drawString("HDMI OK - 1280x720", 40, 260);
  g->setTextSize(2);
  g->setTextColor(C_DIM, C_BG);
  g->drawString("bring-up firmware / stage 1: display", 40, 300);
  present();
}

static void drawLive() {
  bool up = mqtt.connected();
  char st[64];
  snprintf(st, sizeof(st), up ? "MQTT ONLINE  rx=%lu" : "MQTT ...",
           (unsigned long)msgTotal);
  drawHeader(st, up ? C_OK : C_AMBER);

  g->fillRect(0, 53, SCR_W, 30, C_BG);
  g->setTextDatum(textdatum_t::top_left);
  g->setTextColor(C_DIM, C_BG);
  g->setTextSize(2);
  char sub[110];
  snprintf(sub, sizeof(sub), "broker %s:%u   wifi %s   ip %s   sub agriha/#",
           mqttHost, mqttPort, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  g->drawString(sub, 24, 58);
  g->drawFastHLine(0, 84, SCR_W, C_ORANGE);

  g->fillRect(0, 86, SCR_W, SCR_H - 86, C_BG);
  g->setTextSize(2);
  int y = 92, lh = 22;
  for (int i = 0; i < logCount; i++) {
    g->setTextColor(C_TEXT, C_BG);
    g->drawString(logLines[i], 24, y);
    y += lh;
  }
  if (logCount == 0) {
    g->setTextColor(C_DIM, C_BG);
    g->drawString("waiting for retained agriha data ...", 24, 92);
  }
  present();
}

// ---- MQTT -------------------------------------------------------------------
static void onMqtt(char* topic, byte* payload, unsigned int len) {
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len && i < 80; i++) p += (char)payload[i];
  msgTotal++;
  String line = String(topic) + "  =  " + p;
  if (line.length() > 96) line = line.substring(0, 96);
  logPush(line);                       // append only; drawing happens in loop()
  Serial.printf("[MQTT] %s = %s\n", topic, p.c_str());
}

static void mqttConnect() {
  IPAddress ip;
  bool haveIp = false;
  String h(mqttHost);
  if (h.endsWith(".local")) {
    String name = h.substring(0, h.length() - 6);
    Serial.printf("[mDNS] resolving %s ...\n", name.c_str());
    ip = MDNS.queryHost(name.c_str(), 3000);
    haveIp = (ip != IPAddress(0, 0, 0, 0));
    Serial.printf("[mDNS] %s -> %s\n", name.c_str(), haveIp ? ip.toString().c_str() : "FAILED");
  }
  if (haveIp) mqtt.setServer(ip, mqttPort);
  else        mqtt.setServer(mqttHost, mqttPort);

  mqtt.setBufferSize(1024);
  mqtt.setCallback(onMqtt);

  String cid = String("agri-display-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] connecting to %s:%u as %s\n", mqttHost, mqttPort, cid.c_str());
  if (mqtt.connect(cid.c_str())) {
    mqtt.subscribe("agriha/#");
    Serial.println("[MQTT] connected, subscribed agriha/#");
    logPush("[sys] MQTT connected, subscribed agriha/#");
  } else {
    Serial.printf("[MQTT] connect failed rc=%d\n", mqtt.state());
  }
}

// ---- WiFiManager portal -----------------------------------------------------
static void onPortal(WiFiManager* wm) {
  g->fillScreen(C_BG);
  drawHeader("SETUP MODE", C_AMBER);
  g->setTextDatum(textdatum_t::top_left);
  g->setTextColor(C_ORANGE_HI, C_BG);
  g->setTextSize(4);
  g->drawString("WiFi SETUP REQUIRED", 40, 120);
  g->setTextColor(C_TEXT, C_BG);
  g->setTextSize(3);
  g->drawString("1) Join WiFi AP:  agri-display-setup", 40, 200);
  g->drawString("2) Open 192.168.4.1 in a browser", 40, 250);
  g->drawString("3) Pick your network + MQTT broker", 40, 300);
  g->setTextColor(C_DIM, C_BG);
  g->setTextSize(2);
  g->drawString("(default broker: pi4.local)", 40, 360);
  present();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] agri-display-atom bring-up");

  display.init();
  display.setColorDepth(16);
  display.setRotation(1);            // panel comes up 90 deg CCW; rotate CW to landscape
  SCR_W = display.width();
  SCR_H = display.height();
  Serial.printf("[disp] W=%d H=%d\n", SCR_W, SCR_H);

  // full-screen back-buffer in PSRAM (1280x720x16 = 1.84 MB)
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  useCanvas = canvas.createSprite(SCR_W, SCR_H);
  g = useCanvas ? (LovyanGFX*)&canvas : (LovyanGFX*)&display;
  Serial.printf("[disp] back-buffer %s\n", useCanvas ? "PSRAM sprite OK" : "FAILED -> direct draw");

  C_BG        = display.color888(10, 8, 6);
  C_ORANGE    = display.color888(235, 102, 8);
  C_ORANGE_HI = display.color888(255, 122, 20);
  C_AMBER     = display.color888(245, 166, 35);
  C_TEXT      = display.color888(243, 231, 211);
  C_DIM       = display.color888(194, 149, 102);
  C_OK        = display.color888(79, 194, 122);
  C_CRIT      = display.color888(240, 57, 43);

  drawTestPattern();
  delay(1200);

  WiFiManager wm;
  WiFiManagerParameter p_host("host", "MQTT broker (host or IP)", mqttHost, sizeof(mqttHost));
  wm.addParameter(&p_host);
  wm.setConfigPortalTimeout(240);
  wm.setAPCallback(onPortal);

  if (!wm.autoConnect("agri-display-setup")) {
    Serial.println("[wifi] portal timeout, restarting");
    ESP.restart();
  }
  strncpy(mqttHost, p_host.getValue(), sizeof(mqttHost) - 1);
  if (strlen(mqttHost) == 0) strcpy(mqttHost, "pi4.local");

  Serial.printf("[wifi] connected %s  ip=%s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  if (MDNS.begin(MDNS_NAME)) Serial.printf("[mDNS] responder up: %s.local\n", MDNS_NAME);
  configTime(9 * 3600, 0, "pool.ntp.org");

  logPush(String("[sys] wifi ") + WiFi.SSID() + " ip " + WiFi.localIP().toString());
  drawLive();
  mqttConnect();
}

void loop() {
  static uint32_t lastReconnect = 0;
  static uint32_t lastDraw = 0;

  if (!mqtt.connected()) {
    if (millis() - lastReconnect > 3000) { lastReconnect = millis(); mqttConnect(); }
  } else {
    mqtt.loop();
  }

  // redraw at a fixed 5 Hz, decoupled from message rate
  if (millis() - lastDraw > 200) {
    lastDraw = millis();
    drawLive();
  }
}
