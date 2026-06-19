// Test script for the Pocket 2.66 PCB
// Written by Quinten Luyten 2026
// Modified for ESP32-C3 + Heltec WirelessPaper V1.1 / JD79656 panel.
// This version uses a WiFi AP + browser UI instead of a Serial command interface.
// Upload to "ESP32C3 Dev Module" target with USB CDC On Boot ENABLED.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <stdarg.h>

// -----------------------------------------------------------------------------
// Pinout: GPIO numbers, not package pin numbers.
// Existing Pocket 2.66 / ESP32-C3FH4 layout.
// -----------------------------------------------------------------------------
#define SPI_MISO        10
#define SPI_MOSI        21
#define SPI_SCK         20

#define SD_CS           1   // microSD CS

#define EPD_CS          8   // E-ink CS
#define EPD_DC          7   // E-ink Data/Command line
#define EPD_RESET       6   // E-ink Reset
#define EPD_BUSY        5   // JD79656 / Heltec V1.1: LOW = busy, HIGH = idle

#define BUTTON_ADC_PIN  0   // GPIO0 resistor ladder: ~3.3V none, ~1.65V BUT2, ~0V BUT1

#define EPD_BLACK       0x0000
#define EPD_WHITE       0xFFFF

#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

// -----------------------------------------------------------------------------
// WiFi AP settings.
// -----------------------------------------------------------------------------
static const char *AP_SSID = "Pocket-EPD-Test";
static const char *AP_PASS = "pockettest";   // minimum 8 chars for WPA2 AP

IPAddress ap_ip(192, 168, 4, 1);
IPAddress ap_gateway(192, 168, 4, 1);
IPAddress ap_subnet(255, 255, 255, 0);

WebServer server(80);

// -----------------------------------------------------------------------------
// Small browser-visible log.
// -----------------------------------------------------------------------------
String eventLog;
static constexpr size_t MAX_LOG_CHARS = 7000;

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\') out += F("\\\\");
    else if (c == '"') out += F("\\\"");
    else if (c == '\n') out += F("\\n");
    else if (c == '\r') out += F("\\r");
    else out += c;
  }
  return out;
}

void addLogLine(const String &line) {
  Serial.println(line);
  eventLog += line;
  eventLog += '\n';

  if (eventLog.length() > MAX_LOG_CHARS) {
    eventLog.remove(0, eventLog.length() - MAX_LOG_CHARS);
  }
}

void logf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  addLogLine(String(buf));
}

// -----------------------------------------------------------------------------
// Heltec Wireless Paper V1.1 panel, as used by heltec-eink-modules.
// LCMEN2R13EFC1 / JD79656-style command set.
// Exposes a normal landscape 250 x 122 Adafruit_GFX framebuffer.
// -----------------------------------------------------------------------------
class HeltecV11_JD79656 : public Adafruit_GFX {
public:
  static constexpr int16_t SCREEN_W = 250;
  static constexpr int16_t SCREEN_H = 122;

private:
  static constexpr int16_t PANEL_W = 128;               // controller RAM width, byte-aligned
  static constexpr int16_t PANEL_H = 250;
  static constexpr uint16_t BUFFER_SIZE = PANEL_W * PANEL_H / 8;

  SPIClass *spi;
  int8_t pin_dc;
  int8_t pin_rst;
  int8_t pin_cs;
  int8_t pin_busy;
  uint8_t buffer[BUFFER_SIZE];

public:
  HeltecV11_JD79656(int8_t dc, int8_t rst, int8_t cs, int8_t busy, SPIClass *spi_bus = &SPI)
      : Adafruit_GFX(SCREEN_W, SCREEN_H),
        spi(spi_bus), pin_dc(dc), pin_rst(rst), pin_cs(cs), pin_busy(busy) {
    clearBuffer();
  }

  void begin() {
    pinMode(pin_cs, OUTPUT);
    pinMode(pin_dc, OUTPUT);
    pinMode(pin_rst, OUTPUT);
    pinMode(pin_busy, INPUT);     // do not enable pullup/pulldown on BUSY

    digitalWrite(pin_cs, HIGH);
    digitalWrite(pin_dc, HIGH);
    digitalWrite(pin_rst, HIGH);

    hardwareReset();
    configFullRefresh();
  }

  void clearBuffer(uint16_t color = EPD_WHITE) {
    memset(buffer, color == EPD_BLACK ? 0x00 : 0xFF, sizeof(buffer));
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= width() || y >= height()) return;

