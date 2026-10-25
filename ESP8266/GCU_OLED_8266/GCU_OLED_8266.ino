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
static const char *WIFI_SSID = "NUN";
static const char *WIFI_PASS = "";   // 开放网络，无密码

// ---------------- 校园网 Portal 认证 ----------------
// NUN 是认证后才解除设备隔离的，且认证跟着设备走（手机认证了不代表板子认证了），
// 所以板子连上 WiFi 后要自己发一次登录请求，之后定时续签防止会话过期。
static const char *PORTAL_URL = "http://172.16.230.50/api/account/login";
static const char *PORTAL_BODY =
    "username=20267620&password=xzsywsbczyy8&nasId=2&isp=local&timeLimit=";
static const unsigned long PORTAL_REAUTH_MS = 10UL * 60 * 1000;  // 10 分钟续一次

// ---------------- IP 心跳信令（可选） ----------------
// 内网服务器跑 heartbeat_server.py 后，把地址填到这里；
// 板子每 30 秒上报一次，电脑端 GcuCast 自动取到板子 IP，换 IP 免改配置。
// 留空 "" 关闭该功能。
static const char *SIGNAL_URL = "http://172.16.50.83:8300/heartbeat";
static const char *SIGNAL_TOKEN = "gcu-hud-2026";
static const unsigned long SIGNAL_PERIOD_MS = 30UL * 1000;

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

// 两个翻页按键（外接轻触开关）：一端接引脚、另一端接 GND，内部上拉，按下为低电平。
//   按键1 D5 (GPIO14)：短按 = 下一页；长按 1.5 秒 = 立即 Portal 重认证
//   按键2 D6 (GPIO12)：短按 = 上一页；长按 1.5 秒 = 熄屏 / 亮屏
// 都可换 D7(GPIO13)，填 -1 关闭对应按键
static const int PAGE_BTN_PIN  = 14;   // 按键1（下一页 / 长按重认证）
static const int PAGE_BTN2_PIN = 12;   // 按键2（上一页 / 长按熄屏）
// ========================================================

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiUDP udp;

// struct 要定义在任何函数之前：Arduino 编译前会自动在文件开头生成函数原型，
// 若 struct 定义在后面，原型里的类型名会找不到（编译报 'Btn' was not declared）
struct Btn {
  int pin;
  bool raw, deb;
  unsigned long tChange, tDown;
  bool longFired;
};

Btn btn1;   // setup 里再赋值（PAGE_BTN_PIN 等常量在前面）
Btn btn2;

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
  float dnK, upK;                 // 网速数值（Kbps），NET 页画曲线用
  bool valid;
} m;

unsigned long lastPacketMs = 0;
unsigned long lastDrawMs = 0;
unsigned long lastWifiTryMs = 0;
unsigned long lastLedMs = 0;
unsigned long lastPortalMs = 0;
unsigned long lastSignalMs = 0;
bool ledOn = false;
bool wasWifiOk = false;
bool screenOn = true;
uint8_t curPage = 0;   // 0 主页 / 1 详情 / 2 NET 曲线

// ---------------- 下行速率历史（NET 页曲线） ----------------
// 每 4 秒采一个点，64 个点 = 最近 4 分 16 秒；量程自动缩放到本次峰值
static const uint8_t HIST_N = 64;
static const unsigned long HIST_PERIOD_MS = 4000;
uint8_t dnHist[HIST_N];
unsigned long lastHistMs = 0;
float histPeakK = 0;            // 开机以来下行峰值（Kbps）

void sampleHistory() {
  unsigned long now = millis();
  if (now - lastHistMs < HIST_PERIOD_MS) return;
  lastHistMs = now;
  float v = isnan(m.dnK) ? 0.0f : (m.dnK > 0 ? m.dnK : 0.0f);
  if (v > histPeakK) histPeakK = v;
  float scale = histPeakK > 50.0f ? histPeakK : 50.0f;   // 最低量程 50K，避免全零时除小数
  memmove(dnHist, dnHist + 1, HIST_N - 1);
  dnHist[HIST_N - 1] = (uint8_t)constrain(v * 255.0f / scale, 0.0f, 255.0f);
}

