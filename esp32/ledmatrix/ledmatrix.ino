// HUB75 64x64 LED matrix - thin MQTT client.
// The server hosts the web UI + Home Assistant integration and sends the full
// desired state as JSON on <BASE_TOPIC>/set (retained). This firmware only:
//   - plays one GIF from LittleFS (downloaded from the server on demand)
//   - renders an overlay (clock / scrolling text / game of life) once per tick
//   - reports status + telemetry
//
// Libraries: ESP32-HUB75-MatrixPanel-I2S-DMA, AnimatedGIF, PubSubClient,
//            ArduinoJson (v7), Adafruit GFX (pulled in by the HUB75 lib)

#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <AnimatedGIF.h>
#include <Adafruit_GFX.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <time.h>
#include "mbedtls/base64.h"
#include "config.h"

#define FIRMWARE_VERSION "v5.5.2"

// SD card (SPI). Override any of these in config.h if you wired it differently.
#ifndef SD_CS
#define SD_CS 5
#endif
#ifndef SD_SCK
#define SD_SCK 18
#endif
#ifndef SD_MISO
#define SD_MISO 19
#endif
#ifndef SD_MOSI
#define SD_MOSI 23
#endif

#define PANEL_W 64
#define PANEL_H 64
#define PANEL_CHAIN 1

#define A_PIN 22
#define B_PIN 32
#define C_PIN 33
#define D_PIN 17
#define E_PIN 21

// ---------------------------------------------------------------- globals
MatrixPanel_I2S_DMA *dma_display = nullptr;
GFXcanvas16 *fg = nullptr;            // overlay layer, colour 0 = transparent
uint16_t bgBuf[PANEL_W * PANEL_H];    // GIF layer

AnimatedGIF gif;
File gifHandle;
fs::FS *store = &LittleFS;   // where GIFs live: SD card if present, else internal flash
bool usingSD = false;
WiFiClient net;
PubSubClient mqtt(net);

struct State {
  bool power = true;
  uint8_t brightness = 100;
  uint8_t cc[3] = {255, 255, 255};  // clock colour
  uint8_t tc[3] = {255, 255, 255};  // scrolling text colour
  uint8_t lc[3] = {0, 255, 100};    // game of life colour
  bool gifEnabled = true;
  String gif = "";
  String overlay = "clock";   // none | clock | text | life
  String text = "Hello";
  uint8_t textSize = 2;
  uint16_t scrollDelay = 18;  // ms per pixel step
  bool clock24 = true;
  bool lifeRun = true;        // false = paused
  uint16_t lifeSpeed = 100;   // ms per generation
  bool lifeWrap = true;       // false = dead edges (gliders die at the border)
  bool lifeReseed = false;    // restart with random cells when it dies / gets stale
} st;

String tSet, tCmd, tStatus, tTelemetry, tLife;

bool dirty = true;
bool gifOpen = false;
bool gifChange = true;
bool redrawOverlay = true;
bool pendingReboot = false;
uint32_t nextFrameAt = 0, gifRetryAt = 0, lastTelemetry = 0;
uint32_t lastMqttTry = 0, lastWifiTry = 0, lastOverlayTick = 0;
int lastSec = -1;
int16_t scrollX = PANEL_W;
uint16_t textW = 0;

bool life[2][PANEL_H][PANEL_W];
uint8_t lifeCur = 0;
uint32_t lifeGen = 0;

// ---------------------------------------------------------------- helpers
uint16_t col(const uint8_t *c) { return dma_display->color565(c[0], c[1], c[2]); }

// Combine overlay over GIF and push to the panel (only when something changed)
void present() {
  uint16_t *fgBuf = fg->getBuffer();
  int i = 0;
  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++, i++) {
      uint16_t p = fgBuf[i];
      if (!p) p = bgBuf[i];
      dma_display->drawPixel(x, y, p);
    }
  }
}

// ---------------------------------------------------------------- GIF
void *GIFOpenFile(const char *fname, int32_t *pSize) {
  gifHandle = store->open(fname);
  if (gifHandle) {
    *pSize = gifHandle.size();
    return (void *)&gifHandle;
  }
  return NULL;
}

