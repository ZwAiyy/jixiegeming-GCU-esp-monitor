using System.Net;
using System.Text;
using System.Text.Json;

namespace GcuCast;

/// <summary>转发器配置，默认从程序目录的 config.json 读取（不存在则自动生成一份）。</summary>
internal sealed class CastConfig
{
    /// <summary>GCUBridge 的地址，固定本机回环（它只监听回环）。</summary>
    public string MqttHost { get; set; } = "127.0.0.1";
    public int MqttPort { get; set; } = 13688;

    /// <summary>MQTT clientId，必须命中 GCUBridge 白名单。OcScanner / OcTool / OpenCL 均可。</summary>
    public string ClientId { get; set; } = "OcScanner";

    /// <summary>ESP32 监听的 UDP 端口。</summary>
    public int UdpPort { get; set; } = 8266;

    /// <summary>是否向广播地址发送（ESP32 配好 WiFi 就能收，不用知道电脑 IP）。</summary>
    public bool UseBroadcast { get; set; } = true;

    /// <summary>
    /// 广播地址，逗号分隔可填多个（会全部同时发送）：
    ///   255.255.255.255        受限广播，一般不出本路由器
    ///   172.26.255.255         定向广播（按本机网段算 /16），能不能穿过校园网路由器看运气
    /// 广播和 Targets 单播是同时发的：广播通了板子换 IP 也不用改配置。
    /// </summary>
    public string BroadcastAddress { get; set; } = "255.255.255.255,172.26.255.255";

    /// <summary>额外的单播目标（"192.168.1.50" 或 "192.168.1.50:8266"），广播不通时用这个。</summary>
    public string[] Targets { get; set; } = Array.Empty<string>();

    /// <summary>发送周期（毫秒）。</summary>
    public int IntervalMs { get; set; } = 1000;

    /// <summary>为 true 时把收到的每条原始报文打到日志（排障用，平时关掉）。</summary>
    public bool Verbose { get; set; } = false;

    /// <summary>
    /// IP 心跳信令服务 URL（内网 Linux 上跑 heartbeat_server.py），如 "http://172.16.50.83:8300/ip"。
    /// 板子每 30 秒向同一服务器 POST 心跳，这里轮询拿到板子的最新 IP，
    /// 动态追加为单播目标——换 IP 不用再改配置。留空 = 不启用。
    /// </summary>
    public string SignalingUrl { get; set; } = "";

    /// <summary>信令口令，与服务器端 heartbeat_server.py 里的 TOKEN 一致。</summary>
    public string SignalingToken { get; set; } = "gcu-hud-2026";

    static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    /// <summary>写配置文件用无 BOM 的 UTF-8，避免其它工具（Python/编辑器）读不进去。</summary>
    static readonly Encoding Utf8NoBom = new UTF8Encoding(false);

    public static CastConfig Load(string[] args)
    {
        string path = Path.Combine(AppContext.BaseDirectory, "config.json");

        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] is "--config" or "-c")
                path = Path.GetFullPath(args[i + 1]);

        if (!File.Exists(path))
        {
            var def = new CastConfig();
            try
            {
                File.WriteAllText(path, JsonSerializer.Serialize(def, JsonOpts), Utf8NoBom);
                Console.WriteLine($"未找到配置文件，已生成默认配置：{path}");
            }
            catch { }
            return def;
        }

        try
        {
            var cfg = JsonSerializer.Deserialize<CastConfig>(File.ReadAllText(path, Encoding.UTF8), JsonOpts);
            return cfg ?? new CastConfig();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"配置文件解析失败（{ex.Message}），改用默认值。");
            return new CastConfig();
        }
    }

    /// <summary>
    /// 换算成实际发送目标：显式单播 Targets 永远发，UseBroadcast 时把配置里的
    /// （可能多个）广播地址追加进去，去重。广播通时板子随便换 IP 都能收。
    /// </summary>
    public List<IPEndPoint> ResolveTargets()
    {
        var list = new List<IPEndPoint>();

        foreach (var t in Targets)
        {
            if (string.IsNullOrWhiteSpace(t)) continue;
            string s = t.Trim();

            int port = UdpPort;
            int colon = s.LastIndexOf(':');
            if (colon > 0 && int.TryParse(s[(colon + 1)..], out int p))
            {
                port = p;
                s = s[..colon];
            }

            if (IPAddress.TryParse(s, out var ip))
                list.Add(new IPEndPoint(ip, port));
        }

        if (UseBroadcast)
        {
            foreach (var raw in BroadcastAddress.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                if (IPAddress.TryParse(raw, out var bcast))
                    list.Add(new IPEndPoint(bcast, UdpPort));
            }
        }

        return list.Distinct().ToList();
    }
}
