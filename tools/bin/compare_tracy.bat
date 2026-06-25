@echo off
where /q python && (set "PY=python") || (set "PY=py")
%PY% "%~dp0..\profile\compare_tracy.py" %*
exit /b %errorlevel%
