using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace Lan3DBandwidth
{
    internal sealed class Measure
    {
        public long Bytes, Milliseconds;
        public double MinimumMbps;
        public double Mbps { get { return Bytes * 8.0 / Math.Max(1, Milliseconds) / 1000.0; } }
        public override string ToString() { return String.Format("平均 {0:N1} Mbps；最低完整一秒 {1:N1} Mbps\r\n实际接收 {2:N1} MB，用时 {3:N2} 秒", Mbps, MinimumMbps, Bytes / 1000000.0, Milliseconds / 1000.0); }
    }

    internal sealed class Engine : IDisposable
    {
        const string Magic = "L3DBW002";
        const int Chunk = 256 * 1024;
        const long MaximumBytes = 64L * 1024 * 1024 * 1024;
        readonly object gate = new object();
        readonly List<TcpClient> clients = new List<TcpClient>();
        TcpListener listener;
        volatile bool stopping;
        int busy;
        public Action<string> Status = delegate {};
        public Action<string> Finished = delegate {};
        public Action<Measure, Measure> Completed = delegate {};
        public bool IsBusy { get { return Interlocked.CompareExchange(ref busy, 0, 0) != 0; } }

        static bool Allowed(IPAddress address) { return IPAddress.IsLoopback(address) || address.Equals(IPAddress.Parse("192.168.3.42")) || address.Equals(IPAddress.Parse("192.168.3.197")); }
        static void Configure(TcpClient client)
        {
            client.NoDelay = true;
            client.ReceiveTimeout = 3500;
            client.SendTimeout = 3500;
            client.ReceiveBufferSize = 1024 * 1024;
            client.SendBufferSize = 1024 * 1024;
        }
        void Track(TcpClient client) { lock (gate) { if (stopping) { client.Close(); throw new OperationCanceledException(); } clients.Add(client); } }
        void Untrack(TcpClient client) { lock (gate) { clients.Remove(client); } client.Close(); }
        public void Listen(int port, bool loopbackOnly)
        {
            listener = new TcpListener(loopbackOnly ? IPAddress.Loopback : IPAddress.Any, port);
            listener.Start(2);
            new Thread(AcceptLoop) { IsBackground = true }.Start();
        }
        void AcceptLoop()
        {
            while (!stopping)
            {
                TcpClient client = null;
                try
                {
                    client = listener.AcceptTcpClient();
                    if (!Allowed(((IPEndPoint)client.Client.RemoteEndPoint).Address)) { client.Close(); continue; }
                    Configure(client);
                    if (Interlocked.CompareExchange(ref busy, 1, 0) != 0)
                    {
                        client.GetStream().WriteByte(2); client.Close(); continue;
                    }
                    Track(client);
                    TcpClient accepted = client;
                    new Thread(delegate() { Serve(accepted); }) { IsBackground = true }.Start();
                }
                catch (Exception ex)
                {
                    if (client != null) client.Close();
                    if (!stopping) Status("监听失败：" + ex.Message);
                }
            }
        }
        void Serve(TcpClient client)
        {
            try
            {
                NetworkStream stream = client.GetStream();
                var reader = new BinaryReader(stream, Encoding.UTF8);
                var writer = new BinaryWriter(stream, Encoding.UTF8);
                byte[] magic = ReadExact(reader, 8);
                int duration = reader.ReadInt32();
                if (Encoding.ASCII.GetString(magic) != Magic || duration < 2000 || duration > 10000) throw new IOException("无效测速请求");
                writer.Write((byte)1); writer.Flush();
                Status("正在接收对方数据（第一方向）…");
                Measure incoming = Receive(reader, duration);
                WriteMeasure(writer, incoming);
                Status("正在发送数据给对方（第二方向）…");
                long sent = Send(writer, duration);
                Measure outgoing = ReadMeasure(reader);
                if (sent != outgoing.Bytes) throw new IOException("对方确认的接收字节数与实际发送不符");
                Completed(outgoing, incoming);
                Finished("测速完成\r\n\r\n本机 → 对方\r\n" + outgoing + "\r\n\r\n对方 → 本机\r\n" + incoming);
            }
            catch (Exception ex) { if (!stopping) Finished("测速未完成：" + ex.Message + "\r\n请保持两端程序打开后重试。"); }
            finally { Untrack(client); Interlocked.Exchange(ref busy, 0); }
        }
        public bool Start(string target, int port, int duration)
        {
            IPAddress address;
            if (!IPAddress.TryParse(target, out address) || !Allowed(address)) throw new ArgumentException("仅允许预设两台电脑和本机测试地址");
            if (duration < 2000 || duration > 10000) throw new ArgumentException("测速时长无效");
            if (Interlocked.CompareExchange(ref busy, 1, 0) != 0) return false;
            new Thread(delegate()
            {
                var client = new TcpClient(AddressFamily.InterNetwork);
                try
                {
                    Track(client); Configure(client);
                    Status("正在连接对方…");
                    IAsyncResult pending = client.BeginConnect(address, port, null, null);
                    using (pending.AsyncWaitHandle)
                    {
                        if (!pending.AsyncWaitHandle.WaitOne(4000)) throw new IOException("无法连接。请先在对方双击打开测速工具，并允许专用网络访问。");
                        client.EndConnect(pending);
                    }
                    NetworkStream stream = client.GetStream();
                    var reader = new BinaryReader(stream, Encoding.UTF8);
                    var writer = new BinaryWriter(stream, Encoding.UTF8);
                    writer.Write(Encoding.ASCII.GetBytes(Magic)); writer.Write(duration); writer.Flush();
                    byte accepted = reader.ReadByte();
                    if (accepted != 1) throw new IOException(accepted == 2 ? "对方正在测速，请稍后重试" : "对方握手无效");
                    Status("正在发送数据给对方（第一方向，约 9 秒）…");
                    long sent = Send(writer, duration);
                    Measure outgoing = ReadMeasure(reader);
                    if (sent != outgoing.Bytes) throw new IOException("对方确认的接收字节数与实际发送不符");
                    Status("正在接收对方数据（第二方向，约 9 秒）…");
                    Measure incoming = Receive(reader, duration);
                    WriteMeasure(writer, incoming);
                    Completed(outgoing, incoming);
                    Finished("测速完成\r\n\r\n本机 → 对方\r\n" + outgoing + "\r\n\r\n对方 → 本机\r\n" + incoming);
                }
                catch (Exception ex) { if (!stopping) Finished("测速未完成：" + ex.Message); }
                finally { Untrack(client); Interlocked.Exchange(ref busy, 0); }
            }) { IsBackground = true }.Start();
            return true;
        }
        long Send(BinaryWriter writer, int duration)
        {
            byte[] data = new byte[Chunk];
            using (var random = RandomNumberGenerator.Create()) random.GetBytes(data);
            Stopwatch clock = Stopwatch.StartNew();
            long total = 0;
            while (!stopping && clock.ElapsedMilliseconds < duration && total < MaximumBytes)
            {
                writer.Write(data.Length); writer.Write(data); total += data.Length;
            }
            if (stopping) throw new OperationCanceledException();
            writer.Write(0); writer.Flush();
            return total;
        }
        Measure Receive(BinaryReader reader, int duration)
        {
            byte[] buffer = new byte[Chunk];
            long total = 0;
            long[] seconds = new long[20];
            Stopwatch clock = Stopwatch.StartNew();
            while (!stopping)
            {
                if (clock.ElapsedMilliseconds > duration + 5000) throw new IOException("接收超过时限");
                int length = reader.ReadInt32();
                if (length == 0) break;
                if (length < 1 || length > Chunk || total + length > MaximumBytes) throw new IOException("收到超出限制的数据块");
                int offset = 0;
                while (offset < length)
                {
                    int got = reader.Read(buffer, offset, length - offset);
                    if (got == 0) throw new EndOfStreamException("对方断开连接");
                    offset += got; total += got;
                    int second = (int)(clock.ElapsedMilliseconds / 1000);
                    if (second >= seconds.Length) throw new IOException("接收超过时限");
                    seconds[second] += got;
                }
            }
            if (stopping) throw new OperationCanceledException();
            long elapsed = Math.Max(1, clock.ElapsedMilliseconds);
            double min = Double.MaxValue;
            int fullSeconds = Math.Min(seconds.Length, (int)(elapsed / 1000));
            for (int i = 0; i < fullSeconds; ++i) min = Math.Min(min, seconds[i] * 8.0 / 1000000.0);
            return new Measure { Bytes = total, Milliseconds = elapsed, MinimumMbps = min == Double.MaxValue ? 0 : min };
        }
        static byte[] ReadExact(BinaryReader reader, int length)
        {
            byte[] data = reader.ReadBytes(length);
            if (data.Length != length) throw new EndOfStreamException("对方断开连接");
            return data;
        }
        static void WriteMeasure(BinaryWriter writer, Measure value)
        {
            writer.Write(value.Bytes); writer.Write(value.Milliseconds); writer.Write(value.MinimumMbps); writer.Flush();
        }
        static Measure ReadMeasure(BinaryReader reader)
        {
            var value = new Measure { Bytes = reader.ReadInt64(), Milliseconds = reader.ReadInt64(), MinimumMbps = reader.ReadDouble() };
            if (value.Bytes <= 0 || value.Bytes > MaximumBytes || value.Milliseconds < 1 || value.Milliseconds > 16000 || Double.IsNaN(value.MinimumMbps) || Double.IsInfinity(value.MinimumMbps) || value.MinimumMbps < 0) throw new IOException("对方返回的测速结果无效");
            return value;
        }
        public void Dispose()
        {
            stopping = true;
            if (listener != null) listener.Stop();
            lock (gate) { foreach (TcpClient client in clients) client.Close(); clients.Clear(); }
        }
    }

    internal sealed class MainWindow : Form
    {
        readonly Engine engine = new Engine();
        readonly Label state = new Label();
        readonly TextBox result = new TextBox();
        readonly Button start = new Button();
        string target;
        public MainWindow()
        {
            Text = "LAN3DShare · 局域网测速";
            Font = new Font("Microsoft YaHei UI", 10);
            ClientSize = new Size(650, 480);
            MinimumSize = new Size(600, 440);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(248, 249, 251);
            var addresses = new List<string>();
            foreach (NetworkInterface adapter in NetworkInterface.GetAllNetworkInterfaces())
                if (adapter.OperationalStatus == OperationalStatus.Up)
                    foreach (UnicastIPAddressInformation ip in adapter.GetIPProperties().UnicastAddresses)
                        if (ip.Address.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(ip.Address)) addresses.Add(ip.Address.ToString());
            target = addresses.Contains("192.168.3.197") ? "192.168.3.42" : "192.168.3.197";
            var title = new Label { Text = "测一下两台电脑之间能跑多快", Font = new Font(Font.FontFamily, 17, FontStyle.Bold), AutoSize = true, Location = new Point(24, 20) };
            var devices = new Label { Text = "本机：" + Environment.MachineName + "  " + String.Join(" / ", addresses.ToArray()) + "\r\n对方：" + (target.EndsWith("197") ? "Huangwei-Gaming" : "PureDark-Gaming") + "  " + target, Location = new Point(24, 65), Size = new Size(600, 64), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            state.Text = "已就绪。对方打开同一工具后，任一端点击开始测速。";
            state.Location = new Point(24, 130); state.Size = new Size(600, 42); state.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            start.Text = "开始测速"; start.Location = new Point(24, 177); start.Size = new Size(140, 40);
            start.Click += delegate { try { if (!engine.Start(target, 48732, 9000)) state.Text = "正在测速，请等待完成。"; } catch (Exception ex) { state.Text = ex.Message; } };
            result.Location = new Point(24, 235); result.Size = new Size(600, 177); result.ReadOnly = true; result.Multiline = true; result.ScrollBars = ScrollBars.Vertical; result.BackColor = Color.White; result.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            result.Text = "两方向各约 9 秒，使用一条 TCP 连接。\r\n按实际接收字节统计，不在磁盘上读写测速数据。\r\n首次打开如出现 Windows 网络提示，请允许专用网络访问。";
            var foot = new Label { Text = "结果会自动保存到测速日志。关闭窗口即可取消测速。", Location = new Point(24, 432), Size = new Size(600, 26), Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right, ForeColor = Color.DimGray };
            Controls.AddRange(new Control[] { title, devices, state, start, result, foot });
            engine.Status = delegate(string text) { Post(delegate { state.Text = text; }); };
            engine.Finished = delegate(string text) { Log(text); Post(delegate { result.Text = text; state.Text = text.StartsWith("测速完成") ? "测速完成，可再次测量。" : "测速未完成，请查看下方信息。"; }); };
            var timer = new System.Windows.Forms.Timer { Interval = 250 };
            timer.Tick += delegate { start.Enabled = !engine.IsBusy; }; timer.Start();
            Shown += delegate { try { engine.Listen(48732, false); } catch (Exception ex) { state.Text = "无法开启监听：" + ex.Message; start.Enabled = false; timer.Stop(); } };
            FormClosed += delegate { timer.Stop(); timer.Dispose(); engine.Dispose(); };
        }
        void Post(Action action) { if (!IsDisposed && IsHandleCreated) { try { BeginInvoke(action); } catch (InvalidOperationException) {} } }
        static void Log(string text)
        {
            string entry = DateTimeOffset.Now.ToString("o") + "  " + Environment.MachineName + "\r\n" + text + "\r\n\r\n";
            try { File.AppendAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "LAN3DBandwidth.log"), entry, Encoding.UTF8); }
            catch { try { string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LAN3DShare"); Directory.CreateDirectory(path); File.AppendAllText(Path.Combine(path, "LAN3DBandwidth.log"), entry, Encoding.UTF8); } catch {} }
        }
    }

    internal static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            // Internal automation entry: the remote user still starts the ordinary GUI.
            if (args.Length == 3 && args[0] == "--measure-peer")
            {
                string peer = args[1];
                if (peer != "192.168.3.42" && peer != "192.168.3.197") return 3;
                string output = Path.GetFullPath(args[2]);
                using (var complete = new ManualResetEvent(false))
                using (var engine = new Engine())
                {
                    int code = 1;
                    engine.Completed = delegate(Measure outgoing, Measure incoming)
                    {
                        string report = "PASS\r\nUTC=" + DateTime.UtcNow.ToString("o") + "\r\nLOCAL=" + Environment.MachineName + "\r\nPEER=" + peer +
                            "\r\nOUT_BYTES=" + outgoing.Bytes + "\r\nOUT_MS=" + outgoing.Milliseconds +
                            "\r\nOUT_MBPS=" + outgoing.Mbps.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) +
                            "\r\nOUT_MIN_SECOND_MBPS=" + outgoing.MinimumMbps.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) +
                            "\r\nIN_BYTES=" + incoming.Bytes + "\r\nIN_MS=" + incoming.Milliseconds +
                            "\r\nIN_MBPS=" + incoming.Mbps.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) +
                            "\r\nIN_MIN_SECOND_MBPS=" + incoming.MinimumMbps.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) + "\r\n\r\n本机 → 对方\r\n" + outgoing + "\r\n\r\n对方 → 本机\r\n" + incoming;
                        File.WriteAllText(output, report, Encoding.UTF8);
                        code = 0;
                    };
                    engine.Finished = delegate(string message)
                    {
                        if (code != 0) { try { File.WriteAllText(output, "FAIL\r\n" + message, Encoding.UTF8); } catch {} }
                        complete.Set();
                    };
                    engine.Start(peer, 48732, 9000);
                    if (!complete.WaitOne(28000))
                    {
                        try { File.WriteAllText(output, "FAIL timeout after 28 seconds", Encoding.UTF8); } catch {}
                        return 2;
                    }
                    return code;
                }
            }
            if (args.Length == 3 && (args[0] == "--selftest-server" || args[0] == "--selftest-client"))
            {
                int port = Int32.Parse(args[1]);
                string path = Path.GetFullPath(args[2]);
                using (var complete = new ManualResetEvent(false))
                using (var engine = new Engine())
                {
                    int code = 1;
                    engine.Completed = delegate(Measure outgoing, Measure incoming) { code = 0; File.WriteAllText(path, "PASS\r\nOUT_BYTES=" + outgoing.Bytes + "\r\nIN_BYTES=" + incoming.Bytes + "\r\nOUT_MS=" + outgoing.Milliseconds + "\r\nIN_MS=" + incoming.Milliseconds + "\r\n", Encoding.UTF8); };
                    engine.Finished = delegate(string text) { if (code != 0) File.WriteAllText(path, "FAIL\r\n" + text, Encoding.UTF8); complete.Set(); };
                    if (args[0] == "--selftest-server") { engine.Listen(port, true); File.WriteAllText(path + ".ready", "ready"); }
                    else engine.Start("127.0.0.1", port, 2500);
                    if (!complete.WaitOne(15000)) { File.WriteAllText(path, "FAIL timeout"); return 2; }
                    return code;
                }
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainWindow());
            return 0;
        }
    }
}
