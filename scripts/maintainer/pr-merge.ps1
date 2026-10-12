#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 合并子 PR（只进 feature/**，squash）。三步，任何一步失败都停下并打印状态，不重试：
#   -Check -Pr <n> -Card <卡>     逐条核对「干净」判据，输出一行 JSON：code = ready | not-ready | needs-user
#                                 （退出码 0 / 1 / 2）；needs-user 带 level（user 进给用户的汇总表，orchestrator 由统筹分诊）
#   -Merge -Pr <n> -Card <卡> -SummaryFile <f> [-ApprovedByUser]
#                                 重新核对，ready 才合并：squash，钉住审过的 head（sha=…），显式写 subject「<标题> (#n)」
#                                 与 body（摘要段 + 空行 + 唯一一行签名）。碰基础设施路径的 PR 永远是 needs-user，
#                                 只有用户批准后由主会话加 -ApprovedByUser 执行，其他 needs-user 不能这样放行
#   -After -Pr <n> -Card <卡>     回读 squash 提交（DCO、作者与提交者、恰好一行签名）、删远端子支线、持 git 锁移除 worktree
#   -SelfTest                     用 fixtures/ 里录制的 PR 响应离线跑，对每条判据给反例
# 只从主 clone 运行（它跟随 origin/feature/v1）：判据脚本取自 feature 线，而不是待合并 PR 自己的版本。

[CmdletBinding()]
param(
    [switch]$Check, [switch]$Merge, [switch]$After, [switch]$SelfTest,
    [int]$Pr, [string]$Card, [string]$SummaryFile, [switch]$ApprovedByUser
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

$TierRank = @{ Quick = 1; Standard = 2; Full = 3 }
$ReviewWorkflows = @('.github/workflows/claude-review.yml', '.github/workflows/review-dispatch.yml')
# 改到这些路径的子支线合入前要对它的 head 手动 dispatch 一次构建（卡片清单的 needsBranchDispatch 是计划值，这里按实际 diff 再算）
$DispatchGlobs = @(
    '**/CMakeLists.txt', '**/*.cmake', 'cmake/**', '**/*.mm', '**/mac/**', '**/macos/**',
    '.github/workflows/build.yml', '.github/workflows/release.yml', 'packaging/**', 'scripts/package*'
)

# ---- 收集事实（联网、读本机文件、跑 git 与 Node 判据） -------------------------------------

function Read-Waivers {
    param([int]$Pr)
    $path = Get-RinneSetting -Env 'RINNE_WAIVERS' -GitKey 'rinne.waivers'
    if (-not $path -or -not (Test-Path -LiteralPath $path)) { return @() }
    $all = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json -Depth 20
    Get-RinneProp $all ([string]$Pr)
}

function Test-PrText {
    param([string]$Title, [string]$Body)
    $file = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($file, $Body, [Text.UTF8Encoding]::new($false))
        Invoke-RinneNodeScript 'scripts/check-commits.mjs' @('pr-text', '--title', $Title, '--body-file', $file) -AllowFail
    } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}

<# 在合并时对当前 origin/feature/v1 重跑一次契约守卫：feature 线的 required checks 不 strict，冻结后可能才合入。 #>
function Test-ContractGuard {
    param([int]$Pr, [string]$Body)
    $file = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($file, $Body, [Text.UTF8Encoding]::new($false))
        $argv = @('contract-guard', '--base', 'origin/feature/v1', '--head', "refs/remotes/origin/pr/$Pr", '--body-file', $file)
        Invoke-RinneNodeScript 'scripts/check-commits.mjs' $argv -AllowFail
    } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}

<# Standard 及以上：在 <根>/wt/tip 把 PR 合到当前 feature/v1 上做一次增量构建，构建脚本取自合并后的树。 #>
function Invoke-Premerge {
    param([int]$Pr)
    $tip = Join-Path (Get-RinneRoot) 'wt/tip'
    # 结果写进这张表，不依赖锁函数的返回值（它可能另有输出）
    $merged = @{ Ok = $false }
    Invoke-RinneGitLocked {
        if (-not (Test-Path -LiteralPath $tip)) { Invoke-RinneGit @('worktree', 'add', '--detach', $tip, 'origin/feature/v1') | Out-Null }
        Invoke-RinneGit @('checkout', '--detach', '--force', 'origin/feature/v1') -Repo $tip | Out-Null
        $m = Invoke-RinneGit @('merge', '--no-ff', '--no-commit', "refs/remotes/origin/pr/$Pr") -Repo $tip -AllowFail
        if ($m.Code -ne 0) { Invoke-RinneGit @('merge', '--abort') -Repo $tip -AllowFail | Out-Null }
        $merged.Ok = $m.Code -eq 0
    } | Out-Null
    if (-not $merged.Ok) { return @{ Ok = $false; Reason = '与当前 feature/v1 合并有冲突' } }
    try {
        $build = Join-Path $tip 'scripts/build.ps1'
        if (-not (Test-Path -LiteralPath $build)) { return @{ Ok = $false; Reason = '合并后的树里没有 scripts/build.ps1，做不了合并后构建' } }
        $r = Invoke-RinneExternal pwsh @('-NoProfile', '-File', $build) -Cwd $tip -AllowFail
        return @{ Ok = ($r.Code -eq 0); Reason = "合并后构建退出码 $($r.Code)" }
    } finally {
        Invoke-RinneGitLocked { Invoke-RinneGit @('merge', '--abort') -Repo $tip -AllowFail | Out-Null } | Out-Null
    }
}

