#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 手动触发构建（build.yml 的 workflow_dispatch），按 sha 触发、分片等待、逐 job 判读：
#   pwsh scripts/maintainer/dispatch.ps1 -Ref <feature/v1 | feat/<slug>> -Sha <sha40> [-Probe] [-KitVersion <v>] [-Wait] [-Judge]
#   pwsh scripts/maintainer/dispatch.ps1 -RunId <id> [-Wait] [-Judge]        续等已经触发的那次运行
#   pwsh scripts/maintainer/dispatch.ps1 -SelfTest
# 只对 feature 线与 feat/ 子支线触发，不碰 dev / prod 与 tag；-Sha 必须就是该分支此刻的 tip（构建对应到确定的提交）。
# 只传 build.yml 在该提交上声明了的输入（多传会被接口拒掉）。-Wait 单次最多 9 分钟（-TimeoutMin），没跑完退出 3，
# 下次带 -RunId 接着等；-Judge 退出 0 = 全绿、1 = 有红、3 = 还在跑。

[CmdletBinding()]
param(
    [switch]$SelfTest, [string]$Ref, [string]$Sha, [switch]$Probe, [string]$KitVersion,
    [switch]$Wait, [switch]$Judge, [long]$RunId, [int]$TimeoutMin = 9
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

$PollSeconds = 20
$Workflow = 'build.yml'

<# build.yml 在该提交上声明的 workflow_dispatch 输入名。 #>
function Get-DispatchInputs {
    param([string]$Sha)
    $f = Get-RinneApi "repos/$(Get-RinneRepo)/contents/.github/workflows/$($Workflow)?ref=$Sha"
    $text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($f.content -replace '\s', ''))) -replace "`r`n", "`n"
    # inputs 块到下一个顶格或两格缩进的键为止，也可能一直到文件末尾
    $m = [regex]::Match($text, '(?ms)^  workflow_dispatch:[ \t]*\n    inputs:[ \t]*\n(.*?)(?=^\S|^  \S|\z)')
    $names = if ($m.Success) { @([regex]::Matches($m.Groups[1].Value, '(?m)^      ([A-Za-z0-9_-]+):') | ForEach-Object { $_.Groups[1].Value }) } else { @() }
    # 不按 sha 触发就对应不到确定的提交：解析不出 ref 输入（写法变了或真的没有）宁可报错，不静默跳过
    if ($names -notcontains 'ref') { throw "build.yml 在 $($Sha.Substring(0, 7)) 上解析不出 workflow_dispatch 的 ref 输入，不能按 sha 触发" }
    $names
}

