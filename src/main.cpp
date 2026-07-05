// agri-display-atom — EVA/NERV dashboard (first real frame)
//
// Rendering model that fits the Atom Display (framebuffer lives in the FPGA):
//   - draw the STATIC chrome (panels, labels, borders) ONCE, directly
//   - each live value is redrawn only when its MQTT topic changes, by drawing
//     into a SMALL per-widget sprite and pushing just that rect. Small sprite =
//     fast push (a few ms) AND flicker-free (no visible clear). A full-screen
//     PSRAM back-buffer is the opposite of helpful here (~350 ms/frame).
//
// This frame: house2 — room-temp hero (ring gauge + big value), a 2x3 tile grid
// (humidity / CO2 / HD / pressure / current / flow) and the two side windows.
// Data is parsed from the retained agriha MQTT topics. Skins, whole-screen
// severity themes, the 60-min trend and the WebUI come next.

#include <M5AtomDisplay.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ---- display ----------------------------------------------------------------
M5AtomDisplay display(1280, 720);
M5Canvas sprHero(&display);   // room-temp hero (ring + value)
M5Canvas sprTile(&display);   // reused for every value tile
M5Canvas sprBar(&display);    // window aperture bar
static int SCR_W = 1280, SCR_H = 720;

static uint32_t C_BG, C_PANEL, C_LINE, C_ACCENT, C_ACCENT_HI, C_AMBER,
                C_TEXT, C_DIM, C_OK, C_WARN, C_CRIT;

// ---- network ----------------------------------------------------------------
WiFiClient   net;
PubSubClient mqtt(net);
static char     mqttHost[48] = "pi4.local";
static uint16_t mqttPort     = 1883;
static const char* MDNS_NAME = "agri-display-01";

// ---- value store ------------------------------------------------------------
struct F { float v = NAN; bool dirty = true; };
static F g_temp, g_humid, g_co2, g_hd, g_pres, g_cur, g_flow;
struct Win { int pct = 0, target = 0; char src[10] = "-"; bool dirty = true; };
static Win g_win1, g_win2;
static uint32_t g_rx = 0;

// severity from room temp (whole-screen theming comes later; ring uses it now)
static float TH_CAUTION = 26.8f, TH_ALERT = 27.6f, TH_DANGER = 28.6f, TEMP_SP = 26.0f;
static uint32_t sevColor(float t) {
  if (isnan(t)) return C_DIM;
  if (t >= TH_DANGER)  return C_CRIT;
  if (t >= TH_ALERT)   return C_ACCENT_HI;
  if (t >= TH_CAUTION) return C_WARN;
  return C_OK;
}

// ---- static chrome ----------------------------------------------------------
static void drawChrome() {
  display.fillScreen(C_BG);
  // header
  display.fillRect(0, 0, SCR_W, 54, C_PANEL);
  display.fillRect(0, 0, 8, 54, C_ACCENT);
  display.setFont(&fonts::lgfxJapanGothic_28);
  display.setTextDatum(textdatum_t::middle_left);
  display.setTextColor(C_TEXT);
  display.drawString("AGRIHA 統合環境管制", 24, 26);
  display.setFont(&fonts::Font2);
  display.setTextColor(C_ACCENT_HI);
  display.setTextDatum(textdatum_t::middle_left);
  display.drawString("HOUSE 02 / BEPPO-UNIT", 360, 28);
  display.drawFastHLine(0, 54, SCR_W, C_ACCENT);

  // panel outlines
  display.drawRect(16, 68, 452, 540, C_LINE);   // hero
  for (int i = 0; i < 6; i++) {                  // tiles 2 cols x 3 rows
    int x = 484 + (i % 2) * 400;
    int y = 68 + (i / 2) * 132;
    display.drawRect(x, y, 384, 120, C_LINE);
  }
  display.drawRect(16, 620, 1248, 84, C_LINE);   // windows strip
}

// ---- widgets ----------------------------------------------------------------
static void fmt1(char* buf, size_t n, float v) {
  if (isnan(v)) snprintf(buf, n, "--.-");
  else          snprintf(buf, n, "%.1f", v);
}
static void fmt0(char* buf, size_t n, float v) {
  if (isnan(v)) snprintf(buf, n, "---");
  else          snprintf(buf, n, "%.0f", v);
}

