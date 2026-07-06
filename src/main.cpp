// agri-display-atom — EVA/NERV dashboard
//
// Rendering model that fits the Atom Display (framebuffer lives in the FPGA):
//   - header/background drawn once
//   - every panel is a self-contained widget rendered into a small PSRAM sprite
//     and pushed to just its rect, only when its data changes. Small sprite =
//     fast push + flicker-free. (A full-screen PSRAM back-buffer is ~350 ms.)
//
// TV-safe: all content lives inside a MARGIN inset from every edge, because
// consumer TVs (e.g. REGZA) overscan a 720p signal and crop/​wrap the outer
// ~5%. On a no-overscan monitor this just shows a thin black border.

#include <M5AtomDisplay.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ---- display / sprites ------------------------------------------------------
M5AtomDisplay display(1280, 720);
M5Canvas sprHero(&display);    // 340x524
M5Canvas sprTrend(&display);   // 520x240
M5Canvas sprBig(&display);     // 254x272  (CO2 / HD, reused)
M5Canvas sprSm(&display);      // 308x120  (humid/pres/cur/flow, reused)
M5Canvas sprWin(&display);     // 596x88   (window bar, reused)
static int SCR_W = 1280, SCR_H = 720;

static uint32_t C_BG, C_PANEL, C_LINE, C_GRID, C_AREA, C_ACCENT, C_ACCENT_HI,
                C_AMBER, C_TEXT, C_DIM, C_OK, C_WARN, C_CRIT;

// ---- layout (all inside a 40 px TV-safe margin) -----------------------------
#define MX      40
#define HDR_H   58
#define HERO_X  MX
#define HERO_Y  66
#define HERO_W  340
#define HERO_H  524
#define TR_X    396
#define TR_Y    66
#define TR_W    520
#define TR_H    240
#define BT_Y    320
#define BT_W    254
#define BT_H    272
#define CO2_X   396
#define HD_X    662
#define RC_X    932
#define RC_W    308
#define RC_H    120
#define WIN_Y   604
#define WIN_W   596
#define WIN_H   88

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

// ---- 60-min room-temp trend (time-sampled ring buffer) ----------------------
static constexpr int TR_N = 90;          // 90 samples x 40 s = 60 min
static float    trendBuf[TR_N];
static int      trendCount = 0;
static bool     trendDirty = true;

static float TH_CAUTION = 26.8f, TH_ALERT = 27.6f, TH_DANGER = 28.6f, TEMP_SP = 26.0f;
static uint32_t sevColor(float t) {
  if (isnan(t)) return C_DIM;
  if (t >= TH_DANGER)  return C_CRIT;
  if (t >= TH_ALERT)   return C_ACCENT_HI;
  if (t >= TH_CAUTION) return C_WARN;
  return C_OK;
}
static void trendPush(float v) {
  if (trendCount < TR_N) trendBuf[trendCount++] = v;
  else { memmove(trendBuf, trendBuf + 1, (TR_N - 1) * sizeof(float)); trendBuf[TR_N - 1] = v; }
  trendDirty = true;
}

// ---- whole-screen severity (driven by room temp) ----------------------------
enum Sev { SEV_NORMAL, SEV_CAUTION, SEV_ALERT, SEV_DANGER };
static Sev g_sev = SEV_NORMAL;
static Sev computeSev(float t) {
  if (isnan(t)) return SEV_NORMAL;
  if (t >= TH_DANGER)  return SEV_DANGER;
  if (t >= TH_ALERT)   return SEV_ALERT;
  if (t >= TH_CAUTION) return SEV_CAUTION;
  return SEV_NORMAL;
}
static uint32_t sevMain() { switch (g_sev) { case SEV_DANGER: return C_CRIT; case SEV_ALERT: return C_ACCENT_HI; case SEV_CAUTION: return C_WARN; default: return C_OK; } }
static uint32_t borderCol() { switch (g_sev) { case SEV_DANGER: return C_CRIT; case SEV_ALERT: return C_ACCENT; case SEV_CAUTION: return C_AMBER; default: return C_LINE; } }
static uint32_t headerCol() { switch (g_sev) { case SEV_DANGER: return C_CRIT; case SEV_ALERT: return C_ACCENT_HI; case SEV_CAUTION: return C_AMBER; default: return C_ACCENT; } }
static const char* sevWord() { switch (g_sev) { case SEV_DANGER: return "危険"; case SEV_ALERT: return "警告"; case SEV_CAUTION: return "注意"; default: return "正常"; } }
static const char* sevMsg()  { switch (g_sev) { case SEV_DANGER: return "危険 — 室温 危険域 緊急換気"; case SEV_ALERT: return "警告 — 室温 上限超過 換気要求"; case SEV_CAUTION: return "注意 — 室温 上昇中"; default: return "SYSTEM NOMINAL"; } }
static void markAllDirty() {
  g_temp.dirty = g_humid.dirty = g_co2.dirty = g_hd.dirty = g_pres.dirty = g_cur.dirty = g_flow.dirty = true;
  trendDirty = true; g_win1.dirty = g_win2.dirty = true;
}

