using System.Net.Sockets;
using System.Text;

namespace GcuHud;

/// <summary>
/// 连接机械革命控制台的本地 MQTT 总线（GCUBridge，默认 127.0.0.1:13688），
/// 订阅系统监控遥测。
///
/// GCUBridge 的鉴权规则：clientId 必须在白名单内，且必须搭配该 clientId 对应的
/// 固定账号密码；否则 CONNACK 返回 0x02（标识符被拒）或 0x04（账号密码错）。
///
/// 开启遥测的方式：向 System/Control 发布 {"Action":"System_ON"}，
/// 关闭时发布 {"Action":"System_OFF"}。
/// </summary>
public sealed class GcuMqtt : IDisposable
{
    public const string Host = "127.0.0.1";
    public const int Port = 13688;
    public const string TopicControl = "System/Control";

    /// <summary>单条报文上限，防止对端异常数据把内存打爆。</summary>
    const int MaxPacketSize = 1 << 20;

    enum State { Idle, Connecting, Connected, Disposed }

    readonly object _gate = new();
    TcpClient? _tcp;
    NetworkStream? _stream;
    Thread? _rxThread;
    Thread? _kaThread;
    State _state = State.Idle;
    ushort _packetId;
    volatile bool _stop;

    public string ClientId { get; }
    public string Username { get; }
    public string Password { get; }

    /// <summary>
    /// 退出时是否发送 System_OFF。注意这是 GCU 的**全局**开关：
    /// 只要还有一个消费者（如悬浮窗）在跑，就不能关，否则它们会一起断流。
    /// </summary>
    public bool SendOffOnDispose { get; set; } = true;

    /// <summary>连接状态变化（true=已连上）。</summary>
    public event Action<bool>? ConnectionChanged;

    /// <summary>收到 PUBLISH 报文。</summary>
    public event Action<string, string>? MessageReceived;

    public GcuMqtt(string clientId, string username, string password)
    {
        ClientId = clientId;
        Username = username;
        Password = password;
    }

    public bool IsConnected => _state == State.Connected;

    /// <summary>按白名单规则，为已知 clientId 推导出默认账号密码。</summary>
    public static GcuMqtt ForClientId(string clientId)
    {
        string user = clientId + "User";
        string pass = clientId switch
        {
            "OcScanner" => "OcScannerPwd123",
            "OcTool" => "OcToolPwd123",
            "OpenCL" => "OpenCLPwd123",
            _ => clientId + "Pwd888881772688",
        };
        return new GcuMqtt(clientId, user, pass);
    }

    /// <summary>启动后台连接与接收循环（自动重连）。</summary>
    public void Start()
    {
        _stop = false;
        _rxThread = new Thread(RxLoop) { IsBackground = true, Name = "gcu-mqtt" };
        _rxThread.Start();
        _kaThread = new Thread(KeepAliveLoop) { IsBackground = true, Name = "gcu-ka" };
        _kaThread.Start();
    }

    void RxLoop()
    {
        while (!_stop)
        {
            try
            {
                if (!TryConnect())
                {
                    Thread.Sleep(3000);
                    continue;
                }

                Subscribe("System/#");
                Subscribe("Fan/Status");
                Subscribe("EC/Status");
                Subscribe("GPUDevice/Status");

                // 让 GCU 开始推送系统监控遥测
                Publish(TopicControl, "{\"Action\":\"System_ON\"}");

                _state = State.Connected;
                ConnectionChanged?.Invoke(true);

                ReadLoop();
            }
            catch (Exception)
            {
                // 掉线，走重连
            }
            finally
            {
                bool wasConnected = _state == State.Connected;
                _state = State.Idle;
                CloseSocket();
                if (wasConnected) ConnectionChanged?.Invoke(false);
            }

            if (!_stop) Thread.Sleep(2000);
        }
    }

    bool TryConnect()
    {
        _state = State.Connecting;
        var tcp = new TcpClient { NoDelay = true };
        tcp.Connect(Host, Port);
        var s = tcp.GetStream();
        s.ReadTimeout = 10000;
        s.WriteTimeout = 5000;

        // 先挂上连接，WritePacket / ReadPacket 都依赖这两个字段
        _tcp = tcp;
        _stream = s;

        // CONNECT 报文
        var body = new List<byte>();
        body.AddRange(Str("MQTT"));
        body.Add(4);          // 协议级别 3.1.1
        body.Add(0xC2);       // username + password + clean session
        body.Add(0); body.Add(60);
        body.AddRange(Str(ClientId));
        body.AddRange(Str(Username));
        body.AddRange(Str(Password));
        WritePacket(0x10, body);

        // ReadPacket 返回的是 [固定头][负载]，剩余长度字节已经丢掉，
        // 所以 CONNACK 只剩 3 字节：ack[1]=会话标志、ack[2]=返回码。
        var ack = ReadPacket();
        if (ack is null || ack.Length < 3 || ack[2] != 0x00)   // 非「接受」
        {
            CloseSocket();
            return false;
        }

        return true;
    }