static void drawHero() {
  const int W = 452, H = 540;
  sprHero.fillSprite(C_BG);
  int cx = W / 2, cy = 250, rO = 150, rI = 128;
  // gauge track (270 deg, gap at bottom): LovyanGFX angle 0 = 3 o'clock, CW
  sprHero.fillArc(cx, cy, rO, rI, 135, 45, C_LINE);
  // value arc
  float t = g_temp.v;
  if (!isnan(t)) {
    float frac = (t - 15.0f) / 20.0f; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    sprHero.fillArc(cx, cy, rO, rI, 135, 135 + frac * 270.0f, sevColor(t));
  }
  // big value
  char buf[8]; fmt1(buf, sizeof(buf), t);
  sprHero.setFont(&fonts::Font7);
  sprHero.setTextColor(C_TEXT);
  sprHero.setTextDatum(textdatum_t::middle_center);
  sprHero.drawString(buf, cx, cy - 6);
  sprHero.setFont(&fonts::lgfxJapanGothic_24);
  sprHero.setTextColor(C_DIM);
  sprHero.drawString("室温 / IN-AIR TEMP  ℃", cx, cy + 78);
  // setpoint / range line
  char sp[40]; snprintf(sp, sizeof(sp), "SP %.1f℃   range 15-35", TEMP_SP);
  sprHero.setFont(&fonts::lgfxJapanGothic_16);   // ASCII font can't render ℃
  sprHero.setTextColor(C_DIM);
  sprHero.setTextDatum(textdatum_t::top_center);
  sprHero.drawString(sp, cx, cy + 120);
  sprHero.drawRect(0, 0, W, H, C_LINE);
  sprHero.pushSprite(16, 68);
}

// draw one value tile into sprTile and push at slot i (0..5)
static void drawTile(int i, const char* label, float val, int decimals, const char* unit) {
  const int W = 384, H = 120;
  int x = 484 + (i % 2) * 400;
  int y = 68 + (i / 2) * 132;
  sprTile.fillSprite(C_BG);
  sprTile.drawRect(0, 0, W, H, C_LINE);
  sprTile.setFont(&fonts::lgfxJapanGothic_28);
  sprTile.setTextColor(C_ACCENT);
  sprTile.setTextDatum(textdatum_t::top_left);
  sprTile.drawString(label, 14, 10);
  char buf[8]; (decimals == 0) ? fmt0(buf, sizeof(buf), val) : fmt1(buf, sizeof(buf), val);
  sprTile.setFont(&fonts::Font7);
  sprTile.setTextColor(C_TEXT);
  sprTile.setTextDatum(textdatum_t::bottom_right);
  sprTile.drawString(buf, W - 92, H - 12);
  sprTile.setFont(&fonts::lgfxJapanGothic_20);
  sprTile.setTextColor(C_DIM);
  sprTile.setTextDatum(textdatum_t::bottom_right);
  sprTile.drawString(unit, W - 12, H - 16);
  sprTile.pushSprite(x, y);
}

static void drawWindow(int idx, Win& w) {
  const int W = 612, H = 74;
  int x = 20 + idx * 624;
  int y = 624;
  sprBar.fillSprite(C_BG);
  sprBar.setFont(&fonts::lgfxJapanGothic_24);
  sprBar.setTextColor(C_ACCENT);
  sprBar.setTextDatum(textdatum_t::middle_left);
  sprBar.drawString(idx == 0 ? "東窓" : "西窓", 8, 20);   // 東窓/西窓
  // track
  int tx = 96, tw = W - 210, th = 26, ty = 8;
  sprBar.drawRect(tx, ty, tw, th, C_LINE);
  int fillw = (int)(tw * (w.pct / 100.0f));
  sprBar.fillRect(tx + 1, ty + 1, fillw > 2 ? fillw - 2 : 0, th - 2, C_ACCENT);
  int mx = tx + (int)(tw * (w.target / 100.0f));
  sprBar.fillRect(mx - 1, ty - 3, 3, th + 6, C_AMBER);      // target marker
  char pc[8]; snprintf(pc, sizeof(pc), "%d%%", w.pct);
  sprBar.setFont(&fonts::Font4);
  sprBar.setTextColor(C_TEXT);
  sprBar.setTextDatum(textdatum_t::middle_right);
  sprBar.drawString(pc, W - 8, 20);
  sprBar.setFont(&fonts::Font2);
  sprBar.setTextColor(C_DIM);
  sprBar.setTextDatum(textdatum_t::top_left);
  char sub[40]; snprintf(sub, sizeof(sub), "target %d%%   src %s", w.target, w.src);
  sprBar.drawString(sub, 96, 44);
  sprBar.pushSprite(x, y);
}