// ---- helpers ----------------------------------------------------------------
static void fmt(char* b, size_t n, float v, int dec) {
  if (isnan(v)) { snprintf(b, n, dec ? "--.-" : "---"); return; }
  snprintf(b, n, dec ? "%.1f" : "%.0f", v);
}

// ---- header (accent + status shift with severity) ---------------------------
static void drawHeader() {
  display.fillRect(MX, 6, SCR_W - 2 * MX, HDR_H - 6, C_PANEL);
  display.fillRect(MX, 6, 8, HDR_H - 6, headerCol());
  display.setFont(&fonts::lgfxJapanGothic_28);
  display.setTextDatum(textdatum_t::middle_left);
  display.setTextColor(C_TEXT);
  display.drawString("AGRIHA 統合環境管制", MX + 22, 33);
  display.setFont(&fonts::Font2);
  display.setTextColor(C_ACCENT_HI);
  display.drawString("HOUSE 02 / BEPPO-UNIT", 400, 34);
  display.setFont(&fonts::lgfxJapanGothic_24);
  display.setTextDatum(textdatum_t::middle_right);
  display.setTextColor(g_sev == SEV_NORMAL ? C_DIM : sevMain());
  display.drawString(sevMsg(), SCR_W - MX - 10, 33);
  display.drawFastHLine(MX, HDR_H + 2, SCR_W - 2 * MX, headerCol());
}
static void drawChrome() { display.fillScreen(C_BG); drawHeader(); }

// ---- hero: room-temp ring ---------------------------------------------------
static void drawHero() {
  sprHero.fillSprite(C_BG);
  sprHero.drawRect(0, 0, HERO_W, HERO_H, borderCol());
  int cx = HERO_W / 2, cy = 196, rO = 140, rI = 118;
  sprHero.fillArc(cx, cy, rO, rI, 135, 45, C_LINE);          // 270 deg track, gap at bottom
  float t = g_temp.v;
  if (!isnan(t)) {
    float frac = (t - 15.0f) / 20.0f; frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
    sprHero.fillArc(cx, cy, rO, rI, 135, 135 + frac * 270.0f, sevColor(t));
  }
  char buf[8]; fmt(buf, sizeof(buf), t, 1);
  sprHero.setFont(&fonts::Font7);
  sprHero.setTextColor(C_TEXT);
  sprHero.setTextDatum(textdatum_t::middle_center);
  sprHero.drawString(buf, cx, cy - 4);
  sprHero.setFont(&fonts::lgfxJapanGothic_24);
  sprHero.setTextColor(C_DIM);
  sprHero.drawString("室温 / IN-AIR TEMP ℃", cx, cy + 72);
  char sp[40]; snprintf(sp, sizeof(sp), "SP %.1f℃   range 15-35", TEMP_SP);
  sprHero.setFont(&fonts::lgfxJapanGothic_16);
  sprHero.setTextColor(C_DIM);
  sprHero.setTextDatum(textdatum_t::top_center);
  sprHero.drawString(sp, cx, cy + 104);
  // footer: unit id + status
  sprHero.drawFastHLine(16, HERO_H - 84, HERO_W - 32, C_LINE);
  sprHero.setFont(&fonts::lgfxJapanGothic_16);
  sprHero.setTextDatum(textdatum_t::top_left);
  sprHero.setTextColor(C_DIM);
  sprHero.drawString("HOUSE 02  別棟", 20, HERO_H - 74);
  sprHero.setFont(&fonts::lgfxJapanGothic_28);
  sprHero.setTextColor(sevMain());
  sprHero.drawString(sevWord(), 20, HERO_H - 50);
  sprHero.pushSprite(HERO_X, HERO_Y);
}

