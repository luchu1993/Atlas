@echo off
where /q python && (set "PY=python") || (set "PY=py")

%PY% "%~dp0..\cluster_control\run_world_stress.py" ^
  --build-dir build/profile ^
  --config RelWithDebInfo ^
  --clients 200 ^
  --account-pool 200 ^
  --duration-sec 120 ^
  --ramp-per-sec 20 ^
  --rpc-rate-hz 2 ^
  --move-rate-hz 10 ^
  --spread-radius 500 ^
  --walk-step-meters 5 ^
  --walk-range-meters 200 ^
  --teleport-pct 5 ^
  --space-count 1 ^
  --shortline-pct 0 ^
  --hold-min-ms 5000 ^
  --hold-max-ms 30000 ^
  --login-rate-limit-per-ip 0 ^
  --login-rate-limit-global 10000 ^
  --capture-dir "%~dp0..\..\.tmp\prof\baseline" ^
  --capture-procs "loginapp,dbapp,baseappmgr,baseapp,cellappmgr,cellapp" ^
  %*
exit /b %errorlevel%