static void redrawDirty() {
  if (g_temp.dirty)  { drawHero();                                    g_temp.dirty = false; }
  if (g_humid.dirty) { drawTile(0, "湿度", g_humid.v, 0, "%RH");   g_humid.dirty = false; }
  if (g_co2.dirty)   { drawTile(1, "CO2",     g_co2.v,   0, "ppm");   g_co2.dirty = false; }
  if (g_hd.dirty)    { drawTile(2, "飽差", g_hd.v,    1, "g/m³"); g_hd.dirty = false; }
  if (g_pres.dirty)  { drawTile(3, "気圧", g_pres.v,  0, "hPa");   g_pres.dirty = false; }
  if (g_cur.dirty)   { drawTile(4, "電流", g_cur.v,   1, "A");     g_cur.dirty = false; }
  if (g_flow.dirty)  { drawTile(5, "灌水", g_flow.v,  1, "L/min"); g_flow.dirty = false; }
  if (g_win1.dirty)  { drawWindow(0, g_win1);                          g_win1.dirty = false; }
  if (g_win2.dirty)  { drawWindow(1, g_win2);                          g_win2.dirty = false; }
}

// ---- MQTT -------------------------------------------------------------------
static bool ends(const char* topic, const char* suffix) {
  size_t lt = strlen(topic), ls = strlen(suffix);
  return lt >= ls && strcmp(topic + lt - ls, suffix) == 0;
}

static void onMqtt(char* topic, byte* payload, unsigned int len) {
  g_rx++;
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, payload, len)) return;   // ignore non-JSON

  if      (ends(topic, "/sensor/InAirTemp"))     { g_temp.v  = doc["value"] | NAN; g_temp.dirty = true; }
  else if (ends(topic, "/sensor/InAirHumid"))    { g_humid.v = doc["value"] | NAN; g_humid.dirty = true; }
  else if (ends(topic, "/sensor/InAirCO2"))      { g_co2.v   = doc["value"] | NAN; g_co2.dirty = true; }
  else if (ends(topic, "/sensor/InAirHD"))       { g_hd.v    = doc["value"] | NAN; g_hd.dirty = true; }
  else if (ends(topic, "/sensor/InAirPressure")) { g_pres.v  = doc["value"] | NAN; g_pres.dirty = true; }
  else if (ends(topic, "/sensor/Current"))       { g_cur.v   = doc["current_a"] | (doc["value"] | NAN); g_cur.dirty = true; }
  else if (ends(topic, "/sensor/Flow"))          { g_flow.v  = doc["flow_lpm"]  | (doc["value"] | NAN); g_flow.dirty = true; }
  else if (ends(topic, "/window/1")) { g_win1.pct = doc["pct"] | 0; g_win1.target = doc["target"] | 0;
                                       strlcpy(g_win1.src, doc["src"] | "-", sizeof(g_win1.src)); g_win1.dirty = true; }
  else if (ends(topic, "/window/2")) { g_win2.pct = doc["pct"] | 0; g_win2.target = doc["target"] | 0;
                                       strlcpy(g_win2.src, doc["src"] | "-", sizeof(g_win2.src)); g_win2.dirty = true; }
}

static void mqttConnect() {
  IPAddress ip; bool haveIp = false;
  String h(mqttHost);
  if (h.endsWith(".local")) {
    String name = h.substring(0, h.length() - 6);
    ip = MDNS.queryHost(name.c_str(), 3000);
    haveIp = (ip != IPAddress(0, 0, 0, 0));
    Serial.printf("[mDNS] %s -> %s\n", name.c_str(), haveIp ? ip.toString().c_str() : "FAILED");
  }
  if (haveIp) mqtt.setServer(ip, mqttPort); else mqtt.setServer(mqttHost, mqttPort);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(onMqtt);
  String cid = String("agri-display-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(cid.c_str())) {
    mqtt.subscribe("agriha/#");
    Serial.println("[MQTT] connected, subscribed agriha/#");
  } else {
    Serial.printf("[MQTT] connect failed rc=%d\n", mqtt.state());
  }
}

