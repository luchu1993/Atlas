@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\cluster_control\run_unreliable_recovery.py" %*
exit /b %errorlevel%
