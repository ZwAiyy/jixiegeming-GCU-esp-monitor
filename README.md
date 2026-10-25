# GCU ESP Monitor — 无线硬件监控小屏

用一块 ESP32 / ESP8266 开发板 + 0.96 寸 OLED，把电脑的 **CPU / GPU / 内存 / 风扇 / SSD 温度 / 网速** 实时显示在桌面上，无线、免驱动、插电即用。

数据来自本机的 GCU 监控服务（MQTT），由电脑上的小转发器转成 UDP，开发板只管收包画屏——端侧不跑 MQTT、不解析 JSON，代码短、内存省、断线不卡。

```
  GCU 主程序（本机 MQTT 127.0.0.1:13688，仅回环可连）
        │
        ▼
  GcuCast（电脑端转发器，本项目 / GcuCast 目录）
        │  UDP 广播/单播，每秒一包，约 105 字节
        ▼
  ESP32-C3 / ESP8266 + SSD1306 OLED（本项目 / ESP32-C3、/ ESP8266 目录）
```

## 仓库结构

```
GcuCast/            电脑端转发器源码（.NET 10 控制台程序）
  Shared/           MQTT 客户端与遥测解析（纯手写 MQTT 3.1.1，无第三方依赖）
  config.example.json   配置模板
signaling/          可选：IP 心跳信令服务（部署在内网任意 Linux 上，
                    解决电脑和板子 IP 都是 DHCP 动态分配、广播又被
                    路由器挡住时互相找不到的问题。零依赖，Python 3 标准库）
ESP32-C3/
  GCU_OLED/         ESP32-C3 SuperMini 固件（Arduino）
  使用说明.txt       接线 / 烧录 / 排障（写得比较全，推荐先看）
ESP8266/
  GCU_OLED_8266/    ESP8266 固件（Arduino），比 C3 版多几样：
                    外接双键翻页 + 长按功能（重认证/熄屏）、NET 速率曲线页、
                    校园网 Portal 自动认证、WiFi 信号/堆内存显示
  烧录.bat           esptool 命令行一键烧录
  串口下载说明.txt   裸模块串口下载注意事项
```

## 硬件准备

- ESP32-C3 SuperMini **或** ESP8266（NodeMCU / Wemos D1 mini / ESP-12F；ESP-01 也能跑，用 1MB 布局）
- 0.96 寸 128x64 I2C OLED（SSD1306，4 针，地址一般 0x3C）
- 杜邦线 4 根
- ESP8266 方案再备两个轻触开关（翻页 / 长按功能，一端接引脚、一端接 GND）

### 接线

| OLED | ESP32-C3 SuperMini | ESP8266 NodeMCU |
|------|--------------------|-----------------|
| VCC  | 3V3                | 3V3             |
| GND  | GND                | GND             |
| SDA  | GPIO5              | D2 (GPIO4)      |
| SCL  | GPIO6              | D1 (GPIO5)      |

> OLED 必须接 3V3，不要接 5V。

## 快速开始

### 1. 固件

1. Arduino IDE 打开对应 `.ino`（装好 Adafruit SSD1306 / GFX / BusIO 库）
2. 改文件开头的 `WIFI_SSID / WIFI_PASS`（ESP8266 版如需校园网 Portal 认证，再填 `PORTAL_URL / PORTAL_BODY`，不需要就留空 `""`）
3. 开发板选择：
   - ESP32-C3：`ESP32C3 Dev Module`（Flash 4MB，**USB CDC On Boot: Enabled**）
   - ESP8266：`NodeMCU 1.0`；ESP-01 选 `Generic ESP8266 Module`，Flash Size 1MB
4. 上传。ESP8266 裸模块进下载模式：GPIO0 接地 + 复位

### 2. 电脑端转发器 GcuCast

需要 .NET 10 SDK 编译（也是 GCU 悬浮窗同款运行时）：

```bash
cd GcuCast
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true
```

首次运行会在程序目录生成 `config.json`（可从 `config.example.json` 复制改）：

```json
{
  "ClientId": "OcScanner",         // 本机 MQTT 白名单身份，被占用时换 "OcTool"
  "UdpPort": 8266,                 // 与固件里的 UDP_PORT 一致
  "UseBroadcast": true,            // 广播：板子不用知道电脑 IP
  "BroadcastAddress": "255.255.255.255,192.168.1.255",  // 逗号分隔可多个，第二个填自己网段的定向广播
  "Targets": [],                   // 广播不通时改单播，填板子 IP
  "IntervalMs": 1000,
  "SignalingUrl": "",              // 心跳信令服务地址（可选），如 http://172.16.0.100:8300/ip
  "SignalingToken": "gcu-hud-2026" // 信令口令，与服务器端一致
}
```

广播和单播是**同时发**的：广播通了的话，板子换 IP 也不用改任何配置。

