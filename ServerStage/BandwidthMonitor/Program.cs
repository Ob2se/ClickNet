using System.Drawing.Drawing2D;
using System.Diagnostics;
using System.Text.Json;

namespace BandwidthMonitor;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        string defaultPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "bandwidth-live.json"));
        string path = args.Length >= 2 && args[0] == "--file" ? Path.GetFullPath(args[1]) : defaultPath;
        Application.Run(new MonitorForm(path));
    }
}

internal sealed class Snapshot
{
    public int SchemaVersion { get; set; }
    public string UpdatedUtc { get; set; } = "";
    public bool ServerRunning { get; set; }
    public uint ServerTick { get; set; }
    public int PlayerCount { get; set; }
    public List<PlayerStats> Players { get; set; } = [];
}

internal sealed class PlayerStats
{
    public uint Id { get; set; }
    public string Name { get; set; } = "";
    public int PingMs { get; set; }
    public float InBytesPerSecond { get; set; }
    public float OutBytesPerSecond { get; set; }
    public float InPacketsPerSecond { get; set; }
    public float OutPacketsPerSecond { get; set; }
    public ulong PayloadBytesIn { get; set; }
    public ulong PayloadBytesOut { get; set; }
}

internal sealed class MonitorForm : Form
{
    private readonly Label fileLabel = new();
    private readonly Label summaryLabel = new();
    private readonly Label tickLabel = new();
    private readonly Label statusLabel = new();
    private readonly DataGridView grid = new();
    private readonly RateGraph graph = new();
    private readonly System.Windows.Forms.Timer timer = new();
    private readonly JsonSerializerOptions jsonOptions = new() { PropertyNameCaseInsensitive = true };
    private string filePath;
    private uint lastGraphedTick;
    private readonly Queue<(long Time, uint Tick)> tickSamples = new();
    private uint? lastRateTick;