    // Present the display as landscape 250 x 122.
    // Controller RAM is portrait-like: 128 x 250, of which 122 x 250 is visible.
    // This mapping fixes the right-to-left mirrored text from the first version.
    int16_t phys_x = y;                         // 0..121 visible columns in 128-wide RAM
    int16_t phys_y = SCREEN_W - 1 - x;          // 249..0 rows, fixes left/right mirror

    // Alternatives to try if your FPC/display mounting differs:
    // int16_t phys_x = y;                 phys_y = x;                 // original mirrored mapping
    // int16_t phys_x = SCREEN_H - 1 - y;  phys_y = x;                 // vertical mirror
    // int16_t phys_x = SCREEN_H - 1 - y;  phys_y = SCREEN_W - 1 - x;  // 180 degree rotate

    if (phys_x < 0 || phys_x >= PANEL_W || phys_y < 0 || phys_y >= PANEL_H) return;

    const uint16_t index = phys_y * (PANEL_W / 8) + (phys_x / 8);
    const uint8_t mask = 0x80 >> (phys_x & 7);

    if (color == EPD_BLACK) {
      buffer[index] &= ~mask;     // 0 = black
    } else {
      buffer[index] |= mask;      // 1 = white
    }
  }

  void display() {
    configFullRefresh();

    // JD79656 / LCMEN2R13EFC1 writes OLD memory with 0x10 and NEW memory with 0x13.
    sendCommand(0x10);
    sendBuffer(buffer, sizeof(buffer));

    sendCommand(0x13);
    sendBuffer(buffer, sizeof(buffer));

    // Power on, refresh, then power off.
    sendCommand(0x04);
    waitIdle(10000);

    sendCommand(0x12);
    waitIdle(20000);

    sendCommand(0x02);
    waitIdle(10000);
  }

private:
  void hardwareReset() {
    digitalWrite(pin_rst, HIGH);
    delay(20);
    digitalWrite(pin_rst, LOW);
    delay(20);
    digitalWrite(pin_rst, HIGH);
    delay(200);
    waitIdle(10000);
  }

  bool waitIdle(uint32_t timeout_ms) {
    const uint32_t start = millis();

    // Important: this Heltec/JD79656 panel is inverted vs many SSD controllers:
    // LOW = busy, HIGH = idle.
    while (digitalRead(pin_busy) == LOW) {
      if (millis() - start > timeout_ms) {
        Serial.println("EPD BUSY timeout");
        return false;
      }
      delay(1);
      yield();
    }
    return true;
  }

  void configFullRefresh() {
    waitIdle(10000);

    sendCommand(0x00);            // Panel setting
    sendData(0xDF);               // Heltec LCMEN2R13EFC1 full-refresh setting

    sendCommand(0x50);            // VCOM and data interval setting
    sendData(0xB7);               // Heltec LCMEN2R13EFC1 full-refresh setting
  }

  void sendCommand(uint8_t command) {
    digitalWrite(SD_CS, HIGH);    // shared SPI bus: deselect SD card
    spi->beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(pin_dc, LOW);
    digitalWrite(pin_cs, LOW);
    spi->transfer(command);
    digitalWrite(pin_cs, HIGH);
    spi->endTransaction();
  }

  void sendData(uint8_t data) {
    digitalWrite(SD_CS, HIGH);    // shared SPI bus: deselect SD card
    spi->beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(pin_dc, HIGH);
    digitalWrite(pin_cs, LOW);
    spi->transfer(data);
    digitalWrite(pin_cs, HIGH);
    spi->endTransaction();
  }

  void sendBuffer(const uint8_t *data, uint16_t len) {
    digitalWrite(SD_CS, HIGH);    // shared SPI bus: deselect SD card
    spi->beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(pin_dc, HIGH);
    digitalWrite(pin_cs, LOW);
    for (uint16_t i = 0; i < len; i++) {
      spi->transfer(data[i]);
    }
    digitalWrite(pin_cs, HIGH);
    spi->endTransaction();
  }
};

HeltecV11_JD79656 display(EPD_DC, EPD_RESET, EPD_CS, EPD_BUSY, &SPI);

// -----------------------------------------------------------------------------
// Buttons.
// -----------------------------------------------------------------------------
typedef struct ButtonReading {
  int raw;
  uint32_t millivolts_est;
  const char *state;
};

