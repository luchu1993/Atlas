@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\check_recast_isolation.py" %*
) else (
  py "%~dp0..\check_recast_isolation.py" %*
)
exit /b %errorlevel%
