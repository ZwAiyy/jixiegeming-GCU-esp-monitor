using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using GcuHud;

namespace GcuCast;

/// <summary>
/// 把 GCU 的本地 MQTT 遥测转成 UDP 广播，供 ESP32 等局域网设备直接显示。
///
/// 为什么需要它：GCUBridge（127.0.0.1:13688）只接受来自回环地址的连接，
/// 局域网设备直连会被 CONNACK 拒绝（返回码 2），所以必须由本机代收再转发。
///
/// 报文格式（一行 UTF-8，约 150~200 字节，'|' 分隔的 key=value）：
///   GCU1|cpu=18.5|ct=61|gpu=3|gt=52|mem=48|mt=15.7|mu=7.5|f1=2921|f2=2906|ssd=45|dn=1.2K|up=0.3K|bat=85|fe=0|age=0
/// </summary>
internal static class Program
{
    static CastConfig _cfg = new();
    static readonly Telemetry Data = new();
    static UdpClient _udp = null!;
    static GcuMqtt? _mqtt;
    static readonly object LogGate = new();
    static readonly Dictionary<string, int> TopicCount = new();
    static volatile bool _stop;

    static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        _cfg = CastConfig.Load(args);

        Log($"GCU → UDP 转发器启动");
        Log($"  MQTT  : {_cfg.MqttHost}:{_cfg.MqttPort}  (clientId={_cfg.ClientId})");
        Log($"  UDP   : 端口 {_cfg.UdpPort}  周期 {_cfg.IntervalMs}ms");

        // 列出本机地址，方便填进 ESP32 固件
        var locals = LocalAddresses();
        if (locals.Count > 0)
        {
            Log($"  本机地址（固件里可填这些，或让它自动发现）:");
            foreach (var ip in locals) Log($"            {ip}");
        }

        _udp = new UdpClient { EnableBroadcast = true };

        var targets = _cfg.ResolveTargets();
        if (targets.Count == 0 && string.IsNullOrWhiteSpace(_cfg.SignalingUrl))
        {
            Log("  错误：没有可用的发送目标，请检查 config.json");
            return 1;
        }
        if (targets.Count > 0)
            Log($"  发送到 : {string.Join(", ", targets.Select(t => t.ToString()))}");
        if (!string.IsNullOrWhiteSpace(_cfg.SignalingUrl))
        {
            Signaling.Start(_cfg.SignalingUrl, _cfg.SignalingToken);
            Log($"  信令  : {_cfg.SignalingUrl}（板子 IP 自动获取，动态追加）");
        }
        Log("");

        _mqtt = new GcuMqtt(_cfg.ClientId, _cfg.ClientId + "User", PasswordFor(_cfg.ClientId))
        {
            // 关键：退出时绝不能发 System_OFF，那是全局开关，
            // 会把悬浮窗等其它消费者的数据一起关掉。
            SendOffOnDispose = false,
        };
        _mqtt.MessageReceived += (topic, payload) =>
        {
            Data.Apply(topic, payload);
            lock (TopicCount)
            {
                TopicCount.TryGetValue(topic, out int n);
                TopicCount[topic] = n + 1;
            }
            if (_cfg.Verbose)
                Log($"  ← {topic}  {(payload.Length > 90 ? payload[..90] + "…" : payload)}");
        };
        _mqtt.ConnectionChanged += up => Log(up ? "已连接 GCUBridge" : "与 GCUBridge 断开，重连中…");
        _mqtt.Start();
        ProcessSampler.Start();
        Log("  进程占用采样：CPU/内存 Top5（任务管理器同口径），每 2 秒");

        Console.CancelKeyPress += (_, e) => { e.Cancel = true; _stop = true; };

        int tick = 0;
        long sent = 0;
        DateTime lastBeat = DateTime.UtcNow;

        string? lastSeenBoard = null;

        while (!_stop)
        {
            Thread.Sleep(Math.Clamp(_cfg.IntervalMs, 200, 10000));

            string line = BuildPayload();
            byte[] bytes = Encoding.UTF8.GetBytes(line);

            // 静态目标 + 信令发现的板子 IP（动态）
            var eps = _cfg.ResolveTargets();
            var board = Signaling.BoardIp;
            if (board != lastSeenBoard)
            {
                lastSeenBoard = board;
                if (board != null) Log($"  [信令] 目标更新: {board}:{_cfg.UdpPort}");
            }
            if (board != null && System.Net.IPAddress.TryParse(board, out var baddr))
            {
                var bep = new IPEndPoint(baddr, _cfg.UdpPort);
                if (!eps.Contains(bep)) eps.Add(bep);
            }

            int ok = 0;
            foreach (var ep in eps)
            {
                try { _udp.Send(bytes, bytes.Length, ep); ok++; }
                catch (Exception ex) { if (tick % 30 == 0) Log($"发送到 {ep} 失败: {ex.Message}"); }
            }
            if (ok > 0) sent++;

            tick++;
            // 每 15 秒打一行心跳，避免刷屏
            if ((DateTime.UtcNow - lastBeat).TotalSeconds >= 15)
            {
                lastBeat = DateTime.UtcNow;
                string state = Data.LastUpdateUtc == DateTime.MinValue
                    ? "尚未收到遥测"
                    : $"已发 {sent} 包 | CPU {Fmt(Data.CpuUsage)}% {Fmt(Data.CpuTemp)}℃ | GPU {Fmt(Data.GpuUsage)}% {Fmt(Data.GpuTemp)}℃ | 内存 {Fmt(Data.MemUsage)}%";
                Log(state);
            }
        }

