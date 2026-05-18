using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;

namespace Atlas.Client.IntegrationTests;

// One cluster per test class via IClassFixture; teardown closes the
// launcher's stdin so it cleans up subprocesses on its own.
public sealed class RealClusterFixture : IDisposable
{
    private Process? _launcher;
    private readonly StringBuilder _stderrBuf = new();
    private readonly StringBuilder _stdoutBuf = new();
    public ushort LoginAppPort { get; private set; }
    public ushort BaseAppExternalPort { get; private set; }

    public RealClusterFixture()
    {
        var repoRoot = ResolveRepoRoot();
        var launcher = Path.Combine(repoRoot, "tools", "cluster_control", "test_cluster.py");
        if (!File.Exists(launcher))
            throw new FileNotFoundException("test_cluster.py not found", launcher);

        _launcher = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = ResolvePython(),
                Arguments = $"\"{launcher}\"",
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = repoRoot,
            },
        };

        // Async stderr drain so a chatty launcher can't deadlock on a full
        // OS pipe; the buffer feeds diagnostics on the timeout path below.
        _launcher.ErrorDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            lock (_stderrBuf) _stderrBuf.AppendLine(e.Data);
        };
        _launcher.Start();
        _launcher.BeginErrorReadLine();

        // Outer deadline ≥ launcher's per-process registration timeout × N
        // so a slow cold-start cluster (dbapp first-init dominates) still fits.
        var deadline = DateTime.UtcNow.AddSeconds(180);
        string? line = null;
        while (DateTime.UtcNow < deadline)
        {
            line = _launcher.StandardOutput.ReadLine();
            if (line is null) break;
            _stdoutBuf.AppendLine(line);
            if (line.StartsWith("READY ", StringComparison.Ordinal))
            {
                ParseReady(line);
                // test_cluster.py emits READY as soon as machined acknowledges
                // every process's registration, but peer-to-peer RUDP channels
                // (loginapp ↔ dbapp etc.) are set up via async Birth events
                // that fire after registration. Give them a moment to settle
                // so the first login request doesn't hit "no DBApp connection".
                Thread.Sleep(1500);
                return;
            }
        }

        // Failure path: collect everything we know. Wait briefly for the
        // launcher to actually exit so ExitCode is meaningful + stderr has
        // settled (BeginErrorReadLine flushes on process exit).
        _launcher.WaitForExit(2_000);
        string stderr;
        lock (_stderrBuf) stderr = _stderrBuf.ToString();
        int exitCode = _launcher.HasExited ? _launcher.ExitCode : -1;
        TeardownAndThrow(
            $"test_cluster.py never reported READY; exit_code={exitCode}; " +
            $"last stdout line: {line ?? "<EOF>"}\n" +
            $"--- stdout transcript ---\n{_stdoutBuf}" +
            $"--- stderr transcript ---\n{stderr}");
    }

    private void ParseReady(string line)
    {
        foreach (var tok in line.AsSpan(6).ToString().Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = tok.IndexOf('=');
            if (eq <= 0) continue;
            var key = tok.AsSpan(0, eq);
            var value = tok.AsSpan(eq + 1);
            if (key.SequenceEqual("loginapp_port")) LoginAppPort = ushort.Parse(value);
            else if (key.SequenceEqual("baseapp_external_port")) BaseAppExternalPort = ushort.Parse(value);
        }
        if (LoginAppPort == 0)
            TeardownAndThrow("READY line missing loginapp_port");
    }

    private void TeardownAndThrow(string message)
    {
        Dispose();
        throw new InvalidOperationException(message);
    }

    public void Dispose()
    {
        if (_launcher is null) return;
        try
        {
            // Closing stdin signals test_cluster.py's readline loop to exit;
            // its finally clause then runs stop_logged_processes.
            _launcher.StandardInput.Close();
            if (!_launcher.WaitForExit(15_000))
            {
                _launcher.Kill(entireProcessTree: true);
                _launcher.WaitForExit(5_000);
            }
        }
        catch { /* teardown is best-effort */ }
        _launcher.Dispose();
        _launcher = null;
    }

    private static string ResolveRepoRoot()
    {
        // tests/csharp/Atlas.Client.IntegrationTests/bin/Release/<tfm>/ → repo root.
        // BaseDirectory carries a trailing slash so the first GetDirectoryName
        // is a no-op; bump the hop budget to 8 so the loop still reaches Atlas/.
        var dir = AppContext.BaseDirectory;
        for (int i = 0; i < 8 && dir is not null; i++)
        {
            if (Directory.Exists(Path.Combine(dir, "tools", "cluster_control")))
                return dir;
            dir = Path.GetDirectoryName(dir);
        }
        throw new DirectoryNotFoundException("Cannot locate repo root from " + AppContext.BaseDirectory);
    }

    private static string ResolvePython()
    {
        var pyEnv = Environment.GetEnvironmentVariable("ATLAS_PYTHON");
        if (!string.IsNullOrEmpty(pyEnv)) return pyEnv;
        return OperatingSystem.IsWindows() ? "python" : "python3";
    }
}
