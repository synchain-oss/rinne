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
# 认领运行：接口不回运行编号。build.yml 在该提交上声明了 nonce 输入时，传一个随机 nonce（32 位十六进制），按运行名精确认领：
# 运行名（接口里的 display_title）按该提交上 build.yml 的 run-name 模板现算（Get-DispatchRunName，不在这里手抄格式），
# 要求恰好相等，两个进程同时对同一提交触发也认得准。没声明 nonce、或 run-name 里没用到它时，退回「触发前已有的运行不算」，
# 这时同时触发的两个进程会认成同一次运行。
# 并发限制：build.yml 的 concurrency 按提交分组、dispatch 不取消运行中的那次，但 GitHub 每个组只保留一个排队项：同一提交
# 一次在跑、一次在排时再触发第三次，排着的那次会被取消。同一提交不要同时触发两次以上。

[CmdletBinding()]
param(
    [switch]$SelfTest, [string]$Ref, [string]$Sha, [switch]$Probe, [string]$KitVersion,
    [switch]$Wait, [switch]$Judge, [long]$RunId, [int]$TimeoutMin = 9
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

$PollSeconds = 20
$FindPollSeconds = 5
$Workflow = 'build.yml'

<# build.yml 在该提交上的文本（换行统一成 LF）。 #>
function Get-DispatchYml {
    param([string]$Sha)
    $f = Get-RinneApi "repos/$(Get-RinneRepo)/contents/.github/workflows/$($Workflow)?ref=$Sha"
    [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($f.content -replace '\s', ''))) -replace "`r`n", "`n"
}

<# build.yml 声明的 workflow_dispatch 输入名（$Sha 只用来写报错）。 #>
function Get-DispatchInputs {
    param([string]$Text, [string]$Sha)
    # inputs 块到下一个顶格或两格缩进的键为止，也可能一直到文件末尾
    $m = [regex]::Match($Text, '(?ms)^  workflow_dispatch:[ \t]*\n    inputs:[ \t]*\n(.*?)(?=^\S|^  \S|\z)')
    $names = if ($m.Success) { @([regex]::Matches($m.Groups[1].Value, '(?m)^      ([A-Za-z0-9_-]+):') | ForEach-Object { $_.Groups[1].Value }) } else { @() }
    # 不按 sha 触发就对应不到确定的提交：解析不出 ref 输入（写法变了或真的没有）宁可报错，不静默跳过
    if ($names -notcontains 'ref') { throw "build.yml 在 $($Sha.Substring(0, 7)) 上解析不出 workflow_dispatch 的 ref 输入，不能按 sha 触发" }
    $names
}

<#
  按 build.yml 的 run-name 模板算出这次 dispatch 的运行名（接口里的 display_title）；没有 run-name 时返回 $null。
  只认几种表达式：github.event_name（就是 workflow_dispatch）、github.sha（就是 -Sha，触发前核对过它是分支 tip）、
  inputs.<名>（这次传的值，没传是空串）、单引号字符串，以及用 || 连起来的这几种（取第一个非空的）；别的表达式算不出来，报错。
  GitHub 不修剪运行名：nonce 是空串时末尾留一个空格，这里也一样。