// ---------------- 校园网 Portal 认证 ----------------
bool portalLogin() {
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

// ---------------- IP 心跳信令上报 ----------------
void signalKeepAlive() {
  if (SIGNAL_URL[0] == '\0' || WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastSignalMs < SIGNAL_PERIOD_MS) return;
  lastSignalMs = now;
  WiFiClient wc;
  HTTPClient http;
  String url = String(SIGNAL_URL) + "?token=" + SIGNAL_TOKEN;
  if (!http.begin(wc, url)) return;
  int code = http.POST("");          // 服务器记录来源 IP，报文内容无所谓
  http.end();
  Serial.printf("[signal] HTTP %d\n", code);
}

void resetMetrics() {
  m.cpu = m.ct = m.gpu = m.gt = NAN;
  m.mem = m.mt = m.mu = NAN;
  m.f1 = m.f2 = NAN;
  m.ssd = NAN;
  m.bat = m.fe = NAN;
  strcpy(m.dn, "--");
  strcpy(m.up, "--");
  m.dnK = m.upK = NAN;
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

// "856 Kbps" → 856.0；"1.23 Mbps" → 1230.0；无效 → NAN（单位 Kbps）
static float parseRateK(const char *s) {
  String t(s);
  if (t == "--" || t.length() == 0) return NAN;
  bool mb = t.indexOf("Mbps") >= 0 || t.indexOf("MB/s") >= 0;
  float f = t.toFloat();
  return mb ? f * 1000.0f : f;
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
      else if (!strcmp(k, "dn"))  { strncpy(m.dn, v, sizeof(m.dn) - 1); m.dn[sizeof(m.dn) - 1] = 0; m.dnK = parseRateK(v); }
      else if (!strcmp(k, "up"))  { strncpy(m.up, v, sizeof(m.up) - 1); m.up[sizeof(m.up) - 1] = 0; m.upK = parseRateK(v); }
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

// ---------------- 页面 2：NET 速率曲线 ----------------
void drawNet() {
  if (!oledOk) return;
  bool live = m.valid && (millis() - lastPacketMs < 3000);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("NET");
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

  // 曲线区：y 14..48，64 个点连成折线
  const int gy = 14, gh = 34;
  for (int i = 1; i < HIST_N; i++) {
    int x0 = (i - 1) * 127 / (HIST_N - 1);
    int x1 = i * 127 / (HIST_N - 1);
    int y0 = gy + gh - 1 - (int)((uint32_t)dnHist[i - 1] * (gh - 2) / 255);
    int y1 = gy + gh - 1 - (int)((uint32_t)dnHist[i] * (gh - 2) / 255);
    display.drawLine(x0, y0, x1, y1, SSD1306_WHITE);
  }
  display.drawFastHLine(0, gy + gh, 128, SSD1306_WHITE);   // 底线

  // 底行：当前下行速率（左） + 本次峰值（右）
  display.setCursor(0, 52);
  display.print("DN ");
  display.print(m.dn);
  char b[16];
  if (histPeakK > 0) snprintf(b, sizeof(b), "pk %.1fM", histPeakK / 1000.0f);
  else               snprintf(b, sizeof(b), "pk --");
  int16_t x1b, y1b; uint16_t wb, hb;
  display.getTextBounds(b, 0, 52, &x1b, &y1b, &wb, &hb);
  display.setCursor(127 - (int)wb, 52);
  display.print(b);

  display.display();
}

// ---------------- 按键（两键 + 长按） ----------------
// 按键1 D5：短按下一页；长按 1.5s = Portal 立即重认证（掉线救急）
// 按键2 D6：短按上一页；长按 1.5s = 熄屏 / 亮屏
// 返回：0 无事件，1 短按（松开时触发），2 长按（按住 1.5s 触发一次）
uint8_t btnUpdate(Btn &b) {
  if (b.pin < 0) return 0;
  bool raw = (digitalRead(b.pin) == LOW);
  unsigned long now = millis();
  if (raw != b.raw) { b.raw = raw; b.tChange = now; }
  if (raw != b.deb && now - b.tChange > 50) {   // 50ms 消抖
    b.deb = raw;
    if (raw) {
      b.tDown = now;
      b.longFired = false;
    } else if (!b.longFired) {
      return 1;                                  // 短按：松开时触发
    }
  }
  if (b.deb && !b.longFired && now - b.tDown > 1500) {
    b.longFired = true;                          // 长按：按住不放立即触发
    return 2;
  }
  return 0;
}

void updateButtons() {
  btn1.pin = PAGE_BTN_PIN;
  btn2.pin = PAGE_BTN2_PIN;
  uint8_t e = btnUpdate(btn1);
  if (e == 1) {
    curPage = (curPage + 1) % 3;
    lastDrawMs = 0;
    Serial.printf("[ui] page %u\n", curPage);
  } else if (e == 2) {
    Serial.println("[ui] portal re-auth");
    splash("Portal re-auth...", "please wait");
    portalLogin();
    lastDrawMs = 0;
  }

  e = btnUpdate(btn2);
  if (e == 1) {
    curPage = (curPage + 2) % 3;
    lastDrawMs = 0;
    Serial.printf("[ui] page %u\n", curPage);
  } else if (e == 2 && oledOk) {
    screenOn = !screenOn;
    if (screenOn) { display.ssd1306_command(SSD1306_DISPLAYON); lastDrawMs = 0; }
    else            display.ssd1306_command(SSD1306_DISPLAYOFF);
    Serial.printf("[ui] screen %s\n", screenOn ? "on" : "off");
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

  if (PAGE_BTN_PIN >= 0)  pinMode(PAGE_BTN_PIN, INPUT_PULLUP);
  if (PAGE_BTN2_PIN >= 0) pinMode(PAGE_BTN2_PIN, INPUT_PULLUP);

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
    splash("Portal auth...", "please wait");
    delay(600);
    portalLogin();                 // 连上就先认证一次，解除设备隔离
    lastPortalMs = millis();
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
  signalKeepAlive();

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
  updateButtons();
  sampleHistory();
  if (screenOn && now - lastDrawMs >= 500) {   // 2 帧/秒：全量刷 1KB 约 25ms，太快会有肉眼可见的闪动
    lastDrawMs = now;
    if (curPage == 0) drawMain();
    else if (curPage == 1) drawDetail();
    else drawNet();
  }

  updateLed();
  delay(5);
}
