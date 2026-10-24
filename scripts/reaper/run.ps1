#Requires -Version 7
# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 本机宿主烟测的入口：Cubase 上机前的预检与本机回归，不进 CI，不挂 gates。说明见 docs/testing/REAPER.md。
#   pwsh scripts/reaper/run.ps1 -CfgDir <私有配置目录> -BuildDir build-wf -LogDir <结果目录> -UserTestingMarker <标记文件路径> [-Repeat 5] [-Only <id 正则>] [-NoNegative]
#   pwsh scripts/reaper/run.ps1 -SelfTest [-CollisionSample <一份含大小写碰撞变量的 .ps1>]   不启动 REAPER 的自测
# -CfgDir 只接受显式的绝对路径，必须有 doctor.ps1 -Init 写的哨兵文件；永不碰 %APPDATA%\REAPER。
# 退出码：0 全 PASS、1 有 FAIL、2 有 INCOMPLETE（没得出结论，不当 PASS）、3 整套 SKIPPED、64 被护栏拒绝或用法错误（含 -Only 一个场景都没选中）。
[CmdletBinding()]
param(
    [string]$CfgDir = '',
    [string]$ReaperExe = (Join-Path $env:ProgramFiles 'REAPER (x64)\reaper.exe'),
    [string]$BuildDir = '',
    [string]$NullTest = '',
    [string]$LogDir = '',
    [string]$UserTestingMarker = '',
    [string]$ScenarioFile = '',
    [string]$Only = '',
    [int]$Repeat = 1,
    [switch]$NoNegative,
    [switch]$SelfTest,
    [string]$CollisionSample = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ($SelfTest) {
    Import-Module (Join-Path $repoRoot 'tests/reaper/SelfTest.psm1') -ErrorAction Stop
    exit (Invoke-ReaperSelfTest -RepoRoot $repoRoot -CollisionSample $CollisionSample)
}

foreach ($need in @(@('-BuildDir', $BuildDir), @('-LogDir', $LogDir), @('-UserTestingMarker', $UserTestingMarker))) {
    if (-not $need[1]) { Write-Host "用法错误：缺 $($need[0])"; exit 64 }
}
if ($Repeat -lt 1) { Write-Host '用法错误：-Repeat 至少是 1'; exit 64 }
Import-Module (Join-Path $PSScriptRoot 'Suite.psm1') -ErrorAction Stop

$params = @{
    CfgDir = $CfgDir; ReaperExe = $ReaperExe; NullTest = $NullTest; LogDir = $LogDir; UserTestingMarker = $UserTestingMarker
    BuildDir = ([IO.Path]::IsPathRooted($BuildDir) ? $BuildDir : (Join-Path $repoRoot $BuildDir))
    ScenarioFile = ($ScenarioFile ? $ScenarioFile : (Join-Path $repoRoot 'tests/reaper/scenarios.json'))
    Only = $Only; Repeat = $Repeat; NoNegative = $NoNegative
}
try {
    $outcome = Invoke-ReaperSmoke @params
} catch {
    if ($_.Exception.Message -like 'REFUSED:*' -or $_.Exception.Message -like 'USAGE:*') { Write-Host $_.Exception.Message; exit 64 }
    throw
}
$resultFile = Write-ReaperSmokeResult -Data $outcome -LogDir $LogDir
if ($outcome.verdict -eq 'SKIPPED') { Write-Host "SKIPPED：$($outcome.skipReasons -join '；')" }
Write-Host ("REAPER 烟测 {0}  构建 {1}  {2} 轮  REAPER 启动 {3} 次  结果 {4}" -f $outcome.verdict, $outcome.build.sha7, $outcome.runs.Count, $outcome.launches, $resultFile)
$exitCodes = @{ PASS = 0; FAIL = 1; INCOMPLETE = 2; SKIPPED = 3 }
exit $exitCodes[$outcome.verdict]
