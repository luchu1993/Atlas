@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\cluster_control\test_cluster.py" %*
exit /b %errorlevel%