    public MonitorForm(string path)
    {
        filePath = path;
        Text = "ClickNet Bandwidth Monitor";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(900, 560);
        Size = new Size(1120, 700);
        BackColor = Color.FromArgb(24, 28, 35);
        ForeColor = Color.WhiteSmoke;

        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4, Padding = new Padding(16) };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 62));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 54));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 184));
        Controls.Add(layout);

        var top = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3 };
        top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
        top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 130));
        fileLabel.Dock = DockStyle.Fill;
        fileLabel.AutoEllipsis = true;
        fileLabel.TextAlign = ContentAlignment.MiddleLeft;
        fileLabel.ForeColor = Color.Gainsboro;
        fileLabel.Text = filePath;
        var browse = MakeButton("Browse file");
        browse.Click += (_, _) => BrowseFile();
        var history = MakeButton("History CSV");
        history.Click += (_, _) => OpenHistory();
        top.Controls.Add(fileLabel, 0, 0);
        top.Controls.Add(browse, 1, 0);
        top.Controls.Add(history, 2, 0);
        layout.Controls.Add(top, 0, 0);

        var summary = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3 };
        summary.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        summary.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 170));
        summary.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 220));
        summaryLabel.Dock = DockStyle.Fill;
        summaryLabel.Font = new Font(Font.FontFamily, 15, FontStyle.Bold);
        summaryLabel.TextAlign = ContentAlignment.MiddleLeft;
        summaryLabel.Text = "Waiting for server data";
        tickLabel.Dock = DockStyle.Fill;
        tickLabel.TextAlign = ContentAlignment.MiddleRight;
        tickLabel.ForeColor = Color.Gainsboro;
        tickLabel.Text = "Tick rate --";
        statusLabel.Dock = DockStyle.Fill;
        statusLabel.TextAlign = ContentAlignment.MiddleRight;
        statusLabel.ForeColor = Color.Silver;
        summary.Controls.Add(summaryLabel, 0, 0);
        summary.Controls.Add(tickLabel, 1, 0);
        summary.Controls.Add(statusLabel, 2, 0);
        layout.Controls.Add(summary, 0, 1);

        grid.Dock = DockStyle.Fill;
        grid.ReadOnly = true;
        grid.AllowUserToAddRows = false;
        grid.AllowUserToDeleteRows = false;
        grid.AllowUserToResizeRows = false;
        grid.RowHeadersVisible = false;
        grid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
        grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill;
        grid.BackgroundColor = Color.FromArgb(31, 36, 44);
        grid.GridColor = Color.FromArgb(58, 65, 75);
        grid.BorderStyle = BorderStyle.None;
        grid.EnableHeadersVisualStyles = false;
        grid.ColumnHeadersDefaultCellStyle.BackColor = Color.FromArgb(42, 49, 59);
        grid.ColumnHeadersDefaultCellStyle.ForeColor = Color.WhiteSmoke;
        grid.DefaultCellStyle.BackColor = Color.FromArgb(31, 36, 44);
        grid.DefaultCellStyle.ForeColor = Color.WhiteSmoke;
        grid.DefaultCellStyle.SelectionBackColor = Color.FromArgb(55, 93, 130);
        grid.DefaultCellStyle.SelectionForeColor = Color.White;
        grid.Columns.Add("name", "Player");
        grid.Columns.Add("id", "ID");
        grid.Columns.Add("ping", "Ping ms");
        grid.Columns.Add("inRate", "In KB/s");
        grid.Columns.Add("outRate", "Out KB/s");
        grid.Columns.Add("inPackets", "In pkt/s");
        grid.Columns.Add("outPackets", "Out pkt/s");
        grid.Columns.Add("totalIn", "Payload in MB");
        grid.Columns.Add("totalOut", "Payload out MB");
        grid.Columns[0].FillWeight = 160;
        layout.Controls.Add(grid, 0, 2);

        graph.Dock = DockStyle.Fill;
        layout.Controls.Add(graph, 0, 3);

        timer.Interval = 250;
        timer.Tick += (_, _) => RefreshSnapshot();
        timer.Start();
        RefreshSnapshot();
    }

    private Button MakeButton(string label) => new()
    {
        Text = label, Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat,
        BackColor = Color.FromArgb(47, 65, 82), ForeColor = Color.WhiteSmoke,
        Margin = new Padding(6, 10, 6, 10), Cursor = Cursors.Hand
    };

    private void BrowseFile()
    {
        using var dialog = new OpenFileDialog { Filter = "Bandwidth snapshot|*.json|All files|*.*", FileName = filePath };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        filePath = dialog.FileName;
        fileLabel.Text = filePath;
        lastGraphedTick = 0;
        tickSamples.Clear();
        lastRateTick = null;
        tickLabel.Text = "Tick rate --";
        graph.Clear();
        RefreshSnapshot();
    }

    private void OpenHistory()
    {
        string historyPath = Path.ChangeExtension(filePath, ".csv");
        if (!File.Exists(historyPath))
        {
            MessageBox.Show(this, $"History will appear after the server records a player:\n{historyPath}",
                "History not found", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(historyPath) { UseShellExecute = true });
    }

    private void RefreshSnapshot()
    {
        try
        {
            if (!File.Exists(filePath))
            {
                statusLabel.Text = "Waiting for bandwidth-live.json";
                tickLabel.Text = "Tick rate --";
                return;
            }
            string json = File.ReadAllText(filePath);
            Snapshot? snapshot = JsonSerializer.Deserialize<Snapshot>(json, jsonOptions);
            if (snapshot is null || snapshot.SchemaVersion != 1) throw new JsonException("Unsupported snapshot schema");
            var players = snapshot.Players.OrderBy(p => p.Id).ToList();
            grid.Rows.Clear();
            foreach (PlayerStats player in players)
            {
                grid.Rows.Add(player.Name, player.Id, player.PingMs < 0 ? "—" : player.PingMs.ToString(),
                    (player.InBytesPerSecond / 1024f).ToString("N1"),
                    (player.OutBytesPerSecond / 1024f).ToString("N1"),
                    player.InPacketsPerSecond.ToString("N1"), player.OutPacketsPerSecond.ToString("N1"),
                    (player.PayloadBytesIn / 1048576d).ToString("N2"),
                    (player.PayloadBytesOut / 1048576d).ToString("N2"));
            }
            float totalIn = players.Sum(p => p.InBytesPerSecond);
            float totalOut = players.Sum(p => p.OutBytesPerSecond);
            summaryLabel.Text = $"{players.Count} players     In {totalIn / 1024f:N1} KB/s     Out {totalOut / 1024f:N1} KB/s";
            if (snapshot.ServerTick != lastGraphedTick)
            {
                graph.Add(totalIn / 1024f, totalOut / 1024f);
                lastGraphedTick = snapshot.ServerTick;
            }
            bool fresh = DateTimeOffset.TryParse(snapshot.UpdatedUtc, out var updated)
                && DateTimeOffset.UtcNow - updated < TimeSpan.FromSeconds(3);
            UpdateTickRate(snapshot, fresh);
            statusLabel.Text = !snapshot.ServerRunning ? "Server stopped" : fresh
                ? $"Live · {updated.ToLocalTime():HH:mm:ss}" : "Waiting for new samples";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            statusLabel.Text = $"Cannot read file: {error.Message}";
        }
    }

    private void UpdateTickRate(Snapshot snapshot, bool fresh)
    {
        if (!snapshot.ServerRunning || !fresh)
        {
            tickSamples.Clear();
            lastRateTick = null;
            tickLabel.Text = snapshot.ServerRunning ? "Tick rate --" : "Tick rate 0.0/s";
            return;
        }

        long now = Stopwatch.GetTimestamp();
        if (lastRateTick is uint previous && snapshot.ServerTick < previous)
        {
            tickSamples.Clear(); // A new server session started.
            lastRateTick = null;
        }
        if (lastRateTick != snapshot.ServerTick)
        {
            tickSamples.Enqueue((now, snapshot.ServerTick));
            lastRateTick = snapshot.ServerTick;
        }
        while (tickSamples.Count > 2
            && Stopwatch.GetElapsedTime(tickSamples.Peek().Time, now).TotalSeconds > 1.5)
            tickSamples.Dequeue();

        if (tickSamples.Count < 2)
        {
            tickLabel.Text = "Tick rate --";
            return;
        }
        var oldest = tickSamples.Peek();
        var newest = tickSamples.Last();
        double seconds = Stopwatch.GetElapsedTime(oldest.Time, newest.Time).TotalSeconds;
        double rate = seconds > 0 ? (newest.Tick - oldest.Tick) / seconds : 0;
        if (Stopwatch.GetElapsedTime(newest.Time, now).TotalSeconds > 2) rate = 0;
        tickLabel.Text = $"Tick rate {rate:N1}/s";
    }
}

