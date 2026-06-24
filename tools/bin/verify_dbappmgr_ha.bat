@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\cluster_control\verify_dbappmgr_ha.py" %*
exit /b %errorlevel%
