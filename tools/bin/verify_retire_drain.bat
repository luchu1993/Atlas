@echo off
setlocal
python "%~dp0..\cluster_control\verify_retire_drain.py" %*