function Get-MergeFacts {
    param([Parameter(Mandatory)][int]$Pr, [Parameter(Mandatory)][string]$CardId)
    $repo = Get-RinneRepo
    $card = Get-RinneCard $CardId
    $pull = Get-RinneApi "repos/$repo/pulls/$Pr"
    $head = [string]$pull.head.sha
    $activity = Get-RinnePrActivity $Pr
    $state = Get-RinneReviewState $activity
    $runId = if ($state.summary) { [string]$state.summary.runId } else { '' }
    $run = if ($runId) { Get-RinneApi "repos/$repo/actions/runs/$runId" -AllowFail } else { $null }
    $jobs = if ($run) { @(Get-RinneApi "repos/$repo/actions/runs/$runId/jobs?per_page=100" -Paginate | ForEach-Object { $_.jobs } | ForEach-Object { $_ }) } else { @() }
    $files = Get-RinneApi "repos/$repo/pulls/$Pr/files?per_page=100" -Paginate
    $gatesPath = Join-Path (Get-RinneRoot) "_logs/$CardId/gates-$head.json"
    $gates = if (Test-Path -LiteralPath $gatesPath) { Get-Content -Raw -LiteralPath $gatesPath | ConvertFrom-Json } else { $null }
    $needsDispatch = [bool]$card.needsBranchDispatch -or @($files | Where-Object { Test-GlobMatch $_.filename $DispatchGlobs }).Count -gt 0
    $dispatchRuns = if ($needsDispatch) {
        $p = "repos/$repo/actions/workflows/build.yml/runs?event=workflow_dispatch&head_sha=$head&per_page=20"
        @((Get-RinneApi $p).workflow_runs)
    } else { @() }
    Invoke-RinneGitLocked {
        $specs = @('+refs/heads/feature/v1:refs/remotes/origin/feature/v1', '+refs/heads/dev:refs/remotes/origin/dev', "+refs/pull/${Pr}/head:refs/remotes/origin/pr/$Pr")
        Invoke-RinneGit (@('fetch', '--no-tags', 'origin') + $specs) | Out-Null
    } | Out-Null
    $body = [string](Get-RinneProp $pull 'body')
    $premerge = if ($TierRank[[string]$card.tier] -ge 2) { Invoke-Premerge $Pr } else { $null }
    [pscustomobject]@{
        Pr = $Pr; Card = $card; Pull = $pull; Head = $head; Files = @($files)
        State = $state; Items = @(Get-RinneCommentItems -Activity $activity -State $state); Threads = @($activity.Threads)
        Run = $run; Jobs = $jobs; Checks = Get-RinneCheckRuns $head; Required = Get-RinneRequiredChecks
        Gates = $gates; Waivers = Read-Waivers $Pr
        PrRef = (Invoke-RinneGit @('rev-parse', "refs/remotes/origin/pr/$Pr")).Out.Trim()
        Dco = [int](Invoke-RinneGit @('rev-list', '--count', 'origin/dev..origin/feature/v1')).Out.Trim()
        Guard = Test-ContractGuard $Pr $body
        PrText = Test-PrText ([string]$pull.title) $body
        NeedsDispatch = $needsDispatch; DispatchRuns = $dispatchRuns; Premerge = $premerge
    }
}

# ---- 判据（纯函数，自测逐条给反例） ---------------------------------------------------------

<# 处置汇总表里判为「不改」或「挂账」的【红旗】【重要】：原文整行列出，供统筹记录、交用户裁决。 #>
function Get-DeferredSevere {
    param([object[]]$Items)
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($i in @($Items | Where-Object { $_.Kind -eq 'agent' -and (Get-AgentMarker $_.Body).Kind -eq 'disposition' })) {
        foreach ($line in $i.Body -split "`r?`n") {
            if ($line -notmatch '^\s*\|') { continue }
            $cells = @(($line -replace '\\\|', '') -split '\|' | ForEach-Object { $_.Trim() })
            $severe = @($cells | Where-Object { $_ -match '【(红旗|重要)】' }).Count -gt 0
            $kept = @($cells | Where-Object { $_ -match '^(不改|挂账)' }).Count -gt 0
            if ($severe -and $kept) { $rows.Add($line.Trim()) }
        }
    }
    $rows.ToArray()
}

function Get-Waiver {
    param($Facts)
    @($Facts.Waivers) | Where-Object { $_ -and $Facts.Head.StartsWith([string]$_.head) -and ([string]$_.head).Length -ge 7 } | Select-Object -First 1
}