void GIFCloseFile(void *pHandle) {
  File *f = static_cast<File *>(pHandle);
  if (f != NULL) f->close();
}

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  // Reading to the very last byte breaks seek(), so stop one byte early
  if ((pFile->iSize - pFile->iPos) < iLen) iBytesRead = pFile->iSize - pFile->iPos - 1;
  if (iBytesRead <= 0) return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// Only writes into the RAM buffer - no text, no time, no panel access here.
void GIFDraw(GIFDRAW *pDraw) {
  int y = pDraw->iY + pDraw->y;
  if (y < 0 || y >= PANEL_H) return;
  int iWidth = pDraw->iWidth;
  if (pDraw->iX + iWidth > PANEL_W) iWidth = PANEL_W - pDraw->iX;
  if (iWidth <= 0) return;

  uint8_t *s = pDraw->pPixels;
  uint16_t *pal = pDraw->pPalette;
  uint16_t *row = &bgBuf[y * PANEL_W + pDraw->iX];

  if (pDraw->ucDisposalMethod == 2) {  // restore to background colour
    for (int x = 0; x < iWidth; x++)
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    pDraw->ucHasTransparency = 0;
  }

  if (pDraw->ucHasTransparency) {
    uint8_t t = pDraw->ucTransparent;
    for (int x = 0; x < iWidth; x++)
      if (s[x] != t) row[x] = pal[s[x]];
  } else {
    for (int x = 0; x < iWidth; x++) row[x] = pal[s[x]];
  }
}

void closeGif() {
  if (gifOpen) {
    gif.close();
    gifOpen = false;
  }
}

bool openGif(const String &name) {
  String path = "/" + name;
  if (!store->exists(path)) return false;
  if (gif.open(path.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
    gifOpen = true;
    nextFrameAt = millis();
    Serial.printf("Playing %s (%dx%d)\n", name.c_str(), gif.getCanvasWidth(), gif.getCanvasHeight());
    return true;
  }
  return false;
}

// Download /gifs/<name> from the server into LittleFS if we don't have it yet
bool ensureGif(const String &name) {
  String path = "/" + name;
  if (store->exists(path)) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(SERVER_URL) + "/gifs/" + name;
  Serial.println("Downloading " + url);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Download failed: HTTP %d\n", code);
    http.end();
    return false;
  }
  int expected = http.getSize();
  String tmp = path + ".tmp";
  store->remove(tmp);  // leftover from an interrupted download
  File f = store->open(tmp, FILE_WRITE);
  if (!f) {
    http.end();
    return false;
  }
  int written = http.writeToStream(&f);
  f.close();
  http.end();
  if (written <= 0 || (expected > 0 && written != expected)) {
    Serial.println("Download incomplete (is the storage full?)");
    store->remove(tmp);
    return false;
  }
  store->rename(tmp, path);
  return true;
}

void handleGifChange() {
  if (!gifChange || millis() < gifRetryAt) return;
  gifChange = false;

  bool wanted = st.gifEnabled && !st.gif.isEmpty();
  bool ok = wanted && ensureGif(st.gif);  // may block a moment while downloading

  closeGif();
  memset(bgBuf, 0, sizeof(bgBuf));
  dirty = true;

  if (wanted && !(ok && openGif(st.gif))) {
    gifRetryAt = millis() + 10000;
    gifChange = true;
  }
}

void gifTick() {
  if (!gifOpen || (int32_t)(millis() - nextFrameAt) < 0) return;
  int delayMs = 0;
  int more = gif.playFrame(false, &delayMs);  // non-blocking: we do our own timing
  if (delayMs < 30) delayMs = 30;
  nextFrameAt = millis() + delayMs;
  dirty = true;
  if (more <= 0) gif.reset();  // loop
}

// ---------------------------------------------------------------- overlays
void computeTextWidth() {
  int16_t x1, y1;
  uint16_t w, h;
  fg->setTextSize(st.textSize);
  fg->getTextBounds(st.text.c_str(), 0, 0, &x1, &y1, &w, &h);
  textW = w;
}

void drawClock(const struct tm &t) {
  uint16_t c = col(st.cc);
  char hh[3], mm[3];
  strftime(hh, sizeof(hh), st.clock24 ? "%H" : "%I", &t);
  strftime(mm, sizeof(mm), "%M", &t);
  fg->fillScreen(0);
  fg->setTextWrap(false);
  fg->setTextSize(2);
  fg->setTextColor(c);
  fg->setCursor(6, 24);
  fg->print(hh);
  fg->setCursor(34, 24);
  fg->print(mm);
  if (t.tm_sec % 2 == 0) {  // blinking colon
    fg->fillRect(31, 28, 2, 2, c);
    fg->fillRect(31, 34, 2, 2, c);
  }
}

