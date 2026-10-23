/*
 * GCU OLED HUD — ESP8266 版（NodeMCU / Wemos D1 mini / ESP-12F / ESP-01）
 * ------------------------------------------------------------------
 * 和 ESP32 版完全同一个协议：接收电脑上 GcuCast 转发出来的 UDP 报文，
 * 在 0.96" 128x64 OLED 上显示 CPU / GPU / 内存 / 风扇 / SSD / 网络。
 * 电脑端不用改任何配置，只是换了块板子。
 *
 * 依赖库（Arduino IDE → 库管理器）：
 *   Adafruit SSD1306 / Adafruit GFX Library / Adafruit BusIO
 *
 * 开发板选择：
 *   NodeMCU 1.0 (ESP-12E Module)   ← 带 USB 口的开发板（最常见）
 *   Generic ESP8266 Module         ← 裸 ESP-12F / ESP-01 模块
 *
 * 接线（OLED → 板子，NodeMCU 丝印为准）：
 *   VCC → 3V3      GND → GND
 *   SDA → D2 (GPIO4)      SCL → D1 (GPIO5)
 *   ESP-01 只能接 GPIO0/GPIO2，见「串口下载说明.txt」
 *
 * 注意：ESP8266 只有约 40KB 可用堆，比 C3 紧张，但这份固件实测够用。
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==================== 需要你修改的地方 ====================
// 注意：ESP8266 只支持 2.4G 频段，5G 的 WiFi 连不上
static const char *WIFI_SSID = "YourWiFi";
static const char *WIFI_PASS = "YourPassword";   // 开放网络就留空 ""

// ---------------- 校园网 Portal 认证（可选） ----------------
// 有些校园网 / 公司网要 Portal 认证后才解除设备隔离，且认证跟着设备走：
// 板子连上 WiFi 后会自己 POST 一次登录请求，之后每 10 分钟续签。
// 不需要这个功能就把 PORTAL_URL 改成 ""（两个引号中间什么都不写）。
static const char *PORTAL_URL = "";   // 例如 "http://172.16.0.1/api/account/login"
static const char *PORTAL_BODY = "";  // 例如 "username=学号&password=密码&nasId=2&isp=local&timeLimit="
static const unsigned long PORTAL_REAUTH_MS = 10UL * 60 * 1000;  // 10 分钟续一次

static const int PIN_SDA = 4;      // NodeMCU 的 D2。裸模块按你的接法改
static const int PIN_SCL = 5;      // NodeMCU 的 D1。ESP-01 改成 0 和 2
static const uint8_t OLED_ADDR = 0x3C;   // 少数模块是 0x3D

// 监听端口，要和 GcuCast 的 config.json 里 UdpPort 一致
static const uint16_t UDP_PORT = 8266;

// 屏幕装反了就改成 2（旋转 180°），改成 1 / 3 是转 90°
static const uint8_t SCREEN_ROTATION = 0;

// 板载 LED 做状态指示：-1 关闭。
// NodeMCU / ESP-12F 的 LED 在 GPIO2 且低电平点亮；ESP-01 是 GPIO1（串口TX）
static const int STATUS_LED_PIN = 2;
static const bool STATUS_LED_ACTIVE_LOW = true;

// 翻页按键（外接轻触开关）：一端接这个引脚、另一端接 GND，内部上拉，按下为低电平。
// 默认 D5 (GPIO14)，也可换 D6(GPIO12) / D7(GPIO13)。-1 关闭该功能
static const int PAGE_BTN_PIN = 14;
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
unsigned long lastLedMs = 0;
unsigned long lastPortalMs = 0;
bool ledOn = false;
bool wasWifiOk = false;
uint8_t curPage = 0;   // 0 = 主页面，1 = DETAIL 详情页

// ---------------- 校园网 Portal 认证 ----------------
bool portalLogin() {
  if (PORTAL_URL[0] == '\0') return true;   // 没配 Portal 就直接当成功
  WiFiClient wc;
  HTTPClient http;
  if (!http.begin(wc, PORTAL_URL)) {
    Serial.println("[portal] begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
  http.addHeader("X-Requested-With", "XMLHttpRequest");
  http.addHeader("Referer", "http://172.16.230.50/tpl/default/login_account.html");
  int code = http.POST(PORTAL_BODY);
  String resp = http.getString();
  http.end();
  resp.trim();
  if (resp.length() > 96) resp = resp.substring(0, 96);
  Serial.printf("[portal] HTTP %d %s\n", code, resp.c_str());
  return code == 200;
}

void portalKeepAlive() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (!wifiOk) {
    wasWifiOk = false;
    return;
  }
  // 刚连上（或重连后）立即认证；之后每 10 分钟续签
  if (!wasWifiOk || millis() - lastPortalMs > PORTAL_REAUTH_MS) {
    portalLogin();
    lastPortalMs = millis();
  }
  wasWifiOk = true;
}

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

// ---------------- 翻页按键 ----------------
void updatePageButton() {
  if (PAGE_BTN_PIN < 0) return;
  static bool lastRaw = false;
  static bool debounced = false;
  static unsigned long tChange = 0;
  bool raw = (digitalRead(PAGE_BTN_PIN) == LOW);
  unsigned long now = millis();
  if (raw != lastRaw) {
    lastRaw = raw;
    tChange = now;
  }
  if (raw != debounced && now - tChange > 50) {   // 50ms 消抖
    debounced = raw;
    if (debounced) {          // 按下的那一刻翻页
      curPage = (curPage + 1) % 2;
      lastDrawMs = 0;         // 让主循环立刻重绘
      Serial.printf("[ui] page %u\n", curPage);
    }
  }
}

// ---------------- 页面 1：DETAIL 详情页 ----------------
void drawDetail() {
  if (!oledOk) return;
  bool live = m.valid && (millis() - lastPacketMs < 3000);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("DETAIL");
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

  char b[32];
  if (!m.valid) {
    display.setCursor(0, 20);
    display.print("waiting for GcuCast...");
    display.setCursor(0, 34);
    display.print(WiFi.localIP().toString().c_str());
    snprintf(b, sizeof(b), "%d dBm  heap %uK", WiFi.RSSI(), ESP.getFreeHeap() / 1024);
    display.setCursor(0, 46);
    display.print(b);
    display.display();
    return;
  }

  // 内存：已用 / 总量
  display.setCursor(0, 14);
  display.print("MEM");
  if (isnan(m.mu) || isnan(m.mt)) snprintf(b, sizeof(b), "-- / -- G");
  else snprintf(b, sizeof(b), "%.1f / %.1f G", m.mu, m.mt);
  display.setCursor(30, 14);
  display.print(b);

  // 网速全称（主页面是缩写，这里给完整的）
  display.setCursor(0, 26);
  display.print("DN");
  display.setCursor(30, 26);
  display.print(m.dn);
  display.setCursor(0, 36);
  display.print("UP");
  display.setCursor(30, 36);
  display.print(m.up);

  // SSD 温度 / 电池
  display.setCursor(0, 46);
  if (isnan(m.ssd)) display.print("SSD --");
  else { snprintf(b, sizeof(b), "SSD %.0fC", m.ssd); display.print(b); }
  if (!isnan(m.bat) && m.bat > 0) {
    snprintf(b, sizeof(b), "BAT %.0f%%", m.bat);
    display.setCursor(60, 46);
    display.print(b);
  }

  // WiFi 信号 / 剩余堆内存
  snprintf(b, sizeof(b), "%d dBm  heap %uK", WiFi.RSSI(), ESP.getFreeHeap() / 1024);
  display.setCursor(0, 56);
  display.print(b);

  display.display();
}

// ---------------- 状态灯 ----------------
// 常灭=有数据；慢闪(1s)=WiFi 通但没数据；快闪(200ms)=WiFi 没连上
void updateLed() {
  if (STATUS_LED_PIN < 0) return;
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool live = m.valid && (millis() - lastPacketMs < 3000);

  unsigned long period = live ? 0 : (wifiOk ? 1000 : 200);
  unsigned long now = millis();

  bool want;
  if (live) {
    want = false;
  } else if (now - lastLedMs >= period) {
    lastLedMs = now;
    ledOn = !ledOn;
    want = ledOn;
  } else {
    want = ledOn;
  }

  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW ? !want : want);
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
  Serial.println("\n[boot] GCU OLED HUD (ESP8266)");

  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);
  }

  if (PAGE_BTN_PIN >= 0) pinMode(PAGE_BTN_PIN, INPUT_PULLUP);

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

  WiFi.persistent(false);        // 别把凭据反复写 flash
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // 关省电，降低 UDP 丢包
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
    if (PORTAL_URL[0] != '\0') {
      splash("Portal auth...", "please wait");
      delay(600);
      portalLogin();                 // 连上就先认证一次，解除设备隔离
      lastPortalMs = millis();
    }
    wasWifiOk = true;
    splash("WiFi OK", ip.c_str());
    delay(1200);
  } else {
    splash("WiFi FAILED", "check SSID / PASS");
    delay(2500);
  }

  udp.begin(UDP_PORT);
  Serial.printf("[udp] listening on %u, free heap %u\n", UDP_PORT, ESP.getFreeHeap());
}

void loop() {
  ensureWifi();
  portalKeepAlive();

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
  updatePageButton();
  if (now - lastDrawMs >= 250) {   // 4 帧/秒。8266 上 I2C 全量刷 1KB 约 25ms，别调太快
    lastDrawMs = now;
    if (curPage == 0) drawMain();
    else drawDetail();
  }

  updateLed();
  delay(5);
}
