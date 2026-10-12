#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 推送当前子支线：只推 HEAD:refs/heads/feat/<slug>，没有 force 这个选项，长期分支与分离 HEAD 一律拒绝；
# 已推送过的分支只许快进。推之前对要推的区间跑一遍钉版 gitleaks（RINNE_GITLEAKS / rinne.gitleaks），
# 工作树必须干净（gates json 对应的是提交过的状态）。推送照常经过 pre-push hook，不加 --no-verify。
#   pwsh scripts/maintainer/rn-push.ps1 [-Repo <worktree>]
#   pwsh scripts/maintainer/rn-push.ps1 -SelfTest

[CmdletBinding()]
param([switch]$SelfTest, [string]$Repo)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

function Invoke-Push {
    param([Parameter(Mandatory)][string]$Repo)
    $branch = (Invoke-RinneGit @('symbolic-ref', '--quiet', '--short', 'HEAD') -Repo $Repo -AllowFail).Out.Trim()
    if ($branch -notmatch '^feat/[a-z0-9][a-z0-9-]*$') { throw "只推 feat/<slug> 子支线，当前是「$branch」" }
    $dirty = (Invoke-RinneGit @('status', '--porcelain') -Repo $Repo).Out.Trim()
    if ($dirty) { throw '工作树不干净：先提交或清理，再推送' }
    $head = (Invoke-RinneGit @('rev-parse', 'HEAD') -Repo $Repo).Out.Trim()
    Invoke-RinneGitLocked {
        Invoke-RinneGit @('fetch', '--no-tags', 'origin', '+refs/heads/feature/v1:refs/remotes/origin/feature/v1') -Repo $Repo | Out-Null
    } | Out-Null
    $remote = (Invoke-RinneGit @('ls-remote', '--heads', 'origin', "refs/heads/$branch") -Repo $Repo).Out.Trim()
    $from = 'origin/feature/v1'
    if ($remote) {
        $old = ($remote -split '\s+')[0]
        if ((Invoke-RinneGit @('merge-base', '--is-ancestor', $old, $head) -Repo $Repo -AllowFail).Code -ne 0) {
            throw "远端 $branch 不是当前 HEAD 的祖先：已推送的分支只许快进，不 force-push"
        }
        if ($old -eq $head) { return "$branch 已是最新（$($head.Substring(0, 7))），不用推" }
        $from = $old
    }
    $leaks = Get-RinneSetting -Env 'RINNE_GITLEAKS' -GitKey 'rinne.gitleaks'
    if (-not $leaks) { throw '未配置 gitleaks：设置 RINNE_GITLEAKS 或 git config rinne.gitleaks' }
    $scan = Invoke-RinneExternal $leaks @('git', $Repo, '--no-banner', '--redact', "--log-opts=$from..$head --no-ext-diff --no-textconv") -AllowFail
    if ($scan.Code -ne 0) { throw "gitleaks 在 $from..$($head.Substring(0, 7)) 里有发现，不推送" }
    Invoke-RinneGit @('push', 'origin', "HEAD:refs/heads/$branch") -Repo $Repo | Out-Null
    "已推送 $branch → $($head.Substring(0, 7))"
}

function Invoke-SelfTest {
    $cases = New-RinneCases
    $state = @{ Branch = 'feat/example-card'; Dirty = ''; Remote = ''; Ancestor = 0; Leaks = 0 }
    $log = [System.Collections.Generic.List[string]]::new()
    $head = 'a' * 40
    $routes = [ordered]@{
        'symbolic-ref'         = { @{ Code = $(if ($state.Branch) { 0 } else { 1 }); Out = $state.Branch; Err = '' } }
        'status --porcelain'   = { $state.Dirty }
        'rev-parse HEAD'       = { $head }
        ' fetch '              = { '' }
        'ls-remote'            = { $state.Remote }
        'merge-base'           = { @{ Code = $state.Ancestor; Out = ''; Err = '' } }
        'config --get rinne.gitleaks' = { 'gitleaks-fake' }
        '^gitleaks-fake git'   = { @{ Code = $state.Leaks; Out = ''; Err = '' } }
        ' push origin '        = { '' }
    }
    $oldLeaks = $env:RINNE_GITLEAKS
    try {
        $env:RINNE_GITLEAKS = 'gitleaks-fake'
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Set-RinneLockImpl { param($sb) & $sb }
        $r = Invoke-Push -Repo 'wt'
        $push = @($log | Where-Object { $_ -match ' push ' })
        Add-RinneCase $cases '新分支：先 gitleaks 扫 origin/feature/v1..HEAD，再推 HEAD:refs/heads/feat/<slug>' $false (-not (
                $r -match '已推送' -and $push.Count -eq 1 -and $push[0] -match 'push origin HEAD:refs/heads/feat/example-card$' -and
                @($log | Where-Object { $_ -match "gitleaks-fake git wt .*--log-opts=origin/feature/v1\.\.$head" }).Count -eq 1))
        Add-RinneCase $cases '推送命令里没有 force 与 --no-verify' $false (@($log | Where-Object { $_ -match ' push .*(--force|-f\b|--no-verify|\+HEAD)' }).Count -gt 0)
        $log.Clear(); $state.Remote = "$('b' * 40)`trefs/heads/feat/example-card"
        Invoke-Push -Repo 'wt' | Out-Null
        Add-RinneCase $cases '已推送过：只扫新增区间' $false (@($log | Where-Object { $_ -match "--log-opts=$('b' * 40)\.\.$head" }).Count -ne 1)
        $state.Ancestor = 1
        Add-RinneCase $cases '远端不是祖先（需要 force）' $true (Test-Throws { Invoke-Push -Repo 'wt' })
        $state.Ancestor = 0; $state.Remote = ''
        $state.Leaks = 1; $log.Clear()
        Add-RinneCase $cases 'gitleaks 有发现时不推' $true ((Test-Throws { Invoke-Push -Repo 'wt' }) -and -not @($log | Where-Object { $_ -match ' push ' }).Count)
        $state.Leaks = 0
        foreach ($b in 'feature/v1', 'dev', 'prod', 'main') {
            $state.Branch = $b
            Add-RinneCase $cases "长期分支 $b" $true (Test-Throws { Invoke-Push -Repo 'wt' })
        }
        $state.Branch = ''
        Add-RinneCase $cases '分离 HEAD' $true (Test-Throws { Invoke-Push -Repo 'wt' })
        $state.Branch = 'feat/example-card'; $state.Dirty = ' M x.txt'
        Add-RinneCase $cases '工作树不干净' $true (Test-Throws { Invoke-Push -Repo 'wt' })
    } finally {
        Set-RinneFake $null
        Set-RinneLockImpl $null
        $env:RINNE_GITLEAKS = $oldLeaks
    }
    Complete-RinneSelfTest $cases 'rn-push'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
$target = if ($Repo) { $Repo } else { (Get-Location).Path }
Write-Output (Invoke-Push -Repo $target)
