#Requires -Version 5.1
<#
.SYNOPSIS
    Run a world_stress baseline against the profile build.

.DESCRIPTION
    Wraps run_world_stress.py with the standard baseline parameters.
    Prerequisites: cmake --build build/profile --config RelWithDebInfo

.PARAMETER Clients
    Number of virtual clients (default: 100).

.PARAMETER DurationSec
    Stress duration in seconds (default: 120).

.EXAMPLE
    .\tools\cluster_control\run_baseline_profile.ps1
    .\tools\cluster_control\run_baseline_profile.ps1 -Clients 50 -DurationSec 60
#>
param(
    [int]$Clients     = 100,
    [int]$DurationSec = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot  = (Resolve-Path "$PSScriptRoot\..\..")
$Script    = "$PSScriptRoot\run_world_stress.py"

python $Script `
    --build-dir             build/profile `
    --config                RelWithDebInfo `
    --clients               $Clients `
    --account-pool          $Clients `
    --duration-sec          $DurationSec `
    --ramp-per-sec          20 `
    --rpc-rate-hz           2 `
    --move-rate-hz          10 `
    --spread-radius         200 `
    --space-count           1 `
    --shortline-pct         0 `
    --hold-min-ms           120000 `
    --hold-max-ms           120000 `
    --login-rate-limit-per-ip    200 `
    --login-rate-limit-global    10000 `
    --capture-dir           "$RepoRoot\.tmp\prof\baseline" `
    --capture-procs         "loginapp,dbapp,baseappmgr,baseapp,cellappmgr,cellapp" `
    @args

exit $LASTEXITCODE
