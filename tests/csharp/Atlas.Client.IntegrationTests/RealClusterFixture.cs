using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace Atlas.Client.IntegrationTests;

// One cluster per test class via IClassFixture; teardown closes the
// launcher's stdin so it cleans up subprocesses on its own.
public sealed class RealClusterFixture : IDisposable
{
    private Process? _launcher;
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
        _launcher.Start();

        // Block on stdout READY line; if launcher dies first the read returns null.
        var deadline = DateTime.UtcNow.AddSeconds(60);
        string? line = null;
        while (DateTime.UtcNow < deadline)
        {
            line = _launcher.StandardOutput.ReadLine();
            if (line is null) break;
            if (line.StartsWith("READY ", StringComparison.Ordinal))
            {
                ParseReady(line);
                return;
            }
        }
        TeardownAndThrow($"test_cluster.py never reported READY; last line: {line ?? "<EOF>"}");
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
        // tests/csharp/Atlas.Client.IntegrationTests/bin/Debug/net9.0/ → repo root.
        var dir = AppContext.BaseDirectory;
        for (int i = 0; i < 6 && dir is not null; i++)
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
