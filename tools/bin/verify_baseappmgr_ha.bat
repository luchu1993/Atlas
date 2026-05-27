@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\cluster_control\verify_baseappmgr_ha.py" %*
exit /b %errorlevel%
