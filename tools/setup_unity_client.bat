@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0setup_unity_client.py" %*
) else (
  py "%~dp0setup_unity_client.py" %*
)
exit /b %errorlevel%