ButtonReading readButtons() {
  ButtonReading r;
  r.raw = analogRead(BUTTON_ADC_PIN);
  r.millivolts_est = (uint32_t)r.raw * 3300UL / 4095UL;

  // Designed ladder:
  //   BUT1 pressed: ~0V
  //   BUT2 pressed: ~1.65V
  //   no button:    ~3.3V
  // Thresholds are deliberately wide because the ESP32 ADC is not a precision meter.
  if (r.raw < 800) {
    r.state = "BUT1";
  } else if (r.raw > 2400) {
    r.state = "NONE";
  } else {
    r.state = "BUT2";
  }

  return r;
}

// -----------------------------------------------------------------------------
// SD card test.
// -----------------------------------------------------------------------------
bool sdMounted = false;
String lastSDStatus = "Not tested yet.";

String cardTypeName(uint8_t cardType) {
  if (cardType == CARD_NONE) return "None";
  if (cardType == CARD_MMC) return "MMC";
  if (cardType == CARD_SD) return "SDSC";
  if (cardType == CARD_SDHC) return "SDHC/SDXC";
  return "Unknown";
}

void listRootDirectoryToLog() {
  File root = SD.open("/");
  if (!root) {
    addLogLine("Could not open SD root directory");
    return;
  }
  if (!root.isDirectory()) {
    addLogLine("SD root is not a directory");
    root.close();
    return;
  }

  addLogLine("Root directory:");
  File file = root.openNextFile();
  uint16_t count = 0;
  while (file) {
    if (file.isDirectory()) {
      logf("  <DIR>  %s", file.name());
    } else {
      logf("  %7lu  %s", (unsigned long)file.size(), file.name());
    }
    file.close();
    count++;
    file = root.openNextFile();
  }
  root.close();

  if (count == 0) {
    addLogLine("  <empty>");
  }
}

bool initSD() {
  digitalWrite(EPD_CS, HIGH);     // shared SPI bus: deselect EPD
  digitalWrite(SD_CS, HIGH);

  addLogLine("Initializing SD card in SPI mode...");

  // 400 kHz is intentionally slow for first board bring-up.
  // Increase later to 4-20 MHz after the slot/wiring are proven.
  if (!SD.begin(SD_CS, SPI, 400000)) {
    lastSDStatus = "SD.begin() failed";
    addLogLine("SD.begin() failed. Check card insertion, CS=GPIO1, MISO/MOSI/SCK, 3V3, and GND.");
    sdMounted = false;
    return false;
  }

  uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    lastSDStatus = "No card detected";
    addLogLine("No SD card detected.");
    sdMounted = false;
    return false;
  }

  uint64_t sizeMb = SD.cardSize() / (1024ULL * 1024ULL);
  lastSDStatus = "OK: " + cardTypeName(type) + ", " + String((unsigned long)sizeMb) + " MB";
  logf("Card type: %s", cardTypeName(type).c_str());
  logf("Card size: %llu MB", SD.cardSize() / (1024ULL * 1024ULL));

  sdMounted = true;
  return true;
}

bool testSDCard() {
  addLogLine("");
  addLogLine("=== microSD SPI test ===");

  if (!initSD()) {
    addLogLine("SD test FAILED during init.");
    return false;
  }

  listRootDirectoryToLog();

  const char *path = "/esp32c3_sd_test.txt";
  logf("Appending to %s ...", path);

  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    addLogLine("Append open failed, trying FILE_WRITE...");
    f = SD.open(path, FILE_WRITE);
  }

  if (!f) {
    lastSDStatus = "File open for write failed";
    addLogLine("Could not open test file for writing.");
    return false;
  }

  f.print("ESP32-C3 SD SPI test, millis=");
  f.println(millis());
  f.close();
  addLogLine("Write OK.");

  logf("Reading back %s ...", path);
  f = SD.open(path, FILE_READ);
  if (!f) {
    lastSDStatus = "File open for read failed";
    addLogLine("Could not open test file for reading.");
    return false;
  }

  addLogLine("--- file content start ---");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    addLogLine(line);
  }
  addLogLine("--- file content end ---");
  f.close();

  listRootDirectoryToLog();
  lastSDStatus = "SD test OK";
  addLogLine("SD test DONE.");
  return true;
}