// ---- trend: room-temp 60 min ------------------------------------------------
static void drawTrend() {
  sprTrend.fillSprite(C_BG);
  sprTrend.drawRect(0, 0, TR_W, TR_H, borderCol());
  sprTrend.setFont(&fonts::lgfxJapanGothic_20);
  sprTrend.setTextColor(C_ACCENT);
  sprTrend.setTextDatum(textdatum_t::top_left);
  sprTrend.drawString("記録  IN-AIR TEMP  60min", 12, 8);

  const int px = 44, py = 40, pw = TR_W - 58, ph = TR_H - 54;
  if (trendCount < 1) {
    sprTrend.setFont(&fonts::lgfxJapanGothic_20);
    sprTrend.setTextColor(C_DIM);
    sprTrend.setTextDatum(textdatum_t::middle_center);
    sprTrend.drawString("収集中 ...", TR_W / 2, TR_H / 2);
    sprTrend.pushSprite(TR_X, TR_Y);
    return;
  }
  float mn = 1e9f, mx = -1e9f;
  for (int i = 0; i < trendCount; i++) { mn = min(mn, trendBuf[i]); mx = max(mx, trendBuf[i]); }
  mn = min(mn, TEMP_SP); mx = max(mx, TEMP_SP);
  float lo = floorf(mn - 1.0f), hi = ceilf(mx + 1.0f);
  if (hi - lo < 4.0f) { float c = (hi + lo) / 2; lo = c - 2; hi = c + 2; }
  auto Y = [&](float v){ return py + ph - (int)((v - lo) / (hi - lo) * ph); };
  auto X = [&](int i){ return trendCount == 1 ? px : px + (int)((float)i / (trendCount - 1) * pw); };

  for (int d = (int)ceilf(lo); d <= (int)hi; d++) {
    int yy = Y(d);
    sprTrend.drawFastHLine(px, yy, pw, C_GRID);
    sprTrend.setFont(&fonts::Font2);
    sprTrend.setTextColor(C_DIM);
    sprTrend.setTextDatum(textdatum_t::middle_right);
    sprTrend.drawString(String(d), px - 4, yy);
  }
  int spy = Y(TEMP_SP);
  for (int x = px; x < px + pw; x += 8) sprTrend.drawFastHLine(x, spy, 4, C_AMBER);
  sprTrend.setFont(&fonts::Font2);
  sprTrend.setTextColor(C_AMBER);
  sprTrend.setTextDatum(textdatum_t::top_left);
  sprTrend.drawString("SP", px + 2, spy + 2);

  int base = py + ph;
  for (int i = 1; i < trendCount; i++) {
    int x0 = X(i - 1), x1 = X(i), y0 = Y(trendBuf[i - 1]), y1 = Y(trendBuf[i]);
    sprTrend.fillTriangle(x0, y0, x1, y1, x0, base, C_AREA);
    sprTrend.fillTriangle(x1, y1, x1, base, x0, base, C_AREA);
  }
  for (int i = 1; i < trendCount; i++)
    sprTrend.drawLine(X(i - 1), Y(trendBuf[i - 1]), X(i), Y(trendBuf[i]), C_ACCENT_HI);
  int ex = X(trendCount - 1), ey = Y(trendBuf[trendCount - 1]);
  sprTrend.fillCircle(ex, ey, 3, C_AMBER);
  sprTrend.pushSprite(TR_X, TR_Y);
}

