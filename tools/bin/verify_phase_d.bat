@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\cluster_control\verify_phase_d.py" %*
) else (
  py "%~dp0..\cluster_control\verify_phase_d.py" %*
)
exit /b %errorlevel%
