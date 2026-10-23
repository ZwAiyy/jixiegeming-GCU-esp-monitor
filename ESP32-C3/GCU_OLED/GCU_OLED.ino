/*
 * GCU OLED HUD — ESP32 无线系统监控屏
 * ------------------------------------------------------------------
 * 接收电脑上 GcuCast 转发出来的 UDP 报文，在 0.96" 128x64 OLED 上
 * 显示 CPU / GPU / 内存 / 风扇 / SSD / 网络 的实时数据。
 *
 * 依赖库（Arduino IDE → 库管理器，装过就不用再装）：
 *   Adafruit SSD1306
 *   Adafruit GFX Library
 *   Adafruit BusIO
 *
 * 开发板：ESP32C3 Dev Module（ESP32-C3 SuperMini）
 *         Flash Size: 4MB，USB CDC On Boot: Enabled
 *
 * 接线（OLED → 板子）：
 *   VCC → 3V3      GND → GND
 *   SDA → GPIO5    SCL → GPIO6
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==================== 需要你修改的地方 ====================
static const char *WIFI_SSID = "你的WiFi名称";
static const char *WIFI_PASS = "你的WiFi密码";

static const int PIN_SDA = 5;      // OLED SDA 接的 GPIO
static const int PIN_SCL = 6;      // OLED SCL 接的 GPIO
static const uint8_t OLED_ADDR = 0x3C;   // 少数模块是 0x3D

// 监听端口，要和 GcuCast 的 config.json 里 UdpPort 一致
static const uint16_t UDP_PORT = 8266;

// 屏幕装反了就改成 2（旋转 180°），改成 1 / 3 是转 90°
static const uint8_t SCREEN_ROTATION = 0;
// ========================================================

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiUDP udp;

// OLED 是否初始化成功。失败时绝不能调 display 的绘图 API
// （内部 buffer 未分配，clearDisplay 会踩空指针 → 板子反复重启）
bool oledOk = false;

// ---------------- 数据模型 ----------------
struct Metrics {
  float cpu, ct, gpu, gt;
  float mem, mt, mu;
  float f1, f2;
  float ssd;
  float bat, fe;
  char dn[16];
  char up[16];
  bool valid;
} m;

unsigned long lastPacketMs = 0;
unsigned long lastDrawMs = 0;
unsigned long lastWifiTryMs = 0;

void resetMetrics() {
  m.cpu = m.ct = m.gpu = m.gt = NAN;
  m.mem = m.mt = m.mu = NAN;
  m.f1 = m.f2 = NAN;
  m.ssd = NAN;
  m.bat = m.fe = NAN;
  strcpy(m.dn, "--");
  strcpy(m.up, "--");
  m.valid = false;
}

static float toF(const char *s) {
  if (!s || !*s || !strcmp(s, "null")) return NAN;
  return atof(s);
}

// "856 Kbps" → "856K"，"1.23 Mbps" → "1.2M"
static String shortRate(const char *s) {
  String t(s);
  if (t == "--" || t.length() == 0) return "--";
  t.replace(" Kbps", "K");
  t.replace(" Mbps", "M");
  t.replace(" KB/s", "K");
  t.replace(" MB/s", "M");
  t.trim();
  if (t.length() > 5) t = t.substring(0, 5);   // 留出 "xx/yy" 双栏宽度
  return t;
}

// ---------------- 解析 "GCU1|cpu=..|ct=.." ----------------
void parsePacket(char *buf) {
  if (strncmp(buf, "GCU1", 4) != 0) return;

  char *tok = strtok(buf + 4, "|");
  while (tok) {
    char *eq = strchr(tok, '=');
    if (eq) {
      *eq = '\0';
      const char *k = tok;
      const char *v = eq + 1;
      if (!strcmp(k, "cpu"))      m.cpu = toF(v);
      else if (!strcmp(k, "ct"))  m.ct  = toF(v);
      else if (!strcmp(k, "gpu")) m.gpu = toF(v);
      else if (!strcmp(k, "gt"))  m.gt  = toF(v);
      else if (!strcmp(k, "mem")) m.mem = toF(v);
      else if (!strcmp(k, "mt"))  m.mt  = toF(v);
      else if (!strcmp(k, "mu"))  m.mu  = toF(v);
      else if (!strcmp(k, "f1"))  m.f1  = toF(v);
      else if (!strcmp(k, "f2"))  m.f2  = toF(v);
      else if (!strcmp(k, "ssd")) m.ssd = toF(v);
      else if (!strcmp(k, "bat")) m.bat = toF(v);
      else if (!strcmp(k, "fe"))  m.fe  = toF(v);
      else if (!strcmp(k, "dn"))  { strncpy(m.dn, v, sizeof(m.dn) - 1); m.dn[sizeof(m.dn) - 1] = 0; }
      else if (!strcmp(k, "up"))  { strncpy(m.up, v, sizeof(m.up) - 1); m.up[sizeof(m.up) - 1] = 0; }
    }
    tok = strtok(NULL, "|");
  }

  m.valid = true;
  lastPacketMs = millis();
}

// ---------------- 绘制工具 ----------------
void drawBar(int x, int y, int w, int h, float pct) {
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  if (isnan(pct)) return;
  int fill = (int)((w - 2) * constrain(pct, 0.0f, 100.0f) / 100.0f + 0.5f);
  if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

// 把数字右对齐到 rightX
void numRight(int rightX, int baselineY, float v, const char *suffix, int decimals = 0) {
  char b[20];
  if (isnan(v)) {
    snprintf(b, sizeof(b), "--%s", suffix);
  } else if (decimals == 0) {
    snprintf(b, sizeof(b), "%.0f%s", v, suffix);
  } else {
    snprintf(b, sizeof(b), "%.1f%s", v, suffix);
  }
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(b, 0, baselineY, &x1, &y1, &w, &h);
  display.setCursor(rightX - (int)w, baselineY);
  display.print(b);
}

void splash(const char *l1, const char *l2 = nullptr) {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("GCU OLED HUD");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  display.setCursor(0, 24);
  display.print(l1 ? l1 : "");
  if (l2) {
    display.setCursor(0, 38);
    display.print(l2);
  }
  display.display();
}

// ---------------- 主界面 ----------------
void drawMain() {
  if (!oledOk) return;
  bool live = m.valid && (millis() - lastPacketMs < 3000);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 标题行
  display.setCursor(0, 0);
  display.print("GCU MONITOR");
  if (live) {
    display.fillRect(94, 1, 5, 5, SSD1306_WHITE);
    display.setCursor(102, 0);
    display.print("LIVE");
  } else if (m.valid) {
    display.drawRect(94, 1, 5, 5, SSD1306_WHITE);
    display.setCursor(102, 0);
    display.print("LATE");
  } else {
    display.setCursor(90, 0);
    display.print("NO SIG");
  }
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  if (!m.valid) {
    display.setCursor(28, 26);
    display.print("waiting for");
    display.setCursor(20, 38);
    display.print("GcuCast UDP...");
    display.setCursor(0, 54);
    display.print(WiFi.localIP().toString().c_str());
    display.display();
    return;
  }

  // CPU
  display.setCursor(0, 12);
  display.print("CPU");
  drawBar(26, 12, 40, 7, m.cpu);
  numRight(94, 12, m.cpu, "%");
  numRight(127, 12, m.ct, "C");

  // GPU
  display.setCursor(0, 22);
  display.print("GPU");
  drawBar(26, 22, 40, 7, m.gpu);
  numRight(94, 22, m.gpu, "%");
  numRight(127, 22, m.gt, "C");

  // MEM
  display.setCursor(0, 32);
  display.print("MEM");
  drawBar(26, 32, 40, 7, m.mem);
  numRight(94, 32, m.mem, "%");
  numRight(127, 32, m.mu, "G", 1);

  // FAN
  display.setCursor(0, 42);
  display.print("FAN");
  {
    char b[24];
    if (isnan(m.f1) && isnan(m.f2)) {
      snprintf(b, sizeof(b), "-- / --");
    } else {
      snprintf(b, sizeof(b), "%.0f/%.0f",
               isnan(m.f1) ? 0.0f : m.f1, isnan(m.f2) ? 0.0f : m.f2);
    }
    display.setCursor(26, 42);
    display.print(b);
  }
  if (!isnan(m.fe) && m.fe > 0) {
    display.setCursor(104, 42);
    display.print("ERR");
  }

  // SSD + 网络
  display.setCursor(0, 52);
  display.print("SSD");
  numRight(40, 52, m.ssd, "C");
  {
    String net = shortRate(m.dn) + "/" + shortRate(m.up);
    display.setCursor(52, 52);
    display.print(net);
  }

  display.display();
}

// ---------------- WiFi ----------------
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiTryMs < 5000) return;
  lastWifiTryMs = now;
  Serial.println("[wifi] reconnecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // 先按指定引脚初始化 I2C，再交给 SSD1306
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oledOk = false;
    Serial.println("[oled] init failed: check wiring / 0x3C or 0x3D");
  } else {
    oledOk = true;
    display.setRotation(SCREEN_ROTATION);
    display.setTextWrap(false);
  }

  resetMetrics();
  splash("WiFi connecting...", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // 关省电，降低丢包
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(400);
    char b[24];
    snprintf(b, sizeof(b), "%lus...", (millis() - t0) / 1000);
    splash("WiFi connecting...", b);
  }

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.println("[wifi] ok " + ip);
    splash("WiFi OK", ip.c_str());
    delay(1200);
  } else {
    splash("WiFi FAILED", "check SSID / PASS");
    delay(2500);
  }

  udp.begin(UDP_PORT);
  Serial.printf("[udp] listening on %u\n", UDP_PORT);
}

void loop() {
  ensureWifi();

  int sz = udp.parsePacket();
  if (sz > 0) {
    char buf[512];
    int n = udp.read(buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      parsePacket(buf);
    }
  }

  unsigned long now = millis();
  if (now - lastDrawMs >= 250) {   // 4 帧/秒，足够顺滑，I2C 也不吃满
    lastDrawMs = now;
    drawMain();
  }

  delay(5);
}
