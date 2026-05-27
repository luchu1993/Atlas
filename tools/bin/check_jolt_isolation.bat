@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\check_jolt_isolation.py" %*
) else (
  py "%~dp0..\check_jolt_isolation.py" %*
)
exit /b %errorlevel%