// ---- generic value tile -----------------------------------------------------
static void drawTileInto(M5Canvas& spr, int x, int y, int w, int h,
                         const char* label, float val, int dec, const char* unit,
                         int valSize) {
  spr.fillSprite(C_BG);
  spr.drawRect(0, 0, w, h, borderCol());
  spr.setFont(&fonts::lgfxJapanGothic_28);
  spr.setTextColor(C_ACCENT);
  spr.setTextDatum(textdatum_t::top_left);
  spr.drawString(label, 14, 10);
  char buf[8]; fmt(buf, sizeof(buf), val, dec);
  spr.setFont(&fonts::Font7);
  spr.setTextSize(valSize);
  spr.setTextColor(C_TEXT);
  if (valSize > 1) {                          // big tile: centered so 3-4 digits always fit
    spr.setTextDatum(textdatum_t::middle_center);
    spr.drawString(buf, w / 2, h / 2 + 24);
  } else {                                    // small tile: right-aligned
    spr.setTextDatum(textdatum_t::bottom_right);
    spr.drawString(buf, w - 92, h - 12);
  }
  spr.setTextSize(1);
  spr.setFont(&fonts::lgfxJapanGothic_20);
  spr.setTextColor(C_DIM);
  spr.setTextDatum(textdatum_t::bottom_right);
  spr.drawString(unit, w - 12, h - 16);
  spr.pushSprite(x, y);
}

// ---- window aperture bar ----------------------------------------------------
static void drawWindow(int idx, Win& w) {
  int x = (idx == 0) ? MX : 644;
  sprWin.fillSprite(C_BG);
  sprWin.drawRect(0, 0, WIN_W, WIN_H, borderCol());
  sprWin.setFont(&fonts::lgfxJapanGothic_24);
  sprWin.setTextColor(C_ACCENT);
  sprWin.setTextDatum(textdatum_t::middle_left);
  sprWin.drawString(idx == 0 ? "東窓" : "西窓", 14, 28);
  int tx = 100, tw = WIN_W - 210, th = 24, ty = 14;
  sprWin.drawRect(tx, ty, tw, th, C_LINE);
  int fw = (int)(tw * (w.pct / 100.0f));
  if (fw > 2) sprWin.fillRect(tx + 1, ty + 1, fw - 2, th - 2, C_ACCENT);
  int mx = tx + (int)(tw * (w.target / 100.0f));
  sprWin.fillRect(mx - 1, ty - 4, 3, th + 8, C_AMBER);
  char pc[8]; snprintf(pc, sizeof(pc), "%d%%", w.pct);
  sprWin.setFont(&fonts::Font4);
  sprWin.setTextColor(C_TEXT);
  sprWin.setTextDatum(textdatum_t::middle_right);
  sprWin.drawString(pc, WIN_W - 14, 28);
  sprWin.setFont(&fonts::Font2);
  sprWin.setTextColor(C_DIM);
  sprWin.setTextDatum(textdatum_t::top_left);
  char sub[48]; snprintf(sub, sizeof(sub), "target %d%%   src %s", w.target, w.src);
  sprWin.drawString(sub, 100, 52);
  sprWin.pushSprite(x, WIN_Y);
}

static void redrawDirty() {
  if (g_temp.dirty) {
    Sev ns = computeSev(g_temp.v);
    if (ns != g_sev) { g_sev = ns; drawHeader(); markAllDirty(); }  // whole-screen recolor
    drawHero(); g_temp.dirty = false;
  }
  if (trendDirty)    { drawTrend(); trendDirty = false; }
  if (g_co2.dirty)   { drawTileInto(sprBig, CO2_X, BT_Y, BT_W, BT_H, "CO2",  g_co2.v,  0, "ppm",   2); g_co2.dirty = false; }
  if (g_hd.dirty)    { drawTileInto(sprBig, HD_X,  BT_Y, BT_W, BT_H, "飽差", g_hd.v,  1, "g/m³", 2); g_hd.dirty = false; }
  if (g_humid.dirty) { drawTileInto(sprSm, RC_X, 66,  RC_W, RC_H, "湿度", g_humid.v, 0, "%RH",  1); g_humid.dirty = false; }
  if (g_pres.dirty)  { drawTileInto(sprSm, RC_X, 194, RC_W, RC_H, "気圧", g_pres.v,  0, "hPa",  1); g_pres.dirty = false; }
  if (g_cur.dirty)   { drawTileInto(sprSm, RC_X, 322, RC_W, RC_H, "電流", g_cur.v,   1, "A",    1); g_cur.dirty = false; }
  if (g_flow.dirty)  { drawTileInto(sprSm, RC_X, 450, RC_W, RC_H, "灌水", g_flow.v,  1, "L/min",1); g_flow.dirty = false; }
  if (g_win1.dirty)  { drawWindow(0, g_win1); g_win1.dirty = false; }
  if (g_win2.dirty)  { drawWindow(1, g_win2); g_win2.dirty = false; }
}