#>
function Get-DispatchRunName {
    param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][string]$Sha, [Parameter(Mandatory)]$Inputs)
    $m = [regex]::Match($Text, '(?m)^run-name:[ \t]*(.*?)[ \t]*$')
    if (-not $m.Success) { return $null }
    $t = $m.Groups[1].Value
    if ($t -match "^'(.*)'$") { $t = $Matches[1] -replace "''", "'" }
    elseif ($t -match '^"(.*)"$') {
        if ($Matches[1].Contains('\')) { throw "build.yml 的 run-name 是带转义的双引号写法，算不出来：$t" }
        $t = $Matches[1]
    } else { $t = $t -replace '[ \t]+#.*$', '' }
    $eval = {
        param($e)
        foreach ($term in $e -split '\|\|') {
            $x = $term.Trim()
            $v = if ($x -ieq 'github.event_name') { 'workflow_dispatch' }
            elseif ($x -ieq 'github.sha') { $Sha }
            elseif ($x -imatch '^inputs\.([A-Za-z0-9_-]+)$') { [string]$Inputs[$Matches[1]] }
            elseif ($x -match "^'([^']*)'$") { $Matches[1] }
            else { throw "build.yml 的 run-name 里有算不出来的表达式：$x" }
            if ($v) { return $v }
        }
        ''
    }
    $out, $at = [Text.StringBuilder]::new(), 0
    foreach ($x in [regex]::Matches($t, '\$\{\{(.*?)\}\}')) {
        $null = $out.Append($t, $at, $x.Index - $at).Append([string](& $eval $x.Groups[1].Value))
        $at = $x.Index + $x.Length
    }
    $out.Append($t.Substring($at)).ToString()
}

function Invoke-Dispatch {
    param([string]$Ref, [string]$Sha, [switch]$Probe, [string]$KitVersion, [datetime]$Now = [datetime]::UtcNow, [int]$FindSeconds = 60)
    if ($Ref -notmatch '^(?:feature/[a-z0-9][a-z0-9.-]*|feat/[a-z0-9][a-z0-9-]*)$') { throw "只对 feature/** 与 feat/<slug> 触发：$Ref" }
    if ($Sha -notmatch '^[0-9a-f]{40}$') { throw '-Sha 必须是 40 位' }
    if ($KitVersion -and $KitVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?$') { throw "-KitVersion 不像版本号：$KitVersion" }
    $repo = Get-RinneRepo
    $tip = (Get-RinneApi "repos/$repo/branches/$([uri]::EscapeDataString($Ref))").commit.sha
    if ($tip -ne $Sha) { throw "$Ref 此刻的 tip 是 $($tip.Substring(0, 7))，不是 $($Sha.Substring(0, 7))" }
    $yml = Get-DispatchYml $Sha
    $declared = @(Get-DispatchInputs $yml $Sha)
    $inputs = [ordered]@{}
    $inputs['ref'] = $Sha
    if ($Probe) { if ($declared -notcontains 'probe') { throw 'build.yml 在该提交上没有 probe 输入' }; $inputs['probe'] = 'true' }
    if ($KitVersion) { if ($declared -notcontains 'kit-version') { throw 'build.yml 在该提交上没有 kit-version 输入' }; $inputs['kit-version'] = $KitVersion }
    $nonce = if ($declared -contains 'nonce') { [guid]::NewGuid().ToString('N') } else { $null }
    if ($nonce) { $inputs['nonce'] = $nonce }
    # 运行名按模板现算；模板里没用到 nonce（或没有 run-name）时运行名认不出是哪一次，退回旧办法
    $name = if ($nonce) { Get-DispatchRunName -Text $yml -Sha $Sha -Inputs $inputs }
    if ($name -and -not $name.Contains($nonce)) { $name = $null }
    $list = "repos/$repo/actions/workflows/$Workflow/runs?event=workflow_dispatch&branch=$([uri]::EscapeDataString($Ref))&per_page=10"
    # 没有 nonce 时触发前已有的运行不算：同一提交连着触发两次时，第二次的运行还没建出来，按时间找会找回第一次的那个
    $before = @(@((Get-RinneApi $list).workflow_runs) | ForEach-Object { [long]$_.id })
    Invoke-RinneApiWrite -Method POST -Path "repos/$repo/actions/workflows/$Workflow/dispatches" -Body @{ ref = $Ref; inputs = $inputs } | Out-Null
    $deadline = [datetime]::UtcNow.AddSeconds($FindSeconds)
    while ($true) {
        $runs = @((Get-RinneApi $list).workflow_runs) | Where-Object { $_ -and $_.head_sha -eq $Sha }
        # 两个分支整体包一层 @()：一次也没筛到时是空数组而不是 $null（触发后头一两次查询通常还看不到新运行）
        $mine = @(if ($name) { $runs | Where-Object { [string](Get-RinneProp $_ 'display_title') -ceq $name } }
            else { $runs | Where-Object { [long]$_.id -notin $before -and (ConvertTo-RinneUtc $_.created_at) -ge $Now.AddSeconds(-30) } })
        if ($name -and $mine.Count -gt 1) { throw "有 $($mine.Count) 次运行的运行名都带着这次的 nonce，认不准" }
        $run = $mine | Sort-Object { [long]$_.id } | Select-Object -Last 1
        if ($run) { return $run }
        if ([datetime]::UtcNow -ge $deadline) {
            throw "已触发，但 $FindSeconds 秒内没找到对应的运行$(if ($name) { "（按运行名「$name」找）" })；稍后用 -RunId 查"
        }
        Start-Sleep -Seconds $FindPollSeconds
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
    # 触发前接口里是 Runs；触发时 OnPost（拿到请求体）给出新建的运行；Later 里的运行要到触发之后再查一次才出现，
    # 它也可以是拿到请求体再给出运行的块（运行名里要带这次的 nonce）
    $box = @{ Runs = @(); OnPost = { param($p) @($run) }; Later = @(); Posted = $false; LastPost = $null }
    $jobs = @{ jobs = @(@{ name = 'build-win'; status = 'completed'; conclusion = 'success' }, @{ name = 'web-smoke'; status = 'completed'; conclusion = 'success' }) }
    $routes = [ordered]@{
        'branches/feature%2Fv1'   = { @{ commit = @{ sha = $tip } } }
        'contents/\.github/workflows/build\.yml' = { @{ content = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($doc.Yml)) } }
        'api -X POST \S+/dispatches --input' = {
            param($argv)
            $p = Get-Content -Raw ($argv[-1]) | ConvertFrom-Json
            $posted.Add($p); $box.Posted = $true; $box.LastPost = $p; $box.Runs = @($box.Runs) + @(& $box.OnPost $p); ''
        }
        'workflows/build\.yml/runs' = {
            $r = @($box.Runs)
            if ($box.Posted) {
                $later = @(if ($box.Later -is [scriptblock]) { & $box.Later $box.LastPost } else { $box.Later })
                if ($later.Count) { $box.Runs = @($box.Runs) + $later; $box.Later = @() }
            }
            @{ workflow_runs = $r }
        }
        'actions/runs/42/jobs'    = { $jobs }
        'actions/runs/42$'        = { $run }
    }
    $savedPoll = $script:FindPollSeconds
    try {
        $script:FindPollSeconds = 0
        Set-RinneFake (New-RinneFake -Routes $routes)
        $r = Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Probe -Now $now
        $p = $posted[-1]
        Add-RinneCase $cases '按 sha 触发：ref 输入是 sha40，-Probe 传 probe' $false (-not ($r.id -eq 42 -and $p.ref -eq 'feature/v1' -and $p.inputs.ref -eq $tip -and $p.inputs.probe -eq 'true'))
        Add-RinneCase $cases '没传的输入不带（build.yml 没声明 nonce 时也不带 nonce）' $false (@($p.inputs.PSObject.Properties.Name | Where-Object { $_ -in 'kit-version', 'nonce' }).Count -gt 0)
        # 没有 nonce、同一提交连着触发第二次：触发后第一次查只看得到前一次的运行（41），新建的（43）第二次查才出现；
        # 触发前已有的运行不算，所以接着等，报出的是 43
        $older = @{ id = 41; head_sha = $tip; created_at = $now.ToString('o'); status = 'in_progress'; conclusion = $null; html_url = 'u' }
        $box.Runs, $box.OnPost, $box.Later, $box.Posted = @($older), { @() }, @(@{ id = 43; head_sha = $tip; created_at = $now.ToString('o'); status = 'queued'; conclusion = $null; html_url = 'u' }), $false
        Add-RinneCase $cases '同一提交第二次触发：报出的是新建的运行，不是前一次的' $true ((Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now).id -eq 43)
        $box.Runs, $box.OnPost, $box.Later, $box.Posted = @($older), { @() }, @(), $false
        Add-RinneCase $cases '新运行还没出现：不拿触发前的运行充数' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 0 })
        # build.yml 声明了 nonce：传一个随机 nonce，按运行名认领。运行都要到触发之后第二次查询才出现（接口建运行是异步的）；
        # 另一个进程同时对同一提交触发的运行（51）编号更大，不认它
        # run-name 的写法照仓里 build.yml 那一行（自测里是数据；真跑时模板取自该提交上的文件）
        $runName = 'run-name: Build & Validate ${{ github.event_name }} ${{ inputs.ref || github.sha }} ${{ inputs.nonce }}'
        $nonceYml = "on:`n  workflow_dispatch:`n    inputs:`n      ref:`n        type: string`n      nonce:`n        type: string`n"
        $doc.Yml = "$runName`n$nonceYml"
        $byName = { param($id, $title) @{ id = $id; head_sha = $tip; display_title = $title; created_at = $now.ToString('o'); status = 'queued'; conclusion = $null; html_url = 'u' } }
        $title = { param($p, $fmt = 'Build & Validate workflow_dispatch {0} {1}') $fmt -f $tip, $p.inputs.nonce, ('d' * 40) }
        $later = { param($blk) $box.Runs, $box.OnPost, $box.Later, $box.Posted = @(), { @() }, $blk, $false }
        # 抛错的消息（没抛就是空串）：「不认」的用例要看到的是找不到运行的超时报错，不是别的异常
        $thrown = { param($sb) try { & $sb | Out-Null; '' } catch { $_.Exception.Message } }
        & $later { param($p) @((& $byName 50 (& $title $p)), (& $byName 51 "Build & Validate workflow_dispatch $tip $('e' * 32)")) }
        $rn = try { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 5 } catch { $null }
        Add-RinneCase $cases '有 nonce：新运行第二次查询才出现、同一提交同时触发两次，认领的是运行名带着自己 nonce 的那次' $true ($rn -and $rn.id -eq 50 -and $posted[-1].inputs.nonce -match '^[0-9a-f]{32}$')
        & $later { param($p) @(& $byName 52 "Build & Validate workflow_dispatch $tip ") }
        Add-RinneCase $cases '有 nonce：运行名里没有这次的 nonce（末尾是空串），等到超时也不认' $true ((& $thrown { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 1 }) -match '秒内没找到对应的运行')
        # 运行名只是「含有」这次的 nonce 不算：格式必须逐字对上（别的事件、别的提交、多了前后缀都不认）
        foreach ($t in @('Build & Validate pull_request {0} {1}', 'Build & Validate workflow_dispatch {2} {1}', 'Build & Validate workflow_dispatch {0} {1}x', 're: Build & Validate workflow_dispatch {0} {1}')) {
            & $later { param($p) @(& $byName 53 (& $title $p $t)) }.GetNewClosure()
            Add-RinneCase $cases "有 nonce：运行名「$($t -f '<sha>', '<nonce>', '<别的 sha>')」等到超时也不认" $true ((& $thrown { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 1 }) -match '秒内没找到对应的运行')
        }
        # 两次运行的运行名都是这次的：认不准，报错而不是随便挑一个
        & $later { param($p) @((& $byName 54 (& $title $p)), (& $byName 55 (& $title $p))) }
        Add-RinneCase $cases '有 nonce：两次运行的运行名都带着这次的 nonce，报认不准' $true ((& $thrown { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 5 }) -match '认不准')
        # 运行名按该提交上 build.yml 的模板现算：模板换了写法，认领跟着换，不靠手抄的格式
        $doc.Yml = 'run-name: ''Build ${{ inputs.nonce }} on ${{ github.sha }}''' + "`n$nonceYml"
        & $later { param($p) @((& $byName 56 "Build $($p.inputs.nonce) on $tip"), (& $byName 57 (& $title $p))) }
        $rn = try { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 5 } catch { $null }
        Add-RinneCase $cases '有 nonce：run-name 模板换了写法，按新模板算出的运行名认领' $false (-not ($rn -and $rn.id -eq 56))
        # 声明了 nonce、run-name 却没用到它：运行名认不出是哪一次，退回「触发前已有的运行不算」
        $doc.Yml = 'run-name: Build & Validate ${{ github.sha }}' + "`n$nonceYml"
        $box.Runs, $box.OnPost, $box.Later, $box.Posted = @($older), { @() }, @(@{ id = 58; head_sha = $tip; display_title = "Build & Validate $tip"; created_at = $now.ToString('o'); status = 'queued'; conclusion = $null; html_url = 'u' }), $false
        Add-RinneCase $cases '有 nonce、run-name 没用到它：退回按触发前后的差认领' $false ((Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -Now $now -FindSeconds 5).id -ne 58)
        # 模板求值：几种写法与认不出的表达式
        $sx, $nx = ('a' * 40), ('b' * 32)
        $ev = { param($line, $in) Get-DispatchRunName -Text "$line`n$nonceYml" -Sha $sx -Inputs $in }
        Add-RinneCase $cases 'run-name 求值：事件、inputs.ref || github.sha、nonce 都代进去' $false ((& $ev $runName ([ordered]@{ ref = $sx; nonce = $nx })) -cne "Build & Validate workflow_dispatch $sx $nx")
        Add-RinneCase $cases 'run-name 求值：ref 是空串时取 github.sha，nonce 没传时末尾留一个空格（GitHub 不修剪）' $false ((& $ev $runName ([ordered]@{ ref = '' })) -cne "Build & Validate workflow_dispatch $sx ")
        Add-RinneCase $cases 'run-name 求值：单引号写法、行尾注释' $false (
            (& $ev 'run-name: ''it''''s ${{ inputs.nonce }}''' ([ordered]@{ nonce = $nx })) -cne "it's $nx" -or (& $ev 'run-name: x ${{ inputs.nonce }} # c' ([ordered]@{ nonce = $nx })) -cne "x $nx")
        Add-RinneCase $cases 'run-name 求值：没有 run-name 时是 $null' $false ($null -ne (Get-DispatchRunName -Text $nonceYml -Sha $sx -Inputs @{}))
        Add-RinneCase $cases 'run-name 求值：认不出的表达式报错，不当成空串' $true ((& $thrown { & $ev 'run-name: x ${{ github.actor }} ${{ inputs.nonce }}' ([ordered]@{ nonce = $nx }) }) -match '算不出来')
        Add-RinneCase $cases 'run-name 求值：带转义的双引号写法报错' $true ((& $thrown { & $ev 'run-name: "x\t${{ inputs.nonce }}"' ([ordered]@{ nonce = $nx }) }) -match '算不出来')
        # 与仓里真的 build.yml 对拍：声明了 nonce 时，它的 run-name 模板必须算得出来，而且带着这次的 sha 与 nonce
        $real = (Get-Content -Raw -LiteralPath (Join-Path (Get-RinneRepoRoot) '.github/workflows/build.yml')) -replace "`r`n", "`n"
        $realName = if (@(Get-DispatchInputs $real $sx) -contains 'nonce') { Get-DispatchRunName -Text $real -Sha $sx -Inputs ([ordered]@{ ref = $sx; nonce = $nx }) } else { "$sx $nx" }
        Add-RinneCase $cases '仓里的 build.yml：声明了 nonce 时 run-name 算得出来，带着 sha 与 nonce' $false (-not ($realName -and $realName.Contains($sx) -and $realName.Contains($nx)))
        $box.OnPost = { param($p) @($run) }
        $doc.Yml = "on:`n  workflow_dispatch:`n    inputs:`n      ref:`n        type: string`n      probe:`n        type: boolean`npermissions:`n  contents: read`n"
        Add-RinneCase $cases 'build.yml 没声明的输入不传' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -KitVersion '0.1.0-preview.1' -Now $now })
        Add-RinneCase $cases '-Sha 不是分支此刻的 tip' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha ('d' * 40) -Now $now })
        foreach ($bad in 'dev', 'prod', 'v0.9.0-rc.1') { Add-RinneCase $cases "不对 $bad 触发" $true (Test-Throws { Invoke-Dispatch -Ref $bad -Sha $tip -Now $now }) }
        Add-RinneCase $cases 'kit 版本号不像版本' $true (Test-Throws { Invoke-Dispatch -Ref 'feature/v1' -Sha $tip -KitVersion 'abc' -Now $now })
        $doc.Yml = "on:`r`n  workflow_dispatch:`r`n    inputs:`r`n      ref:`r`n        type: string`r`n      kit-version:`r`n        type: string"
        Add-RinneCase $cases 'inputs 块在文件末尾、CRLF 换行也能解析' $false (-not ((@(Get-DispatchInputs (Get-DispatchYml $tip) $tip) -join ',') -eq 'ref,kit-version'))
        $doc.Yml = "on:`n  workflow_dispatch:`n    inputs:`n      probe:`n        type: boolean`n"
        Add-RinneCase $cases '解析不出 ref 输入时报错，不静默跳过' $true (Test-Throws { Get-DispatchInputs (Get-DispatchYml $tip) $tip })
        $doc.Yml = "on:`n  workflow_dispatch: {}`n"
        Add-RinneCase $cases '写法变了（解析不出 inputs）也报错' $true (Test-Throws { Get-DispatchInputs (Get-DispatchYml $tip) $tip })
        $j = Get-Judgement ([pscustomobject]$run)
        Add-RinneCase $cases '全绿' $false (-not ($j.State -eq 'green' -and $j.Jobs.Count -eq 2))
        $run.conclusion = 'failure'; $jobs.jobs[1].conclusion = 'failure'
        Add-RinneCase $cases '有 job 红' $true ((Get-Judgement ([pscustomobject]$run)).State -eq 'red')
        $run.status = 'in_progress'
        Add-RinneCase $cases '还在跑' $true ((Get-Judgement ([pscustomobject]$run)).State -eq 'pending')
        Add-RinneCase $cases '-Wait 超时立即返回还在跑的运行' $true ((Wait-Run -RunId 42 -TimeoutMin 0).status -ne 'completed')
    } finally {
        Set-RinneFake $null
        $script:FindPollSeconds = $savedPoll
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