void drawText() {
  fg->fillScreen(0);
  fg->setTextWrap(false);
  fg->setTextSize(st.textSize);
  fg->setTextColor(col(st.tc));
  fg->setCursor(scrollX, (PANEL_H - 8 * st.textSize) / 2);
  fg->print(st.text);
}

void lifeInit() {
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++) life[lifeCur][y][x] = random(100) < 30;
  lifeGen = 0;
}

void lifeStep() {
  uint8_t nxt = lifeCur ^ 1;
  int alive = 0;
  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++) {
      int n = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          if (!dx && !dy) continue;
          int ny = y + dy, nx2 = x + dx;
          if (st.lifeWrap) {
            ny = (ny + PANEL_H) % PANEL_H;
            nx2 = (nx2 + PANEL_W) % PANEL_W;
          } else if (ny < 0 || ny >= PANEL_H || nx2 < 0 || nx2 >= PANEL_W) {
            continue;
          }
          n += life[lifeCur][ny][nx2];
        }
      bool cell = life[lifeCur][y][x] ? (n == 2 || n == 3) : (n == 3);
      life[nxt][y][x] = cell;
      alive += cell;
    }
  }
  lifeCur = nxt;
  lifeGen++;
  if (st.lifeReseed && (alive == 0 || lifeGen > 600)) lifeInit();  // optional screensaver mode
}

void drawLife() {
  uint16_t c = col(st.lc);
  fg->fillScreen(0);
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++)
      if (life[lifeCur][y][x]) fg->drawPixel(x, y, c);
}

void overlayTick(uint32_t now) {
  if (redrawOverlay) {
    redrawOverlay = false;
    lastSec = -1;
    if (st.overlay == "none") {
      fg->fillScreen(0);
    } else if (st.overlay == "text") {
      drawText();
    } else if (st.overlay == "life") {
      drawLife();
    }
    dirty = true;
  }

  if (st.overlay == "clock") {
    struct tm t;
    if (getLocalTime(&t, 0) && t.tm_sec != lastSec) {
      lastSec = t.tm_sec;
      drawClock(t);
      dirty = true;
    }
  } else if (st.overlay == "text") {
    if (now - lastOverlayTick >= st.scrollDelay) {
      lastOverlayTick = now;
      if (--scrollX + (int)textW <= 0) scrollX = PANEL_W;
      drawText();
      dirty = true;
    }
  } else if (st.overlay == "life") {
    if (st.lifeRun && now - lastOverlayTick >= st.lifeSpeed) {
      lastOverlayTick = now;
      lifeStep();
      drawLife();
      dirty = true;
    }
  }
}

// ---------------------------------------------------------------- state / MQTT
// Pattern from the editor: base64 of 512 bytes, cell i = y*64+x is bit (i & 7) of byte (i >> 3)
void loadLifePattern(const uint8_t *payload, unsigned int len) {
  uint8_t raw[PANEL_W * PANEL_H / 8];
  size_t olen = 0;
  if (mbedtls_base64_decode(raw, sizeof(raw), &olen, payload, len) != 0 || olen != sizeof(raw)) {
    Serial.println("Life pattern rejected (bad size)");
    return;
  }
  for (int i = 0; i < PANEL_W * PANEL_H; i++)
    life[lifeCur][i / PANEL_W][i % PANEL_W] = (raw[i >> 3] >> (i & 7)) & 1;
  lifeGen = 0;
  redrawOverlay = true;
  dirty = true;
  Serial.println("Life pattern loaded");
}

void readColor(JsonDocument &d, const char *key, uint8_t *dst) {
  JsonArray a = d[key];
  if (a.size() == 3) {
    dst[0] = a[0];
    dst[1] = a[1];
    dst[2] = a[2];
  }
}

