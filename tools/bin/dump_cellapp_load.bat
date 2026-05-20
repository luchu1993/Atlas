@echo off
where /q python
if not errorlevel 1 (
  python "%~dp0..\cluster_control\dump_cellapp_load.py" %*
) else (
  py "%~dp0..\cluster_control\dump_cellapp_load.py" %*
)
exit /b %errorlevel%