function Invoke-Dispatch {
    param([string]$Ref, [string]$Sha, [switch]$Probe, [string]$KitVersion, [datetime]$Now = [datetime]::UtcNow, [int]$FindSeconds = 60)
    if ($Ref -notmatch '^(?:feature/[a-z0-9][a-z0-9.-]*|feat/[a-z0-9][a-z0-9-]*)$') { throw "只对 feature/** 与 feat/<slug> 触发：$Ref" }
    if ($Sha -notmatch '^[0-9a-f]{40}$') { throw '-Sha 必须是 40 位' }
    if ($KitVersion -and $KitVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?$') { throw "-KitVersion 不像版本号：$KitVersion" }
    $repo = Get-RinneRepo
    $tip = (Get-RinneApi "repos/$repo/branches/$([uri]::EscapeDataString($Ref))").commit.sha
    if ($tip -ne $Sha) { throw "$Ref 此刻的 tip 是 $($tip.Substring(0, 7))，不是 $($Sha.Substring(0, 7))" }
    $declared = @(Get-DispatchInputs $Sha)
    $inputs = [ordered]@{}
    $inputs['ref'] = $Sha
    if ($Probe) { if ($declared -notcontains 'probe') { throw 'build.yml 在该提交上没有 probe 输入' }; $inputs['probe'] = 'true' }
    if ($KitVersion) { if ($declared -notcontains 'kit-version') { throw 'build.yml 在该提交上没有 kit-version 输入' }; $inputs['kit-version'] = $KitVersion }
    Invoke-RinneApiWrite -Method POST -Path "repos/$repo/actions/workflows/$Workflow/dispatches" -Body @{ ref = $Ref; inputs = $inputs } | Out-Null
    # 接口不回运行编号：按分支、事件、head 与触发时间找刚建的那次运行
    $deadline = [datetime]::UtcNow.AddSeconds($FindSeconds)
    while ($true) {
        $runs = (Get-RinneApi "repos/$repo/actions/workflows/$Workflow/runs?event=workflow_dispatch&branch=$([uri]::EscapeDataString($Ref))&per_page=10").workflow_runs
        $run = @($runs) | Where-Object { $_.head_sha -eq $Sha -and (ConvertTo-RinneUtc $_.created_at) -ge $Now.AddSeconds(-30) } |
            Sort-Object { [long]$_.id } | Select-Object -Last 1
        if ($run) { return $run }
        if ([datetime]::UtcNow -ge $deadline) { throw '已触发，但一分钟内没找到对应的运行；稍后用 -RunId 查' }
        Start-Sleep -Seconds 5
    }
}

<# 逐 job 判读：green（运行成功）/ red（运行失败、取消或超时）/ pending（还在跑）。 #>
function Get-Judgement {
    param([Parameter(Mandatory)]$Run)
    $jobs = @(Get-RinneApi "repos/$(Get-RinneRepo)/actions/runs/$($Run.id)/jobs?per_page=100" -Paginate | ForEach-Object { $_.jobs } | ForEach-Object { $_ })
    $state = if ($Run.status -ne 'completed') { 'pending' } elseif ($Run.conclusion -eq 'success') { 'green' } else { 'red' }
    [pscustomobject]@{
        State = $state; RunId = $Run.id; Sha = $Run.head_sha; Url = $Run.html_url
        Jobs = @($jobs | ForEach-Object { [pscustomobject]@{ Name = $_.name; Status = $_.status; Conclusion = $_.conclusion } })
    }
}

function Wait-Run {
    param([long]$RunId, [int]$TimeoutMin)
    $deadline = [datetime]::UtcNow.AddMinutes([Math]::Min([Math]::Max($TimeoutMin, 0), 9))
    while ($true) {
        $run = Get-RinneApi "repos/$(Get-RinneRepo)/actions/runs/$RunId"
        if ($run.status -eq 'completed' -or [datetime]::UtcNow -ge $deadline) { return $run }
        Start-Sleep -Seconds $PollSeconds
    }
}

function Invoke-SelfTest {
    $cases = New-RinneCases
    $tip = 'c' * 40
    $posted = [System.Collections.Generic.List[object]]::new()
    $doc = @{ Yml = "on:`n  workflow_dispatch:`n    inputs:`n      ref:`n        type: string`n      probe:`n        type: boolean`npermissions:`n  contents: read`n" }
    $now = [datetime]::UtcNow
    $run = @{ id = 42; head_sha = $tip; created_at = $now.ToString('o'); status = 'completed'; conclusion = 'success'; html_url = 'u' }
    $jobs = @{ jobs = @(@{ name = 'build-win'; status = 'completed'; conclusion = 'success' }, @{ name = 'web-smoke'; status = 'completed'; conclusion = 'success' }) }
    $routes = [ordered]@{
        'branches/feature%2Fv1'   = { @{ commit = @{ sha = $tip } } }
        'contents/\.github/workflows/build\.yml' = { @{ content = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($doc.Yml)) } }
        'api -X POST \S+/dispatches --input' = { param($argv) $posted.Add((Get-Content -Raw ($argv[-1]) | ConvertFrom-Json)); '' }
        'workflows/build\.yml/runs' = { @{ workflow_runs = @($run) } }
        'actions/runs/42/jobs'    = { $jobs }
        'actions/runs/42$'        = { $run }
    }
    try {
        Set-RinneFake (New-RinneFake -Routes $routes)
        $r = Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Probe -Now $now
        $p = $posted[-1]
        Add-RinneCase $cases '按 sha 触发：ref 输入是 sha40，-Probe 传 probe' $false (-not ($r.id -eq 42 -and $p.ref -eq 'feature/v1' -and $p.inputs.ref -eq $tip -and $p.inputs.probe -eq 'true'))
        Add-RinneCase $cases '没传的输入不带' $false (@($p.inputs.PSObject.Properties.Name) -contains 'kit-version')
        Add-RinneCase $cases 'build.yml 没声明的输入不传' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -KitVersion '0.1.0-preview.1' -Now $now })
        Add-RinneCase $cases '-Sha 不是分支此刻的 tip' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha ('d' * 40) -Now $now })
        foreach ($bad in 'dev', 'prod', 'v0.9.0-rc.1') { Add-RinneCase $cases "不对 $bad 触发" $true (Test-Throws { Invoke-Dispatch -Ref $bad -Sha $tip -Now $now }) }
        Add-RinneCase $cases 'kit 版本号不像版本' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -KitVersion 'abc' -Now $now })
        $doc.Yml = "on:`r`n  workflow_dispatch:`r`n    inputs:`r`n      ref:`r`n        type: string`r`n      kit-version:`r`n        type: string"
        Add-RinneCase $cases 'inputs 块在文件末尾、CRLF 换行也能解析' $false (-not ((@(Get-DispatchInputs $tip) -join ',') -eq 'ref,kit-version'))
        $doc.Yml = "on:`n  workflow_dispatch:`n    inputs:`n      probe:`n        type: boolean`n"
        Add-RinneCase $cases '解析不出 ref 输入时报错，不静默跳过' $true (Test-Throws { Get-DispatchInputs $tip })
        $doc.Yml = "on:`n  workflow_dispatch: {}`n"
        Add-RinneCase $cases '写法变了（解析不出 inputs）也报错' $true (Test-Throws { Get-DispatchInputs $tip })
        $j = Get-Judgement ([pscustomobject]$run)
        Add-RinneCase $cases '全绿' $false (-not ($j.State -eq 'green' -and $j.Jobs.Count -eq 2))
        $run.conclusion = 'failure'; $jobs.jobs[1].conclusion = 'failure'
        Add-RinneCase $cases '有 job 红' $true ((Get-Judgement ([pscustomobject]$run)).State -eq 'red')
        $run.status = 'in_progress'
        Add-RinneCase $cases '还在跑' $true ((Get-Judgement ([pscustomobject]$run)).State -eq 'pending')
        Add-RinneCase $cases '-Wait 超时立即返回还在跑的运行' $true ((Wait-Run -RunId 42 -TimeoutMin 0).status -ne 'completed')
    } finally {
        Set-RinneFake $null
    }
    Complete-RinneSelfTest $cases 'dispatch'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
Initialize-RinneIdentity
$run = if ($RunId) { Get-RinneApi "repos/$(Get-RinneRepo)/actions/runs/$RunId" }
else {
    if (-not $Ref -or -not $Sha) { throw '需要 -Ref 与 -Sha（或用 -RunId 续等）' }
    Invoke-Dispatch -Ref $Ref -Sha $Sha -Probe:$Probe -KitVersion $KitVersion
}
Write-Output "运行 $($run.id)：$($run.html_url)"
if ($Wait) { $run = Wait-Run -RunId $run.id -TimeoutMin $TimeoutMin }
if ($Judge -or $Wait) {
    $j = Get-Judgement $run
    Write-Output (ConvertTo-RinneJsonText $j)
    exit $(switch ($j.State) { 'green' { 0 } 'red' { 1 } default { 3 } })
}
