@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\run_mvp_unity_bots.py" %*
) else (
  py "%~dp0..\run_mvp_unity_bots.py" %*
)
exit /b %errorlevel%
