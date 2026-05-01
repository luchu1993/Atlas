#Requires -Version 5.1
<#
.SYNOPSIS
    Bring up a local Atlas cluster for dev / Unity client testing (no stress).

.DESCRIPTION
    Wraps run_world_stress.py with --clients 0 --keep-cluster. LoginApp lands
    on 127.0.0.1:$LoginPort by default. Stop with Ctrl+C; orphaned server
    processes need taskkill /F /IM atlas_*.exe.

.PARAMETER Build
    Build directory. Default: build/debug.

.PARAMETER Config
    CMake configuration name. Default: Debug.

.PARAMETER BaseAppCount
    Number of BaseApp instances. Default: 1.

.PARAMETER CellAppCount
    Number of CellApp instances. Default: 1.

.PARAMETER LoginPort
    External LoginApp port. Default: 20013.

.EXAMPLE
    .\tools\cluster_control\run_cluster.ps1
    .\tools\cluster_control\run_cluster.ps1 -Build build/release -Config Release
    .\tools\cluster_control\run_cluster.ps1 -BaseAppCount 2 -CellAppCount 2
#>
param(
    [string]$Build        = 'build/debug',
    [string]$Config       = 'Debug',
    [int]   $BaseAppCount = 1,
    [int]   $CellAppCount = 1,
    [int]   $LoginPort    = 20013
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Script = "$PSScriptRoot\run_world_stress.py"

python $Script `
    --build-dir       $Build `
    --config          $Config `
    --baseapp-count   $BaseAppCount `
    --cellapp-count   $CellAppCount `
    --login-port      $LoginPort `
    --clients         0 `
    --keep-cluster `
    @args

exit $LASTEXITCODE