// -----------------------------------------------------------------------------
// EPD test.
// -----------------------------------------------------------------------------
bool drawDisplayTest() {
  addLogLine("");
  addLogLine("=== EPD display test ===");

  ButtonReading br = readButtons();

  display.clearBuffer(EPD_WHITE);
  display.setTextColor(EPD_BLACK);
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.print("Pocket EPD");

  display.setTextSize(1);
  display.setCursor(10, 38);
  display.print("Heltec V1.1 / JD79656");

  display.setCursor(10, 56);
  display.print("AP: ");
  display.print(AP_SSID);

  display.setCursor(10, 70);
  display.print("IP: ");
  display.print(WiFi.softAPIP());

  display.setCursor(10, 88);
  display.print("Button: ");
  display.print(br.state);
  display.print(" raw=");
  display.print(br.raw);

  display.setCursor(10, 104);
  display.print("SD CS=GPIO");
  display.print(SD_CS);

  display.drawRect(0, 0, display.width(), display.height(), EPD_BLACK);
  display.drawLine(0, 82, display.width() - 1, 82, EPD_BLACK);

  addLogLine("Refreshing EPD...");
  display.display();
  addLogLine("EPD test DONE.");
  return true;
}

void printPinSummary() {
  addLogLine("");
  addLogLine("=== Pin summary ===");
  logf("SPI: SCK=GPIO%d, MISO=GPIO%d, MOSI=GPIO%d", SPI_SCK, SPI_MISO, SPI_MOSI);
  logf("EPD: CS=GPIO%d, DC=GPIO%d, RESET=GPIO%d, BUSY=GPIO%d", EPD_CS, EPD_DC, EPD_RESET, EPD_BUSY);
  logf("SD:  CS=GPIO%d", SD_CS);
  logf("Buttons ADC ladder: GPIO%d", BUTTON_ADC_PIN);
}