void applyState(JsonDocument &d) {
  bool prevPower = st.power, prevGifEn = st.gifEnabled;
  String prevGif = st.gif, prevOverlay = st.overlay, prevText = st.text;
  uint8_t prevSize = st.textSize;

  if (d["power"].is<bool>()) st.power = d["power"].as<bool>();
  if (d["brightness"].is<int>()) {
    st.brightness = constrain(d["brightness"].as<int>(), 0, 255);
    dma_display->setBrightness8(st.brightness);
  }
  JsonArray all = d["color"];  // legacy: one colour for everything
  if (all.size() == 3) {
    for (int i = 0; i < 3; i++) st.cc[i] = st.tc[i] = st.lc[i] = all[i];
  }
  readColor(d, "clock_color", st.cc);
  readColor(d, "text_color", st.tc);
  readColor(d, "life_color", st.lc);
  if (d["gif_enabled"].is<bool>()) st.gifEnabled = d["gif_enabled"].as<bool>();
  if (d["gif"].is<const char *>()) st.gif = d["gif"].as<const char *>();
  if (d["overlay"].is<const char *>()) st.overlay = d["overlay"].as<const char *>();
  if (d["text"].is<const char *>()) st.text = d["text"].as<const char *>();
  if (d["text_size"].is<int>()) st.textSize = constrain(d["text_size"].as<int>(), 1, 4);
  if (d["scroll_delay"].is<int>()) st.scrollDelay = constrain(d["scroll_delay"].as<int>(), 1, 500);
  if (d["clock_24h"].is<bool>()) st.clock24 = d["clock_24h"].as<bool>();
  if (d["life_run"].is<bool>()) st.lifeRun = d["life_run"].as<bool>();
  if (d["life_speed"].is<int>()) st.lifeSpeed = constrain(d["life_speed"].as<int>(), 10, 2000);
  if (d["life_wrap"].is<bool>()) st.lifeWrap = d["life_wrap"].as<bool>();
  if (d["life_reseed"].is<bool>()) st.lifeReseed = d["life_reseed"].as<bool>();

  if (st.gif != prevGif || st.gifEnabled != prevGifEn) gifChange = true;
  if (st.overlay != prevOverlay || st.text != prevText || st.textSize != prevSize) {
    scrollX = PANEL_W;
  }
  computeTextWidth();
  redrawOverlay = true;
  dirty = true;

  if (!st.power && prevPower) dma_display->clearScreen();
}

void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  if (tLife == topic) {
    loadLifePattern(payload, len);
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, (const char *)payload, len)) return;

  if (tSet == topic) {
    applyState(d);
  } else if (tCmd == topic) {
    if (d["reboot"].as<bool>()) pendingReboot = true;
    if (d["life_step"].as<bool>()) {
      lifeStep();
      redrawOverlay = true;
    }
    if (d["life_random"].as<bool>()) {
      lifeInit();
      redrawOverlay = true;
    }
    if (d["delete"].is<const char *>()) {
      String name = d["delete"].as<const char *>();
      if (name.indexOf('/') < 0 && name.length() > 0) {
        if (name == st.gif) {
          closeGif();
          gifChange = true;  // will re-download if the server still has it
        }
        store->remove("/" + name);
      }
    }
  }
}

void publishTelemetry() {
  JsonDocument d;
  d["fw"] = FIRMWARE_VERSION;
  d["ip"] = WiFi.localIP().toString();
  d["rssi"] = WiFi.RSSI();
  d["free_heap"] = ESP.getFreeHeap();
  d["uptime"] = millis() / 1000;
  d["storage"] = usingSD ? "sd" : "flash";
  d["fs_free"] = usingSD ? (uint64_t)(SD.totalBytes() - SD.usedBytes())
                         : (uint64_t)(LittleFS.totalBytes() - LittleFS.usedBytes());
  char buf[256];
  size_t n = serializeJson(d, buf, sizeof(buf));
  mqtt.publish(tTelemetry.c_str(), (const uint8_t *)buf, n, false);
}