// ---- WiFi portal ------------------------------------------------------------
static void onPortal(WiFiManager* wm) {
  display.fillScreen(C_BG);
  display.fillRect(0, 0, SCR_W, 54, C_PANEL);
  display.fillRect(0, 0, 8, 54, C_ACCENT);
  display.setFont(&fonts::lgfxJapanGothic_28);
  display.setTextColor(C_AMBER);
  display.setTextDatum(textdatum_t::middle_left);
  display.drawString("WiFi SETUP", 24, 26);
  display.setFont(&fonts::lgfxJapanGothic_24);
  display.setTextColor(C_TEXT);
  display.setTextDatum(textdatum_t::top_left);
  display.drawString("1) WiFi AP に接続:  agri-display-setup", 40, 140);
  display.drawString("2) ブラウザで 192.168.4.1", 40, 200);
  display.drawString("3) ネットワークと MQTT broker を選択", 40, 260);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] agri-display-atom EVA dashboard");

  // Let the FPGA finish booting its internal-flash bitstream before init, so
  // _read_fpga_id() returns the real ID (skip the slow JTAG-reprogram path).
  delay(600);
  display.init();
  if (display.width() == 0) { Serial.println("[disp] retry init"); delay(400); display.init(); }
  display.setColorDepth(16);
  display.setRotation(1);
  SCR_W = display.width();
  SCR_H = display.height();
  Serial.printf("[disp] W=%d H=%d\n", SCR_W, SCR_H);

  C_BG        = display.color888(10, 8, 6);
  C_PANEL     = display.color888(22, 16, 10);
  C_LINE      = display.color888(96, 54, 18);
  C_ACCENT    = display.color888(235, 102, 8);
  C_ACCENT_HI = display.color888(255, 122, 20);
  C_AMBER     = display.color888(245, 166, 35);
  C_TEXT      = display.color888(243, 231, 211);
  C_DIM       = display.color888(150, 112, 72);
  C_OK        = display.color888(79, 194, 122);
  C_WARN      = display.color888(245, 166, 35);
  C_CRIT      = display.color888(240, 57, 43);

  // per-widget sprites (PSRAM): hero 452x540, tile 384x120, bar 612x74
  sprHero.setPsram(true); sprHero.setColorDepth(16); sprHero.createSprite(452, 540);
  sprTile.setPsram(true); sprTile.setColorDepth(16); sprTile.createSprite(384, 120);
  sprBar.setPsram(true);  sprBar.setColorDepth(16);  sprBar.createSprite(612, 74);

  drawChrome();

  WiFiManager wm;
  WiFiManagerParameter p_host("host", "MQTT broker (host or IP)", mqttHost, sizeof(mqttHost));
  wm.addParameter(&p_host);
  wm.setConfigPortalTimeout(240);
  wm.setAPCallback(onPortal);
  if (!wm.autoConnect("agri-display-setup")) { ESP.restart(); }
  strncpy(mqttHost, p_host.getValue(), sizeof(mqttHost) - 1);
  if (strlen(mqttHost) == 0) strcpy(mqttHost, "pi4.local");
  Serial.printf("[wifi] %s ip=%s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  MDNS.begin(MDNS_NAME);
  configTime(9 * 3600, 0, "pool.ntp.org");
  drawChrome();
  redrawDirty();
  mqttConnect();
}

void loop() {
  static uint32_t lastReconnect = 0, lastDraw = 0;
  if (!mqtt.connected()) {
    if (millis() - lastReconnect > 3000) { lastReconnect = millis(); mqttConnect(); }
  } else {
    mqtt.loop();
  }
  // repaint changed widgets, throttled so a retained burst can't thrash
  if (millis() - lastDraw > 120) { lastDraw = millis(); redrawDirty(); }
}
