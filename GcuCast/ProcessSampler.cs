using System.Diagnostics;
using System.Text;

namespace GcuCast;

/// <summary>
/// 采样每个"进程"的 CPU / 内存占用（任务管理器"进程"页同口径），
/// 维护占用最高的前 5 名，供 OLED "TOP PROC" 页显示。
///
/// CPU：两次快照的 TotalProcessorTime 差值 ÷ 时间差 ÷ 逻辑核数（任务管理器同口径）。
/// 内存：WorkingSet64（工作集，任务管理器默认"内存"列）。
/// 排序：CPU 优先，同分比内存；纯挂机进程（CPU≈0 且内存小）不上榜。
/// 在后台线程每 2 秒采样一次，几乎不占 UDP 发送主循环的时间。
/// </summary>
internal static class ProcessSampler
{
    public sealed record ProcStat(string Name, double Cpu, double MemMB);

    static readonly object Lock = new();
    static List<ProcStat> _top = new();

    static Dictionary<int, (string Name, TimeSpan Cpu)> _prevCpu = new();
    static DateTime _prevTime = DateTime.UtcNow;

    /// <summary>当前 Top5 快照（无重负载进程时为空表）。</summary>
    public static List<ProcStat> Top
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
                Thread.Sleep(1000);   // 任务管理器同款 1 秒刷新，抓得住瞬时尖峰
            }
        })
        { IsBackground = true, Name = "ProcessSampler" };
        t.Start();
    }

    static void Sample()
    {
        DateTime now = DateTime.UtcNow;
        var snap = new Dictionary<int, (string Name, TimeSpan Cpu, long Mem)>();
        foreach (var p in Process.GetProcesses())
        {
            try
            {
                if (p.Id == 0) continue;               // Idle 的 CPU 时间无意义
                snap[p.Id] = (p.ProcessName, p.TotalProcessorTime, p.WorkingSet64);
            }
            catch { }   // 进程刚好退出了
        }

        var rows = new List<ProcStat>();
        double dt = (now - _prevTime).TotalSeconds;
        if (dt > 0.5)
        {
            int cores = Environment.ProcessorCount;
            foreach (var (pid, (name, cpu, mem)) in snap)
            {
                if (!_prevCpu.TryGetValue(pid, out var prev)) continue;   // 首个周期没有基线
                double cpuPct = Math.Clamp((cpu - prev.Cpu).TotalSeconds / dt / cores * 100, 0, 100);
                double memMB = mem / 1024.0 / 1024.0;
                // 纯挂机进程不上榜：CPU≈0 且内存不足 50MB（系统后台进程一大堆，全列没意义）
                if (cpuPct < 0.5 && memMB < 50) continue;
                rows.Add(new ProcStat(CleanName(name), Math.Round(cpuPct, 1), Math.Round(memMB)));
            }
        }
        _prevCpu = snap.Select(kv => (kv.Key, (kv.Value.Name, kv.Value.Cpu)))
                       .ToDictionary(x => x.Item1, x => x.Item2);
        _prevTime = now;

        var top = rows
            .OrderByDescending(r => r.Cpu)          // CPU 优先
            .ThenByDescending(r => r.MemMB)         // 同分比内存
            .Take(5)
            .ToList();

        lock (Lock) _top = top;
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