void mqttTick() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 15000) {
      lastWifiTry = millis();
      Serial.println("WiFi: retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry < 5000) return;
    lastMqttTry = millis();
    String id = "ledmatrix-" + WiFi.macAddress();
    id.replace(":", "");
    bool ok = strlen(MQTT_USER)
                  ? mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, tStatus.c_str(), 1, true, "offline")
                  : mqtt.connect(id.c_str(), nullptr, nullptr, tStatus.c_str(), 1, true, "offline");
    if (ok) {
      Serial.println("MQTT connected");
      mqtt.publish(tStatus.c_str(), "online", true);
      mqtt.subscribe(tSet.c_str(), 1);
      mqtt.subscribe(tCmd.c_str());
      mqtt.subscribe(tLife.c_str(), 1);
      publishTelemetry();
      lastTelemetry = millis();
    } else {
      Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
  if (millis() - lastTelemetry > 30000) {
    lastTelemetry = millis();
    publishTelemetry();
  }
}

// ---------------------------------------------------------------- setup / loop
// Prints WHY the router dropped/refused us:
//   2/6/7/202 = auth failed, 15 = 4-way handshake timeout (wrong password or WPA3/PMF problem),
//   201 = SSID not found, 205 = connection failed, 17/13 = router refused (client limit / security mode)
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("WiFi disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.println("WiFi got IP: " + WiFi.localIP().toString());
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nLED matrix " FIRMWARE_VERSION);

  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, PANEL_CHAIN);
  mxconfig.gpio.a = A_PIN;
  mxconfig.gpio.b = B_PIN;
  mxconfig.gpio.c = C_PIN;
  mxconfig.gpio.d = D_PIN;
  mxconfig.gpio.e = E_PIN;
  mxconfig.clkphase = false;
  mxconfig.driver = HUB75_I2S_CFG::FM6126A;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(st.brightness);
  dma_display->clearScreen();

  fg = new GFXcanvas16(PANEL_W, PANEL_H);
  fg->fillScreen(0);
  memset(bgBuf, 0, sizeof(bgBuf));

  // Prefer the SD card; fall back to internal flash if there is none
  Serial.printf("SD pins: CS=%d SCK=%d MISO=%d MOSI=%d\n", SD_CS, SD_SCK, SD_MISO, SD_MOSI);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  // Jumper wires often can't do 20 MHz, so retry slower before giving up
  const uint32_t sdSpeeds[] = {20000000, 10000000, 4000000, 1000000};
  for (uint32_t hz : sdSpeeds) {
    if (SD.begin(SD_CS, SPI, hz)) {
      store = &SD;
      usingSD = true;
      const char *types[] = {"none", "MMC", "SD", "SDHC", "unknown"};
      Serial.printf("SD card mounted at %u kHz (%s): %llu MB total, %llu MB used\n", hz / 1000,
                    types[min((int)SD.cardType(), 4)], SD.totalBytes() / (1024 * 1024),
                    SD.usedBytes() / (1024 * 1024));
      break;
    }
    Serial.printf("SD mount failed at %u kHz\n", hz / 1000);
    SD.end();
    delay(100);
  }
  if (!usingSD) {
    Serial.println("SD card NOT found (check wiring: CS/SCK/MISO/MOSI, 3.3V/5V, FAT32). Using internal flash.");
    if (!LittleFS.begin(true)) {
      Serial.println("LittleFS mount failed, rebooting");
      delay(1000);
      ESP.restart();
    }
    Serial.printf("LittleFS: %u / %u bytes used\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  }
  gif.begin(LITTLE_ENDIAN_PIXELS);

  tSet = String(BASE_TOPIC) + "/set";
  tCmd = String(BASE_TOPIC) + "/cmd";
  tStatus = String(BASE_TOPIC) + "/status";
  tTelemetry = String(BASE_TOPIC) + "/telemetry";
  tLife = String(BASE_TOPIC) + "/life/set";

  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false);          // don't keep stale credentials in flash
  WiFi.disconnect(true, true);     // wipe any saved WiFi state from earlier firmware
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
#if USE_STATIC_IP
  WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GW), IPAddress(STATIC_MASK),
              IPAddress(STATIC_DNS1), IPAddress(STATIC_DNS2));
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) Serial.println("IP: " + WiFi.localIP().toString());

  configTzTime(TZ_INFO, NTP_SERVER, "pool.ntp.org");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(30);

  // Boot screen: IP for 2 seconds
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  dma_display->setCursor(0, 0);
  dma_display->print(FIRMWARE_VERSION);
  dma_display->setCursor(0, 16);
  dma_display->print(WiFi.localIP());
  delay(2000);
  dma_display->clearScreen();

  randomSeed(esp_random());
  lifeInit();
  computeTextWidth();
}

void loop() {
  mqttTick();

  if (pendingReboot) {
    mqtt.publish(tStatus.c_str(), "offline", true);
    mqtt.disconnect();
    delay(200);
    ESP.restart();
  }

  if (st.power) {
    handleGifChange();
    gifTick();
    overlayTick(millis());
    if (dirty) {
      dirty = false;
      present();
    }
  }
  delay(1);
}
