#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 给一张卡建实施 worktree：持 git 锁 fetch，然后
#   git worktree add --no-track -b feat/<slug> <根>/wt/<卡> origin/feature/v1
# 并建 <根>/_logs/<卡>/。卡号只出现在本机路径里，分支名不带卡号（check-commits branch 核对）。
# 同一张卡的 worktree 已在同一条分支上时只报告，不重建。
#   pwsh scripts/maintainer/new-worktree.ps1 -Card <卡> -Slug <slug>
#   pwsh scripts/maintainer/new-worktree.ps1 -SelfTest

[CmdletBinding()]
param([switch]$SelfTest, [string]$Card, [string]$Slug)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

function New-CardWorktree {
    param([Parameter(Mandatory)][string]$Card, [Parameter(Mandatory)][string]$Slug)
    if ($Card -notmatch '^(?:RN-\d{2}[a-z]?|K\d+)$') { throw "卡号格式不对：$Card" }
    if ($Slug -notmatch '^[a-z0-9][a-z0-9-]*$') { throw "slug 只许小写字母、数字与连字符：$Slug" }
    $branch = "feat/$Slug"
    $b = Invoke-RinneNodeScript 'scripts/check-commits.mjs' @('branch', '--head', $branch, '--base', 'feature/v1') -AllowFail
    if ($b.Code -ne 0) { throw "分支名没过检查：$(($b.Out + ' ' + $b.Err).Trim())" }
    $root = Get-RinneRoot
    $wt = Join-Path $root "wt/$Card"
    $logs = Join-Path $root "_logs/$Card"
    if (Test-Path -LiteralPath $wt) {
        $cur = (Invoke-RinneGit @('symbolic-ref', '--quiet', '--short', 'HEAD') -Repo $wt -AllowFail).Out.Trim()
        if ($cur -ne $branch) { throw "$wt 已存在，但在分支「$cur」上，不是 $branch" }
        New-Item -ItemType Directory -Force -Path $logs | Out-Null
        return "worktree 已存在：$wt（$branch）"
    }
    Invoke-RinneGitLocked {
        Invoke-RinneGit @('fetch', '--no-tags', 'origin', '+refs/heads/feature/v1:refs/remotes/origin/feature/v1') | Out-Null
        if ((Invoke-RinneGit @('rev-parse', '--verify', '--quiet', "refs/heads/$branch") -AllowFail).Code -eq 0) {
            throw "本地已有分支 $branch（换一个 slug，或先清理旧分支）"
        }
        Invoke-RinneGit @('worktree', 'add', '--no-track', '-b', $branch, $wt, 'origin/feature/v1') | Out-Null
    } | Out-Null
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    "已建 worktree：$wt（$branch，基于 origin/feature/v1）"
}

function Invoke-SelfTest {
    $cases = New-RinneCases
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "rinne-wt-selftest-$PID"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    # 假卡号分开拼：源码里不出现完整的卡号形态（隐私规则表会拦）
    $cid = 'RN' + '-99'
    $oldRoot = $env:RINNE_ROOT
    $log = [System.Collections.Generic.List[string]]::new()
    $locked = [System.Collections.Generic.List[string]]::new()
    $state = @{ LocalBranch = 1; Current = 'feat/example-card' }
    $routes = [ordered]@{
        ' fetch '                  = { '' }
        'rev-parse --verify'       = { @{ Code = $state.LocalBranch; Out = ''; Err = '' } }
        'worktree add'             = { param($argv) New-Item -ItemType Directory -Force -Path $argv[-2] | Out-Null; '' }
        'symbolic-ref'             = { $state.Current }
    }
    $needles = Enter-RinneSelfTestNeedles
    try {
        $env:RINNE_ROOT = $tmp
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Set-RinneLockImpl { param($name, $sb) $locked.Add('lock'); & $sb }
        $r = New-CardWorktree -Card $cid -Slug 'example-card'
        $add = @($log | Where-Object { $_ -match 'worktree add' })
        $wt = Join-Path $tmp "wt/$cid"
        Add-RinneCase $cases '持锁 fetch 后 worktree add --no-track -b feat/<slug> <根>/wt/<卡> origin/feature/v1' $false (-not (
                $locked.Count -eq 1 -and $add.Count -eq 1 -and $add[0].EndsWith("worktree add --no-track -b feat/example-card $wt origin/feature/v1") -and
                @($log | Where-Object { $_ -match ' fetch ' }).Count -eq 1))
        Add-RinneCase $cases '建 _logs/<卡>/' $false (-not (Test-Path -LiteralPath (Join-Path $tmp "_logs/$cid")))
        $n = $log.Count
        $again = New-CardWorktree -Card $cid -Slug 'example-card'
        Add-RinneCase $cases '已存在且在同一分支：不重建' $false (-not ($again -match '已存在' -and -not @($log | Select-Object -Skip $n | Where-Object { $_ -match 'worktree add' }).Count))
        $state.Current = 'feat/other'
        Add-RinneCase $cases '已存在但在别的分支上' $true (Test-Throws { New-CardWorktree -Card $cid -Slug 'example-card' })
        $state.LocalBranch = 0
        Add-RinneCase $cases '本地已有同名分支' $true (Test-Throws { New-CardWorktree -Card ($cid + 'b') -Slug 'example-card' })
        Add-RinneCase $cases 'slug 带卡号' $true (Test-Throws { New-CardWorktree -Card ($cid + 'c') -Slug ('rn' + '-03a-x') })
        Add-RinneCase $cases 'slug 有大写或斜杠' $true (Test-Throws { New-CardWorktree -Card ($cid + 'c') -Slug 'Bad/Slug' })
        Add-RinneCase $cases '卡号格式不对' $true (Test-Throws { New-CardWorktree -Card '../x' -Slug 'example' })
    } finally {
        Set-RinneFake $null
        Exit-RinneSelfTestNeedles $needles
        Set-RinneLockImpl $null
        $env:RINNE_ROOT = $oldRoot
        Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
    Complete-RinneSelfTest $cases 'new-worktree'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
if (-not $Card -or -not $Slug) { throw '需要 -Card <卡> 与 -Slug <slug>' }
Write-Output (New-CardWorktree -Card $Card -Slug $Slug)
