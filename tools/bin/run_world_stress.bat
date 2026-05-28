@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\cluster_control\run_world_stress.py" %*
exit /b %errorlevel%
