#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 合并子 PR（只进 feature/**，squash）。三步，任何一步失败都停下并打印状态，不重试：
#   -Check -Pr <n> -Card <卡>     逐条核对「干净」判据，输出一行 JSON：code = ready | not-ready | needs-user
#                                 （退出码 0 / 1 / 2）；needs-user 带 level（user 进给用户的汇总表，orchestrator 由统筹分诊）
#   -Merge -Pr <n> -Card <卡> -SummaryFile <f>
#                                 重新核对，ready 才合并：squash，钉住审过的 head（sha=…），显式写 subject「<标题> (#n)」
#                                 与 body（摘要段 + 空行 + 唯一一行签名）
#   -After -Pr <n> -Card <卡>     回读 squash 提交（DCO、作者与提交者、恰好一行签名）、删远端子支线、持 git 锁移除 worktree
#                                 与 wt/tip-<n>
#   -SelfTest                     用 fixtures/ 里录制的 PR 响应离线跑，对每条判据给反例
# 只从主 clone 运行，而且主 clone 必须正好在刚取回的 origin/feature/v1 上、工作树干净：判据取自 feature 线，
# 而不是待合并 PR 自己的版本。
#
# 用户的决定只认记录文件：主 clone 的 git config rinne.waivers（--local，不认环境变量与命令行注入的配置）指向的 JSON；
# RINNE_WAIVERS 只在 -SelfTest 下用。每条记录都要 "by": "user"，绑定一个完整的 40 位 head，之后再有推送就作废，
# 要用户对新 head 重新登记：
#   { "waivers":          { "<pr>": [{ "head": "<sha40>", "by": "user", "red": 0, "important": 1, "deferred": false, "note": "…" }] },
#     "approvals":        { "<pr>": [{ "head": "<sha40>", "by": "user", "note": "…" }] },
#     "external-handled": { "<pr>": [{ "head": "<sha40>", "by": "user", "comments": [{ "id": "<评论编号>", "updated_at": "<处理时它的 updated_at>" }], "note": "…" }] } }
# waivers 放过尾注里的【红旗】【重要】与判为不改 / 挂账的条目；approvals 是对基础设施改动的批准；
# external-handled 是用户已处理过的外部评论，绑定处理时那条评论的 updated_at（pr-comment -List 列出），之后再改过就作废。
#
# 信任模型：记录给的是「绑定到一个 head」与「留下书面痕迹」，不是认证。by 只是标签：能写这个文件的进程都能写出
# "by": "user"，所以 agent 从不写它，只由用户本人登记；-Check 与 -Merge 把依据的每一条记录原文打印出来（ReliedOn），
# 用户能看到这次合并靠的是哪几条。
#
# 合并后构建（Standard 及以上）跑的是 PR 树里的脚本，所以是最后一道：其他判据全部通过、且没碰基础设施时才跑，
# 子进程的环境里拿掉凭据与个人针。-Check 在那之前不会执行 PR 里的任何代码。每个 PR 用自己的 <根>/wt/tip-<n>，
# 用前 reset 并 git clean；建树、合并、构建到 abort 整段持一把构建槽（同一个 PR 固定落在同一把上，两次核对彼此互斥），
# 构建有超时。

