@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\run_mvp_ue.py" %*
) else (
  py "%~dp0..\run_mvp_ue.py" %*
)
exit /b %errorlevel%
