@echo off
setlocal
set BUILD=build\debug
set CONFIG=Debug
set LOGIN_PORT=20018
set EXTRA=

:parse
if "%~1"=="" goto run
if /i "%~1"=="--build" (set BUILD=%~2 & shift & shift & goto parse)
if /i "%~1"=="--config" (set CONFIG=%~2 & shift & shift & goto parse)
if /i "%~1"=="--login-port" (set LOGIN_PORT=%~2 & shift & shift & goto parse)
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage
set EXTRA=%EXTRA% %~1
shift
goto parse

:usage
echo Usage: %~nx0 [--build DIR] [--config CFG] [--login-port N] [extra args...]
echo   --build       build directory (default: build\debug)
echo   --config      CMake configuration (default: Debug)
echo   --login-port  external LoginApp port (default: 20018)
exit /b 0

:run
for %%I in ("%BUILD%") do set BIN_NAME=%%~nxI
set REPO_ROOT=%~dp0..\..
set BASE_DLL=%REPO_ROOT%\bin\%BIN_NAME%\Atlas.Mvp.Base.dll
set CELL_DLL=%REPO_ROOT%\bin\%BIN_NAME%\Atlas.Mvp.Cell.dll

python "%~dp0..\cluster_control\run_world_stress.py" ^
    --build-dir       "%BUILD%" ^
    --config          "%CONFIG%" ^
    --baseapp-count   1 ^
    --cellapp-count   1 ^
    --login-port      "%LOGIN_PORT%" ^
    --base-assembly   "%BASE_DLL%" ^
    --cell-assembly   "%CELL_DLL%" ^
    --cellapp-update-hertz 20 ^
    --baseapp-update-hertz 20 ^
    --clients         0 ^
    --keep-cluster %EXTRA%