[CmdletBinding()]
param(
    [switch]$Check, [switch]$Merge, [switch]$After, [switch]$SelfTest,
    [int]$Pr, [string]$Card, [string]$SummaryFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

$TierRank = @{ Quick = 1; Standard = 2; Full = 3 }
$PremergeTimeoutSeconds = 2700
$SlotWaitSeconds = 2700
$CardPattern = '^(?:RN-\d{2}[a-z]?|K\d+)$'
$ReviewWorkflows = @('.github/workflows/claude-review.yml', '.github/workflows/review-dispatch.yml')
# 改到这些路径的子支线合入前要对它的 head 手动 dispatch 一次构建（卡片清单的 needsBranchDispatch 是计划值，这里按实际 diff 再算）
$DispatchGlobs = @(
    '**/CMakeLists.txt', '**/*.cmake', 'cmake/**', '**/*.mm', '**/mac/**', '**/macos/**',
    '.github/workflows/build.yml', '.github/workflows/release.yml', 'packaging/**', 'scripts/package*'
)

# ---- 收集事实（联网、读本机文件、跑 git 与 Node 判据；不执行 PR 里的代码） ---------------

<# 记录文件的位置：真跑只认主 clone 的 git config（--local）；-FromEnv（自测）只认 RINNE_WAIVERS。 #>
function Get-RecordsPath {
    param([switch]$FromEnv)
    if ($FromEnv) { return $env:RINNE_WAIVERS }
    $r = Invoke-RinneGit @('config', '--local', '--get', 'rinne.waivers') -AllowFail
    if ($r.Code -eq 0 -and $r.Out.Trim()) { return $r.Out.Trim() }
    return $null
}

function Read-Records {
    param([int]$Pr)
    $path = Get-RecordsPath -FromEnv:$SelfTest
    $all = if ($path -and (Test-Path -LiteralPath $path)) { Get-Content -Raw -LiteralPath $path | ConvertFrom-Json -Depth 20 -DateKind String } else { $null }
    $pick = { param($kind) @(Get-RinneProp (Get-RinneProp $all $kind) ([string]$Pr)) | Where-Object { $_ } }
    [pscustomobject]@{ File = $path; Waivers = @(& $pick 'waivers'); Approvals = @(& $pick 'approvals'); ExternalHandled = @(& $pick 'external-handled') }
}

<# 对这个 head、由用户登记的记录：head 必须是完整的 40 位并且相等，by 必须是 user。 #>
function Get-HeadRecord {
    param([object[]]$List, [string]$Head)
    @($List) | Where-Object {
        $_ -and [string](Get-RinneProp $_ 'head') -eq $Head -and $Head -match '^[0-9a-f]{40}$' -and [string](Get-RinneProp $_ 'by') -eq 'user'
    } | Select-Object -First 1
}

<# external-handled 里点名的一条评论：编号相同，而且登记的 updated_at 与评论现在的 updated_at 是同一时刻。 #>
function Test-HandledComment {
    param($Record, [Parameter(Mandatory)]$Item)
    foreach ($c in @(Get-RinneProp $Record 'comments')) {
        if ([string](Get-RinneProp $c 'id') -ne $Item.Id) { continue }
        $at = [string](Get-RinneProp $c 'updated_at')
        try { if ($at -and (ConvertTo-RinneUtc $at) -eq (ConvertTo-RinneUtc $Item.Updated)) { return $true } } catch { }
    }
    return $false
}

<# 合并会不会碰到要分支 dispatch 的路径（改名的新旧路径都算），或者卡片清单预先标了要 dispatch。 #>
function Test-NeedsDispatch {
    param([Parameter(Mandatory)]$Card, [object[]]$Files = @())
    [bool](Get-RinneProp $Card 'needsBranchDispatch') -or @(Get-RinneChangedPaths $Files | Where-Object { Test-GlobMatch $_ $DispatchGlobs }).Count -gt 0
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
    $needsDispatch = Test-NeedsDispatch $card @($files)
    $dispatchRuns = if ($needsDispatch) {
        $p = "repos/$repo/actions/workflows/build.yml/runs?event=workflow_dispatch&head_sha=$head&per_page=20"
        @((Get-RinneApi $p).workflow_runs)
    } else { @() }
    Invoke-RinneGitLocked {
        $specs = @('+refs/heads/feature/v1:refs/remotes/origin/feature/v1', '+refs/heads/dev:refs/remotes/origin/dev', "+refs/pull/${Pr}/head:refs/remotes/origin/pr/$Pr")
        Invoke-RinneGit (@('fetch', '--no-tags', 'origin') + $specs) | Out-Null
    } | Out-Null
    $body = [string](Get-RinneProp $pull 'body')
    [pscustomobject]@{
        Pr = $Pr; Card = $card; Pull = $pull; Head = $head; Files = @($files); Activity = $activity
        State = $state; Items = @(Get-RinneCommentItems -Activity $activity -State $state); Threads = @($activity.Threads)
        Run = $run; Jobs = $jobs; Checks = Get-RinneCheckRuns $head; Required = Get-RinneRequiredChecks
        Gates = $gates; Records = Read-Records $Pr
        # 主 clone 的状态在 fetch 之后取：判据脚本必须就是刚取回的 feature 线上的版本
        MainClone = [pscustomobject]@{
            Head = (Invoke-RinneGit @('rev-parse', 'HEAD')).Out.Trim()
            Tip = (Invoke-RinneGit @('rev-parse', 'origin/feature/v1')).Out.Trim()
            Dirty = (Invoke-RinneGit @('status', '--porcelain')).Out.Trim()
        }
        PrRef = (Invoke-RinneGit @('rev-parse', "refs/remotes/origin/pr/$Pr")).Out.Trim()
        Dco = [int](Invoke-RinneGit @('rev-list', '--count', 'origin/dev..origin/feature/v1')).Out.Trim()
        Guard = Test-ContractGuard $Pr $body
        PrText = Test-PrText ([string]$pull.title) $body
        NeedsDispatch = $needsDispatch; DispatchRuns = $dispatchRuns
    }
}

# ---- 判据（纯函数，自测逐条给反例） ---------------------------------------------------------

<# 处置汇总表里判为「不改」或「挂账」的【红旗】【重要】：原文整行列出，供统筹记录、交用户裁决。 #>
function Get-DeferredSevere {
    param([object[]]$Items)
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($i in @($Items | Where-Object { $_.Kind -eq 'agent' -and (Get-RinneProp (Get-AgentMarker $_.Body) 'Kind') -eq 'disposition' })) {
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

function Get-MergeVerdict {
    param([Parameter(Mandatory)]$Facts)
    $F = $Facts
    $fail = [System.Collections.Generic.List[string]]::new()
    $warn = [System.Collections.Generic.List[string]]::new()
    $needs = [System.Collections.Generic.List[object]]::new()
    $need = { param($level, $kind, $reason) $needs.Add([pscustomobject]@{ Level = $level; Kind = $kind; Reason = $reason }) }
    # 判定依据的每条用户记录，原文带进输出
    $relied = [System.Collections.Generic.List[object]]::new()
    $rely = { param($kind, $record) if (-not @($relied | Where-Object Kind -eq $kind).Count) { $relied.Add([pscustomobject]@{ Kind = $kind; File = $rec.File; Record = $record }) } }
    $p = $F.Pull
    $head = $F.Head
    $rec = $F.Records

    # 0. 主 clone 正好在刚取回的 origin/feature/v1 上、工作树干净（否则判据可能是旧的）
    if ($F.MainClone.Head -ne $F.MainClone.Tip) { $fail.Add('主 clone 不在刚取回的 origin/feature/v1 上（先快进），判据可能是旧的') }
    if ($F.MainClone.Dirty) { $fail.Add('主 clone 的工作树不干净') }

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

    # 2. bot 结论：最近一条总结审的是当前 head、尾注可用、red=0 且 important=0（或有用户对这个 head 的豁免，by = user）
    $s = $F.State.summary
    $waiver = Get-HeadRecord $rec.Waivers $head
    if (-not $s) { $fail.Add('没有 bot 审查总结') }
    else {
        if ($s.head -ne $head) { $fail.Add("最近一条 bot 总结审的是 $(([string]$s.head).Substring(0, [Math]::Min(7, ([string]$s.head).Length)))，不是当前 head") }
        if (-not $s.ok) { $fail.Add("bot 结论不可用：$($s.reason)") }
        else {
            $sameHead = @($F.State.summaries | Where-Object { $_.head -eq $head -and $_.ok } | ForEach-Object { "$($_.red)/$($_.important)/$($_.suggest)" } | Select-Object -Unique)
            if ($sameHead.Count -gt 1) { $fail.Add('当前 head 上有多条 bot 总结且尾注互相矛盾') }
            if ($s.red -gt 0 -or $s.important -gt 0) {
                if ($waiver -and $s.red -le [int](Get-RinneProp $waiver 'red') -and $s.important -le [int](Get-RinneProp $waiver 'important')) {
                    $warn.Add("尾注 red=$($s.red) important=$($s.important)，已有用户对这个 head 的豁免：$(Get-RinneProp $waiver 'note')")
                    & $rely 'waiver' $waiver
                } else { $fail.Add("尾注 red=$($s.red) important=$($s.important)，必须为 0（或有用户对这个 head 的豁免）") }
            }
        }
    }

    # 3. 总结绑定到那次 bot 运行：运行链接指向 claude-review（或 /review）、该运行审的就是当前 head 且成功，
    #    claude-pr-review 这个 check 也指向同一次运行，评论发在运行期间。任何 workflow 都能以 github-actions[bot] 发评论，只看作者不够。
    #    /review 的运行由评论触发，run.head_sha 不是 PR head（head 由总结的 head 标记绑定）：要它的 dispatch job 成功，
    #    当前 head 上的 claude-pr-review check 也照样要 success。
    $crCheck = if ($F.Checks.ContainsKey('claude-pr-review')) { $F.Checks['claude-pr-review'] } else { $null }
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
                if (-not $crCheck -or $crCheck.conclusion -ne 'success' -or [string]$crCheck.details_url -notmatch "/runs/$($s.runId)/") {
                    $fail.Add('当前 head 上的 claude-pr-review check 不是总结引用的那次运行，或没有成功')
                }
            } elseif ($r.path -eq $ReviewWorkflows[1]) {
                if ($r.event -ne 'issue_comment') { $fail.Add('/review 运行的事件不是 issue_comment') }
                $job = @($F.Jobs | Where-Object { $_.name -eq 'dispatch' }) | Select-Object -First 1
                if (-not $job -or $job.conclusion -ne 'success') { $fail.Add('/review 运行里的 dispatch job 没有成功') }
                if (-not $crCheck -or $crCheck.conclusion -ne 'success') { $fail.Add('当前 head 上的 claude-pr-review check 没有成功（总结来自 /review 也要它）') }
            }
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

    # 5. 线程与回应：open thread = 0；每条 bot 与人的评论都有回应（对比原评论的 updated_at，按 in-reply-to 认）；
    #    外部评论交用户，用户处理过的（对这个 head 的 external-handled 记录点名了它，且它之后没再改过）放行
    $open = @($F.Threads | Where-Object { -not $_.isResolved }).Count
    if ($open -gt 0) { $fail.Add("还有 $open 个未 resolve 的线程") }
    $handled = Get-HeadRecord $rec.ExternalHandled $head
    foreach ($i in @($F.Items)) {
        if ($i.Kind -eq 'external') {
            if ($handled -and (Test-HandledComment $handled $i)) {
                $warn.Add("外部评论 $($i.Source) $($i.Id) 已由用户处理：$(Get-RinneProp $handled 'note')")
                & $rely 'external-handled' $handled
            } elseif ($handled -and @(Get-RinneProp $handled 'comments' | Where-Object { [string](Get-RinneProp $_ 'id') -eq $i.Id }).Count) {
                & $need 'user' 'external' "外部评论 $($i.Source) $($i.Id)（$($i.Author)）在用户处理之后改过（现在 updated_at=$($i.Updated)）"
            } else { & $need 'user' 'external' "外部评论 $($i.Source) $($i.Id)（$($i.Author)，updated_at=$($i.Updated)）" }
            continue
        }
        if (-not (Test-RinneResponded -Item $i -Items $F.Items -State $F.State)) { $fail.Add("没有回应的评论：$($i.Source) $($i.Id)（$($i.Kind)）") }
    }
    $deferred = @(Get-DeferredSevere $F.Items)
    if ($deferred.Count -gt 0) {
        if ($waiver -and [bool](Get-RinneProp $waiver 'deferred')) { & $rely 'waiver' $waiver }
        else { & $need 'user' 'deferred' "有 $($deferred.Count) 条【红旗】【重要】判为不改或挂账，只能由用户豁免" }
    }

    # 6. 静态 check：feature 线 required checks 在当前 head 上都是 success
    foreach ($ctx in @($F.Required)) {
        $c = if ($F.Checks.ContainsKey($ctx)) { $F.Checks[$ctx] } else { $null }
        if (-not $c -or $c.conclusion -ne 'success') { $fail.Add("check $ctx 在当前 head 上不是 success") }
    }

    # 7. 本地 gates：json 存在、sha = head、档位够、真跑过（pass ≥ 1）、exit=0、fail=0、dirty=false
    $g = $F.Gates
    if (-not $g) { $fail.Add('缺当前 head 的 gates json') }
    else {
        if ($g.sha40 -ne $head) { $fail.Add('gates json 的 sha 不是当前 head') }
        if (-not $TierRank.ContainsKey([string]$g.tier) -or $TierRank[[string]$g.tier] -lt $TierRank[[string]$F.Card.tier]) {
            $fail.Add("gates 档位 $($g.tier) 低于卡片要求的 $($F.Card.tier)")
        }
        if ([int](Get-RinneProp $g 'pass') -lt 1) { $fail.Add('gates 一项也没通过（pass=0），不算跑过') }
        if ([int]$g.exit -ne 0 -or [int]$g.fail -ne 0) { $fail.Add("gates 没有通过（exit=$($g.exit)，fail=$($g.fail)）") }
        if ([bool]$g.dirty) { $fail.Add('gates 跑在脏工作树上') }
    }

    # 8. 文本隐私复扫：PR 标题与正文（标题格式、隐私、AI 署名、末行签名），以及我方发过的每条评论
    if ($F.PrText.Code -ne 0) { $fail.Add("PR 标题或正文没过检查：$(($F.PrText.Out + "`n" + $F.PrText.Err).Trim() -replace '\s+', ' ')") }
    $ours = @($F.Items | Where-Object Kind -eq 'agent')
    $scans = @(Test-PublicTexts (@("$($p.title)`n$([string](Get-RinneProp $p 'body'))") + @($ours | ForEach-Object Body)))
    if (-not $scans[0].Ok) { $fail.Add("PR 标题或正文没过公开文本检查：$(@($scans[0].Findings | ForEach-Object rule) -join '、')") }
    for ($k = 0; $k -lt $ours.Count; $k++) {
        $t = $scans[$k + 1]
        if (-not $t.Ok) { $fail.Add("我方评论 $($ours[$k].Id) 没过公开文本检查：$(@($t.Findings | ForEach-Object rule) -join '、')") }
    }

    # 9. 范围：改动文件（改名的新旧路径都算）⊆ owns + takesOver + sections；
    #    碰基础设施路径（清单取自 scripts/lib/infra-paths.mjs）交用户批准，只认对这个 head 的批准记录
    $scope = Get-CardScope $F.Card
    $paths = @(Get-RinneChangedPaths $F.Files)
    $outside = @($paths | Where-Object { -not (Test-GlobMatch $_ $scope) })
    if ($outside.Count -gt 0) { & $need 'orchestrator' 'out-of-scope' "越界改动：$($outside -join '、')" }
    $infra = @(Get-InfraTouches -Files $F.Files)
    $approval = Get-HeadRecord $rec.Approvals $head
    if ($infra.Count -gt 0) {
        if ($approval) { $warn.Add("碰到基础设施路径，用户已批准这个 head：$(Get-RinneProp $approval 'note')"); & $rely 'approval' $approval }
        else { & $need 'user' 'infra' "碰到基础设施路径，交用户批准（对 head $head 登记批准记录）：$($infra -join '、')" }
    }

    # 10. 分支 dispatch、契约守卫、DCO 上限（合并后构建在 Invoke-MergeCheck 里最后做）
    if ($F.NeedsDispatch -and -not @($F.DispatchRuns | Where-Object { $_.conclusion -eq 'success' -and $_.head_sha -eq $head })) {
        $fail.Add('需要分支 dispatch，但当前 head 没有成功的 build 运行')
    }
    if ($F.Guard.Code -ne 0) { $fail.Add("对当前 origin/feature/v1 重跑契约守卫没有通过：$(($F.Guard.Out + "`n" + $F.Guard.Err).Trim() -replace '\s+', ' ')") }
    if ($F.Dco -ge 200) { & $need 'orchestrator' 'dco' "origin/dev..origin/feature/v1 已有 $($F.Dco) 个提交，暂停合并" }
    elseif ($F.Dco -ge 180) { $warn.Add("origin/dev..origin/feature/v1 已有 $($F.Dco) 个提交（200 暂停合并）") }

    $code = if ($fail.Count) { 'not-ready' } elseif ($needs.Count) { 'needs-user' } else { 'ready' }
    $level = if ($code -ne 'needs-user') { $null } elseif (@($needs | Where-Object Level -eq 'user').Count) { 'user' } else { 'orchestrator' }
    [pscustomobject]@{
        Code = $code; Level = $level; Pr = $F.Pr; Head = $head
        Failures = $fail.ToArray(); Needs = $needs.ToArray(); Warnings = $warn.ToArray(); ReliedOn = $relied.ToArray()
        OutOfScope = $outside; Infra = $infra; Deferred = $deferred; Premerge = $null
        Command = "pwsh scripts/maintainer/pr-merge.ps1 -Merge -Pr $($F.Pr) -Card $($F.Card.id) -SummaryFile <摘要段文件>"
    }
}

# ---- 合并后构建（最后一道，跑的是 PR 里的代码） ---------------------------------------------

<#
  同一个 PR 固定用同一把构建槽：两次核对同一个 PR（-Check 撞上 -Merge）在槽上互斥，不会同时动 wt/tip-<n>；
  不同 PR 各用自己的目录，槽只限制同时编译的个数（与 gates 共用，同时 ≤ 2）。
#>
function Get-PremergeSlot { param([Parameter(Mandatory)][int]$Pr) "build-slot-$(1 + $Pr % 2)" }

<#
  在 <根>/wt/tip-<n> 把 PR 合到当前 feature/v1 上做一次构建，构建脚本取自合并后的树，环境里没有凭据与个人针。
  建树、清理、合并、构建到 abort 整段持构建槽；用前 reset --hard 并 git clean -ffdx，上一次留下的未跟踪文件与构建产物
  都不带进这一次；构建超时就结束整棵进程树、判红。
#>
function Invoke-Premerge {
    param([Parameter(Mandatory)][int]$Pr)
    $tip = Join-Path (Get-RinneRoot) "wt/tip-$Pr"
    # 结果写进这张表，不依赖锁函数的返回值
    $result = @{ Ok = $false; Merged = $false; Reason = '合并后构建没有跑完' }
    Invoke-RinneBuildSlotLocked -Slot (Get-PremergeSlot $Pr) -TimeoutSeconds $SlotWaitSeconds -Script {
        Invoke-RinneGitLocked {
            if (-not (Test-Path -LiteralPath $tip)) { Invoke-RinneGit @('worktree', 'add', '--detach', $tip, 'origin/feature/v1') | Out-Null }
            else {
                # reset 也清掉上一次被杀时留下的合并状态
                Invoke-RinneGit @('reset', '--hard', '--quiet') -Repo $tip | Out-Null
                Invoke-RinneGit @('checkout', '--detach', '--force', 'origin/feature/v1') -Repo $tip | Out-Null
                Invoke-RinneGit @('clean', '-ffdx', '--quiet') -Repo $tip | Out-Null
            }
            $m = Invoke-RinneGit @('merge', '--no-ff', '--no-commit', "refs/remotes/origin/pr/$Pr") -Repo $tip -AllowFail
            if ($m.Code -ne 0) { Invoke-RinneGit @('merge', '--abort') -Repo $tip -AllowFail | Out-Null }
            $result.Merged = $m.Code -eq 0
        } | Out-Null
        if (-not $result.Merged) { $result.Reason = '与当前 feature/v1 合并有冲突'; return }
        try {
            $build = Join-Path $tip 'scripts/build.ps1'
            if (-not (Test-Path -LiteralPath $build)) { $result.Reason = '合并后的树里没有 scripts/build.ps1，做不了合并后构建'; return }
            $pwsh = [Environment]::ProcessPath
            $r = Invoke-RinneWithoutSecrets {
                Invoke-RinneExternal $pwsh @('-NoProfile', '-NonInteractive', '-File', $build) -Cwd $tip -AllowFail -TimeoutSeconds $PremergeTimeoutSeconds
            }
            $result.Ok = $r.Code -eq 0
            $result.Reason = if ($r.ContainsKey('TimedOut') -and $r.TimedOut) { "合并后构建超过 $PremergeTimeoutSeconds 秒，已结束" } else { "合并后构建退出码 $($r.Code)" }
        } finally {
            Invoke-RinneGitLocked { Invoke-RinneGit @('merge', '--abort') -Repo $tip -AllowFail | Out-Null } | Out-Null
        }
    } | Out-Null
    $result
}

<#
  -Check 与 -Merge 共用的核对：先收集事实、跑全部纯判据；只有判为 ready、卡片档位在 Standard 及以上、
  而且没碰基础设施时才做合并后构建。碰了基础设施的（即便用户已批准）不在本机跑 PR 里的构建脚本。
#>
function Invoke-MergeCheck {
    param([Parameter(Mandatory)][int]$Pr, [Parameter(Mandatory)][string]$CardId)
    $facts = Get-MergeFacts $Pr $CardId
    $v = Get-MergeVerdict $facts
    if ($v.Code -eq 'ready' -and $TierRank[[string]$facts.Card.tier] -ge 2) {
        if (@($v.Infra).Count) { $v.Warnings = @($v.Warnings) + @('碰了基础设施，不在本机做合并后构建') }
        else {
            $v.Premerge = Invoke-Premerge $Pr
            if (-not $v.Premerge.Ok) {
                $v.Failures = @($v.Failures) + @("合并后构建没有通过：$($v.Premerge.Reason)")
                $v.Code = 'not-ready'
            }
        }
    }
    $v
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

function Invoke-MergeUnlocked {
    param([int]$Pr, [string]$CardId, [string]$SummaryFile)
    $v = Invoke-MergeCheck $Pr $CardId
    if ($v.Code -ne 'ready') {
        Write-Output (ConvertTo-RinneJsonText $v)
        throw "不能合并：code=$($v.Code)"
    }
    $pull = Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr"
    if ($pull.head.sha -ne $v.Head) { throw '核对之后 PR head 变了，重新核对' }
    $msg = New-MergeMessage -Pull $pull -Summary (Get-Content -Raw -LiteralPath $SummaryFile)
    foreach ($x in @($v.ReliedOn)) { Write-Output "依据的用户记录（$($x.Kind)，原文，取自 $($x.File)）：$(ConvertTo-RinneJsonText $x.Record)" }
    # sha 钉住核对过的 head：其间再有推送，GitHub 拒绝合并（批准记录也只对这个 head 有效）
    $body = @{ merge_method = 'squash'; sha = $v.Head; commit_title = $msg.Title; commit_message = $msg.Body }
    $res = Invoke-RinneApiWrite -Method PUT -Path "repos/$(Get-RinneRepo)/pulls/$Pr/merge" -Body $body
    if (-not $res.merged) { throw "合并没有成功：$($res.message)" }
    Write-Output "已合并 #$Pr → $($res.sha)"
}

function Invoke-AfterUnlocked {
    param([int]$Pr, [string]$CardId)
    if ($CardId -notmatch $CardPattern) { throw "卡号格式不对：$CardId" }
    Get-RinneCard $CardId | Out-Null
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
    # worktree 必须就在这条子支线上，免得卡号写错删了别的卡
    $wt = Join-Path (Get-RinneRoot) "wt/$CardId"
    if (Test-Path -LiteralPath $wt) {
        $cur = (Invoke-RinneGit @('symbolic-ref', '--quiet', '--short', 'HEAD') -Repo $wt -AllowFail).Out.Trim()
        if ($cur -ne $branch) { throw "$wt 在分支「$cur」上，不是 #$Pr 的 $branch：卡号对不上，不清理" }
    }
    # 删远端子支线（pre-push hook 放行 feat/* 的删除），再持锁移除 worktree 与本地分支
    if ((Invoke-RinneGit @('ls-remote', '--exit-code', '--heads', 'origin', $branch) -AllowFail).Code -eq 0) {
        Invoke-RinneGit @('push', 'origin', '--delete', "refs/heads/$branch") | Out-Null
    }
    $tip = Join-Path (Get-RinneRoot) "wt/tip-$Pr"
    Invoke-RinneGitLocked {
        if (Test-Path -LiteralPath $wt) { Invoke-RinneGit @('worktree', 'remove', $wt) | Out-Null }
        # 合并后构建的目录只装着 feature/v1 与构建产物，强制移除
        if (Test-Path -LiteralPath $tip) { Invoke-RinneGit @('worktree', 'remove', '--force', $tip) | Out-Null }
        if ((Invoke-RinneGit @('rev-parse', '--verify', '--quiet', "refs/heads/$branch") -AllowFail).Code -eq 0) {
            Invoke-RinneGit @('branch', '-D', $branch) | Out-Null
        }
    } | Out-Null
    Write-Output "#$Pr 已合入 $($sha.Substring(0, 7))「$($pull.title)」；回读通过，远端子支线、worktree 与合并后构建目录已清理"
}

<# -Merge 与 -After 整段持有 merge 锁：同一时间只合一个 PR（核对、合并、回读之间不会插进另一次合并）。 #>
function Invoke-Merge {
    param([int]$Pr, [string]$CardId, [string]$SummaryFile)
    Invoke-RinneMergeLocked { Invoke-MergeUnlocked -Pr $Pr -CardId $CardId -SummaryFile $SummaryFile }
}

function Invoke-After {
    param([int]$Pr, [string]$CardId)
    Invoke-RinneMergeLocked { Invoke-AfterUnlocked -Pr $Pr -CardId $CardId }
}

# ---- 自测 -----------------------------------------------------------------------------------

function Invoke-SelfTest {
    $cases = New-RinneCases
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "rinne-merge-selftest-$PID"
    # 假卡号分开拼：源码里不出现完整的卡号形态（隐私规则表会拦）
    $cid = 'RN' + '-99a'
    $cardsFile = Join-Path $tmp 'card-list.json'
    $recordsFile = Join-Path $tmp 'records.json'
    New-Item -ItemType Directory -Force -Path (Join-Path $tmp "_logs/$cid") | Out-Null
    $saved = @{ Root = $env:RINNE_ROOT; Cards = $env:RINNE_CARDS; Waivers = $env:RINNE_WAIVERS }
    $needles = Enter-RinneSelfTestNeedles
    try {
        $env:RINNE_ROOT = $tmp
        $env:RINNE_CARDS = $cardsFile
        $env:RINNE_WAIVERS = $recordsFile
        Set-Content -LiteralPath $recordsFile -Value '{}'
        Clear-RinnePermCache
        $pull = Read-RinneFixture 'pr3-pull.json'
        $pull.state = 'open'; $pull.merged = $false; $pull.mergeable = $true; $pull.mergeable_state = 'clean'
        # 基线：改动只在卡的 owns 之内、不碰基础设施（PR #3 本身碰了 workflow，换成一份文档）
        $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'added' })
        $card = [pscustomobject]@{ id = $cid; owns = @('docs/example.md'); takesOver = @(); sections = @(); tier = 'Quick'; needsBranchDispatch = $false }
        $writeCard = { ConvertTo-Json -InputObject @($card) -Depth 10 | Set-Content -LiteralPath $cardsFile }
        & $writeCard
        $head = $pull.head.sha
        $gates = @{ sha40 = $head; tier = 'Quick'; pass = 9; fail = 0; skip = 2; flaky = 0; exit = 0; dirty = $false }
        $writeGates = { $gates | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $tmp "_logs/$cid/gates-$head.json") }
        & $writeGates
        $log = [System.Collections.Generic.List[string]]::new()
        $builds = [System.Collections.Generic.List[object]]::new()
        $tipOps = [System.Collections.Generic.List[object]]::new()
        $buildResult = @{ Code = 0; Out = 'built'; Err = '' }
        $tipMergeCode = 0
        # GraphQL 的 review 修改时间可以按 id 改写（造「回复之后又改过」）
        $reviewEdits = @{}
        # 锁的假实现记下持有顺序与当前持有的锁，构建与 wt/tip-<n> 的 git 操作据此断言自己在哪些锁里
        $lockLog = [System.Collections.Generic.List[string]]::new()
        $held = [System.Collections.Generic.List[string]]::new()
        $routes = [ordered]@{
            'collaborators/DLsnows/permission' = { 'admin' }
            'collaborators/[^/]+/permission'   = { @{ Code = 0; Out = 'read'; Err = '' } }
            'pulls/3/files'                    = { $files }
            'pulls/3/comments'                 = { Read-RinneFixture 'pr3-review-comments.json' }
            'pulls/3/reviews'                  = { Read-RinneFixture 'pr3-reviews.json' }
            'pulls/3$'                         = { $pull }
            'issues/3/comments'                = { Read-RinneFixture 'pr3-issue-comments.json' }
            'api -X POST graphql'              = {
                param($argv)
                if ((Get-Content -Raw ($argv[-1]) | ConvertFrom-Json).query -notmatch 'reviews\(') { return Read-RinneFixture 'pr3-threads.json' }
                $d = Read-RinneFixture 'pr3-review-edits.json'
                foreach ($n in $d.data.repository.pullRequest.reviews.nodes) { if ($reviewEdits.ContainsKey([string]$n.databaseId)) { $n.lastEditedAt = $reviewEdits[[string]$n.databaseId] } }
                $d
            }
            'actions/runs/\d+/jobs'            = { Read-RinneFixture 'pr3-run-jobs.json' }
            'actions/runs/\d+$'                = { Read-RinneFixture 'pr3-run.json' }
            'actions/workflows/build\.yml/runs' = { @{ workflow_runs = @() } }
            'check-runs'                       = { Read-RinneFixture 'pr3-check-runs.json' }
            '^git .* fetch '                   = { '' }
            '^git .* rev-parse refs/remotes/origin/pr/3' = { $head }
            '^git .* rev-parse (HEAD|origin/feature/v1)$' = { 'c' * 40 }
            '^git .* status --porcelain$'      = { '' }
            '^git .* rev-list --count'         = { '12' }
            # 合并后构建：wt/tip-<n> 的 git 操作与构建脚本；记下当时持有的锁、目录、超时与子进程能看到的凭据
            '^git .* worktree add --detach '   = {
                param($argv)
                $tipOps.Add([pscustomobject]@{ Line = $argv -join ' '; Held = @($held) })
                New-Item -ItemType Directory -Force -Path (Join-Path $argv[-2] 'scripts') | Out-Null
                Set-Content -LiteralPath (Join-Path $argv[-2] 'scripts/build.ps1') -Value 'exit 0'
                ''
            }
            '^git -C .*tip-\d+ (reset --hard|checkout --detach|clean -ffdx|merge --no-ff|merge --abort)' = {
                param($argv)
                $tipOps.Add([pscustomobject]@{ Line = $argv -join ' '; Held = @($held) })
                if ($argv -contains '--no-ff') { return @{ Code = $tipMergeCode; Out = ''; Err = $(if ($tipMergeCode) { 'CONFLICT' } else { '' }) } }
                ''
            }
            '-NoProfile -NonInteractive -File .*build\.ps1$' = {
                param($argv, $opts)
                $getEnv = { param($n) [Environment]::GetEnvironmentVariable($n) }
                $builds.Add([pscustomobject]@{
                        GhToken = & $getEnv 'GH_TOKEN'; OtherKey = & $getEnv 'RINNE_SELFTEST_KEY'; Secret = & $getEnv 'RINNE_SELFTEST_SECRET'
                        Password = & $getEnv 'RINNE_SELFTEST_PASSWORD'; Needles = & $getEnv 'RINNE_PRIVACY_LOCAL'; Plain = & $getEnv 'RINNE_SELFTEST_PLAIN'
                        Held = @($held); Cwd = $opts.Cwd; Timeout = $opts.TimeoutSeconds
                    })
                $buildResult
            }
            'check-commits\.mjs contract-guard' = { @{ Code = 0; Out = 'check-commits contract-guard：通过'; Err = '' } }
        }
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Set-RinneLockImpl { param($name, $sb) $lockLog.Add($name); $held.Add($name); try { & $sb } finally { $held.RemoveAt($held.Count - 1) } }
        # 基础设施清单换成 fixture 里的一份（匹配规则仍取自 scripts/lib/infra-paths.mjs）
        Set-RinneInfraList (Join-Path $PSScriptRoot 'fixtures/infra-paths.json')
        $base = Get-MergeFacts -Pr 3 -CardId $cid
        $v = Get-MergeVerdict $base
        Add-RinneCase $cases '干净的 PR 判 ready' $false ($v.Code -ne 'ready')
        if ($v.Code -ne 'ready') { [Console]::Error.WriteLine((ConvertTo-RinneJsonText $v)) }
        Add-RinneCase $cases '契约守卫对当前 origin/feature/v1 重跑' $false (-not ($log | Where-Object { $_ -match 'contract-guard --base origin/feature/v1 --head refs/remotes/origin/pr/3' }))
        Add-RinneCase $cases '合并前 fetch 了 feature/v1、dev 与 PR head' $false (-not ($log | Where-Object { $_ -match 'fetch --no-tags origin .*feature/v1.*dev.*refs/pull/3/head' }))
        Add-RinneCase $cases '没有用到用户记录时 ReliedOn 为空' $false (@($v.ReliedOn).Count -gt 0)
        # review 的 updated_at：REST 没有这一项，取 GraphQL 修改时间与提交时间中较晚的一个
        $rv = @($base.Activity.Reviews | Where-Object { [string]$_.id -eq '5486375750' })[0]
        Add-RinneCase $cases 'review 的 updated_at 取 GraphQL 的 updatedAt（晚于提交时间时）' $false ((Get-RinneProp $rv 'updated_at') -ne '2026-10-12T03:51:22Z')
        $reviewEdits['5486547528'] = '2026-10-12T06:30:00Z'
        $act = Get-RinnePrActivity 3
        $reviewEdits.Clear()
        Add-RinneCase $cases 'review 正文改过：lastEditedAt 记作它的 updated_at' $true ((Get-RinneProp @($act.Reviews | Where-Object { [string]$_.id -eq '5486547528' })[0] 'updated_at') -eq '2026-10-12T06:30:00Z')
        # 记录文件：真跑只认主 clone 的 git config --local，环境变量 RINNE_WAIVERS（此刻指向自测的文件）不算
        Set-RinneFake (New-RinneFake -Routes ([ordered]@{ ' config --local --get rinne\.waivers$' = { 'records-from-git-config.json' } }))
        $fromGit = Get-RecordsPath
        $fromEnv = Get-RecordsPath -FromEnv
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Add-RinneCase $cases '真跑不认 RINNE_WAIVERS，只认 git config --local rinne.waivers' $true ($fromGit -eq 'records-from-git-config.json' -and $fromEnv -eq $recordsFile)

        # 对每条判据给一个反例：改一份事实的拷贝，只动一处
        $copy = {
            $f = [pscustomobject]@{}
            foreach ($prop in $base.PSObject.Properties) { $f | Add-Member -NotePropertyName $prop.Name -NotePropertyValue (Copy-RinneObject $prop.Value) }
            $f.Checks = @{}; foreach ($k in $base.Checks.Keys) { $f.Checks[$k] = Copy-RinneObject $base.Checks[$k] }
            $f.Items = @($base.Items | ForEach-Object { Copy-RinneObject $_ })
            $f
        }
        $bad = {
            param([string]$label, [scriptblock]$mutate, [string]$expect = 'not-ready', [scriptblock]$also = { param($r) $true })
            $f = & $copy
            & $mutate $f
            $r = Get-MergeVerdict $f
            if ($env:RINNE_SELFTEST_VERBOSE) { [Console]::Error.WriteLine("$label => $($r.Code) | $(@($r.Failures) + @($r.Needs | ForEach-Object Reason) -join ' ; ')") }
            Add-RinneCase $cases $label ($expect -ne 'ready') ($(if ($expect -eq 'ready') { $r.Code -ne 'ready' } else { $r.Code -eq $expect -and (& $also $r) }))
        }
        # 评论表重新从（改过的）原始来源生成，连分类一起测
        $reitem = { param($f, [scriptblock]$edit) $a = Copy-RinneObject $f.Activity; & $edit $a; $f.Activity = $a; $f.Items = @(Get-RinneCommentItems -Activity $a -State $f.State) }
        $user = { param($id, $login, $body, $created, $updated) [pscustomobject]@{ id = [long]$id; user = $(if ($login) { [pscustomobject]@{ login = $login; type = 'User' } } else { $null }); author_association = 'NONE'; created_at = $created; updated_at = $updated; body = $body; html_url = "https://github.com/x/y/pull/3#issuecomment-$id" } }
        $reply = { param($id, $to, $created, $body = '已改。') & $user $id 'DLsnows' "<!-- rinne-agent v1 role=review kind=reply round=3 head=f35523c$(if ($to) { " in-reply-to=$to" }) -->`n$body" $created $created }

        & $bad '主 clone 不在最新的 feature 线上' { param($f) $f.MainClone.Head = 'd' * 40 }
        & $bad '主 clone 的工作树不干净' { param($f) $f.MainClone.Dirty = ' M scripts/check-commits.mjs' }
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
        # 总结来自 /review：dispatch job 成功，当前 head 上的 claude-pr-review check 也要 success
        $viaReview = { param($f) $f.Run.path = '.github/workflows/review-dispatch.yml'; $f.Run.event = 'issue_comment'; $f.Run.head_sha = 'd' * 40; $f.Jobs = @([pscustomobject]@{ name = 'dispatch'; conclusion = 'success' }) }
        & $bad '总结来自 /review：dispatch job 与 head 上的 claude-pr-review 都成功放行' { param($f) & $viaReview $f } 'ready'
        & $bad '总结来自 /review：dispatch job 没有成功' { param($f) & $viaReview $f; $f.Jobs[0].conclusion = 'failure' }
        & $bad '总结来自 /review：当前 head 上的 claude-pr-review check 没有成功' { param($f) & $viaReview $f; $f.Checks['claude-pr-review'].conclusion = 'failure' }
        & $bad '总结来自 /review：当前 head 上没有 claude-pr-review check' { param($f) & $viaReview $f; $f.Checks.Remove('claude-pr-review') }
        & $bad '总结来自 /review：事件不是 issue_comment' { param($f) & $viaReview $f; $f.Run.event = 'workflow_dispatch' }
        & $bad '没有处置汇总' { param($f) $f.State.disposition = $null }
        & $bad '处置汇总引用的不是这次运行' { param($f) $f.State.disposition.botRun = '1' }
        & $bad '有一轮的总结没有处置汇总' { param($f) $f.State.dispositionRuns = @($f.State.dispositionRuns | Select-Object -Skip 1) }
        & $bad '还有未 resolve 的线程' { param($f) $f.Threads[0].isResolved = $false; ($f.Items | Where-Object ThreadId -eq $f.Threads[0].id) | ForEach-Object { $_.Resolved = $false } }

        # 回应：对比原评论的 updated_at、只认 in-reply-to、review 状态
        & $bad '维护者的评论没有回应' { param($f) & $reitem $f { param($a) $a.Issue += & $user 999 'DLsnows' '这里再看看' '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z' } }
        & $bad '点名回复过的维护者评论放行' { param($f) & $reitem $f { param($a) $a.Issue += & $user 999 'DLsnows' '这里再看看' '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z'; $a.Issue += & $reply 1000 '999' '2026-10-12T05:10:00Z' } } 'ready'
        & $bad '回复之后原评论又改过' { param($f) & $reitem $f { param($a) $a.Issue += & $user 999 'DLsnows' '这里再看看（补充）' '2026-10-12T05:00:00Z' '2026-10-12T06:00:00Z'; $a.Issue += & $reply 1000 '999' '2026-10-12T05:10:00Z' } }
        & $bad '只是正文里碰巧有这个编号' { param($f) & $reitem $f { param($a) $a.Issue += & $user 995 'DLsnows' '再跑一遍' '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z'; $a.Issue += & $reply 1000 $null '2026-10-12T05:10:00Z' '已跑 995 个用例' } }
        & $bad '维护者 Request changes（空正文）没有回应' { param($f) & $reitem $f { param($a) $a.Reviews += [pscustomobject]@{ id = 777; state = 'CHANGES_REQUESTED'; body = ''; user = [pscustomobject]@{ login = 'DLsnows'; type = 'User' }; submitted_at = '2026-10-12T05:00:00Z'; html_url = 'u' } } }
        & $bad 'Request changes 之后同一人 APPROVED 放行' { param($f) & $reitem $f { param($a) $a.Reviews += [pscustomobject]@{ id = 777; state = 'CHANGES_REQUESTED'; body = ''; user = [pscustomobject]@{ login = 'DLsnows'; type = 'User' }; submitted_at = '2026-10-12T05:00:00Z'; html_url = 'u' }; $a.Reviews += [pscustomobject]@{ id = 778; state = 'APPROVED'; body = ''; user = [pscustomobject]@{ login = 'DLsnows'; type = 'User' }; submitted_at = '2026-10-12T05:30:00Z'; html_url = 'u' } } } 'ready'
        & $bad 'Request changes 有点名回复放行' { param($f) & $reitem $f { param($a) $a.Reviews += [pscustomobject]@{ id = 777; state = 'CHANGES_REQUESTED'; body = ''; user = [pscustomobject]@{ login = 'DLsnows'; type = 'User' }; submitted_at = '2026-10-12T05:00:00Z'; html_url = 'u' }; $a.Issue += & $reply 1000 '777' '2026-10-12T05:10:00Z' } } 'ready'
        & $bad '空正文、没有 inline 评论的 COMMENTED review 没有回应' { param($f) & $reitem $f { param($a) $a.Reviews += [pscustomobject]@{ id = 779; state = 'COMMENTED'; body = ''; user = [pscustomobject]@{ login = 'DLsnows'; type = 'User' }; submitted_at = '2026-10-12T05:00:00Z'; html_url = 'u' } } }
        # review 正文：REST 没有 updated_at，改过的时间来自 GraphQL（Get-RinnePrActivity 补进 updated_at）
        $bodyReview = { param($a, $updated) $a.Reviews += [pscustomobject]@{ id = 780; state = 'COMMENTED'; body = '整体再看看命名'; user = [pscustomobject]@{ login = 'DLsnows'; type = 'User' }; submitted_at = '2026-10-12T05:00:00Z'; updated_at = $updated; html_url = 'u' }; $a.Issue += & $reply 1000 '780' '2026-10-12T05:10:00Z' }
        & $bad '点名回复过的 review 正文放行' { param($f) & $reitem $f { param($a) & $bodyReview $a '2026-10-12T05:00:00Z' } } 'ready'
        & $bad 'review 正文在我方回复之后改过' { param($f) & $reitem $f { param($a) & $bodyReview $a '2026-10-12T06:00:00Z' } }
        # 维护者本人的 inline 评论：要有我方回复，线程 resolve 了也不算回应
        $userInline = {
            param($a, [bool]$withReply)
            $c = & $user 8001 'DLsnows' '这里换个名字' '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z'
            $c | Add-Member -NotePropertyName pull_request_review_id -NotePropertyValue 9001
            $a.Review += $c
            $nodes = @([pscustomobject]@{ databaseId = 8001 })
            if ($withReply) {
                $r = & $reply 8002 $null '2026-10-12T05:10:00Z' '已改名。'
                $r | Add-Member -NotePropertyName pull_request_review_id -NotePropertyValue 9002
                $a.Review += $r
                $nodes += [pscustomobject]@{ databaseId = 8002 }
            }
            $a.Threads += [pscustomobject]@{ id = 'PRRT_user'; isResolved = $true; comments = [pscustomobject]@{ nodes = $nodes } }
        }
        & $bad '维护者的 inline 评论只 resolve、没有回复' { param($f) & $reitem $f { param($a) & $userInline $a $false } }
        & $bad '维护者的 inline 评论线程里有我方回复放行' { param($f) & $reitem $f { param($a) & $userInline $a $true } } 'ready'
        & $bad '账号已删除的评论当外部评论' { param($f) & $reitem $f { param($a) $a.Issue += & $user 996 $null '……' '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z' } } 'needs-user' { param($r) $r.Level -eq 'user' }
        # 外部评论：交用户；external-handled 绑定 head、by = user，以及处理时那条评论的 updated_at
        $external = { param($a, $updated = '2026-10-12T05:00:00Z') $a.Issue += & $user 998 'someone' 'lgtm' '2026-10-12T05:00:00Z' $updated }
        $handledRec = { param($h = $head, $by = 'user', $comments = @([pscustomobject]@{ id = '998'; updated_at = '2026-10-12T05:00:00Z' })) @([pscustomobject]@{ head = $h; by = $by; comments = $comments; note = '已回复' }) }
        & $bad '外部评论交用户' { param($f) & $reitem $f { param($a) & $external $a } } 'needs-user'
        & $bad '用户对这个 head 登记过已处理的外部评论放行' { param($f) & $reitem $f { param($a) & $external $a }; $f.Records.ExternalHandled = & $handledRec } 'ready'
        & $bad '已处理记录是给别的 head 的' { param($f) & $reitem $f { param($a) & $external $a }; $f.Records.ExternalHandled = & $handledRec ('e' * 40) } 'needs-user'
        & $bad '已处理记录不是用户登记的' { param($f) & $reitem $f { param($a) & $external $a }; $f.Records.ExternalHandled = & $handledRec $head 'orchestrator' } 'needs-user'
        & $bad '外部评论在用户处理之后改过' { param($f) & $reitem $f { param($a) & $external $a '2026-10-12T07:00:00Z' }; $f.Records.ExternalHandled = & $handledRec } 'needs-user' { param($r) @($r.Needs | Where-Object { $_.Reason -match '处理之后改过' }).Count -eq 1 }
        & $bad '已处理记录只写了编号、没有 updated_at' { param($f) & $reitem $f { param($a) & $external $a }; $f.Records.ExternalHandled = & $handledRec $head 'user' @('998') } 'needs-user'
        $x = & $copy
        & $reitem $x { param($a) & $external $a }
        $x.Records.ExternalHandled = & $handledRec
        $vx = Get-MergeVerdict $x
        Add-RinneCase $cases '-Check 输出依据的已处理记录原文' $false (-not ($vx.Code -eq 'ready' -and @($vx.ReliedOn | Where-Object { $_.Kind -eq 'external-handled' -and $_.Record.comments[0].updated_at -eq '2026-10-12T05:00:00Z' -and $_.File -eq $recordsFile }).Count -eq 1))
        $deferRow = { param($f) $i = $f.Items | Where-Object { $_.Kind -eq 'agent' -and $_.Source -eq 'issue' } | Select-Object -First 1; $i.Body += "`n| 9 | bot | 【重要】 | a.mjs:1 | x | 不改 | 理由 |" }
        & $bad '【重要】判为不改' { param($f) & $deferRow $f } 'needs-user'
        & $bad '挂账的【重要】只有 by=orchestrator 的豁免' { param($f) & $deferRow $f; $f.Records.Waivers = @([pscustomobject]@{ head = $head; by = 'orchestrator'; red = 0; important = 0; deferred = $true; note = '' }) } 'needs-user'
        & $bad '挂账的【重要】有用户对这个 head 的豁免放行' { param($f) & $deferRow $f; $f.Records.Waivers = @([pscustomobject]@{ head = $head; by = 'user'; red = 0; important = 0; deferred = $true; note = '用户同意挂账' }) } 'ready'
        & $bad 'required check 不是 success' { param($f) $f.Checks['docs-truth'].conclusion = 'failure' }
        & $bad '缺 gates json' { param($f) $f.Gates = $null }
        & $bad 'gates 的 sha 不是 head' { param($f) $f.Gates.sha40 = 'e' * 40 }
        & $bad 'gates 档位不够' { param($f) $f.Card.tier = 'Standard' }
        & $bad 'gates 一项也没通过（pass=0）' { param($f) $f.Gates.pass = 0 }
        & $bad 'gates 没通过' { param($f) $f.Gates.exit = 1 }
        & $bad 'gates 跑在脏工作树上' { param($f) $f.Gates.dirty = $true }
        & $bad 'PR 正文没过检查' { param($f) $f.PrText = @{ Code = 1; Out = '正文最后一行必须是签名'; Err = '' } }
        & $bad 'PR 正文含 Co-Authored-By' { param($f) $f.Pull.body = ($f.Pull.body -replace '(?m)^## DCO', "## DCO`nCo-Authored-By: a <a@b.c>") }
        & $bad '我方评论里有本机路径' { param($f) $i = $f.Items | Where-Object Kind -eq 'agent' | Select-Object -First 1; $i.Body += "`n日志在 C:" + '\Users\someone\x.log' }
        & $bad '我方评论里有个人针' { param($f) $i = $f.Items | Where-Object Kind -eq 'agent' | Select-Object -First 1; $i.Body += "`nrinne-selftest-fake-needle-7c1e" }
        & $bad '越界改动' { param($f) $f.Files = @($f.Files) + @([pscustomobject]@{ filename = 'src/core/x.cpp'; status = 'added' }) } 'needs-user' { param($r) $r.Level -eq 'orchestrator' -and $r.OutOfScope -contains 'src/core/x.cpp' }
        & $bad '从范围外改名挪进来也算越界' { param($f) $f.Files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'renamed'; previous_filename = 'src/core/other/Thing.cpp' }) } 'needs-user' { param($r) $r.OutOfScope -contains 'src/core/other/Thing.cpp' }
        & $bad '非基础设施卡改了 .github/workflows/x.yml' { param($f) $f.Card.owns = @($f.Card.owns) + @('.github/workflows/x.yml'); $f.Files = @($f.Files) + @([pscustomobject]@{ filename = '.github/workflows/x.yml'; status = 'modified' }) } 'needs-user' { param($r) $r.Level -eq 'user' -and $r.Infra.Count -gt 0 }
        & $bad '把 workflow 改名挪走也算' { param($f) $f.Card.owns = @('**'); $f.Files = @([pscustomobject]@{ filename = 'old/x.yml'; status = 'renamed'; previous_filename = '.github/workflows/x.yml' }) } 'needs-user' { param($r) $r.Infra.Count -gt 0 }
        & $bad 'PR #3 原样的改动（含 workflow）交用户批准' { param($f) $f.Card.owns = @('**'); $f.Files = Read-RinneFixture 'pr3-files.json' } 'needs-user' { param($r) $r.Level -eq 'user' -and @($r.Infra | Where-Object { $_ -like '.github/workflows/*' }).Count -ge 3 }
        & $bad '改了 scripts/checks.d 下的模块' { param($f) $f.Card.takesOver = @([pscustomobject]@{ path = 'scripts/checks.d/labels.mjs' }); $f.Files = @([pscustomobject]@{ filename = 'scripts/checks.d/labels.mjs'; status = 'modified' }) } 'needs-user' { param($r) $r.Infra.Count -gt 0 -and $r.OutOfScope.Count -eq 0 }
        $infraFile = { param($f) $f.Card.owns = @('scripts/review-bot.mjs'); $f.Files = @([pscustomobject]@{ filename = 'scripts/review-bot.mjs'; status = 'modified' }) }
        & $bad '基础设施改动有用户对这个 head 的批准' { param($f) & $infraFile $f; $f.Records.Approvals = @([pscustomobject]@{ head = $head; by = 'user'; note = '用户看过 diff' }) } 'ready'
        & $bad '批准是在 H，之后推了 H2' { param($f) & $infraFile $f; $f.Records.Approvals = @([pscustomobject]@{ head = 'a' * 40; by = 'user'; note = '批的是上一个 head' }) } 'needs-user' { param($r) $r.Level -eq 'user' }
        & $bad '批准只写了 7 位 head' { param($f) & $infraFile $f; $f.Records.Approvals = @([pscustomobject]@{ head = $head.Substring(0, 7); by = 'user'; note = '' }) } 'needs-user'
        & $bad '批准不是用户做的' { param($f) & $infraFile $f; $f.Records.Approvals = @([pscustomobject]@{ head = $head; by = 'orchestrator'; note = '' }) } 'needs-user'
        $x = & $copy
        & $infraFile $x
        $x.Records.Approvals = @([pscustomobject]@{ head = $head; by = 'user'; note = '用户看过 diff' })
        $vx = Get-MergeVerdict $x
        Add-RinneCase $cases '-Check 输出依据的批准记录原文' $false (-not ($vx.Code -eq 'ready' -and @($vx.ReliedOn | Where-Object { $_.Kind -eq 'approval' -and $_.Record.note -eq '用户看过 diff' -and $_.Record.head -eq $head }).Count -eq 1))
        & $bad '需要分支 dispatch 却没有成功的运行' { param($f) $f.NeedsDispatch = $true; $f.DispatchRuns = @() }
        & $bad '契约守卫在当前 feature/v1 上失败' { param($f) $f.Guard = @{ Code = 1; Out = '改到冻结契约'; Err = '' } }
        & $bad 'DCO 提交数到 200 暂停' { param($f) $f.Dco = 200 } 'needs-user'

        $w = & $copy
        $w.State.summary.important = 1
        $w.Records.Waivers = @([pscustomobject]@{ head = $head; by = 'user'; red = 0; important = 1; note = '用户豁免' })
        $vw = Get-MergeVerdict $w
        Add-RinneCase $cases '尾注 important=1 但有用户对这个 head 的豁免' $false ($vw.Code -ne 'ready')
        Add-RinneCase $cases '-Check 输出依据的豁免记录原文' $false (-not (@($vw.ReliedOn).Count -eq 1 -and $vw.ReliedOn[0].Kind -eq 'waiver' -and $vw.ReliedOn[0].Record.note -eq '用户豁免' -and $vw.ReliedOn[0].File -eq $recordsFile))
        $w.Records.Waivers = @([pscustomobject]@{ head = 'a' * 40; by = 'user'; red = 0; important = 1; note = '别的 head' })
        Add-RinneCase $cases '豁免是给别的 head 的不算' $true ((Get-MergeVerdict $w).Code -eq 'not-ready')
        $w.Records.Waivers = @([pscustomobject]@{ head = $head; by = 'orchestrator'; red = 0; important = 1; note = '统筹自己记的' })
        Add-RinneCase $cases '尾注 important=1，豁免是 by=orchestrator 写的' $true ((Get-MergeVerdict $w).Code -eq 'not-ready')
        $w.State.summary.important = 0; $w.State.summary.red = 1
        $w.Records.Waivers = @([pscustomobject]@{ head = $head; red = 1; important = 0; note = '没写 by' })
        Add-RinneCase $cases '尾注 red=1，豁免没写 by' $true ((Get-MergeVerdict $w).Code -eq 'not-ready')
        $w = & $copy
        $w.Dco = 185
        $vd = Get-MergeVerdict $w
        Add-RinneCase $cases 'DCO 180 只告警' $false ($vd.Code -ne 'ready' -or -not @($vd.Warnings | Where-Object { $_ -match '180|200' }).Count)

        # 分支 dispatch：改名的新旧路径都算，把 cmake/ 下的文件挪出去也要 dispatch
        $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'renamed'; previous_filename = 'cmake/RinneTargets.cmake' })
        $fd = Get-MergeFacts -Pr 3 -CardId $cid
        $vd = Get-MergeVerdict $fd
        $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'added' })
        Add-RinneCase $cases '把 cmake/ 下的文件改名挪走也要分支 dispatch' $true ($fd.NeedsDispatch -and $vd.Code -eq 'not-ready' -and @($vd.Failures | Where-Object { $_ -match 'dispatch' }).Count -eq 1)

        # 带超时的外部命令（真起一个 pwsh）：到点结束、返回 TimedOut；正常退出时照常给退出码
        Set-RinneFake $null
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $slow = Invoke-RinneExternal ([Environment]::ProcessPath) @('-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Seconds 60') -TimeoutSeconds 2 -AllowFail
        $sw.Stop()
        $quick = Invoke-RinneExternal ([Environment]::ProcessPath) @('-NoProfile', '-NonInteractive', '-Command', 'exit 3') -TimeoutSeconds 60 -AllowFail
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Add-RinneCase $cases '外部命令超时：到点结束进程、判超时' $true ($slow.TimedOut -and $slow.Code -ne 0 -and $sw.Elapsed.TotalSeconds -lt 30)
        Add-RinneCase $cases '外部命令没超时：照常返回退出码' $false (-not ($quick.Code -eq 3 -and -not $quick.TimedOut))

        # 合并后构建：最后一道；只在其余判据都通过、没碰基础设施时跑；子进程看不到凭据与个人针；
        # 每个 PR 自己的 wt/tip-<n>，用前清理；整段持构建槽、构建本身不在 git 锁里；带超时
        $tip = Join-Path (Get-RinneRoot) 'wt/tip-3'
        $slot = Get-PremergeSlot 3
        $card.tier = 'Standard'; & $writeCard
        $gates.tier = 'Standard'; & $writeGates
        $secretNames = 'GH_TOKEN', 'RINNE_SELFTEST_KEY', 'RINNE_SELFTEST_SECRET', 'RINNE_SELFTEST_PASSWORD', 'RINNE_SELFTEST_PLAIN'
        $saveTok = @{}; foreach ($k in $secretNames) { $saveTok[$k] = [Environment]::GetEnvironmentVariable($k) }
        try {
            $env:GH_TOKEN = 'tok-selftest'; $env:RINNE_SELFTEST_KEY = 'key-selftest'; $env:RINNE_SELFTEST_PLAIN = 'kept'
            $env:RINNE_SELFTEST_SECRET = 'secret-selftest'; $env:RINNE_SELFTEST_PASSWORD = 'password-selftest'
            $lockLog.Clear(); $tipOps.Clear()
            $vc = Invoke-MergeCheck 3 $cid
            $b = $builds | Select-Object -Last 1
            Add-RinneCase $cases '合并后构建：其余都通过时最后才跑' $false (-not ($vc.Code -eq 'ready' -and $builds.Count -eq 1 -and $vc.Premerge.Ok))
            Add-RinneCase $cases '合并后构建：子进程看不到 GH_TOKEN、*_KEY、*_SECRET、*_PASSWORD 与个人针，其他变量照常' $false (-not (
                    $b -and -not $b.GhToken -and -not $b.OtherKey -and -not $b.Secret -and -not $b.Password -and -not $b.Needles -and $b.Plain -eq 'kept'))
            Add-RinneCase $cases '合并后构建：跑完还原环境' $false ($env:GH_TOKEN -ne 'tok-selftest' -or $env:RINNE_SELFTEST_KEY -ne 'key-selftest' -or $env:RINNE_SELFTEST_SECRET -ne 'secret-selftest' -or $env:RINNE_SELFTEST_PASSWORD -ne 'password-selftest' -or $env:RINNE_PRIVACY_LOCAL -ne $needles.File)
            Add-RinneCase $cases '合并后构建：在本 PR 自己的 wt/tip-3 里跑，带超时' $false (-not ($b.Cwd -eq $tip -and $b.Timeout -gt 0))
            Add-RinneCase $cases '合并后构建：构建持着构建槽，不在 git 锁里' $false (-not ($b.Held -contains $slot -and $b.Held -notcontains 'git'))
            Add-RinneCase $cases '合并后构建：第一次用时 worktree add 到 wt/tip-3' $false (-not ($tipOps.Count -ge 1 -and $tipOps[0].Line -match "worktree add --detach $([regex]::Escape($tip)) origin/feature/v1$"))
            Add-RinneCase $cases '合并后构建：建树、合并、abort 都在构建槽里且持 git 锁' $false (-not (
                    @($tipOps | Where-Object { $_.Line -match 'merge --no-ff --no-commit refs/remotes/origin/pr/3' }).Count -eq 1 -and
                    @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -ge 1 -and
                    @($tipOps | Where-Object { $_.Held -notcontains $slot -or $_.Held -notcontains 'git' }).Count -eq 0))
            Add-RinneCase $cases '合并后构建：先取构建槽再取 git 锁' $false (-not ($lockLog.IndexOf($slot) -ge 0 -and $lockLog.LastIndexOf('git') -gt $lockLog.IndexOf($slot)))
            $tipOps.Clear()
            $vc = Invoke-MergeCheck 3 $cid
            $ops = @($tipOps | ForEach-Object { ($_.Line -replace '^-C \S+ ', '') -replace ' (--quiet|origin/feature/v1|--no-commit refs/remotes/origin/pr/3)', '' })
            Add-RinneCase $cases '合并后构建：再用时先 reset --hard、回到 feature/v1、git clean -ffdx，再合并' $false (($ops -join ' | ') -ne 'reset --hard | checkout --detach --force | clean -ffdx | merge --no-ff | merge --abort')
            Add-RinneCase $cases '合并后构建：同一个 PR 固定一把构建槽，相邻的 PR 用另一把' $false (-not ($slot -match '^build-slot-[12]$' -and (Get-PremergeSlot 3) -eq $slot -and (Get-PremergeSlot 4) -ne $slot))
            $buildResult = @{ Code = -1; Out = ''; Err = '超时'; TimedOut = $true }
            $vc = Invoke-MergeCheck 3 $cid
            $buildResult = @{ Code = 0; Out = 'built'; Err = '' }
            Add-RinneCase $cases '合并后构建：超时判红' $true ($vc.Code -eq 'not-ready' -and @($vc.Failures | Where-Object { $_ -match '超过 \d+ 秒' }).Count -eq 1)
            $gates.exit = 1; & $writeGates
            $n = $builds.Count
            $vc = Invoke-MergeCheck 3 $cid
            Add-RinneCase $cases '合并后构建：别的判据没过时不跑 PR 里的代码' $true ($vc.Code -eq 'not-ready' -and $builds.Count -eq $n)
            $gates.exit = 0; & $writeGates
            $files = @([pscustomobject]@{ filename = 'scripts/review-bot.mjs'; status = 'modified' })
            $card.owns = @('scripts/review-bot.mjs'); & $writeCard
            Set-Content -LiteralPath $recordsFile -Value (ConvertTo-Json -Depth 10 @{ approvals = @{ '3' = @(@{ head = $head; by = 'user'; note = '已批准' }) } })
            $vc = Invoke-MergeCheck 3 $cid
            Add-RinneCase $cases '合并后构建：基础设施 PR（即便已批准）不跑' $true ($vc.Code -eq 'ready' -and $builds.Count -eq $n -and @($vc.Warnings | Where-Object { $_ -match '不在本机做合并后构建' }).Count)
            $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'added' })
            $card.owns = @('docs/example.md'); & $writeCard
            Set-Content -LiteralPath $recordsFile -Value '{}'
            $tipMergeCode = 1; $tipOps.Clear(); $n = $builds.Count
            $vc = Invoke-MergeCheck 3 $cid
            $tipMergeCode = 0
            Add-RinneCase $cases '合并后构建：与 feature/v1 合并有冲突时判红、abort、不构建' $true ($vc.Code -eq 'not-ready' -and $builds.Count -eq $n -and
                @($vc.Failures | Where-Object { $_ -match '冲突' }).Count -eq 1 -and @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -ge 1)
            Remove-Item -LiteralPath (Join-Path $tip 'scripts/build.ps1')
            $vc = Invoke-MergeCheck 3 $cid
            Add-RinneCase $cases '合并后构建：树里没有构建脚本时判红' $true ($vc.Code -eq 'not-ready' -and @($vc.Failures | Where-Object { $_ -match 'build\.ps1' }).Count)
        } finally {
            foreach ($k in $secretNames) { [Environment]::SetEnvironmentVariable($k, $saveTok[$k]) }
            $card.tier = 'Quick'; & $writeCard
            $gates.tier = 'Quick'; & $writeGates
        }

        # 合并动作：消息、钉 head、基础设施 PR 只认对这个 head 的批准记录
        $msg = New-MergeMessage -Pull $pull -Summary '加入示例检查。'
        Add-RinneCase $cases 'squash 消息：subject 带 (#3)、末行唯一签名' $false (-not ($msg.Title -match '\(#3\)$' -and $msg.Body -match "`n`nSigned-off-by: [^\n]+$" -and ([regex]::Matches($msg.Body, 'Signed-off-by')).Count -eq 1))
        Add-RinneCase $cases '摘要段里自带 Co-Authored-By' $true (Test-Throws { New-MergeMessage -Pull $pull -Summary "x`n`nCo-Authored-By: a <a@b.c>" })
        Add-RinneCase $cases '摘要段为空' $true (Test-Throws { New-MergeMessage -Pull $pull -Summary '  ' })
        $sum = Join-Path $tmp 'summary.txt'
        Set-Content -LiteralPath $sum -Value '加入示例检查。' -Encoding utf8NoBOM
        $put = [System.Collections.Generic.List[object]]::new()
        $routes.Insert(0, 'api -X PUT \S+/pulls/3/merge --input', { param($argv) $put.Add((Get-Content -Raw ($argv[-1]) | ConvertFrom-Json)); @{ merged = $true; sha = 'f' * 40 } })
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        $lockLog.Clear()
        Get-MergeVerdict (Get-MergeFacts -Pr 3 -CardId $cid) | Out-Null
        Add-RinneCase $cases '-Check 只拿 git 锁，不拿 merge 锁' $false ($lockLog -contains 'merge' -or -not ($lockLog -contains 'git'))
        $lockLog.Clear()
        Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null
        Add-RinneCase $cases '-Merge 整段持有 merge 锁（先 merge 后 git）' $false (-not ($lockLog.Count -ge 2 -and $lockLog[0] -eq 'merge' -and $lockLog -contains 'git'))
        $m = $put[-1]
        Add-RinneCase $cases '-Merge：squash、sha=审过的 head、subject (#3)、body 末行唯一签名' $false (-not (
                $m.merge_method -eq 'squash' -and $m.sha -eq $head -and $m.commit_title -match '\(#3\)$' -and
                $m.commit_message -match "^加入示例检查。`n`nSigned-off-by: [^\n]+$"))
        $files = @([pscustomobject]@{ filename = '.github/workflows/x.yml'; status = 'modified' })
        $card.owns = @('.github/workflows/x.yml'); & $writeCard
        $n = $put.Count
        Add-RinneCase $cases '-Merge：基础设施 PR 没有批准记录不合' $true ((Test-Throws { Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null }) -and $put.Count -eq $n)
        Set-Content -LiteralPath $recordsFile -Value (ConvertTo-Json -Depth 10 @{ approvals = @{ '3' = @(@{ head = 'a' * 40; by = 'user'; note = '批的是 H' }) } })
        Add-RinneCase $cases '-Merge：批准在 H、现在是 H2，不合' $true ((Test-Throws { Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null }) -and $put.Count -eq $n)
        Set-Content -LiteralPath $recordsFile -Value (ConvertTo-Json -Depth 10 @{ approvals = @{ '3' = @(@{ head = $head; by = 'user'; note = '批的就是当前 head' }) } })
        $mout = @(Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum)
        Add-RinneCase $cases '-Merge：有对当前 head 的批准记录才合，且 sha 钉在这个 head' $false (-not ($put.Count -eq $n + 1 -and $put[-1].sha -eq $head))
        Add-RinneCase $cases '-Merge：输出依据的批准记录原文' $false (-not (@($mout | Where-Object { $_ -match '^依据的用户记录（approval' -and $_ -match '批的就是当前 head' -and $_ -match $head }).Count -eq 1))
        $pull.mergeable_state = 'dirty'
        Add-RinneCase $cases '-Merge：not-ready 时有批准记录也不合' $true ((Test-Throws { Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null }) -and $put.Count -eq $n + 1)
        $pull.mergeable_state = 'clean'
        Set-Content -LiteralPath $recordsFile -Value '{}'

        # -After：回读、删远端子支线、持锁移除 worktree 与本地分支
        $merged = Copy-RinneObject $pull
        $merged.merged = $true; $merged.state = 'closed'; $merged.merge_commit_sha = 'f' * 40
        $sq = "ci(review): 加入评审 bot、PR 模板、标签与 CODEOWNERS (#3)`n`n加入。`n`n$(Get-Signoff)"
        $wtBranch = 'feat/review-bot'
        $locks = [System.Collections.Generic.List[string]]::new()
        Set-RinneLockImpl { param($name, $sb) $locks.Add($name); & $sb }
        New-Item -ItemType Directory -Force -Path (Join-Path $tmp "wt/$cid") | Out-Null
        $after = [ordered]@{
            'pulls/3$'                       = { $merged }
            'merge-base --is-ancestor'       = { '' }
            'check-commits\.mjs commits --range' = { @{ Code = 0; Out = 'check-commits commits：通过'; Err = '' } }
            ' log -1 --format=%B'            = { $sq }
            ' symbolic-ref --quiet --short HEAD' = { $wtBranch }
            'ls-remote --exit-code'          = { "$('a' * 40)`trefs/heads/feat/review-bot" }
            ' push origin --delete refs/heads/feat/review-bot' = { '' }
            ' worktree remove '              = { param($argv) Remove-Item -Recurse -Force -LiteralPath $argv[-1]; '' }
            'rev-parse --verify --quiet refs/heads/feat/review-bot' = { '' }
            ' branch -D feat/review-bot'     = { '' }
            '^git .* fetch '                 = { '' }
        }
        $log.Clear()
        Set-RinneFake (New-RinneFake -Routes $after -Log $log)
        New-Item -ItemType Directory -Force -Path $tip | Out-Null
        $line = Invoke-After -Pr 3 -CardId $cid
        Add-RinneCase $cases '-After：持 merge 锁；回读通过后删远端子支线、持 git 锁移除 worktree 与本地分支' $false (-not (
                $line -match '已合入 fffffff' -and @($log | Where-Object { $_ -match 'push origin --delete refs/heads/feat/review-bot' }).Count -eq 1 -and
                @($log | Where-Object { $_ -match "worktree remove $([regex]::Escape((Join-Path (Get-RinneRoot) "wt/$cid")))$" }).Count -eq 1 -and
                $locks[0] -eq 'merge' -and @($locks | Where-Object { $_ -eq 'git' }).Count -ge 2))
        Add-RinneCase $cases '-After：一并强制移除这个 PR 的 wt/tip-3' $false (-not (
                @($log | Where-Object { $_ -match "worktree remove --force $([regex]::Escape($tip))$" }).Count -eq 1 -and -not (Test-Path -LiteralPath $tip)))
        New-Item -ItemType Directory -Force -Path (Join-Path $tmp "wt/$cid") | Out-Null
        $wtBranch = 'feat/other-card'
        Add-RinneCase $cases '-After：worktree 在别的分支上（卡号写错）不清理' $true ((Test-Throws { Invoke-After -Pr 3 -CardId $cid }) -and (Test-Path -LiteralPath (Join-Path $tmp "wt/$cid")))
        $wtBranch = 'feat/review-bot'
        Add-RinneCase $cases '-After：卡号格式不对' $true (Test-Throws { Invoke-After -Pr 3 -CardId '../x' })
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
        Add-RinneCase $cases '基础设施：合并后构建、压测、打包与 CI 调用的脚本' $true (@(Get-InfraTouches -Files @('scripts/build.ps1', 'scripts/package-macos.sh', 'scripts/stress.ps1', 'scripts/ci/x.ps1' | ForEach-Object { [pscustomobject]@{ filename = $_; status = 'added' } })).Count -eq 4)
        Add-RinneCase $cases '基础设施：src 与 docs 不算' $false (@(Get-InfraTouches -Files @('src/core/x.cpp', 'docs/a.md' | ForEach-Object { [pscustomobject]@{ filename = $_; status = 'modified' } })).Count -gt 0)
        Set-RinneInfraList (Join-Path $tmp 'no-such-list.json')
        Add-RinneCase $cases '基础设施清单取不到时判不了，不放行' $true (Test-Throws { Get-MergeVerdict $base })
    } finally {
        Set-RinneFake $null
        Set-RinneLockImpl $null
        Set-RinneInfraList $null
        Exit-RinneSelfTestNeedles $needles
        $env:RINNE_ROOT = $saved.Root
        $env:RINNE_CARDS = $saved.Cards
        $env:RINNE_WAIVERS = $saved.Waivers
        Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
    Complete-RinneSelfTest $cases 'pr-merge'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
if (@($Check, $Merge, $After | Where-Object { $_ }).Count -ne 1) { throw '用法：-Check | -Merge | -After，三选一（或 -SelfTest）' }
if ($Pr -le 0 -or $Card -notmatch $CardPattern) { throw '需要 -Pr <n> 与 -Card <卡号>' }
Initialize-RinneIdentity
Assert-MainClone
if ($Check) {
    $v = Invoke-MergeCheck $Pr $Card
    Write-Output (ConvertTo-RinneJsonText $v)
    exit $(switch ($v.Code) { 'ready' { 0 } 'needs-user' { 2 } default { 1 } })
}
if ($Merge) {
    if (-not $SummaryFile) { throw '-Merge 需要 -SummaryFile（squash 提交的摘要段）' }
    Invoke-Merge -Pr $Pr -CardId $Card -SummaryFile $SummaryFile
    exit 0
}
Invoke-After -Pr $Pr -CardId $Card