    void ReadLoop()
    {
        while (!_stop)
        {
            var pkt = ReadPacket();
            if (pkt is null) return;

            int type = pkt[0] >> 4;
            switch (type)
            {
                case 3: // PUBLISH
                    OnPublish(pkt);
                    break;

                case 13: // PINGRESP
                    break;

                case 14: // DISCONNECT
                    return;
            }
        }
    }

    void OnPublish(byte[] pkt)
    {
        int qos = (pkt[0] >> 1) & 3;
        int pos = 1;
        if (pos + 2 > pkt.Length) return;
        int tlen = (pkt[pos] << 8) | pkt[pos + 1];
        pos += 2;
        if (tlen <= 0 || pos + tlen > pkt.Length) return;
        string topic = Encoding.UTF8.GetString(pkt, pos, tlen);
        pos += tlen;
        if (qos > 0)                            // 跳过 packet id
        {
            if (pos + 2 > pkt.Length) return;
            pos += 2;
        }
        if (pos > pkt.Length) return;
        string payload = Encoding.UTF8.GetString(pkt, pos, pkt.Length - pos);

        MessageReceived?.Invoke(topic, payload);
    }

    void KeepAliveLoop()
    {
        while (!_stop)
        {
            Thread.Sleep(20000);
            if (!IsConnected) continue;
            try
            {
                lock (_gate) WritePacketUnlocked(0xC0, new List<byte>());  // PINGREQ
            }
            catch { /* 交给接收线程触发重连 */ }
        }
    }

    public void Publish(string topic, string payload)
    {
        var body = new List<byte>();
        body.AddRange(Str(topic));
        body.AddRange(Encoding.UTF8.GetBytes(payload));
        lock (_gate)
        {
            if (_stream is null) return;          // 未连接时静默丢弃，不抛异常
            WritePacketUnlocked(0x30, body);
        }
    }

    public void Subscribe(string topicFilter)
    {
        var body = new List<byte>();
        ushort id;
        lock (_gate) id = ++_packetId;
        body.Add((byte)(id >> 8));
        body.Add((byte)(id & 0xFF));
        body.AddRange(Str(topicFilter));
        body.Add(0);                       // QoS 0
        lock (_gate)
        {
            if (_stream is null) return;
            WritePacketUnlocked(0x82, body);
        }
    }

    void WritePacket(byte header, List<byte> body) { lock (_gate) WritePacketUnlocked(header, body); }

    void WritePacketUnlocked(byte header, List<byte> body)
    {
        if (_stream is null) throw new IOException("未连接");
        var buf = new List<byte>(body.Count + 5) { header };
        int len = body.Count;
        do
        {
            byte d = (byte)(len % 128);
            len /= 128;
            if (len > 0) d |= 0x80;
            buf.Add(d);
        } while (len > 0);
        buf.AddRange(body);
        _stream.Write(buf.ToArray(), 0, buf.Count);
        _stream.Flush();
    }

    /// <summary>读一个完整报文，返回「固定头字节 + 负载」；超时或断开返回 null。</summary>
    byte[]? ReadPacket()
    {
        var s = _stream;
        if (s is null) return null;

        int b0 = s.ReadByte();
        if (b0 < 0) return null;

        int len = 0, mult = 1, guard = 0;
        while (true)
        {
            int d = s.ReadByte();
            if (d < 0) return null;
            len += (d & 0x7F) * mult;
            if ((d & 0x80) == 0) break;
            mult *= 128;
            if (++guard > 4) return null;
        }

        if (len < 0 || len > MaxPacketSize) return null;   // 异常长度直接断开，别去 new 巨型数组

        var payload = new byte[len];
        int read = 0;
        while (read < len)
        {
            int n = s.Read(payload, read, len - read);
            if (n <= 0) return null;
            read += n;
        }

        var pkt = new byte[len + 1];
        pkt[0] = (byte)b0;
        Buffer.BlockCopy(payload, 0, pkt, 1, len);
        return pkt;
    }

    static byte[] Str(string s)
    {
        var b = Encoding.UTF8.GetBytes(s);
        var r = new byte[b.Length + 2];
        r[0] = (byte)(b.Length >> 8);
        r[1] = (byte)(b.Length & 0xFF);
        Buffer.BlockCopy(b, 0, r, 2, b.Length);
        return r;
    }

    void CloseSocket()
    {
        try { _stream?.Dispose(); } catch { }
        try { _tcp?.Close(); } catch { }
        _stream = null;
        _tcp = null;
    }

    public void Dispose()
    {
        if (_state == State.Disposed) return;
        _stop = true;

        try
        {
            if (_state == State.Connected && SendOffOnDispose)
            {
                Publish(TopicControl, "{\"Action\":\"System_OFF\"}");
                Thread.Sleep(120);
            }
        }
        catch { }

        _state = State.Disposed;
        CloseSocket();
    }
}
