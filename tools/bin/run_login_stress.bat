@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\cluster_control\run_login_stress.py" %*
exit /b %errorlevel%
