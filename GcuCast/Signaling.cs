using System.Net.Http;
using System.Text.Json;

namespace GcuCast;

/// <summary>
/// IP 心跳信令：轮询内网信令服务器（heartbeat_server.py）拿板子的最新 IP。
/// 板子每 30 秒 POST 一次心跳，本类每 5 秒 GET /ip 一次；
/// 90 秒内有过心跳才认为 IP 有效，过期自动回退到 config.json 里的静态目标。
/// </summary>
internal static class Signaling
{
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(3) };
    static volatile string? _boardIp;
    static DateTime _utc = DateTime.MinValue;

    /// <summary>板子当前 IP；没有启用信令或心跳过期时返回 null。</summary>
    public static string? BoardIp =>
        DateTime.UtcNow - _utc < TimeSpan.FromSeconds(90) ? _boardIp : null;

    public static void Start(string url, string token)
    {
        var t = new Thread(() =>
        {
            string sep = url.Contains('?') ? "&" : "?";
            string full = $"{url}{sep}token={Uri.EscapeDataString(token)}";
            while (true)
            {
                try
                {
                    string json = Http.GetStringAsync(full).GetAwaiter().GetResult();
                    using var doc = JsonDocument.Parse(json);
                    if (doc.RootElement.TryGetProperty("ip", out var el) &&
                        el.ValueKind == JsonValueKind.String)
                    {
                        string ip = el.GetString() ?? "";
                        if (ip.Length > 0)
                        {
                            if (ip != _boardIp)
                                Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] [信令] 板子 IP: {ip}");
                            _boardIp = ip;
                            _utc = DateTime.UtcNow;
                        }
                    }
                }
                catch { /* 服务器暂时不可达，下个周期再试 */ }
                Thread.Sleep(5000);
            }
        })
        { IsBackground = true, Name = "Signaling" };
        t.Start();
    }
}
