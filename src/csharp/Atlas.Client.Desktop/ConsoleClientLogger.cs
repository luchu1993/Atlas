using System;
using Atlas.Client;

namespace Atlas.Client.Desktop;

// Info -> stdout, Warn/Error -> stderr; matches the pre-injection ClientLog routing
// so the world_stress harness's stdout tap keeps seeing only Info lines.
public sealed class ConsoleClientLogger : IClientLogger
{
    public void Info(string message)  => Console.WriteLine(message);
    public void Warn(string message)  => Console.Error.WriteLine(message);
    public void Error(string message) => Console.Error.WriteLine(message);
}