// ---- MQTT -------------------------------------------------------------------
static bool ends(const char* topic, const char* suffix) {
  size_t lt = strlen(topic), ls = strlen(suffix);
  return lt >= ls && strcmp(topic + lt - ls, suffix) == 0;
}
static void onMqtt(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, payload, len)) return;
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
  if (mqtt.connect(cid.c_str())) { mqtt.subscribe("agriha/#"); Serial.println("[MQTT] connected"); }
  else Serial.printf("[MQTT] connect failed rc=%d\n", mqtt.state());
}

static void onPortal(WiFiManager* wm) {
  display.fillScreen(C_BG);
  display.fillRect(MX, 6, SCR_W - 2 * MX, HDR_H - 6, C_PANEL);
  display.fillRect(MX, 6, 8, HDR_H - 6, C_ACCENT);
  display.setFont(&fonts::lgfxJapanGothic_28);
  display.setTextColor(C_AMBER);
  display.setTextDatum(textdatum_t::middle_left);
  display.drawString("WiFi SETUP", MX + 22, 33);
  display.setFont(&fonts::lgfxJapanGothic_24);
  display.setTextColor(C_TEXT);
  display.setTextDatum(textdatum_t::top_left);
  display.drawString("1) WiFi AP に接続:  agri-display-setup", MX + 20, 150);
  display.drawString("2) ブラウザで 192.168.4.1", MX + 20, 210);
  display.drawString("3) ネットワークと MQTT broker を選択", MX + 20, 270);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] agri-display-atom EVA dashboard");

  delay(600);                          // let the FPGA boot its flash bitstream
  display.init();
  if (display.width() == 0) { delay(400); display.init(); }
  display.setColorDepth(16);
  display.setRotation(1);
  SCR_W = display.width(); SCR_H = display.height();
  Serial.printf("[disp] W=%d H=%d\n", SCR_W, SCR_H);

  C_BG        = display.color888(10, 8, 6);
  C_PANEL     = display.color888(22, 16, 10);
  C_LINE      = display.color888(96, 54, 18);
  C_GRID      = display.color888(44, 26, 10);
  C_AREA      = display.color888(140, 76, 18);
  C_ACCENT    = display.color888(235, 102, 8);
  C_ACCENT_HI = display.color888(255, 122, 20);
  C_AMBER     = display.color888(245, 166, 35);
  C_TEXT      = display.color888(243, 231, 211);
  C_DIM       = display.color888(150, 112, 72);
  C_OK        = display.color888(79, 194, 122);
  C_WARN      = display.color888(245, 166, 35);
  C_CRIT      = display.color888(240, 57, 43);

  sprHero.setPsram(true);  sprHero.setColorDepth(16);  sprHero.createSprite(HERO_W, HERO_H);
  sprTrend.setPsram(true); sprTrend.setColorDepth(16); sprTrend.createSprite(TR_W, TR_H);
  sprBig.setPsram(true);   sprBig.setColorDepth(16);   sprBig.createSprite(BT_W, BT_H);
  sprSm.setPsram(true);    sprSm.setColorDepth(16);    sprSm.createSprite(RC_W, RC_H);
  sprWin.setPsram(true);   sprWin.setColorDepth(16);   sprWin.createSprite(WIN_W, WIN_H);

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
  static uint32_t lastReconnect = 0, lastDraw = 0, lastSample = 0;
  if (!mqtt.connected()) {
    if (millis() - lastReconnect > 3000) { lastReconnect = millis(); mqttConnect(); }
  } else {
    mqtt.loop();
  }
  uint32_t now = millis();
  if (!isnan(g_temp.v) && (lastSample == 0 ? now > 2000 : now - lastSample > 40000)) {
    lastSample = now; trendPush(g_temp.v);
  }
  if (now - lastDraw > 120) { lastDraw = now; redrawDirty(); }
}
