#!/usr/bin/env bash
exec dotnet run --project "$(dirname "$0")/../../src/csharp/Atlas.Tools.CppEmitter/Atlas.Tools.CppEmitter.csproj" --verbosity quiet -- "$@"
