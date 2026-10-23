using System.Diagnostics;
using System.Text;

namespace GcuCast;

/// <summary>
/// 采样每个应用的 CPU / GPU 占用，维护占用最高的前 5 名，供 OLED "TOP APPS" 页显示。
///
/// CPU：两次快照的 TotalProcessorTime 差值 ÷ 时间差 ÷ 逻辑核数（任务管理器同口径）。
/// GPU：Windows "GPU Engine" 性能计数器，实例名形如
///      "pid_1234_luid_..._eng_0_engtype_3D"，按 pid 聚合各引擎占用后封顶 100。
/// 在后台线程每 2 秒采样一次，几乎不占 UDP 发送主循环的时间。
/// </summary>
internal static class ProcessSampler
{
    public sealed record AppStat(string Name, double Cpu, double Gpu);

    static readonly object Lock = new();
    static List<AppStat> _top = new();

    // CPU 采样
    static Dictionary<int, (string Name, TimeSpan Cpu)> _prevCpu = new();
    static DateTime _prevTime = DateTime.UtcNow;

    // GPU 采样（跨周期复用计数器：新建的计数器第一次 NextValue 只是基线）
    static Dictionary<string, PerformanceCounter> _gpuCounters = new();
    static bool _gpuWarned;

    /// <summary>当前 Top5 快照（无重负载应用时为空表）。</summary>
    public static List<AppStat> Top
    {
        get { lock (Lock) return _top.ToList(); }
    }

    public static void Start()
    {
        var t = new Thread(() =>
        {
            while (true)
            {
                try { Sample(); }
                catch { /* 采样失败不影响转发主流程 */ }
                Thread.Sleep(2000);
            }
        })
        { IsBackground = true, Name = "ProcessSampler" };
        t.Start();
    }

    static void Sample()
    {
        DateTime now = DateTime.UtcNow;
        var snap = new Dictionary<int, (string Name, TimeSpan Cpu)>();
        foreach (var p in Process.GetProcesses())
        {
            try
            {
                if (p.Id == 0) continue;               // Idle 的 CPU 时间无意义
                snap[p.Id] = (p.ProcessName, p.TotalProcessorTime);
            }
            catch { }   // 进程刚好退出了
        }

        // ---- CPU% ----
        var cpuByPid = new Dictionary<int, double>();
        double dt = (now - _prevTime).TotalSeconds;
        if (dt > 0.5)
        {
            int cores = Environment.ProcessorCount;
            foreach (var (pid, (_, cpu)) in snap)
                if (_prevCpu.TryGetValue(pid, out var prev))
                    cpuByPid[pid] = Math.Clamp(
                        (cpu - prev.Cpu).TotalSeconds / dt / cores * 100, 0, 100);
        }
        _prevCpu = snap;
        _prevTime = now;

        // ---- GPU% ----
        var gpuByPid = SampleGpu();

        // ---- 按应用名聚合（同一应用多个进程求和），CPU+GPU 排序取前 5 ----
        var byName = new Dictionary<string, (double Cpu, double Gpu)>();
        foreach (var pid in cpuByPid.Keys.Union(gpuByPid.Keys))
        {
            if (!snap.TryGetValue(pid, out var s)) continue;   // 计数器里的进程已退出
            string name = CleanName(s.Name);
            double cpu = cpuByPid.GetValueOrDefault(pid);
            double gpu = gpuByPid.GetValueOrDefault(pid);
            var (c, g) = byName.GetValueOrDefault(name);
            byName[name] = (c + cpu, Math.Min(g + gpu, 100));
        }

        var top = byName
            .Where(kv => kv.Value.Cpu >= 0.5 || kv.Value.Gpu >= 0.5)   // 纯挂机进程不上榜
            .OrderByDescending(kv => kv.Value.Cpu + kv.Value.Gpu)
            .Take(5)
            .Select(kv => new AppStat(kv.Key,
                Math.Round(kv.Value.Cpu, 1), Math.Round(kv.Value.Gpu, 1)))
            .ToList();

        lock (Lock) _top = top;
    }

    static Dictionary<int, double> SampleGpu()
    {
        var result = new Dictionary<int, double>();
        try
        {
            var instances = new PerformanceCounterCategory("GPU Engine").GetInstanceNames();
            var alive = new HashSet<string>(instances, StringComparer.Ordinal);

            // 清理已消失的实例，防止计数器字典无限膨胀
            foreach (var stale in _gpuCounters.Keys.Where(k => !alive.Contains(k)).ToList())
            {
                _gpuCounters[stale].Dispose();
                _gpuCounters.Remove(stale);
            }

            foreach (var inst in instances)
            {
                // 实例名: pid_1234_luid_0x..._phys_0_eng_0_engtype_3D
                if (!inst.StartsWith("pid_", StringComparison.Ordinal)) continue;
                int end = inst.IndexOf('_', 4);
                if (end <= 4 || !int.TryParse(inst.AsSpan(4, end - 4), out int pid)) continue;

                if (!_gpuCounters.TryGetValue(inst, out var cnt))
                {
                    cnt = new PerformanceCounter("GPU Engine", "Utilization Percentage", inst, true);
                    _gpuCounters[inst] = cnt;
                    cnt.NextValue();               // 新计数器第一次读数是基线，丢弃
                    continue;
                }

                float v = cnt.NextValue();
                if (v > 0) result[pid] = result.GetValueOrDefault(pid) + v;
            }
        }
        catch (Exception ex)
        {
            if (!_gpuWarned)
            {
                _gpuWarned = true;
                Console.WriteLine($"[apps] GPU 计数器不可用（只显示 CPU 排行）：{ex.Message}");
            }
        }
        return result.ToDictionary(kv => kv.Key, kv => Math.Min(kv.Value, 100.0));
    }

    /// <summary>去掉 .exe 后缀，替换掉会破坏 '|' 分隔协议的字符，OLED 一行最多放 10 字符。</summary>
    static string CleanName(string name)
    {
        if (name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) name = name[..^4];
        var sb = new StringBuilder(name.Length);
        foreach (char ch in name)
            sb.Append(char.IsLetterOrDigit(ch) || ch is '.' or '-' or '_' ? ch : '_');
        return sb.ToString();
    }
}