// -----------------------------------------------------------------------------
// Web UI.
// -----------------------------------------------------------------------------
String makeHomePage() {
  ButtonReading br = readButtons();
  String page;
  page.reserve(12000);

  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Pocket PCB Test</title>");
  page += F("<style>");
  page += F("body{font-family:system-ui,-apple-system,Segoe UI,Arial,sans-serif;margin:18px;line-height:1.35;background:#f5f5f5;color:#111}");
  page += F(".card{background:white;border:1px solid #ddd;border-radius:12px;padding:14px;margin:0 0 14px 0;box-shadow:0 1px 3px #0001}");
  page += F("a.btn,button{display:inline-block;margin:4px 6px 4px 0;padding:10px 13px;border-radius:9px;border:1px solid #aaa;background:#fff;color:#111;text-decoration:none;font-weight:600}");
  page += F("a.btn:hover,button:hover{background:#eee}");
  page += F("pre{white-space:pre-wrap;background:#111;color:#eee;padding:12px;border-radius:10px;max-height:360px;overflow:auto}");
  page += F(".ok{color:#086}.warn{color:#a60}.small{color:#555;font-size:.92em}");
  page += F("code{background:#eee;padding:1px 4px;border-radius:4px}");
  page += F("</style></head><body>");

  page += F("<h1>Pocket 2.66 PCB test</h1>");

  page += F("<div class='card'><h2>Actions</h2>");
  page += F("<a class='btn' href='/epd'>Refresh EPD</a>");
  page += F("<a class='btn' href='/sd'>Run microSD test</a>");
  page += F("<a class='btn' href='/all'>Run all tests</a>");
  page += F("<a class='btn' href='/clearlog'>Clear log</a>");
  page += F("<a class='btn' href='/restart' onclick='return confirm(\"Restart ESP32-C3?\")'>Restart MCU</a>");
  page += F("<p class='small'>Long actions block this page until the EPD refresh or SD test finishes.</p>");
  page += F("</div>");

  page += F("<div class='card'><h2>Status</h2>");
  page += F("<p><b>AP:</b> ");
  page += AP_SSID;
  page += F(" &nbsp; <b>IP:</b> ");
  page += WiFi.softAPIP().toString();
  page += F("</p>");
  page += F("<p><b>Button:</b> <span id='btnState'>");
  page += br.state;
  page += F("</span> &nbsp; raw=<span id='btnRaw'>");
  page += String(br.raw);
  page += F("</span> &nbsp; approx=<span id='btnMv'>");
  page += String(br.millivolts_est);
  page += F("</span> mV</p>");
  page += F("<p><b>SD:</b> <span id='sdStatus'>");
  page += htmlEscape(lastSDStatus);
  page += F("</span></p>");
  page += F("<p><b>Uptime:</b> <span id='uptime'>");
  page += String(millis() / 1000);
  page += F("</span> s</p>");
  page += F("</div>");

  page += F("<div class='card'><h2>Pinout</h2>");
  page += F("<p><code>SCK=GPIO20</code>, <code>MISO=GPIO10</code>, <code>MOSI=GPIO21</code>, ");
  page += F("<code>SD_CS=GPIO1</code>, <code>EPD_CS=GPIO8</code>, <code>DC=GPIO7</code>, ");
  page += F("<code>RESET=GPIO6</code>, <code>BUSY=GPIO5</code>, <code>buttons=GPIO0 ADC</code>.</p>");
  page += F("</div>");

  page += F("<div class='card'><h2>Log</h2><pre id='log'>");
  page += htmlEscape(eventLog);
  page += F("</pre></div>");

  page += F("<script>");
  page += F("async function poll(){try{let r=await fetch('/api/status',{cache:'no-store'});let j=await r.json();");
  page += F("document.getElementById('btnState').textContent=j.button;");
  page += F("document.getElementById('btnRaw').textContent=j.raw;");
  page += F("document.getElementById('btnMv').textContent=j.mv;");
  page += F("document.getElementById('sdStatus').textContent=j.sd;");
  page += F("document.getElementById('uptime').textContent=j.uptime;");
  page += F("}catch(e){}} setInterval(poll,1000);");
  page += F("</script>");

  page += F("</body></html>");
  return page;
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleRoot() {
  server.send(200, "text/html", makeHomePage());
}

void handleApiStatus() {
  ButtonReading br = readButtons();
  String json = "{";
  json += "\"button\":\"" + String(br.state) + "\",";
  json += "\"raw\":" + String(br.raw) + ",";
  json += "\"mv\":" + String(br.millivolts_est) + ",";
  json += "\"sd\":\"" + jsonEscape(lastSDStatus) + "\",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  server.send(200, "application/json", json);
}

void handleEPD() {
  drawDisplayTest();
  redirectHome();
}

void handleSD() {
  testSDCard();
  redirectHome();
}

void handleAll() {
  drawDisplayTest();
  testSDCard();

  ButtonReading br = readButtons();
  logf("Button ADC GPIO%d: raw=%d, approx=%lu mV, state=%s",
       BUTTON_ADC_PIN, br.raw, (unsigned long)br.millivolts_est, br.state);

  redirectHome();
}

void handleClearLog() {
  eventLog = "";
  addLogLine("Log cleared.");
  redirectHome();
}

void handleRestart() {
  server.send(200, "text/html", "<html><body><h1>Restarting...</h1></body></html>");
  delay(300);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void startAccessPointAndServer() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);

  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASS);
  if (!ap_ok) {
    addLogLine("WiFi.softAP() failed");
  } else {
    logf("WiFi AP started: SSID=%s password=%s", AP_SSID, AP_PASS);
    String apIpString = WiFi.softAPIP().toString();
    logf("Open http://%s/ in your browser", apIpString.c_str());
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/epd", HTTP_GET, handleEPD);
  server.on("/sd", HTTP_GET, handleSD);
  server.on("/all", HTTP_GET, handleAll);
  server.on("/clearlog", HTTP_GET, handleClearLog);
  server.on("/restart", HTTP_GET, handleRestart);
  server.onNotFound(handleNotFound);

  server.begin();
  addLogLine("HTTP server started.");
}

// -----------------------------------------------------------------------------
// Arduino setup / loop.
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  addLogLine("");
  addLogLine("Booted: ESP32-C3 Heltec V1.1 JD79656 + microSD + buttons WiFi AP test");

  // Deselect all SPI devices before starting the bus.
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // ADC setup for the button resistor ladder.
  pinMode(BUTTON_ADC_PIN, INPUT);
#ifdef ARDUINO_ARCH_ESP32
  analogReadResolution(12);
  analogSetPinAttenuation(BUTTON_ADC_PIN, ADC_11db);  // allows readings close to 3.3V
#endif

  // Arduino-ESP32 order is SCK, MISO, MOSI, SS.
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);

  display.begin();

  startAccessPointAndServer();

  printPinSummary();

  ButtonReading br = readButtons();
  logf("Button ADC GPIO%d: raw=%d, approx=%lu mV, state=%s",
       BUTTON_ADC_PIN, br.raw, (unsigned long)br.millivolts_est, br.state);

  // Draw one screen at boot so orientation and AP details are visible.
  drawDisplayTest();
}

void loop() {
  server.handleClient();
  delay(2);
}