function Get-MergeVerdict {
    param([Parameter(Mandatory)]$Facts)
    $F = $Facts
    $fail = [System.Collections.Generic.List[string]]::new()
    $warn = [System.Collections.Generic.List[string]]::new()
    $needs = [System.Collections.Generic.List[object]]::new()
    $need = { param($level, $kind, $reason) $needs.Add([pscustomobject]@{ Level = $level; Kind = $kind; Reason = $reason }) }
    $p = $F.Pull
    $head = $F.Head

    # 1. PR 本身：打开、非 draft、base 是 feature 线、head 是本仓的 feat/ 子支线、可合并
    if ($p.state -ne 'open' -or [bool](Get-RinneProp $p 'merged')) { $fail.Add('PR 不是打开状态') }
    if ([bool]$p.draft) { $fail.Add('PR 是 draft') }
    if ($p.base.ref -notmatch '^feature/') { $fail.Add("base 必须是 feature/**：$($p.base.ref)") }
    if ($p.head.ref -notmatch '^feat/[a-z0-9][a-z0-9-]*$') { $fail.Add("head 必须是 feat/<slug>：$($p.head.ref)") }
    if ($p.head.repo.full_name -ne (Get-RinneRepo)) { $fail.Add('head 不是本仓的分支') }
    if ($F.PrRef -ne $head) { $fail.Add('本地取到的 PR head 与接口不一致（刚有推送？）') }
    if ((Get-RinneProp $p 'mergeable') -ne $true -or (Get-RinneProp $p 'mergeable_state') -in @('dirty', 'draft')) {
        $fail.Add("GitHub 判为不可合并（mergeable=$(Get-RinneProp $p 'mergeable')，state=$(Get-RinneProp $p 'mergeable_state')）")
    }

    # 2. bot 结论：最近一条总结审的是当前 head、尾注可用、red=0 且 important=0（或有用户对这个 head 的豁免）
    $s = $F.State.summary
    $waiver = Get-Waiver $F
    if (-not $s) { $fail.Add('没有 bot 审查总结') }
    else {
        if ($s.head -ne $head) { $fail.Add("最近一条 bot 总结审的是 $(([string]$s.head).Substring(0, [Math]::Min(7, ([string]$s.head).Length)))，不是当前 head") }
        if (-not $s.ok) { $fail.Add("bot 结论不可用：$($s.reason)") }
        else {
            $sameHead = @($F.State.summaries | Where-Object { $_.head -eq $head -and $_.ok } | ForEach-Object { "$($_.red)/$($_.important)/$($_.suggest)" } | Select-Object -Unique)
            if ($sameHead.Count -gt 1) { $fail.Add('当前 head 上有多条 bot 总结且尾注互相矛盾') }
            if ($s.red -gt 0 -or $s.important -gt 0) {
                if ($waiver -and $s.red -le [int]$waiver.red -and $s.important -le [int]$waiver.important) {
                    $warn.Add("尾注 red=$($s.red) important=$($s.important)，已有用户豁免：$($waiver.note)")
                } else { $fail.Add("尾注 red=$($s.red) important=$($s.important)，必须为 0（或有用户豁免）") }
            }
        }
    }

    # 3. 总结绑定到那次 bot 运行：运行链接指向 claude-review（或 /review）、该运行审的就是当前 head 且成功，
    #    claude-pr-review 这个 check 也指向同一次运行，评论发在运行期间。任何 workflow 都能以 github-actions[bot] 发评论，只看作者不够。
    if ($s) {
        $r = $F.Run
        if (-not $s.runId) { $fail.Add('bot 总结里没有运行链接') }
        elseif (-not $r) { $fail.Add("取不到 bot 总结引用的运行 $($s.runId)") }
        else {
            if ($ReviewWorkflows -notcontains $r.path) { $fail.Add("bot 总结引用的运行不是评审 workflow：$($r.path)") }
            if ($r.conclusion -ne 'success') { $fail.Add("bot 运行的结论是 $($r.conclusion)") }
            if ($r.path -eq $ReviewWorkflows[0]) {
                if ($r.event -ne 'pull_request' -or $r.head_sha -ne $head) { $fail.Add('bot 运行审的不是当前 head') }
                $job = @($F.Jobs | Where-Object { $_.name -eq 'claude-pr-review' }) | Select-Object -First 1
                if (-not $job -or $job.conclusion -ne 'success') { $fail.Add('bot 运行里的 claude-pr-review job 没有成功') }
                $cr = if ($F.Checks.ContainsKey('claude-pr-review')) { $F.Checks['claude-pr-review'] } else { $null }
                if (-not $cr -or $cr.conclusion -ne 'success' -or [string]$cr.details_url -notmatch "/runs/$($s.runId)/") {
                    $fail.Add('当前 head 上的 claude-pr-review check 不是总结引用的那次运行，或没有成功')
                }
            } elseif ($r.path -eq $ReviewWorkflows[1] -and $r.event -ne 'issue_comment') { $fail.Add('/review 运行的事件不是 issue_comment') }
            $created = (ConvertTo-RinneUtc $s.createdAt)
            if ($created -lt (ConvertTo-RinneUtc $r.run_started_at) -or $created -gt (ConvertTo-RinneUtc $r.updated_at).AddMinutes(1)) {
                $fail.Add('bot 总结不是在那次运行期间发的')
            }
        }
    }

    # 4. 处置汇总：最后一条 bot 总结之后有一条，且引用那次运行；之前每一轮的总结也都有处置汇总回应
    $d = $F.State.disposition
    if (-not $d) { $fail.Add('最后一条 bot 总结之后没有处置汇总') }
    elseif ($s -and $d.botRun -ne $s.runId) { $fail.Add("处置汇总引用的运行 $($d.botRun) 不是最后一条 bot 总结的 $($s.runId)") }
    foreach ($x in @($F.State.summaries)) {
        if ($x.runId -and @($F.State.dispositionRuns) -notcontains [string]$x.runId) { $fail.Add("运行 $($x.runId) 的 bot 总结没有处置汇总") }
    }

    # 5. 线程与回应：open thread = 0；每条 bot 与人的评论都有回应；外部评论交用户
    $open = @($F.Threads | Where-Object { -not $_.isResolved }).Count
    if ($open -gt 0) { $fail.Add("还有 $open 个未 resolve 的线程") }
    foreach ($i in @($F.Items)) {
        if ($i.Kind -eq 'external') { & $need 'user' 'external' "外部评论 $($i.Source) $($i.Id)（$($i.Author)）" ; continue }
        if (-not (Test-RinneResponded -Item $i -Items $F.Items -State $F.State)) { $fail.Add("没有回应的评论：$($i.Source) $($i.Id)（$($i.Kind)）") }
    }
    $deferred = @(Get-DeferredSevere $F.Items)
    if ($deferred.Count -gt 0 -and -not ($waiver -and [bool](Get-RinneProp $waiver 'deferred'))) {
        & $need 'user' 'deferred' "有 $($deferred.Count) 条【红旗】【重要】判为不改或挂账，只能由用户豁免"
    }

    # 6. 静态 check：feature 线 required checks 在当前 head 上都是 success
    foreach ($ctx in @($F.Required)) {
        $c = if ($F.Checks.ContainsKey($ctx)) { $F.Checks[$ctx] } else { $null }
        if (-not $c -or $c.conclusion -ne 'success') { $fail.Add("check $ctx 在当前 head 上不是 success") }
    }

    # 7. 本地 gates：json 存在、sha = head、档位够、exit=0、fail=0、dirty=false
    $g = $F.Gates
    if (-not $g) { $fail.Add('缺当前 head 的 gates json') }
    else {
        if ($g.sha40 -ne $head) { $fail.Add('gates json 的 sha 不是当前 head') }
        if (-not $TierRank.ContainsKey([string]$g.tier) -or $TierRank[[string]$g.tier] -lt $TierRank[[string]$F.Card.tier]) {
            $fail.Add("gates 档位 $($g.tier) 低于卡片要求的 $($F.Card.tier)")
        }
        if ([int]$g.exit -ne 0 -or [int]$g.fail -ne 0) { $fail.Add("gates 没有通过（exit=$($g.exit)，fail=$($g.fail)）") }
        if ([bool]$g.dirty) { $fail.Add('gates 跑在脏工作树上') }
    }

    # 8. 文本隐私复扫：PR 标题与正文（标题格式、隐私、AI 署名、末行签名），以及我方发过的每条评论
    if ($F.PrText.Code -ne 0) { $fail.Add("PR 标题或正文没过检查：$(($F.PrText.Out + "`n" + $F.PrText.Err).Trim() -replace '\s+', ' ')") }
    $pt = Test-PublicText "$($p.title)`n$([string](Get-RinneProp $p 'body'))"
    if (-not $pt.Ok) { $fail.Add("PR 标题或正文没过公开文本检查：$(@($pt.Findings | ForEach-Object rule) -join '、')") }
    foreach ($i in @($F.Items | Where-Object Kind -eq 'agent')) {
        $t = Test-PublicText $i.Body
        if (-not $t.Ok) { $fail.Add("我方评论 $($i.Id) 没过公开文本检查：$(@($t.Findings | ForEach-Object rule) -join '、')") }
    }

    # 9. 范围：改动文件 ⊆ owns + takesOver + sections；碰基础设施路径（清单取自 scripts/lib/infra-paths.mjs）一律交用户批准，不自合
    $scope = Get-CardScope $F.Card
    $outside = @($F.Files | Where-Object { -not (Test-GlobMatch $_.filename $scope) } | ForEach-Object filename)
    if ($outside.Count -gt 0) { & $need 'orchestrator' 'out-of-scope' "越界改动：$($outside -join '、')" }
    $infra = @(Get-InfraTouches -Files $F.Files)
    if ($infra.Count -gt 0) { & $need 'user' 'infra' "碰到基础设施路径，交用户批准：$($infra -join '、')" }

    # 10. 分支 dispatch、合并后构建、契约守卫、DCO 上限
    if ($F.NeedsDispatch -and -not @($F.DispatchRuns | Where-Object { $_.conclusion -eq 'success' -and $_.head_sha -eq $head })) {
        $fail.Add('需要分支 dispatch，但当前 head 没有成功的 build 运行')
    }
    if ($F.Premerge -and -not $F.Premerge.Ok) { $fail.Add("合并后构建没有通过：$($F.Premerge.Reason)") }
    if ($F.Guard.Code -ne 0) { $fail.Add("对当前 origin/feature/v1 重跑契约守卫没有通过：$(($F.Guard.Out + "`n" + $F.Guard.Err).Trim() -replace '\s+', ' ')") }
    if ($F.Dco -ge 200) { & $need 'orchestrator' 'dco' "origin/dev..origin/feature/v1 已有 $($F.Dco) 个提交，暂停合并" }
    elseif ($F.Dco -ge 180) { $warn.Add("origin/dev..origin/feature/v1 已有 $($F.Dco) 个提交（200 暂停合并）") }

    $code = if ($fail.Count) { 'not-ready' } elseif ($needs.Count) { 'needs-user' } else { 'ready' }
    $level = if ($code -ne 'needs-user') { $null } elseif (@($needs | Where-Object Level -eq 'user').Count) { 'user' } else { 'orchestrator' }
    [pscustomobject]@{
        Code = $code; Level = $level; Pr = $F.Pr; Head = $head
        Failures = $fail.ToArray(); Needs = $needs.ToArray(); Warnings = $warn.ToArray()
        OutOfScope = $outside; Infra = $infra; Deferred = $deferred
        Command = "pwsh scripts/maintainer/pr-merge.ps1 -Merge -Pr $($F.Pr) -Card $($F.Card.id) -SummaryFile <摘要段文件>$(if ($code -eq 'needs-user' -and @($needs | Where-Object Kind -ne 'infra').Count -eq 0) { ' -ApprovedByUser（仅在用户批准后）' })"
    }
}

# ---- 三个动作 -------------------------------------------------------------------------------

function Assert-MainClone {
    $main = [IO.Path]::GetFullPath((Join-Path (Get-RinneRoot) 'rinne'))
    if ((Get-RinneRepoRoot).TrimEnd('\', '/') -ne $main.TrimEnd('\', '/')) { throw 'pr-merge 只从主 clone 运行（判据要取 feature 线上的版本）' }
}

function Get-Signoff { (Invoke-RinneMaint -Command 'signoff' -Content '').signoff }

<# squash 提交消息：subject「<标题> (#n)」、摘要段、空行、唯一一行签名；整条过提交消息判据与公开文本检查。 #>
function New-MergeMessage {
    param([Parameter(Mandatory)]$Pull, [Parameter(Mandatory)][string]$Summary)
    $text = $Summary.Trim()
    if (-not $text) { throw '摘要段不能为空' }
    if ($text -match '(?im)^\s*(signed-off-by|co-authored-by|claude-session)\s*:') { throw '摘要段里不要写尾注，签名由脚本加' }
    $title = "$($Pull.title) (#$($Pull.number))"
    $message = "$title`n`n$text`n`n$(Get-Signoff)"
    $v = Invoke-RinneMaint -Command 'message' -Content $message -Extra @('--needles', 'local')
    if (@($v.errors).Count) { throw "squash 消息没过检查：$(@($v.errors) -join '；')" }
    Assert-PublicText $message 'squash 消息'
    @{ Title = $title; Body = "$text`n`n$(Get-Signoff)" }
}

function Invoke-Merge {
    param([int]$Pr, [string]$CardId, [string]$SummaryFile, [switch]$ApprovedByUser)
    $v = Get-MergeVerdict (Get-MergeFacts $Pr $CardId)
    $onlyInfra = $v.Code -eq 'needs-user' -and @($v.Needs | Where-Object Kind -ne 'infra').Count -eq 0
    if (-not ($v.Code -eq 'ready' -or ($onlyInfra -and $ApprovedByUser))) {
        Write-Output (ConvertTo-RinneJsonText $v)
        throw "不能合并：code=$($v.Code)"
    }
    $pull = Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr"
    if ($pull.head.sha -ne $v.Head) { throw '核对之后 PR head 变了，重新核对' }
    $msg = New-MergeMessage -Pull $pull -Summary (Get-Content -Raw -LiteralPath $SummaryFile)
    $body = @{ merge_method = 'squash'; sha = $v.Head; commit_title = $msg.Title; commit_message = $msg.Body }
    $res = Invoke-RinneApiWrite -Method PUT -Path "repos/$(Get-RinneRepo)/pulls/$Pr/merge" -Body $body
    if (-not $res.merged) { throw "合并没有成功：$($res.message)" }
    Write-Output "已合并 #$Pr → $($res.sha)"
}

function Invoke-After {
    param([int]$Pr, [string]$CardId)
    $pull = Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr"
    if (-not $pull.merged) { throw "#$Pr 还没有合并" }
    $sha = [string]$pull.merge_commit_sha
    $branch = [string]$pull.head.ref
    if ($branch -notmatch '^feat/[a-z0-9][a-z0-9-]*$') { throw "子支线名不对：$branch" }
    Invoke-RinneGitLocked { Invoke-RinneGit @('fetch', '--no-tags', 'origin', '+refs/heads/feature/v1:refs/remotes/origin/feature/v1') | Out-Null } | Out-Null
    if ((Invoke-RinneGit @('merge-base', '--is-ancestor', $sha, 'origin/feature/v1') -AllowFail).Code -ne 0) { throw "$sha 不在 origin/feature/v1 上" }
    # 回读：作者与提交者、DCO（check-commits 同仓档），恰好一行维护者签名，subject 带 (#n)
    $c = Invoke-RinneNodeScript 'scripts/check-commits.mjs' @('commits', '--range', "$sha^..$sha", '--tier', 'same') -AllowFail
    if ($c.Code -ne 0) { throw "squash 提交没过回读：$(($c.Out + ' ' + $c.Err).Trim())" }
    $message = (Invoke-RinneGit @('log', '-1', '--format=%B', $sha)).Out
    $m = Invoke-RinneMaint -Command 'message' -Content $message -Extra @('--needles', 'local')
    if (@($m.errors).Count) { throw "squash 消息回读不通过：$(@($m.errors) -join '；')" }
    if (($message -split "`n")[0] -notmatch "\(#$Pr\)$") { throw "squash subject 没有以 (#$Pr) 结尾" }
    # 删远端子支线（pre-push hook 放行 feat/* 的删除），再持锁移除 worktree 与本地分支
    if ((Invoke-RinneGit @('ls-remote', '--exit-code', '--heads', 'origin', $branch) -AllowFail).Code -eq 0) {
        Invoke-RinneGit @('push', 'origin', '--delete', "refs/heads/$branch") | Out-Null
    }
    $wt = Join-Path (Get-RinneRoot) "wt/$CardId"
    Invoke-RinneGitLocked {
        if (Test-Path -LiteralPath $wt) { Invoke-RinneGit @('worktree', 'remove', $wt) | Out-Null }
        if ((Invoke-RinneGit @('rev-parse', '--verify', '--quiet', "refs/heads/$branch") -AllowFail).Code -eq 0) {
            Invoke-RinneGit @('branch', '-D', $branch) | Out-Null
        }
    } | Out-Null
    Write-Output "#$Pr 已合入 $($sha.Substring(0, 7))「$($pull.title)」；回读通过，远端子支线与 worktree 已清理"
}

# ---- 自测 -----------------------------------------------------------------------------------

function Invoke-SelfTest {
    $cases = New-RinneCases
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "rinne-merge-selftest-$PID"
    # 假卡号分开拼：源码里不出现完整的卡号形态（隐私规则表会拦）
    $cid = 'RN' + '-99a'
    $cardsFile = Join-Path $tmp 'card-list.json'
    New-Item -ItemType Directory -Force -Path (Join-Path $tmp "_logs/$cid") | Out-Null
    $oldRoot = $env:RINNE_ROOT
    $oldCards = $env:RINNE_CARDS
    $oldPriv = $env:PRIVACY_NEEDLES
    try {
        $env:RINNE_ROOT = $tmp
        $env:RINNE_CARDS = $cardsFile
        $env:PRIVACY_NEEDLES = 'rinne-selftest-fake-needle-7c1e'
        Set-RinneNeedlesMode 'ci'
        Clear-RinnePermCache
        $pull = Read-RinneFixture 'pr3-pull.json'
        $pull.state = 'open'; $pull.merged = $false; $pull.mergeable = $true; $pull.mergeable_state = 'clean'
        # 基线：改动只在卡的 owns 之内、不碰基础设施（PR #3 本身碰了 workflow，换成一份文档）
        $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'added' })
        $card = [pscustomobject]@{ id = $cid; owns = @('docs/example.md'); takesOver = @(); sections = @(); tier = 'Quick'; needsBranchDispatch = $false }
        ConvertTo-Json -InputObject @($card) -Depth 10 | Set-Content -LiteralPath $cardsFile
        $head = $pull.head.sha
        @{ sha40 = $head; tier = 'Quick'; pass = 9; fail = 0; skip = 2; flaky = 0; exit = 0; dirty = $false } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $tmp "_logs/$cid/gates-$head.json")
        $log = [System.Collections.Generic.List[string]]::new()
        $routes = [ordered]@{
            'collaborators/DLsnows/permission' = { 'admin' }
            'collaborators/[^/]+/permission'   = { @{ Code = 0; Out = 'read'; Err = '' } }
            'pulls/3/files'                    = { $files }
            'pulls/3/comments'                 = { Read-RinneFixture 'pr3-review-comments.json' }
            'pulls/3/reviews'                  = { Read-RinneFixture 'pr3-reviews.json' }
            'pulls/3$'                         = { $pull }
            'issues/3/comments'                = { Read-RinneFixture 'pr3-issue-comments.json' }
            'api -X POST graphql'              = { Read-RinneFixture 'pr3-threads.json' }
            'actions/runs/\d+/jobs'            = { Read-RinneFixture 'pr3-run-jobs.json' }
            'actions/runs/\d+$'                = { Read-RinneFixture 'pr3-run.json' }
            'check-runs'                       = { Read-RinneFixture 'pr3-check-runs.json' }
            '^git .* fetch '                   = { '' }
            '^git .* rev-parse refs/remotes/origin/pr/3' = { $head }
            '^git .* rev-list --count'         = { '12' }
            'check-commits\.mjs contract-guard' = { @{ Code = 0; Out = 'check-commits contract-guard：通过'; Err = '' } }
        }
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Set-RinneLockImpl { param($sb) & $sb }
        # 基础设施清单换成 fixture 里的一份（匹配规则仍取自 scripts/lib/infra-paths.mjs）
        Set-RinneInfraList (Join-Path $PSScriptRoot 'fixtures/infra-paths.json')
        $base = Get-MergeFacts -Pr 3 -CardId $cid
        $v = Get-MergeVerdict $base
        Add-RinneCase $cases '干净的 PR 判 ready' $false ($v.Code -ne 'ready')
        if ($v.Code -ne 'ready') { [Console]::Error.WriteLine((ConvertTo-RinneJsonText $v)) }
        Add-RinneCase $cases '契约守卫对当前 origin/feature/v1 重跑' $false (-not ($log | Where-Object { $_ -match 'contract-guard --base origin/feature/v1 --head refs/remotes/origin/pr/3' }))
        Add-RinneCase $cases '合并前 fetch 了 feature/v1、dev 与 PR head' $false (-not ($log | Where-Object { $_ -match 'fetch --no-tags origin .*feature/v1.*dev.*refs/pull/3/head' }))

        # 对每条判据给一个反例：改一份事实的拷贝，只动一处
        $bad = {
            param([string]$label, [scriptblock]$mutate, [string]$expect = 'not-ready', [scriptblock]$also = { param($r) $true })
            $f = [pscustomobject]@{}
            foreach ($prop in $base.PSObject.Properties) { $f | Add-Member -NotePropertyName $prop.Name -NotePropertyValue (Copy-RinneObject $prop.Value) }
            $f.Checks = @{}; foreach ($k in $base.Checks.Keys) { $f.Checks[$k] = Copy-RinneObject $base.Checks[$k] }
            $f.Items = @($base.Items | ForEach-Object { Copy-RinneObject $_ })
            & $mutate $f
            $r = Get-MergeVerdict $f
            if ($env:RINNE_SELFTEST_VERBOSE) { [Console]::Error.WriteLine("$label => $($r.Code) | $(@($r.Failures) + @($r.Needs | ForEach-Object Reason) -join ' ; ')") }
            Add-RinneCase $cases $label $true ($r.Code -eq $expect -and (& $also $r))
        }
        & $bad 'base=dev' { param($f) $f.Pull.base.ref = 'dev' }
        & $bad 'head 不是 feat/' { param($f) $f.Pull.head.ref = 'fix/x' }
        & $bad 'draft' { param($f) $f.Pull.draft = $true }
        & $bad '已关闭' { param($f) $f.Pull.state = 'closed' }
        & $bad 'fork 的 head' { param($f) $f.Pull.head.repo.full_name = 'someone/rinne' }
        & $bad '有冲突（mergeable_state=dirty）' { param($f) $f.Pull.mergeable_state = 'dirty' }
        & $bad '本地 PR head 与接口不一致' { param($f) $f.PrRef = 'b' * 40 }
        & $bad '没有 bot 总结' { param($f) $f.State.summary = $null }
        & $bad 'bot 总结审的是旧 head' { param($f) $f.State.summary.head = 'c' * 40 }
        & $bad '尾注 important=1' { param($f) $f.State.summary.important = 1 }
        & $bad '尾注 red=1' { param($f) $f.State.summary.red = 1 }
        & $bad 'bot 结论不可用' { param($f) $f.State.summary.ok = $false; $f.State.summary.reason = '机读尾注多于一行' }
        & $bad '同一 head 上两条总结尾注矛盾' { param($f) $x = Copy-RinneObject $f.State.summaries[-1]; $x.important = 2; $f.State.summaries = @($f.State.summaries) + @($x) }
        & $bad '总结没有运行链接' { param($f) $f.State.summary.runId = $null }
        & $bad '运行链接指向别的 workflow' { param($f) $f.Run.path = '.github/workflows/compliance.yml' }
        & $bad '运行审的是别的 head' { param($f) $f.Run.head_sha = 'd' * 40 }
        & $bad '运行没有成功' { param($f) $f.Run.conclusion = 'failure' }
        & $bad 'claude-pr-review check 指向另一次运行' { param($f) $f.Checks['claude-pr-review'].details_url = 'https://github.com/x/y/actions/runs/1/job/2' }
        & $bad 'claude-pr-review job 没有成功' { param($f) $f.Jobs = @() }
        & $bad '总结发在运行之外' { param($f) $f.State.summary.createdAt = '2026-10-12T06:00:00Z' }
        & $bad '没有处置汇总' { param($f) $f.State.disposition = $null }
        & $bad '处置汇总引用的不是这次运行' { param($f) $f.State.disposition.botRun = '1' }
        & $bad '有一轮的总结没有处置汇总' { param($f) $f.State.dispositionRuns = @($f.State.dispositionRuns | Select-Object -Skip 1) }
        & $bad '还有未 resolve 的线程' { param($f) $f.Threads[0].isResolved = $false; ($f.Items | Where-Object ThreadId -eq $f.Threads[0].id) | ForEach-Object { $_.Resolved = $false } }
        & $bad '维护者的评论没有回应' { param($f) $f.Items += [pscustomobject]@{ Source = 'issue'; Id = '999'; Author = 'DLsnows'; Kind = 'user'; Body = '这里再看看'; Created = '2026-10-12T05:00:00Z'; Updated = '2026-10-12T05:00:00Z'; Url = ''; IsNew = $true; ThreadId = $null; Resolved = $null; ReplyTo = '' } }
        & $bad '外部评论交用户' { param($f) $f.Items += [pscustomobject]@{ Source = 'issue'; Id = '998'; Author = 'someone'; Kind = 'external'; Body = 'lgtm'; Created = '2026-10-12T05:00:00Z'; Updated = '2026-10-12T05:00:00Z'; Url = ''; IsNew = $true; ThreadId = $null; Resolved = $null; ReplyTo = '' } } 'needs-user'
        & $bad '【重要】判为不改' { param($f) $i = $f.Items | Where-Object { $_.Kind -eq 'agent' -and $_.Source -eq 'issue' } | Select-Object -First 1; $i.Body += "`n| 9 | bot | 【重要】 | a.mjs:1 | x | 不改 | 理由 |" } 'needs-user'
        & $bad 'required check 不是 success' { param($f) $f.Checks['docs-truth'].conclusion = 'failure' }
        & $bad '缺 gates json' { param($f) $f.Gates = $null }
        & $bad 'gates 的 sha 不是 head' { param($f) $f.Gates.sha40 = 'e' * 40 }
        & $bad 'gates 档位不够' { param($f) $f.Card.tier = 'Standard' }
        & $bad 'gates 没通过' { param($f) $f.Gates.exit = 1 }
        & $bad 'gates 跑在脏工作树上' { param($f) $f.Gates.dirty = $true }
        & $bad 'PR 正文没过检查' { param($f) $f.PrText = @{ Code = 1; Out = '正文最后一行必须是签名'; Err = '' } }
        & $bad 'PR 正文含 Co-Authored-By' { param($f) $f.Pull.body = ($f.Pull.body -replace '(?m)^## DCO', "## DCO`nCo-Authored-By: a <a@b.c>") }
        & $bad '我方评论里有本机路径' { param($f) $i = $f.Items | Where-Object Kind -eq 'agent' | Select-Object -First 1; $i.Body += "`n日志在 C:" + '\Users\someone\x.log' }
        & $bad '我方评论里有个人针' { param($f) $i = $f.Items | Where-Object Kind -eq 'agent' | Select-Object -First 1; $i.Body += "`nrinne-selftest-fake-needle-7c1e" }
        & $bad '越界改动' { param($f) $f.Files = @($f.Files) + @([pscustomobject]@{ filename = 'src/core/x.cpp'; status = 'added' }) } 'needs-user' { param($r) $r.Level -eq 'orchestrator' -and $r.OutOfScope -contains 'src/core/x.cpp' }
        & $bad '非基础设施卡改了 .github/workflows/x.yml' { param($f) $f.Card.owns = @($f.Card.owns) + @('.github/workflows/x.yml'); $f.Files = @($f.Files) + @([pscustomobject]@{ filename = '.github/workflows/x.yml'; status = 'modified' }) } 'needs-user' { param($r) $r.Level -eq 'user' -and $r.Infra.Count -gt 0 }
        & $bad '把 workflow 改名挪走也算' { param($f) $f.Card.owns = @('**'); $f.Files = @([pscustomobject]@{ filename = 'old/x.yml'; status = 'renamed'; previous_filename = '.github/workflows/x.yml' }) } 'needs-user' { param($r) $r.Infra.Count -gt 0 }
        & $bad 'PR #3 原样的改动（含 workflow）交用户批准' { param($f) $f.Card.owns = @('**'); $f.Files = Read-RinneFixture 'pr3-files.json' } 'needs-user' { param($r) $r.Level -eq 'user' -and @($r.Infra | Where-Object { $_ -like '.github/workflows/*' }).Count -ge 3 }
        & $bad '改了 scripts/checks.d 下的模块' { param($f) $f.Card.takesOver = @([pscustomobject]@{ path = 'scripts/checks.d/labels.mjs' }); $f.Files = @([pscustomobject]@{ filename = 'scripts/checks.d/labels.mjs'; status = 'modified' }) } 'needs-user' { param($r) $r.Infra.Count -gt 0 -and $r.OutOfScope.Count -eq 0 }
        & $bad '需要分支 dispatch 却没有成功的运行' { param($f) $f.NeedsDispatch = $true; $f.DispatchRuns = @() }
        & $bad '合并后构建失败' { param($f) $f.Premerge = @{ Ok = $false; Reason = '构建退出码 1' } }
        & $bad '契约守卫在当前 feature/v1 上失败' { param($f) $f.Guard = @{ Code = 1; Out = '改到冻结契约'; Err = '' } }
        & $bad 'DCO 提交数到 200 暂停' { param($f) $f.Dco = 200 } 'needs-user'

        $w = [pscustomobject]@{}; foreach ($prop in $base.PSObject.Properties) { $w | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value }
        $w.State = Copy-RinneObject $base.State; $w.State.summary.important = 1
        $w.Waivers = @([pscustomobject]@{ head = $head.Substring(0, 7); red = 0; important = 1; note = '用户豁免' })
        Add-RinneCase $cases '尾注 important=1 但有用户对这个 head 的豁免' $false ((Get-MergeVerdict $w).Code -ne 'ready')
        $w.Waivers = @([pscustomobject]@{ head = 'abcdef1'; red = 0; important = 1; note = '别的 head' })
        Add-RinneCase $cases '豁免是给别的 head 的不算' $true ((Get-MergeVerdict $w).Code -eq 'not-ready')
        $w.Dco = 185; $w.Waivers = @(); $w.State = $base.State
        $vd = Get-MergeVerdict $w
        Add-RinneCase $cases 'DCO 180 只告警' $false ($vd.Code -ne 'ready' -or -not @($vd.Warnings | Where-Object { $_ -match '180|200' }).Count)

        # 合并动作：消息、钉 head、基础设施 PR 只在用户批准后放行
        $msg = New-MergeMessage -Pull $pull -Summary '加入示例检查。'
        Add-RinneCase $cases 'squash 消息：subject 带 (#3)、末行唯一签名' $false (-not ($msg.Title -match '\(#3\)$' -and $msg.Body -match "`n`nSigned-off-by: [^\n]+$" -and ([regex]::Matches($msg.Body, 'Signed-off-by')).Count -eq 1))
        Add-RinneCase $cases '摘要段里自带 Co-Authored-By' $true (Test-Throws { New-MergeMessage -Pull $pull -Summary "x`n`nCo-Authored-By: a <a@b.c>" })
        Add-RinneCase $cases '摘要段为空' $true (Test-Throws { New-MergeMessage -Pull $pull -Summary '  ' })

        # -Merge：ready 才合并，钉住 head，显式 subject 与 body；基础设施 PR 只有 -ApprovedByUser 才放行
        $sum = Join-Path $tmp 'summary.txt'
        Set-Content -LiteralPath $sum -Value '加入示例检查。' -Encoding utf8NoBOM
        $put = [System.Collections.Generic.List[object]]::new()
        $routes.Insert(0, 'api -X PUT \S+/pulls/3/merge --input', { param($argv) $put.Add((Get-Content -Raw ($argv[-1]) | ConvertFrom-Json)); @{ merged = $true; sha = 'f' * 40 } })
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null
        $m = $put[-1]
        Add-RinneCase $cases '-Merge：squash、sha=审过的 head、subject (#3)、body 末行唯一签名' $false (-not (
                $m.merge_method -eq 'squash' -and $m.sha -eq $head -and $m.commit_title -match '\(#3\)$' -and
                $m.commit_message -match "^加入示例检查。`n`nSigned-off-by: [^\n]+$"))
        $files = @([pscustomobject]@{ filename = '.github/workflows/x.yml'; status = 'modified' })
        $card.owns = @('.github/workflows/x.yml')
        ConvertTo-Json -InputObject @($card) -Depth 10 | Set-Content -LiteralPath $cardsFile
        $n = $put.Count
        Add-RinneCase $cases '-Merge：基础设施 PR 没有用户批准不合' $true ((Test-Throws { Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null }) -and $put.Count -eq $n)
        Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum -ApprovedByUser | Out-Null
        Add-RinneCase $cases '-Merge：用户批准后基础设施 PR 可合' $false ($put.Count -ne $n + 1)
        $pull.mergeable_state = 'dirty'
        Add-RinneCase $cases '-Merge：not-ready 时 -ApprovedByUser 也不合' $true ((Test-Throws { Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum -ApprovedByUser | Out-Null }) -and $put.Count -eq $n + 1)
        $pull.mergeable_state = 'clean'

        # -After：回读、删远端子支线、持锁移除 worktree 与本地分支
        $merged = Copy-RinneObject $pull
        $merged.merged = $true; $merged.state = 'closed'; $merged.merge_commit_sha = 'f' * 40
        $sq = "ci(review): 加入评审 bot、PR 模板、标签与 CODEOWNERS (#3)`n`n加入。`n`n$(Get-Signoff)"
        $locks = [System.Collections.Generic.List[string]]::new()
        Set-RinneLockImpl { param($sb) $locks.Add('lock'); & $sb }
        New-Item -ItemType Directory -Force -Path (Join-Path $tmp "wt/$cid") | Out-Null
        $after = [ordered]@{
            'pulls/3$'                       = { $merged }
            'merge-base --is-ancestor'       = { '' }
            'check-commits\.mjs commits --range' = { @{ Code = 0; Out = 'check-commits commits：通过'; Err = '' } }
            ' log -1 --format=%B'            = { $sq }
            'ls-remote --exit-code'          = { "$('a' * 40)`trefs/heads/feat/review-bot" }
            ' push origin --delete refs/heads/feat/review-bot' = { '' }
            ' worktree remove '              = { param($argv) Remove-Item -Recurse -Force -LiteralPath $argv[-1]; '' }
            'rev-parse --verify --quiet refs/heads/feat/review-bot' = { '' }
            ' branch -D feat/review-bot'     = { '' }
            '^git .* fetch '                 = { '' }
        }
        $log.Clear()
        Set-RinneFake (New-RinneFake -Routes $after -Log $log)
        $line = Invoke-After -Pr 3 -CardId $cid
        Add-RinneCase $cases '-After：回读通过后删远端子支线、持锁移除 worktree 与本地分支' $false (-not (
                $line -match '已合入 fffffff' -and @($log | Where-Object { $_ -match 'push origin --delete refs/heads/feat/review-bot' }).Count -eq 1 -and
                @($log | Where-Object { $_ -match 'worktree remove' }).Count -eq 1 -and $locks.Count -ge 2))
        $sq = "ci(review): 加入 (#3)`n`n加入。`n`n$(Get-Signoff)`n$(Get-Signoff)"
        Add-RinneCase $cases '-After：squash 消息有两行签名' $true (Test-Throws { Invoke-After -Pr 3 -CardId $cid })
        $sq = "ci(review): 加入`n`n加入。`n`n$(Get-Signoff)"
        Add-RinneCase $cases '-After：subject 没有 (#3)' $true (Test-Throws { Invoke-After -Pr 3 -CardId $cid })
        $merged.merged = $false
        Add-RinneCase $cases '-After：还没合并' $true (Test-Throws { Invoke-After -Pr 3 -CardId $cid })

        # 范围：owns 的通配与 takesOver、sections 里的 path
        $c2 = [pscustomobject]@{ owns = @('src/core/loop/**'); takesOver = @([pscustomobject]@{ path = 'src/core/engine/InputEngine.{h,cpp}' }); sections = @([pscustomobject]@{ path = 'THIRD-PARTY-NOTICES.md'; section = 'catch2' }) }
        $sc = @(Get-CardScope $c2)
        $in = @('src/core/loop/a/b.cpp', 'src/core/engine/InputEngine.cpp', 'THIRD-PARTY-NOTICES.md') | Where-Object { -not (Test-GlobMatch $_ $sc) }
        Add-RinneCase $cases '范围：** 跨目录、{h,cpp} 多选一、takesOver 与 sections 都算' $false (@($in).Count -gt 0)
        $out = @('src/core/engine/OutputEngine.cpp', 'src/core/loopx.cpp', 'src/core/engine/InputEngine.hpp') | Where-Object { Test-GlobMatch $_ $sc }
        Add-RinneCase $cases '范围：外面的路径不算' $true (@($out).Count -eq 0)
        Add-RinneCase $cases '基础设施：gates.ps1、hooks、隐私规则表、CLAUDE.md' $true (@(Get-InfraTouches -Files @('scripts/gates.ps1', 'scripts/hooks/pre-push', 'scripts/lib/privacy-needles.mjs', 'CLAUDE.md', '.github/actions-lock.json' | ForEach-Object { [pscustomobject]@{ filename = $_; status = 'modified' } })).Count -eq 5)
        Add-RinneCase $cases '基础设施：评审 bot 与检查模块（含新增）' $true (@(Get-InfraTouches -Files @('scripts/review-bot.mjs', 'scripts/checks.d/new.mjs', 'scripts/run-checks.mjs', 'REUSE.toml', '.gitleaks-version' | ForEach-Object { [pscustomobject]@{ filename = $_; status = 'added' } })).Count -eq 5)
        Add-RinneCase $cases '基础设施：src 与 docs 不算' $false (@(Get-InfraTouches -Files @('src/core/x.cpp', 'docs/a.md' | ForEach-Object { [pscustomobject]@{ filename = $_; status = 'modified' } })).Count -gt 0)
        Set-RinneInfraList (Join-Path $tmp 'no-such-list.json')
        Add-RinneCase $cases '基础设施清单取不到时判不了，不放行' $true (Test-Throws { Get-MergeVerdict $base })
    } finally {
        Set-RinneFake $null
        Set-RinneLockImpl $null
        Set-RinneInfraList $null
        Set-RinneNeedlesMode 'local'
        $env:RINNE_ROOT = $oldRoot
        $env:RINNE_CARDS = $oldCards
        $env:PRIVACY_NEEDLES = $oldPriv
        Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
    Complete-RinneSelfTest $cases 'pr-merge'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
if (@($Check, $Merge, $After | Where-Object { $_ }).Count -ne 1) { throw '用法：-Check | -Merge | -After，三选一（或 -SelfTest）' }
if ($Pr -le 0 -or -not $Card) { throw '需要 -Pr <n> 与 -Card <卡>' }
Initialize-RinneIdentity
Assert-MainClone
if ($Check) {
    $v = Get-MergeVerdict (Get-MergeFacts $Pr $Card)
    Write-Output (ConvertTo-RinneJsonText $v)
    exit $(switch ($v.Code) { 'ready' { 0 } 'needs-user' { 2 } default { 1 } })
}
if ($Merge) {
    if (-not $SummaryFile) { throw '-Merge 需要 -SummaryFile（squash 提交的摘要段）' }
    Invoke-Merge -Pr $Pr -CardId $Card -SummaryFile $SummaryFile -ApprovedByUser:$ApprovedByUser
    exit 0
}
Invoke-After -Pr $Pr -CardId $Card
