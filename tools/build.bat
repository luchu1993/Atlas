@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0build.py" %*
) else (
  py "%~dp0build.py" %*
)
exit /b %errorlevel%
\r