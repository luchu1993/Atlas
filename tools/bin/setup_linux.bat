@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\setup_linux.py" %*
) else (
  py "%~dp0..\setup_linux.py" %*
)
exit /b %errorlevel%