### IP 心跳信令（可选，推荐内网有 Linux 服务器时启用）

校园网/公司网常见困境：电脑和板子的 IP 都是 DHCP 动态分配，广播又被三层路由挡住——
两边互相找不到，换 IP 就得手动改配置。解决方案是找台内网 Linux（树莓派、NAS、实验室服务器都行）
跑 `signaling/heartbeat_server.py`（零依赖）当"留言板"：

```
8266 ──每30秒 POST 心跳（服务器记录来源 IP）──→ Linux 服务器 ←──每5秒 GET 板子IP── GcuCast
```

1. 服务器上：`python3 heartbeat_server.py`（或用 systemd 常驻，见 signaling/ 目录）
2. 固件里 `SIGNAL_URL` 填 `http://服务器IP:8300/heartbeat`
3. GcuCast 的 config.json 加 `"SignalingUrl": "http://服务器IP:8300/ip"`
4. 以后板子换 IP，GcuCast 自动跟上，日志会打 `[信令] 目标更新: x.x.x.x`

监控数据仍然走局域网 UDP 直连，服务器只传 IP，挂了也不影响原链路（回退手动改配置）。

### 3. 开机自启（可选）

`Win+R` → `shell:startup` → 把 `静默启动.vbs` 的**快捷方式**放进去（用快捷方式，不要复制文件本身，否则它找不到 exe）。

## 屏幕显示

**ESP8266 版四个页面，两个按键导航**：
- 页面 0 主页：CPU / GPU / MEM 条形图 + 温度，FAN 双风扇转速，SSD 温度 + 网速缩写
- 页面 1 详情：内存 已用/总量、上下行网速完整值、SSD 温度、电池、WiFi 信号强度（dBm）、剩余堆内存
- 页面 2 **NET 曲线**：下行速率实时走势图（最近 4 分 16 秒，量程自动缩放）+ 当前值 + 峰值

按键（一端接引脚、一端接 GND，无需电阻）：

| 按键 | 引脚 | 短按 | 长按 1.5 秒 |
|---|---|---|---|
| 按键 1 | D5 (GPIO14) | 下一页 | 立即 Portal 重认证（掉线救急） |
| 按键 2 | D6 (GPIO12) | 上一页 | 熄屏 / 亮屏 |

右上角 `■LIVE` 收包正常 / `□LATE` 超过 3 秒没包 / `NO SIG` 从没收到。

LED 状态（ESP8266 板载灯）：常灭 = 有数据；1s 慢闪 = WiFi 通但没数据；0.2s 快闪 = WiFi 没连上。

## 常见问题

**收不到数据，屏幕一直 waiting**
1. 先看转发器窗口有没有"已连接 GCUBridge"（没有 = GCU 主程序没开，或 ClientId 被占用，换 `OcTool` 试试）
2. 手机和电脑互相 ping 不通？那是 **WiFi AP 隔离**（校园网/公司网/访客网络常见），解决办法：
   - 手机/电脑开热点，让板子连热点
   - 校园网若需 Portal 认证才解除隔离，ESP8266 版内置了自动认证（填 `PORTAL_URL/PORTAL_BODY`，认证是跟着设备走的，板子要自己认证一次）
3. 广播被路由器挡了？把板子 IP 填进 `Targets` 单播

**GPU 数据不动**：GCU 在独显完全空闲时可能不推送 GpuInfo，屏上保持最后一次的值，不是卡了。

**ESP8266 烧录失败 / 反复重启**：九成是 USB-TTL 供电不够（8266 发射要 300~400mA），换供电能力强的口；详见 `ESP8266/串口下载说明.txt`。

更多排障步骤看 `ESP32-C3/使用说明.txt` 第七节，按顺序查基本都能定位。

## 协议（想自己写接收端的话）

UDP，每秒一包，一行 UTF-8 文本，`|` 分隔的 `key=value`：

```
GCU1|cpu=19|ct=62|gpu=10|gt=52|mem=54|mt=15.7|mu=8.5|f1=2722|f2=2650|ssd=47|dn=373 Kbps|up=138 Kbps|bat=100|fe=0|age=0
```

| key | 含义 | key | 含义 |
|-----|------|-----|------|
| cpu | CPU 占用 % | ct | CPU 温度 ℃ |
| gpu | GPU 占用 % | gt | GPU 温度 ℃ |
| mem | 内存占用 % | mt / mu | 总内存 / 已用 GB |
| f1 / f2 | CPU / GPU 风扇 RPM | ssd | SSD 温度 ℃ |
| dn / up | 下载 / 上传速率 | bat | 电池 % |
| fe | 风扇错误码 | age | 数据年龄（秒，-1 = 无数据） |

缺数据的 key 整个不发，接收端保留上一次的值。