        Log("正在退出…");
        lock (TopicCount)
        {
            if (TopicCount.Count > 0)
            {
                Log("本次收到的主题：");
                foreach (var kv in TopicCount.OrderByDescending(k => k.Value))
                    Log($"  {kv.Key,-26} x{kv.Value}");
            }
        }
        _mqtt?.Dispose();
        _udp.Dispose();
        return 0;
    }

    /// <summary>构造 UDP 报文。</summary>
    static string BuildPayload()
    {
        long age = Data.LastUpdateUtc == DateTime.MinValue
            ? -1
            : (long)(DateTime.UtcNow - Data.LastUpdateUtc).TotalSeconds;

        var sb = new StringBuilder(224);
        sb.Append("GCU1");
        Add(sb, "cpu", Data.CpuUsage);
        Add(sb, "ct", Data.CpuTemp);
        Add(sb, "gpu", Data.GpuUsage);
        Add(sb, "gt", Data.GpuTemp);
        Add(sb, "mem", Data.MemUsage);
        Add(sb, "mt", Data.MemTotal);
        Add(sb, "mu", Data.MemUsed);
        Add(sb, "f1", Data.CpuFanRpm);
        Add(sb, "f2", Data.GpuFanRpm);
        Add(sb, "ssd", Data.SsdTemp);
        Add(sb, "dn", Clean(Data.NetDown));
        Add(sb, "up", Clean(Data.NetUp));
        Add(sb, "bat", Data.BatteryPercent);
        Add(sb, "fe", Data.FanError);
        Add(sb, "age", age);

        // 进程占用 Top5（任务管理器"进程"页同口径）：ac=行数，a1=进程名，c1=CPU%，g1=内存MB
        // ac 必须在 a/c/g 之前发，固件收到后会先清掉旧行
        var top = ProcessSampler.Top;
        Add(sb, "ac", (long)top.Count);
        for (int i = 0; i < top.Count && i < 5; i++)
        {
            var a = top[i];
            Add(sb, $"a{i + 1}", a.Name.Length > 10 ? a.Name[..10] : a.Name);
            Add(sb, $"c{i + 1}", (double?)a.Cpu);
            Add(sb, $"g{i + 1}", (double?)a.MemMB);
        }
        return sb.ToString();
    }

    static void Add(StringBuilder sb, string key, double? v)
    {
        if (v is null) return;
        sb.Append('|').Append(key).Append('=').Append(v.Value.ToString("0.#", CultureInfo.InvariantCulture));
    }

    static void Add(StringBuilder sb, string key, int? v)
    {
        if (v is null) return;
        sb.Append('|').Append(key).Append('=').Append(v.Value.ToString(CultureInfo.InvariantCulture));
    }

    static void Add(StringBuilder sb, string key, long v)
        => sb.Append('|').Append(key).Append('=').Append(v.ToString(CultureInfo.InvariantCulture));

    static void Add(StringBuilder sb, string key, string? v)
    {
        if (string.IsNullOrEmpty(v)) return;
        sb.Append('|').Append(key).Append('=').Append(v);
    }

    /// <summary>去掉可能破坏报文结构的分隔符。</summary>
    static string? Clean(string? s)
    {
        if (string.IsNullOrWhiteSpace(s)) return null;
        var t = s.Replace('|', ' ').Replace('\r', ' ').Replace('\n', ' ').Trim();
        return t.Length > 12 ? t[..12] : t;
    }

    static string Fmt(double? v) => v?.ToString("0.#", CultureInfo.InvariantCulture) ?? "--";

    /// <summary>按 GCUBridge 的白名单规则推导密码。</summary>
    static string PasswordFor(string clientId) => clientId switch
    {
        "OcScanner" => "OcScannerPwd123",
        "OcTool" => "OcToolPwd123",
        "OpenCL" => "OpenCLPwd123",
        _ => clientId + "Pwd888881772688",
    };

    static List<string> LocalAddresses()
    {
        var list = new List<string>();
        try
        {
            foreach (var ni in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType == System.Net.NetworkInformation.NetworkInterfaceType.Loopback) continue;
                foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                {
                    if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    var ip = ua.Address.ToString();
                    if (ip.StartsWith("169.254.")) continue;
                    list.Add($"{ip}  ({ni.Name})");
                }
            }
        }
        catch { }
        return list;
    }

    static void Log(string msg)
    {
        string line = $"[{DateTime.Now:HH:mm:ss}] {msg}";
        lock (LogGate)
        {
            Console.WriteLine(line);
            try
            {
                string dir = AppContext.BaseDirectory;
                File.AppendAllText(Path.Combine(dir, "cast.log"), line + Environment.NewLine, Encoding.UTF8);
            }
            catch { }
        }
    }
}
