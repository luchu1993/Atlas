@echo off
dotnet run --project "%~dp0..\..\src\csharp\Atlas.Tools.CppEmitter\Atlas.Tools.CppEmitter.csproj" --verbosity quiet -- %*
