@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\def_id.py" %*
) else (
  py "%~dp0..\def_id.py" %*
)
exit /b %errorlevel%
