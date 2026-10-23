using System.Text.Json;

namespace GcuHud;

/// <summary>
/// 从 GCU 的 MQTT 遥测主题解析出的监控数据模型。
/// 字段来源：System/CpuInfo、System/GpuInfo、System/MemoryInfo、
/// System/FanInfo、System/NetworkInfo、System/DiskInfo、
/// System/BatteryInfo、System/FanErrorInfo
/// </summary>
public sealed class Telemetry
{
    public double? CpuUsage { get; private set; }
    public double? CpuTemp { get; private set; }

    public double? GpuUsage { get; private set; }
    public double? GpuTemp { get; private set; }
    public double? GpuCoreFreq { get; private set; }
    public double? GpuMemFreq { get; private set; }
    public string? GpuPState { get; private set; }
    public double? GpuMem { get; private set; }

    public double? MemTotal { get; private set; }
    public double? MemUsed { get; private set; }
    public double? MemUsage { get; private set; }

    public double? CpuFanRpm { get; private set; }
    public double? GpuFanRpm { get; private set; }
    public double? CpuFanDuty { get; private set; }
    public double? GpuFanDuty { get; private set; }

    public string? NetDown { get; private set; }
    public string? NetUp { get; private set; }

    public double? SsdTemp { get; private set; }
    public string? SsdModel { get; private set; }
    public double? SsdHours { get; private set; }

    public double? BatteryPercent { get; private set; }
    public double? BatteryCycle { get; private set; }

    public int? FanError { get; private set; }

    /// <summary>最后一次收到任意遥测的时间（用于判断数据是否停滞）。</summary>
    public DateTime LastUpdateUtc { get; private set; } = DateTime.MinValue;

    public bool HasCpu => CpuUsage.HasValue || CpuTemp.HasValue;
    public bool HasGpu => GpuUsage.HasValue || GpuTemp.HasValue;

    /// <summary>把某个主题的 JSON 载荷合并进模型。</summary>
    public void Apply(string topic, string payload)
    {
        JsonDocument doc;
        try { doc = JsonDocument.Parse(payload); }
        catch { return; }

        using (doc)
        {
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object) return;

            switch (topic)
            {
                case "System/CpuInfo":
                    CpuUsage = Num(r, "CpuUsage") ?? CpuUsage;
                    CpuTemp = Num(r, "CpuTemperature") ?? CpuTemp;
                    break;

                case "System/GpuInfo":
                    GpuUsage = Num(r, "GpuUsage") ?? GpuUsage;
                    GpuTemp = Num(r, "GpuTemperature") ?? GpuTemp;
                    GpuCoreFreq = Num(r, "GpuCoreFreq") ?? GpuCoreFreq;
                    GpuMemFreq = Num(r, "GpuMemFreq") ?? GpuMemFreq;
                    GpuMem = Num(r, "GpuMem") ?? GpuMem;
                    GpuPState = Str(r, "GpuPState") ?? GpuPState;
                    break;

                case "System/MemoryInfo":
                    MemTotal = Num(r, "TotalMemory") ?? MemTotal;
                    MemUsed = Num(r, "TotalUsingMemory") ?? MemUsed;
                    MemUsage = Num(r, "MemoryUsage") ?? MemUsage;
                    break;

                case "System/FanInfo":
                    CpuFanRpm = Num(r, "CpuFanRpm") ?? CpuFanRpm;
                    GpuFanRpm = Num(r, "GpuFanRpm") ?? GpuFanRpm;
                    CpuFanDuty = Num(r, "CpuFanDuty") ?? CpuFanDuty;
                    GpuFanDuty = Num(r, "GpuFanDuty") ?? GpuFanDuty;
                    break;

                case "System/NetworkInfo":
                    NetDown = Str(r, "NetworkDownload") ?? NetDown;
                    NetUp = Str(r, "NetworkUpload") ?? NetUp;
                    break;

                case "System/DiskInfo":
                    SsdTemp = Num(r, "SSD_One_Temperature") ?? SsdTemp;
                    SsdHours = Num(r, "SSD_One_PowerOnHours") ?? SsdHours;
                    var m = Str(r, "SSD_One_Model");
                    if (m is not null && m != "null") SsdModel = m;
                    break;

                case "System/BatteryInfo":
                    BatteryPercent = Num(r, "BatteryLifePercent") ?? BatteryPercent;
                    BatteryCycle = Num(r, "BatteryCycleCount") ?? BatteryCycle;
                    break;

                case "System/FanErrorInfo":
                    var fe = Num(r, "FanErrorStatus");
                    if (fe.HasValue) FanError = (int)fe.Value;
                    break;

                default:
                    return;   // 其它主题不参与刷新计时
            }
        }

        LastUpdateUtc = DateTime.UtcNow;
    }

    /// <summary>取数值：兼容 "26" 与 26 两种写法，忽略 null/"null"。</summary>
    private static double? Num(JsonElement obj, string name)
    {
        if (!obj.TryGetProperty(name, out var v)) return null;
        switch (v.ValueKind)
        {
            case JsonValueKind.Number:
                return v.TryGetDouble(out var d) ? d : null;
            case JsonValueKind.String:
                var s = v.GetString();
                if (string.IsNullOrWhiteSpace(s) || s == "null") return null;
                return double.TryParse(s, System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture, out var p) ? p : null;
            default:
                return null;
        }
    }

    private static string? Str(JsonElement obj, string name)
    {
        if (!obj.TryGetProperty(name, out var v)) return null;
        if (v.ValueKind == JsonValueKind.String) return v.GetString();
        if (v.ValueKind == JsonValueKind.Number) return v.ToString();
        return null;
    }
}
