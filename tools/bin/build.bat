@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\build.py" %*
) else (
  py "%~dp0..\build.py" %*
)
exit /b %errorlevel%