internal sealed class RateGraph : Control
{
    private readonly List<(float In, float Out)> samples = [];
    private const int Capacity = 120;

    public RateGraph()
    {
        DoubleBuffered = true;
        BackColor = Color.FromArgb(31, 36, 44);
        ForeColor = Color.Gainsboro;
        Margin = new Padding(0, 12, 0, 0);
    }

    public void Clear() { samples.Clear(); Invalidate(); }
    public void Add(float inbound, float outbound)
    {
        samples.Add((Math.Max(0, inbound), Math.Max(0, outbound)));
        if (samples.Count > Capacity) samples.RemoveAt(0);
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        const int left = 48, right = 14, top = 30, bottom = 20;
        var plot = new Rectangle(left, top, Math.Max(1, Width - left - right), Math.Max(1, Height - top - bottom));
        using var muted = new SolidBrush(Color.Silver);
        using var gridPen = new Pen(Color.FromArgb(65, 72, 81));
        using var inPen = new Pen(Color.FromArgb(83, 203, 180), 2);
        using var outPen = new Pen(Color.FromArgb(250, 179, 91), 2);
        using var inBrush = new SolidBrush(inPen.Color);
        using var outBrush = new SolidBrush(outPen.Color);
        e.Graphics.DrawString("Traffic · KB/s", Font, muted, 12, 8);
        e.Graphics.DrawString("In", Font, inBrush, Width - 100, 8);
        e.Graphics.DrawString("Out", Font, outBrush, Width - 55, 8);
        float max = Math.Max(1, samples.SelectMany(s => new[] { s.In, s.Out }).DefaultIfEmpty(0).Max() * 1.2f);
        for (int row = 0; row <= 3; ++row)
        {
            float y = plot.Top + plot.Height * row / 3f;
            e.Graphics.DrawLine(gridPen, plot.Left, y, plot.Right, y);
            e.Graphics.DrawString((max * (3 - row) / 3f).ToString("N1"), Font, muted, 3, y - 7);
        }
        if (samples.Count < 2) return;
        for (int i = 1; i < samples.Count; ++i)
        {
            float x0 = plot.Left + plot.Width * (i - 1) / (float)(Capacity - 1);
            float x1 = plot.Left + plot.Width * i / (float)(Capacity - 1);
            e.Graphics.DrawLine(inPen, x0, plot.Bottom - plot.Height * samples[i - 1].In / max,
                x1, plot.Bottom - plot.Height * samples[i].In / max);
            e.Graphics.DrawLine(outPen, x0, plot.Bottom - plot.Height * samples[i - 1].Out / max,
                x1, plot.Bottom - plot.Height * samples[i].Out / max);
        }
    }
}
