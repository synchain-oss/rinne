#Requires -Version 7
# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 烟测的体检与初始化。不启动 REAPER。
#   pwsh scripts/reaper/doctor.ps1 -CfgDir <私有配置目录> -Init      建配置目录（私有插件目录、最小 reaper.ini）并写哨兵文件
#   pwsh scripts/reaper/doctor.ps1 -CfgDir <私有配置目录> -BuildDir build-wf -UserTestingMarker <标记文件路径>
#       逐项体检；缺项就打印 SKIPPED 和原因（和 run.ps1 同一套预检）
# -CfgDir 在 AppData 之下（或指向那里）一律拒绝，-Init 也一样。
# 退出码：0 可以跑、3 会被 SKIPPED、64 被护栏拒绝或用法错误。
[CmdletBinding()]
param(
    [string]$CfgDir = '',
    [switch]$Init,
    [string]$ReaperExe = (Join-Path $env:ProgramFiles 'REAPER (x64)\reaper.exe'),
    [string]$BuildDir = '',
    [string]$NullTest = '',
    [string]$UserTestingMarker = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Import-Module (Join-Path $PSScriptRoot 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Suite.psm1') -ErrorAction Stop

try {
    if ($Init) {
        $made = New-ReaperCfgDir $CfgDir
        Write-Host "已初始化：哨兵文件与 reaper.ini 就绪（$(Split-Path -Leaf $made.Dir)）"
        exit 0
    }
    $problems = [Collections.Generic.List[string]]::new()
    $null = Assert-ReaperCfgDir $CfgDir # 路径不合格直接拒绝；缺哨兵也是拒绝
    Write-Host '[OK] 配置目录合格（不在默认资源目录下，有哨兵文件与 reaper.ini）'
    if ($BuildDir -and $UserTestingMarker) {
        $dir = [IO.Path]::IsPathRooted($BuildDir) ? $BuildDir : (Join-Path $repoRoot $BuildDir)
        $pre = Get-ReaperPreflight -ReaperExe $ReaperExe -BuildDir $dir -NullTest $NullTest -UserTestingMarker $UserTestingMarker
        foreach ($r in $pre.Reasons) { $problems.Add($r) }
        if (-not $pre.Reasons.Count) { Write-Host "[OK] REAPER $($pre.ReaperVersion)；构建 $($pre.Sha7)（$($pre.Flavor)）；rinne_nulltest 在；没有别的 REAPER 在跑" }
    } else {
        Write-Host '（没给 -BuildDir 与 -UserTestingMarker，只做了配置目录检查）'
    }
    foreach ($p in $problems) { Write-Host "[缺] $p" }
    if ($problems.Count) { Write-Host 'SKIPPED：按上面的缺项整套不会运行'; exit 3 }
    Write-Host '可以运行 run.ps1'
    exit 0
} catch {
    if ($_.Exception.Message -like 'REFUSED:*') { Write-Host $_.Exception.Message; exit 64 }
    throw
}
