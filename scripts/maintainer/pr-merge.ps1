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
# 而不是待合并 PR 自己的版本。gates 的结果 json 是自报的：最低档按 PR 的文件清单与卡片在这里重算（定档规则取自
# 主 clone 的 scripts/lib/Tiers.psm1）；json 的 baseSha 必须是 PR head 与当前 origin/feature/v1 的共同祖先，
# 对不上就要在 PR head 上重跑 gates。
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
# "by": "user"，所以批准、豁免与外部处置记录只在维护者明确指示后写入；-Check 与 -Merge 把依据的每一条记录原文打印出来
# （ReliedOn），用户能看到这次合并靠的是哪几条。
# bot 总结只建不改：updated_at 与 created_at 不同就判红；运行日志里有 review-bot 的发帖记录行时，再核对总结的评论编号与
# 正文 sha256。老运行的日志里没有这一行时退化为只核对 updated_at，并在 -Check 的 Warnings 里注明未核对哈希。
#
# 主 clone 的判据本身：-Check 断言 core.hooksPath 指向主 clone 的 scripts/hooks、rinne.waivers 指向一份存在的记录文件，
# 而且它不在主 clone 与 <根>/wt/ 之下。位置按系统给出的最终路径判（junction、符号链接、短名都展开），并拒绝设备路径与 UNC
# 写法、路径上的 reparse point 和多于一个的硬链接：只比字符串前缀的话，这几种写法都能把 wt/ 里的文件判成合格。没有把位置
# 钉死成一个固定路径：工作区的布局不进公开仓，钉死的路径本身照样可以换成 junction 或硬链接。
# wt/tip-<n> 与主 clone 共用 .git，PR 里的构建脚本与 CMake 配置期执行的代码都写得到这些文件，所以合并后构建前后各取一次
# 指纹（见 Get-CloneIntegrityTargets）：git 实际读到的每个配置文件（含 include 进来的）、hooks 目录里每个文件（含隐藏的）、
# 记录文件、构建之后还要用的 node 与卡片清单，任一项变了就判红；构建之后再断言主 clone 的工作树干净、HEAD 没变。squash
# 消息的生成与公开文本检查（要跑 node）放在构建之前，构建之后只剩核对与合并请求。构建之前写「进行中」锁存标记，核对
# 全部通过才删；判据文件变了就改写成「已篡改」（见 scripts/lib/PremergeLatch.psm1）：标记在时，包装脚本与 gates 在跑任何 git
# 之前就拒绝，直到维护者核对后手动删掉；维护者脚本在每一条外部命令之前还会再查一次，在途的调用撞上之后才写下的标记、或者本
# 进程自己的标记被动过，就停在下一条之前，finally 里的 abort 也不跑。指纹与锁存都是尽力而为，不是隔离：指纹只防构建期间直接改这些文件，脱离进程树的
# 后台进程可以等核对之后再改；锁存防的是「构建期间被篡改之后，后续调用在不知情下继续跑 git」，同一用户身份下的后台进程在
# pr-merge 退出之后能直接删掉标记，这一点挡不住。彻底的做法是独立 clone，或者换一个账户、容器来跑构建。
#
# 合并后构建（Standard 及以上）跑的是 PR 树里的脚本，所以是最后一道：其他判据全部通过、且没碰基础设施时才跑，
# 子进程的环境里拿掉凭据与个人针。-Check 在那之前不会执行 PR 里的任何代码。每个 PR 用自己的 <根>/wt/tip-<n>，
# 用前 reset 并 git clean；建树、合并、构建到 abort 整段持 build-slot-1（合并后构建同一时间只跑一个，见 Get-PremergeSlot），
# 构建脚本经 RINNE_BUILD_SLOT_HELD（本进程的 pid）知道槽已经持着、不再自己取；构建有超时。

[CmdletBinding()]
param(
    [switch]$Check, [switch]$Merge, [switch]$After, [switch]$SelfTest,
    [int]$Pr, [string]$Card, [string]$SummaryFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '../lib/PremergeLatch.psm1')

$PremergeTimeoutSeconds = 2700
$SlotWaitSeconds = 2700
$MergeablePoll = @{ Tries = 5; Seconds = 2 }
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
    param([switch]$FromEnv, [string]$Repo = (Get-RinneRepoRoot))
    if ($FromEnv) { return $env:RINNE_WAIVERS }
    $r = Invoke-RinneGit @('config', '--local', '--get', 'rinne.waivers') -Repo $Repo -AllowFail
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
    # mergeable 由 GitHub 惰性计算：base 动过之后第一次读常是 null（mergeable_state=unknown），稍等再读几次
    for ($k = 0; $k -lt $MergeablePoll.Tries -and $null -eq (Get-RinneProp $pull 'mergeable'); $k++) {
        Start-Sleep -Seconds $MergeablePoll.Seconds
        $pull = Get-RinneApi "repos/$repo/pulls/$Pr"
    }
    $head = [string]$pull.head.sha
    $activity = Get-RinnePrActivity $Pr
    $state = Get-RinneReviewState $activity
    $runId = if ($state.summary) { [string]$state.summary.runId } else { '' }
    $run = if ($runId) { Get-RinneApi "repos/$repo/actions/runs/$runId" -AllowFail } else { $null }
    $jobs = if ($run) { @(Get-RinneApi "repos/$repo/actions/runs/$runId/jobs?per_page=100" -Paginate | ForEach-Object { $_.jobs } | ForEach-Object { $_ }) } else { @() }
    # 发帖记录：review-bot 发完总结打的那一行 id + 正文 sha256，在运行日志里。取不到（老运行、日志过期）时为 $null，
    # 由判据退化为只核对 updated_at 并在 -Check 里注明未核对哈希
    $posted = $null
    if ($state.summary -and $run) {
        $jobName = if ($run.path -eq $ReviewWorkflows[1]) { 'dispatch' } else { 'claude-pr-review' }
        $job = @($jobs | Where-Object { $_.name -eq $jobName }) | Select-Object -First 1
        if ($job) {
            $logText = Get-RinneRunLog -RunId $runId -JobId ([string]$job.id)
            if ($logText) {
                $recs = [regex]::Matches($logText, 'rinne-review-posted id=(\d+) sha256=([0-9a-f]{64})')
                if ($recs.Count) { $posted = [pscustomobject]@{ Id = $recs[$recs.Count - 1].Groups[1].Value; Sha256 = $recs[$recs.Count - 1].Groups[2].Value } }
            }
        }
    }
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
    # 主 clone 的状态在 fetch 之后取：判据脚本必须就是刚取回的 feature 线上的版本
    $clone = Get-RinneCloneState
    $mergeBase = Invoke-RinneGit @('merge-base', 'origin/feature/v1', "refs/remotes/origin/pr/$Pr") -AllowFail
    [pscustomobject]@{
        Pr = $Pr; Card = $card; Pull = $pull; Head = $head; Files = @($files); Activity = $activity
        State = $state; Items = @(Get-RinneCommentItems -Activity $activity -State $state); Threads = @($activity.Threads)
        Run = $run; Jobs = $jobs; Posted = $posted; Checks = Get-RinneCheckRuns $head; Required = Get-RinneRequiredChecks
        Gates = $gates; Records = Read-Records $Pr
        MainClone = [pscustomobject]@{
            Head = [string]$clone.Sha
            Tip = (Invoke-RinneGit @('rev-parse', 'origin/feature/v1')).Out.Trim()
            Dirty = [bool]$clone.Dirty; Ignored = @($clone.Ignored)
        }
        PrRef = (Invoke-RinneGit @('rev-parse', "refs/remotes/origin/pr/$Pr")).Out.Trim()
        # PR head 与当前 feature 线的共同祖先：等于 feature 线的 tip 时，子支线已经包含了它
        PrBase = $(if ($mergeBase.Code -eq 0) { $mergeBase.Out.Trim() } else { '' })
        Dco = [int](Invoke-RinneGit @('rev-list', '--count', 'origin/dev..origin/feature/v1')).Out.Trim()
        Integrity = @(Test-CloneIntegrity (Get-CloneIntegrityTargets) (Get-RinneRoot))
        Guard = Test-ContractGuard $Pr $body
        PrText = Test-PrText ([string]$pull.title) $body
        NeedsDispatch = $needsDispatch; DispatchRuns = $dispatchRuns
    }
}

# ---- 判据（纯函数，自测逐条给反例） ---------------------------------------------------------

<# 单元格去掉 markdown 的强调与代码标记（*、_、反引号），再比决定词。 #>
function Clear-MdCell {
    param([AllowEmptyString()][string]$Cell)
    (($Cell ?? '') -replace '[*_`]', '').Trim()
}

<#
  处置汇总表的数据行：表格的第一行当表头，按列名认出 级别 / 位置 / 摘要 / 处置 四列；认不出表头时这一行按逐格扫描
  处理（Cols 为 $null）。表格之间的文字把表头状态清掉，下一张表重新认。
#>
function Get-DispositionRows {
    param([AllowEmptyString()][string]$Body)
    $rows = [System.Collections.Generic.List[object]]::new()
    $cols = $null
    foreach ($line in (($Body ?? '') -split "`r?`n")) {
        if ($line -notmatch '^\s*\|') { $cols = $null; continue }
        $cells = @((($line -replace '\\\|', '') -split '\|') | ForEach-Object { Clear-MdCell $_ })
        if (-not @($cells | Where-Object { $_ }).Count) { continue }
        if (@($cells | Where-Object { $_ -and $_ -notmatch '^:?-{2,}:?$' }).Count -eq 0) { continue }
        if ($null -eq $cols) {
            $map = @{}
            for ($i = 0; $i -lt $cells.Count; $i++) {
                foreach ($name in '级别', '位置', '摘要', '处置') {
                    if ($cells[$i] -match $name -and -not $map.ContainsKey($name)) { $map[$name] = $i }
                }
            }
            if ($map.Count) { $cols = $map; continue }
        }
        $rows.Add([pscustomobject]@{ Line = $line.Trim(); Cells = @($cells); Cols = $cols })
    }
    $rows.ToArray()
}

<#
  处置汇总表里判为「不改」「不采纳」「挂账」「暂不」「维持」的【红旗】【重要】：原文整行列出，供统筹记录、交用户裁决。
  同一条意见按「位置」认（去掉 markdown 标记；没有位置列时按摘要），以最后一次处置为准：后来写了「已修」并附提交的，
  不再要用户豁免；后来才判为不改 / 挂账的，照样列出。单元格先去掉 * _ 与反引号再认决定词（「**不改**」也认得出来），
  决定词出现在处置列任意位置都算；「采纳」「已采纳」不是拒绝。
#>
function Get-DeferredSevere {
    param([object[]]$Items)
    $rounds = [System.Collections.Generic.List[object]]::new()
    $order = 0
    foreach ($i in @($Items | Where-Object { $_.Kind -eq 'agent' })) {
        $m = Get-AgentMarker $i.Body
        if (-not $m -or $m.Kind -ne 'disposition') { continue }
        $rounds.Add([pscustomobject]@{ Round = [int]$m.Round; Order = $order; Rows = @(Get-DispositionRows $i.Body) })
        $order++
    }
    # 同一个位置以轮次靠后的那一行为准；同一轮里以行序靠后的为准
    $latest = @{}
    foreach ($r in @($rounds | Sort-Object Round, Order)) {
        foreach ($row in @($r.Rows)) {
            $cell = { param($name) if ($row.Cols -and $row.Cols.ContainsKey($name) -and $row.Cols[$name] -lt $row.Cells.Count) { $row.Cells[$row.Cols[$name]] } else { $null } }
            $level = & $cell '级别'
            $severe = if ($level) { $level -match '红旗|重要' } else { @($row.Cells | Where-Object { $_ -match '【(红旗|重要)】' }).Count -gt 0 }
            if (-not $severe) { continue }
            $loc, $sum = (& $cell '位置'), (& $cell '摘要')
            $key = if ($loc) { $loc } elseif ($sum) { $sum } else { $row.Line }
            $decision = if ($row.Cols -and $row.Cols.ContainsKey('处置') -and $row.Cols['处置'] -lt $row.Cells.Count) {
                $row.Cells[$row.Cols['处置']]
            } else { $row.Cells -join ' ' }
            if ($decision -match '(?:^|[^采])(?:不改|不采纳|挂账|暂不|维持)') {
                $latest[$key] = [pscustomobject]@{ Line = $row.Line; Deferred = $true }
            } elseif (($decision -match '已修|修复|已改|采纳') -and @($row.Cells | Where-Object { $_ -match '(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])' }).Count) {
                # 后来写明已修 / 采纳并附提交：这条不再要用户豁免
                $latest[$key] = [pscustomobject]@{ Line = $row.Line; Deferred = $false }
            }
        }
    }
    @($latest.Values | Where-Object Deferred | ForEach-Object Line)
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
    foreach ($x in @($F.Integrity)) { $fail.Add($x) }
    if ($F.MainClone.Dirty) {
        $ign = @($F.MainClone.Ignored | Where-Object { $_ })
        $fail.Add("主 clone 的工作树不干净$(if ($ign.Count) { "（被忽略却会被构建读到：$($ign -join '、')）" })")
    }

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
        # review-bot 只建不改：updated_at 与 created_at 不同说明总结在贴出之后被改过（例如把 red / important 改成 0）
        if ($s.updatedAt -and (ConvertTo-RinneUtc $s.updatedAt) -ne (ConvertTo-RinneUtc $s.createdAt)) {
            $fail.Add("bot 总结 $($s.id) 在贴出之后被改过（created_at=$($s.createdAt)，updated_at=$($s.updatedAt)）")
        }
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
            # 运行日志里 review-bot 记下了这次发帖的评论编号与正文 sha256：对得上才说明评论没被改过。
            # 老运行没有这一行、日志过期或取不到时退化为只核对 updated_at，并明确注明未核对哈希
            if ($F.Posted) {
                if ([string]$F.Posted.Id -ne [string]$s.id -or [string]$F.Posted.Sha256 -ne [string]$s.sha256) {
                    $fail.Add('bot 总结的评论编号或正文哈希与运行日志里的发帖记录不一致（贴出之后被改过）')
                }
            } elseif ($s.ok) {
                $warn.Add("未核对哈希：运行 $($s.runId) 的日志里没有 review-bot 的发帖记录（老运行或取不到日志），只核对了 updated_at")
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
        else { & $need 'user' 'deferred' "有 $($deferred.Count) 条【红旗】【重要】在最新一轮处置里仍判为不改或挂账，只能由用户豁免" }
    }

    # 6. 静态 check：feature 线 required checks 在当前 head 上都是 success；pr-agent-claude 不是 required check，
    #    但 head 上已经有这个名字的 check 时，它要在当前 head 上跑完（结论不限，它只发评论）
    foreach ($ctx in @($F.Required)) {
        $c = if ($F.Checks.ContainsKey($ctx)) { $F.Checks[$ctx] } else { $null }
        if (-not $c -or $c.conclusion -ne 'success') { $fail.Add("check $ctx 在当前 head 上不是 success") }
    }
    $prAgentCheck = Get-RinnePrAgentCheck
    if ($F.Checks.ContainsKey($prAgentCheck) -and $F.Checks[$prAgentCheck].status -ne 'completed') {
        $fail.Add("check $prAgentCheck 在当前 head 上还没跑完（结论不限，completed 即可）")
    }

    # 7. 本地 gates：json 存在、sha = head、档位够、基线对、真跑过（pass ≥ 1）、exit=0、fail=0、dirty=false。
    #    json 是自报的：最低档按 PR 的文件清单（改名的新旧路径都算）与卡片的档在这里重算，规则取自主 clone 的 Tiers.psm1
    $paths = @(Get-RinneChangedPaths $F.Files)
    $tier = Resolve-RinneTierFloor -Files $paths -CardTier ([string]$F.Card.tier)
    $g = $F.Gates
    if (-not $g) { $fail.Add('缺当前 head 的 gates json') }
    else {
        if ($g.sha40 -ne $head) { $fail.Add('gates json 的 sha 不是当前 head') }
        $ran = try { Get-RinneTierOrder ([string]$g.tier) } catch { -1 }
        if ($ran -lt 0) { $fail.Add("gates json 的档位不认识：$($g.tier)") }
        elseif ($ran -lt (Get-RinneTierOrder $tier.Min)) {
            $auto = [string](Get-RinneProp $g 'autoTier')
            $fail.Add("gates 跑的是 $($g.tier)，低于按改动重算的最低档 $($tier.Min)（按文件 $($tier.Auto)，卡片 $($F.Card.tier)$(if ($auto) { "；json 自报按文件 $auto" })）")
        }
        # baseSha 是 gates 算 diff 用的共同祖先：必须等于 PR head 与当前 feature 线的共同祖先（现在重跑 gates 会写出的值）。
        # feature 线往前走、子支线没同步时它不变，不必为此重跑；与 PR 合并后的树由合并后构建（Standard 及以上）覆盖
        $baseSha = [string](Get-RinneProp $g 'baseSha')
        $short = { param($s) if ($s) { $s.Substring(0, [Math]::Min(7, $s.Length)) } else { '（无）' } }
        if (-not $baseSha) { $fail.Add('gates json 没有 baseSha，请用新版 gates 重跑') }
        elseif (-not $F.PrBase) { $fail.Add('取不到 PR head 与当前 origin/feature/v1 的共同祖先，核对不了 gates 的基线') }
        elseif ($baseSha -ne $F.PrBase) {
            $fail.Add("gates 的基线 $(& $short $baseSha) 不是 PR head 与当前 origin/feature/v1 的共同祖先 $(& $short $F.PrBase)：请在 PR head 上重跑 gates")
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
        OutOfScope = $outside; Infra = $infra; Deferred = $deferred; Tier = $tier; Premerge = $null
        Command = "pwsh scripts/maintainer/pr-merge.ps1 -Merge -Pr $($F.Pr) -Card $($F.Card.id) -SummaryFile <摘要段文件>"
    }
}

# ---- 合并后构建（最后一道，跑的是 PR 里的代码） ---------------------------------------------

<#
  合并后构建一律用同一把构建槽 build-slot-1，同一时间只跑一个。锁存标记是整个工作区的：构建期间别的调用在下一条外部命令处
  就被拒（不等），奇偶不同的两个 PR 各持一把槽同时构建，后一个必然撞上前一个的标记失败，所以排成一队。在对方写下标记之前
  已经走到取槽这一步的那一次，在槽上等对方整段（建树到清除或改写标记）结束；拿到槽之后的第一条 git 照样先查标记，对方已锁存
  就一条都不跑。同一个 PR 的 -Check 撞上 -Merge 也因此互斥，不会同时动 wt/tip-<n>。gates 的编译仍可用另一把槽。
#>
function Get-PremergeSlot { 'build-slot-1' }

<#
  主 clone 的判据文件（合并后构建前后各取一次指纹）：
  - Configs：git 配置。先放 .git/config（common dir 的那份）、git var 报的用户级与系统级配置（还不存在的也算：构建可以新建），
    再加 git config --list --show-origin 实际读到的每个文件（include / includeIf 引进来的都在里面），以及各配置里 include
    指向、眼下还不存在的文件（新建出来就会被读到）。眼下存在却没被读到的（条件不满足的 includeIf）不算：条件只取决于
    仓的位置、HEAD 所在的分支与配置本身，这几样都另有核对。
  - Hooks、Records：core.hooksPath 目录与记录文件。
  - Node、Cards：同一次 -Merge 在构建之后还会用到的 node（rinne.node，没配置时是 PATH 上的 node）与卡片清单（rinne.cards）。
  - Info：忽略与属性文件（忽略文件能让构建放进主 clone 的未跟踪文件在 status 里看不见）。共享 .git 里的 info/exclude 与
    info/attributes；用户级的 git/ignore 与 git/attributes（git 不需要任何配置项就读，XDG_CONFIG_HOME 下的与 ~/.config 下的
    都列上）；core.excludesFile / core.attributesFile 指的文件；git var 报的 GIT_ATTR_GLOBAL / GIT_ATTR_SYSTEM。
  -Repo 换一个仓（自测用临时仓）；自测也可以直接把 $script:IntegrityTargets 换成一组临时文件。
#>
$script:IntegrityTargets = $null
function Get-CloneIntegrityTargets {
    param([string]$Repo = (Get-RinneRepoRoot))
    if ($script:IntegrityTargets) { return $script:IntegrityTargets }
    $git = (Invoke-RinneGit @('rev-parse', '--path-format=absolute', '--git-common-dir') -Repo $Repo).Out.Trim()
    $hooks = (Invoke-RinneGit @('config', '--local', '--get', 'core.hooksPath') -Repo $Repo -AllowFail).Out.Trim()
    $homeEnv = [Environment]::GetEnvironmentVariable('HOME') ?? $HOME
    $norm = { param($p) [IO.Path]::GetFullPath($p, $Repo) -replace '\\', '/' }
    $tilde = { param($p) (& $norm $p) -ireplace "^$([regex]::Escape((& $norm $homeEnv).TrimEnd('/')))(?=/)", '~' }
    $configs = [ordered]@{}
    $add = { param($label, $p) if ($p -and -not @($configs.Values | Where-Object { (& $norm $_) -ieq (& $norm $p) }).Count) { $configs[$label] = (& $norm $p) } }
    & $add '.git/config' (Join-Path $git 'config')
    # 路径问 git 自己：它认 GIT_CONFIG_GLOBAL、Windows 上先看 HOME 再看 USERPROFILE、还有 XDG 的那一份；问不到（git 太旧）才按常见位置猜
    $var = Invoke-RinneGit @('var', 'GIT_CONFIG_GLOBAL') -Repo $Repo -AllowFail
    $globals = if ($var.Code -eq 0 -and $var.Out.Trim()) { @($var.Out -split "`r?`n" | ForEach-Object Trim | Where-Object { $_ }) }
    else { @((Join-Path $homeEnv '.gitconfig'), (Join-Path ($env:XDG_CONFIG_HOME ? $env:XDG_CONFIG_HOME : (Join-Path $homeEnv '.config')) 'git/config')) }
    foreach ($f in $globals) { & $add "用户级 git 配置 $(& $tilde $f)" $f }
    $sys = Invoke-RinneGit @('var', 'GIT_CONFIG_SYSTEM') -Repo $Repo -AllowFail
    if ($sys.Code -eq 0 -and $sys.Out.Trim()) { & $add "系统级 git 配置 $(& $tilde $sys.Out.Trim())" $sys.Out.Trim() }
    # -z：来源与键名之间、各条之间都用 NUL 分开；include 的值跟在键名后面、隔一个换行
    $pairs = { param($out) $x = @("$out" -split "`0"); for ($i = 0; $i + 1 -lt $x.Count; $i += 2) { , @($x[$i], $x[$i + 1]) } }
    $read = Invoke-RinneGit @('config', '--list', '--show-origin', '--name-only', '-z') -Repo $Repo -AllowFail
    if ($read.Code -ne 0) { throw "git config --list --show-origin 失败：$($read.Err.Trim())" }
    foreach ($p in @(& $pairs $read.Out | ForEach-Object { $_[0] } | Where-Object { $_ -like 'file:*' } | Select-Object -Unique)) {
        & $add "git 配置 $(& $tilde $p.Substring(5))" $p.Substring(5)
    }
    $inc = Invoke-RinneGit @('config', '--show-origin', '-z', '--get-regexp', '^include(if\..*)?\.path$') -Repo $Repo -AllowFail
    # 退出码 1 是「一条 include 也没有」；别的非 0 是 git 出错，不能当成没有 include 静默放过
    if ($inc.Code -notin 0, 1) { throw "git config --get-regexp include 失败（退出码 $($inc.Code)）：$($inc.Err.Trim())" }
    foreach ($pr in @(& $pairs $inc.Out)) {
        if ($pr[0] -notlike 'file:*') { continue }
        $v = ($pr[1] -split "`n", 2)[1]
        if (-not $v) { continue }
        $target = if ($v -match '^~[\\/]') { Join-Path $homeEnv $v.Substring(2) } elseif ([IO.Path]::IsPathRooted($v)) { $v } else { Join-Path (Split-Path -Parent (& $norm $pr[0].Substring(5))) $v }
        if (-not (Test-Path -LiteralPath $target)) { & $add "git 配置（include，眼下不存在）$(& $tilde $target)" $target }
    }
    $info = [ordered]@{}
    $addInfo = { param($label, $p) if ($p -and -not @($info.Values | Where-Object { (& $norm $_) -ieq (& $norm $p) }).Count) { $info[$label] = (& $norm $p) } }
    & $addInfo '.git/info/exclude' (Join-Path $git 'info/exclude')
    & $addInfo '.git/info/attributes' (Join-Path $git 'info/attributes')
    foreach ($d in @($env:XDG_CONFIG_HOME, (Join-Path $homeEnv '.config')) | Where-Object { $_ }) {
        foreach ($n in 'ignore', 'attributes') { $p = Join-Path $d "git/$n"; & $addInfo "用户级 git/$n $(& $tilde $p)" $p }
    }
    foreach ($k in 'core.excludesFile', 'core.attributesFile') {
        $r = Invoke-RinneGit @('config', '--path', '--get', $k) -Repo $Repo -AllowFail
        if ($r.Code -notin 0, 1) { throw "git config --get $k 失败（退出码 $($r.Code)）：$($r.Err.Trim())" }
        if ($r.Code -eq 0 -and $r.Out.Trim()) { & $addInfo "$k $(& $tilde $r.Out.Trim())" $r.Out.Trim() }
    }
    # 太旧的 git 没有这两个变量，上面几项已经覆盖默认位置
    foreach ($k in 'GIT_ATTR_GLOBAL', 'GIT_ATTR_SYSTEM') {
        $r = Invoke-RinneGit @('var', $k) -Repo $Repo -AllowFail
        if ($r.Code -eq 0 -and $r.Out.Trim()) { & $addInfo "$k $(& $tilde $r.Out.Trim())" $r.Out.Trim() }
    }
    $node = Get-RinneNode
    if (-not [IO.Path]::IsPathRooted($node)) { $node = (Get-Command $node -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source }
    [pscustomobject]@{
        Root = $Repo; Configs = $configs; Hooks = $hooks; Records = (Get-RecordsPath -Repo $Repo)
        Node = $node; Cards = (Get-RinneSetting -Env 'RINNE_CARDS' -GitKey 'rinne.cards'); Info = $info
    }
}

<#
  每个判据文件的指纹；文件不在时记「（不存在）」，这样新建、删掉都算变了。git 配置按 git 自己解析出的键值算（git config
  --file <f> --list，不跟 include，引进来的文件各自单算），并剔除 branch.* 段：构建不在 git 锁里，这期间别的进程合法地
  建带 track 的 worktree、git branch -u，都会往 .git/config 写 branch 段，不该把合并判红；其余的键一变就算。按 git 的解析
  而不是按文本剔除，续行、引号之类的写法就骗不过去。hooks 目录里的文件连隐藏的、系统属性的一起列（git 照样执行它们）。
#>
function Get-CloneFingerprint {
    param([Parameter(Mandatory)]$Targets)
    $there = { param($p) $p -and (Test-Path -LiteralPath $p -PathType Leaf) }
    $sha = { param($p) (& $there $p) ? (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash : '（不存在）' }
    $cfg = {
        param($p)
        if (-not (& $there $p)) { return '（不存在）' }
        # 经 Invoke-RinneExternal：解析之前同样先查锁存
        $r = Invoke-RinneExternal git @('config', '--file', $p, '--list', '-z') -AllowFail
        if ($r.Code -ne 0) { return "（git 解析不了）$(& $sha $p)" }
        $kept = @($r.Out -split "`0" | Where-Object { $_ -and $_ -notmatch '^branch\.' }) -join "`0"
        [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($kept)))
    }
    $h = [ordered]@{}
    foreach ($k in @($Targets.Configs.Keys)) { $h[$k] = & $cfg $Targets.Configs[$k] }
    $h['记录文件'] = & $sha $Targets.Records
    $h['node'] = & $sha $Targets.Node
    $h['卡片清单'] = & $sha $Targets.Cards
    $info = Get-RinneProp $Targets 'Info'
    if ($info) { foreach ($k in @($info.Keys)) { $h[$k] = & $sha $info[$k] } }
    if ($Targets.Hooks -and (Test-Path -LiteralPath $Targets.Hooks -PathType Container)) {
        $base = [IO.Path]::GetFullPath($Targets.Hooks)
        foreach ($f in Get-ChildItem -LiteralPath $base -File -Recurse -Force | Sort-Object FullName) {
            $h["hooks/$($f.FullName.Substring($base.Length).TrimStart('\', '/') -replace '\\', '/')"] = & $sha $f.FullName
        }
    }
    $h
}

<# 指纹的摘要（写进「进行中」锁存标记，维护者核对时可以对照）。 #>
function Get-CloneFingerprintDigest {
    param([Parameter(Mandatory)]$Fingerprint)
    $text = @($Fingerprint.Keys | ForEach-Object { "$_`t$($Fingerprint[$_])" }) -join "`n"
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text))).ToLowerInvariant()
}

<# 两次指纹之间变了的项（新增、删除、内容变化都算）。 #>
function Compare-CloneFingerprint {
    param([Parameter(Mandatory)]$Before, [Parameter(Mandatory)]$After)
    @(@($Before.Keys) + @($After.Keys) | Select-Object -Unique | Where-Object { $Before[$_] -ne $After[$_] })
}

<#
  路径在文件系统里的真实身份（Windows）：Final 是打开句柄后由系统给出的最终路径（junction、符号链接、短名都已展开，去掉 \\?\
  前缀），Links 是硬链接数。路径不在时返回 $null。别的平台上只做 GetFullPath、硬链接数记 1。
#>
function Get-PathIdentity {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    if (-not $IsWindows) { return [pscustomobject]@{ Final = [IO.Path]::GetFullPath($Path); Links = 1 } }
    if (-not ('Rinne.PathIdentity' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace Rinne {
    public static class PathIdentity {
        [StructLayout(LayoutKind.Sequential)]
        struct Info { public uint Attributes, CreatedLow, CreatedHigh, AccessedLow, AccessedHigh, WrittenLow, WrittenHigh, Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow; }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFinalPathNameByHandleW(SafeFileHandle h, StringBuilder buf, uint size, uint flags);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetFileInformationByHandle(SafeFileHandle h, out Info info);
        // 只查属性：访问权限 0，读写删都共享；FILE_FLAG_BACKUP_SEMANTICS 让目录也打得开，不带 OPEN_REPARSE_POINT 所以跟随链接
        static SafeFileHandle Open(string path) {
            var h = CreateFileW(path, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
            if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            return h;
        }
        public static string Final(string path) {
            using (var h = Open(path)) {
                var sb = new StringBuilder(32768);
                uint n = GetFinalPathNameByHandleW(h, sb, (uint)sb.Capacity, 0);
                if (n == 0 || n >= sb.Capacity) throw new Win32Exception(Marshal.GetLastWin32Error());
                return sb.ToString();
            }
        }
        public static uint Links(string path) {
            using (var h = Open(path)) {
                Info i;
                if (!GetFileInformationByHandle(h, out i)) throw new Win32Exception(Marshal.GetLastWin32Error());
                return i.Links;
            }
        }
    }
}
'@
    }
    $final = [Rinne.PathIdentity]::Final($Path) -replace '^\\\\\?\\UNC\\', '\\' -replace '^\\\\\?\\', ''
    [pscustomobject]@{ Final = $final; Links = [Rinne.PathIdentity]::Links($Path) }
}

<# 路径本身与它的每一级上级目录里，第一个 reparse point（junction、符号链接、挂载点之类）；没有返回 $null。 #>
function Find-ReparsePoint {
    param([Parameter(Mandatory)][string]$Path)
    for ($p = [IO.Path]::GetFullPath($Path); $p; $p = [IO.Path]::GetDirectoryName($p)) {
        if ((Test-Path -LiteralPath $p) -and ([IO.File]::GetAttributes($p) -band [IO.FileAttributes]::ReparsePoint)) { return $p }
    }
    $null
}

<# 判据文件的位置（-Check 用）：返回问题列表，没有问题时为空。 #>
function Test-CloneIntegrity {
    param([Parameter(Mandatory)]$Targets, [Parameter(Mandatory)][string]$WorkRoot)
    $full = { param($p) [IO.Path]::GetFullPath($p).TrimEnd('\', '/') }
    # 目录按最终路径比（不存在时按 GetFullPath）：wt/ 或主 clone 本身经 junction 换了地方也认得出
    $real = { param($p) $id = Get-PathIdentity $p; ($id ? $id.Final : [IO.Path]::GetFullPath($p)).TrimEnd('\', '/') }
    $under = { param($p, $dir) $p.StartsWith((& $real $dir) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) }
    $want = Join-Path $Targets.Root 'scripts/hooks'
    # 相对路径按各自工作树的根解析：在 wt/tip-<n> 里跑 git 时会指到 PR 树里的那一份，所以一律要求绝对路径
    if (-not $Targets.Hooks) { '主 clone 没有设 core.hooksPath（应指向主 clone 的 scripts/hooks）' }
    elseif (-not [IO.Path]::IsPathFullyQualified($Targets.Hooks)) { "core.hooksPath 必须是绝对路径（相对路径在 wt/tip-<n> 里会指到 PR 树里的 hooks）：$($Targets.Hooks)" }
    elseif ((& $full $Targets.Hooks) -ine (& $full $want)) { "core.hooksPath 应指向主 clone 的 scripts/hooks，现在是 $($Targets.Hooks)" }
    $r = $Targets.Records
    if (-not $r) { '主 clone 没有设 rinne.waivers（git config --local）' }
    elseif (-not [IO.Path]::IsPathFullyQualified($r)) { "rinne.waivers 必须是绝对路径：$r" }
    elseif ($r -match '^[\\/]{2}') { "rinne.waivers 不能用设备路径或 UNC 路径（以两个斜杠开头）：$r" }
    elseif (-not (Test-Path -LiteralPath $r -PathType Leaf)) { "rinne.waivers 指向的记录文件不存在：$r" }
    elseif ($rp = Find-ReparsePoint $r) { "记录文件的路径上有 reparse point（junction、符号链接之类），位置判不准：$rp" }
    else {
        $id = Get-PathIdentity $r
        if ($id.Links -ne 1) { "记录文件有 $($id.Links) 个硬链接（必须恰好 1 个，否则另一个名字可能就在 wt/ 里）：$r" }
        # 盘符可以是指向共享的别名（net use、DefineDosDevice），最终路径就成了 UNC，和本机路径比前缀比不出来
        elseif ($id.Final -match '^[\\/]{2}') { "记录文件的最终路径不在本机卷上（$($id.Final)），位置判不准：$r" }
        elseif ((& $under $id.Final $Targets.Root) -or (& $under $id.Final (Join-Path $WorkRoot 'wt'))) { "记录文件必须在主 clone 与 wt/ 之外：$r" }
    }
}

<#
  在 <根>/wt/tip-<n> 把 PR 合到当前 feature/v1 上做一次构建，构建脚本取自合并后的树，环境里没有凭据与个人针。
  建树、清理、合并、构建到 abort 整段持构建槽（见 Get-PremergeSlot）；用前 reset --hard 并 git clean -ffdx，上一次留下的未跟踪
  文件与构建产物都不带进这一次；构建超时就结束整棵进程树、判红。构建之前写「进行中」锁存标记（写不进去就不构建），之后的核对
  （标记原样还在、指纹、主 clone 干净且 HEAD 没动）全部通过才删；有一项不过、或者核对途中抛错，就改写成「已篡改」。
  每一条 git 之前都经 Invoke-RinneExternal 再查一次锁存：本进程的标记被动过、改写成已篡改、没走完核对，或者途中撞上别的
  进程写下的标记，之后一条 git 都不跑，finally 里也不 abort；wt/tip-<n> 可能停在半路，Reason 里写明。
#>
function Invoke-Premerge {
    param([Parameter(Mandatory)][int]$Pr)
    $tip = Join-Path (Get-RinneRoot) "wt/tip-$Pr"
    # 结果写进这张表，不依赖锁函数的返回值
    $result = @{ Ok = $false; Merged = $false; Reason = '合并后构建没有跑完' }
    try {
        Invoke-RinneBuildSlotLocked -Slot (Get-PremergeSlot) -TimeoutSeconds $SlotWaitSeconds -Script {
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
            $latch = $null
            try {
                $build = Join-Path $tip 'scripts/build.ps1'
                if (-not (Test-Path -LiteralPath $build)) { $result.Reason = '合并后的树里没有 scripts/build.ps1，做不了合并后构建'; return }
                $targets = Get-CloneIntegrityTargets
                $before = Get-CloneFingerprint $targets
                $cloneBefore = Get-RinneCloneState
                # 构建期间一直握着它（只放行读）：Windows 上构建删不掉、改不了。位置上已经有东西（别的进程刚写下的标记）时按锁存处理
                try { $latch = New-RinneLatch -Pr $Pr -Digest (Get-CloneFingerprintDigest $before) }
                catch {
                    $busy = Get-RinneLatchRefusal
                    $result.Reason = $busy ? "另一次合并后构建进行中或已锁存，不跑这一次：$busy" : "锁存标记写不进去，不跑合并后构建：$($_.Exception.Message)"
                    return
                }
                $pwsh = [Environment]::ProcessPath
                # 构建槽由本进程持着：告诉构建脚本持槽方的 pid，它就不再自己取槽（不然它会再占一把，一次合并后构建占满两把槽）
                $savedHeld = $env:RINNE_BUILD_SLOT_HELD
                $env:RINNE_BUILD_SLOT_HELD = "$PID"
                try {
                    $r = Invoke-RinneWithoutSecrets {
                        Invoke-RinneExternal $pwsh @('-NoProfile', '-NonInteractive', '-File', $build) -Cwd $tip -AllowFail -TimeoutSeconds $PremergeTimeoutSeconds
                    }
                } finally {
                    $env:RINNE_BUILD_SLOT_HELD = $savedHeld
                }
                $result.Ok = $r.Code -eq 0
                $result.Reason = if ($r.ContainsKey('TimedOut') -and $r.TimedOut) { "合并后构建超过 $PremergeTimeoutSeconds 秒，已结束" } else { "合并后构建退出码 $($r.Code)" }
                # 核对。标记先查（不跑 git）：不在原样就不再算指纹，指纹里的 git 配置要 git 解析。核对途中抛错（包括外部命令前的检查
                # 发现标记被动过）同样算判据被动过
                $changed = @()
                try {
                    if (-not (Test-RinneLatchIntact $latch)) { $changed += '锁存标记（构建期间被删掉、换掉或改过）' }
                    else {
                        $changed = @(Compare-CloneFingerprint $before (Get-CloneFingerprint $targets))
                        if (-not $changed.Count) {
                            # 主 clone 的跟踪文件（下一次合并要跑的维护者脚本与检查都在里面）：工作树还是干净的、HEAD 没动
                            $cloneAfter = Get-RinneCloneState
                            if ($cloneAfter.Dirty) { $changed += '主 clone 的工作树（构建之后不干净了）' }
                            if ($cloneAfter.Sha -ne $cloneBefore.Sha) { $changed += '主 clone 的 HEAD' }
                        }
                    }
                } catch { $changed += "核对没有走完（$($_.Exception.Message)）" }
                if ($changed.Count) {
                    $err = Set-RinneLatchTampered -Latch $latch -Changed $changed
                    $note = if ($err) { "$err：$($latch.Path)；维护者先核对主 clone 再继续" } else { "已锁存 $($latch.Path)，维护者核对后手动删除" }
                    $result.Ok = $false
                    $result.Reason = "合并后构建改动了主 clone 的判据文件：$($changed -join '、')（$note）（$($result.Reason)）"
                } elseif (-not (Clear-RinneLatch $latch)) {
                    # 核对全部通过、清除之前标记又被动过：构建留下的进程还活着，核对过的判据文件之后也可能又被改过。同样改写成
                    # 「已篡改」（被删掉时按路径写回）
                    $err = Set-RinneLatchTampered -Latch $latch -Changed @('锁存标记（核对之后、清除之前被动过）')
                    $note = if ($err) { "$err：$($latch.Path)；维护者先核对主 clone 再继续" } else { "已锁存 $($latch.Path)，维护者核对后手动删除" }
                    $result.Ok = $false
                    $result.Reason = "合并后构建改动了主 clone 的判据文件：锁存标记（核对之后、清除之前被动过）（$note）（$($result.Reason)）"
                }
            } finally {
                # 没清除的标记放开句柄、留在原处。abort 也是一条 git，同样先查锁存：本进程的标记被动过、改写成已篡改、没走完核对，
                # 或者位置上有别的进程的标记，就不 abort
                if ($latch) { Close-RinneLatch $latch }
                if (-not (Get-RinneLatchRefusal)) { Invoke-RinneGitLocked { Invoke-RinneGit @('merge', '--abort') -Repo $tip -AllowFail | Out-Null } | Out-Null }
                else { $result.Ok = $false; $result.Reason += "（之后没有再跑 git：wt/tip-$Pr 的合并没有 abort，可能是脏的）" }
            }
        } | Out-Null
    } catch {
        # 途中撞上锁存（别的进程写下的标记，或者本进程的标记被动过）：那一条 git 没有跑，之后也不再跑
        if ($_.Exception.Message -notmatch '^已锁存') { throw }
        $result.Ok = $false
        $result.Reason = "$($_.Exception.Message)（之后没有再跑 git：wt/tip-$Pr 可能停在半路、是脏的）"
    }
    $result
}

<#
  -Check 与 -Merge 共用的核对：先收集事实、跑全部纯判据；只有判为 ready、重算的最低档（按改动与卡片取高）在
  Standard 及以上、而且没碰基础设施时才做合并后构建。碰了基础设施的（即便用户已批准）不在本机跑 PR 里的构建脚本。
#>
function Invoke-MergeCheck {
    param([Parameter(Mandatory)][int]$Pr, [Parameter(Mandatory)][string]$CardId)
    Assert-RinneNoLatch
    $facts = Get-MergeFacts $Pr $CardId
    $v = Get-MergeVerdict $facts
    if ($v.Code -eq 'ready' -and (Get-RinneTierOrder $v.Tier.Min) -ge (Get-RinneTierOrder 'Standard')) {
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
    Assert-RinneNoLatch
    # squash 消息先生成、先过检查（要跑 node 与仓里的判据脚本）：合并后构建跑的是 PR 里的代码，构建之后只剩接口核对与合并请求
    $pull = Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr"
    $msg = New-MergeMessage -Pull $pull -Summary (Get-Content -Raw -LiteralPath $SummaryFile)
    $v = Invoke-MergeCheck $Pr $CardId
    if ($v.Code -ne 'ready') {
        Write-Output (ConvertTo-RinneJsonText $v)
        throw "不能合并：code=$($v.Code)"
    }
    $now = Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr"
    if ($now.head.sha -ne $v.Head -or $pull.head.sha -ne $v.Head) { throw '核对之后 PR head 变了，重新核对' }
    if ([string]$now.title -cne [string]$pull.title) { throw 'PR 标题在生成 squash 消息之后改过，重新核对' }
    foreach ($x in @($v.ReliedOn)) { Write-Output "依据的用户记录（$($x.Kind)，原文，取自 $($x.File)）：$(ConvertTo-RinneJsonText $x.Record)" }
    # sha 钉住核对过的 head：其间再有推送，GitHub 拒绝合并（批准记录也只对这个 head 有效）
    $body = @{ merge_method = 'squash'; sha = $v.Head; commit_title = $msg.Title; commit_message = $msg.Body }
    $res = Invoke-RinneApiWrite -Method PUT -Path "repos/$(Get-RinneRepo)/pulls/$Pr/merge" -Body $body
    if (-not $res.merged) { throw "合并没有成功：$($res.message)" }
    Write-Output "已合并 #$Pr → $($res.sha)"
}

function Invoke-AfterUnlocked {
    param([int]$Pr, [string]$CardId)
    Assert-RinneNoLatch
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
    # 临时目录只经 scripts/lib/TempDir.psm1 建与删：新建的、本进程的，删之前按真实路径核对
    Import-Module (Join-Path $PSScriptRoot '../lib/TempDir.psm1') -ErrorAction Stop
    $tmp = New-RinneTempDir -Prefix 'rinne-merge-selftest-'
    # 假的 worktree remove 拒绝外面路径的用例：外面的目录也是这里新建的
    $away = $null
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
        # 主 clone 的判据文件换成一组临时的：假仓根（带 scripts/hooks）、它的 .git/config、用户级配置、node 与卡片清单，
        # 记录文件就是上面那份
        $fakeClone = Join-Path $tmp 'clone'
        New-Item -ItemType Directory -Force -Path (Join-Path $fakeClone '.git'), (Join-Path $fakeClone 'scripts/hooks'), (Join-Path $tmp 'wt'), (Join-Path $tmp 'outside') | Out-Null
        Set-Content -LiteralPath (Join-Path $fakeClone '.git/config') -Value "[core]`n`tbare = false"
        Set-Content -LiteralPath (Join-Path $fakeClone 'scripts/hooks/pre-push') -Value '#!/bin/sh'
        Set-Content -LiteralPath (Join-Path $tmp 'user.gitconfig') -Value "[user]`n`tname = x"
        Set-Content -LiteralPath (Join-Path $tmp 'node.exe') -Value 'node'
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $fakeClone '.git/info')
        Set-Content -LiteralPath (Join-Path $fakeClone '.git/info/exclude') -Value '# exclude'
        Set-Content -LiteralPath (Join-Path $tmp 'global.ignore') -Value '# ignore'
        $integrity = [pscustomobject]@{
            Root = $fakeClone; Configs = [ordered]@{ '.git/config' = (Join-Path $fakeClone '.git/config'); '用户级 git 配置 ~/.gitconfig' = (Join-Path $tmp 'user.gitconfig') }
            Hooks = (Join-Path $fakeClone 'scripts/hooks'); Records = $recordsFile; Node = (Join-Path $tmp 'node.exe'); Cards = $cardsFile
            Info = [ordered]@{ '.git/info/exclude' = (Join-Path $fakeClone '.git/info/exclude'); '用户级 git/ignore ~/.config/git/ignore' = (Join-Path $tmp 'global.ignore') }
        }
        $script:IntegrityTargets = $integrity
        # 锁存标记写到临时目录（真跑时在工作区根）
        $latchFile = Join-Path $tmp 'premerge-latch.json'
        Set-RinneLatchOverride $latchFile
        # 假构建脚本要不要顺手改一样判据文件或主 clone 的状态（见下面构建的路由）
        $tamper = @{ What = ''; Refused = $null; Late = $false; Mid = $false }
        # 在途的调用撞上别的进程刚写下的标记：在 wt/tip-<n> 的哪一条 git（Tip）、拉 PR 信息（Facts）、构建前查主 clone 工作区
        # （Clone）或解析用户级配置（Config）的时候写下 Text；At 是写下时日志的条数
        $plant = @{ Text = ''; Tip = ''; Facts = $false; Clone = $false; Config = $false; At = -1 }
        $putMarker = { Set-Content -LiteralPath $latchFile -Value $plant.Text -NoNewline; $plant.At = $log.Count }
        # 并发用例：取构建槽时写这个文件，告诉另一个进程这一次已经在槽上等了
        $concur = @{ Signal = $null }
        Clear-RinnePermCache
        $pull = Read-RinneFixture 'pr3-pull.json'
        $pull.state = 'open'; $pull.merged = $false; $pull.mergeable = $true; $pull.mergeable_state = 'clean'
        # 基线：改动只在卡的 owns 之内、不碰基础设施（PR #3 本身碰了 workflow，换成一份文档）
        $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'added' })
        $card = [pscustomobject]@{ id = $cid; owns = @('docs/example.md'); takesOver = @(); sections = @(); tier = 'Quick'; needsBranchDispatch = $false }
        $writeCard = { ConvertTo-Json -InputObject @($card) -Depth 10 | Set-Content -LiteralPath $cardsFile }
        & $writeCard
        $head = $pull.head.sha
        # 运行日志里的发帖记录行：用 fixture 里最后一条 bot 总结的编号与正文哈希造一份；用例可以改它来测「未核对哈希」
        $summaryFixture = (Read-RinneFixture 'pr3-issue-comments.json') | Where-Object { [string]$_.id -eq '6117738666' } | Select-Object -First 1
        $summarySha = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(([string]$summaryFixture.body -replace "`r`n", "`n")))).ToLowerInvariant()
        $runLog = @{ Text = "claude-pr-review`tPost the review summary`t2026-10-12T04:26:27.0000000Z rinne-review-posted id=$($summaryFixture.id) sha256=$summarySha" }
        # 自测里 feature 线的 tip 是 c…c，PR head 与它的共同祖先也是 c…c；gates 的基线就是这个共同祖先
        $tipSha = 'c' * 40
        $gates = @{ sha40 = $head; tier = 'Quick'; pass = 9; fail = 0; skip = 2; flaky = 0; exit = 0; dirty = $false; base = 'origin/feature/v1'; baseSha = $tipSha; autoTier = 'Quick' }
        $writeGates = { $gates | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $tmp "_logs/$cid/gates-$head.json") }
        & $writeGates
        $log = [System.Collections.Generic.List[string]]::new()
        $builds = [System.Collections.Generic.List[object]]::new()
        $tipOps = [System.Collections.Generic.List[object]]::new()
        $buildResult = @{ Code = 0; Out = 'built'; Err = '' }
        $tipMergeCode = 0
        $pullState = @{ Nulls = 0 }
        # 路由里的块按调用时的作用域找变量：被测函数自己也有 $pull，所以这里用别的名字指向同一个对象
        $prFixture = $pull
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
            'pulls/3$'                         = {
                if ($pullState.Nulls -gt 0) { $pullState.Nulls--; $p = Copy-RinneObject $prFixture; $p.mergeable = $null; $p.mergeable_state = 'unknown'; return $p }
                $prFixture
            }
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
            '^gh run view \d+ --repo \S+ --log( --job \d+)?$' = { $runLog.Text }
            'actions/workflows/build\.yml/runs' = { @{ workflow_runs = @() } }
            'check-runs'                       = { if ($plant.Facts) { $plant.Facts = $false; & $putMarker }; Read-RinneFixture 'pr3-check-runs.json' }
            '^git .* fetch '                   = { '' }
            '^git .* rev-parse refs/remotes/origin/pr/3' = { $head }
            '^git .* rev-parse (HEAD|origin/feature/v1)$' = { $tipSha }
            '^git .* merge-base origin/feature/v1 refs/remotes/origin/pr/3$' = { $tipSha }
            '^git .* status --porcelain$'      = { '' }
            '^git .* rev-list --count'         = { '12' }
            # 合并后构建：wt/tip-<n> 的 git 操作与构建脚本；记下当时持有的锁、目录、超时与子进程能看到的凭据
            '^git .* worktree add --detach '   = {
                param($argv)
                $tipOps.Add([pscustomobject]@{ Line = $argv -join ' '; Held = @($held); At = [datetime]::UtcNow.Ticks })
                New-Item -ItemType Directory -Force -Path (Join-Path $argv[-2] 'scripts') | Out-Null
                Set-Content -LiteralPath (Join-Path $argv[-2] 'scripts/build.ps1') -Value 'exit 0'
                ''
            }
            '^git -C .*tip-\d+ (reset --hard|checkout --detach|clean -ffdx|merge --no-ff|merge --abort)' = {
                param($argv)
                $tipOps.Add([pscustomobject]@{ Line = $argv -join ' '; Held = @($held); At = [datetime]::UtcNow.Ticks })
                if ($plant.Tip -and $argv -contains $plant.Tip) { $plant.Tip = ''; & $putMarker }
                if ($argv -contains '--no-ff') { return @{ Code = $tipMergeCode; Out = ''; Err = $(if ($tipMergeCode) { 'CONFLICT' } else { '' }) } }
                ''
            }
            '-NoProfile -NonInteractive -File .*build\.ps1$' = {
                param($argv, $opts)
                $getEnv = { param($n) [Environment]::GetEnvironmentVariable($n) }
                $builds.Add([pscustomobject]@{
                        GhToken = & $getEnv 'GH_TOKEN'; OtherKey = & $getEnv 'RINNE_SELFTEST_KEY'; Secret = & $getEnv 'RINNE_SELFTEST_SECRET'
                        Password = & $getEnv 'RINNE_SELFTEST_PASSWORD'; Needles = & $getEnv 'RINNE_PRIVACY_LOCAL'; Plain = & $getEnv 'RINNE_SELFTEST_PLAIN'
                        Pat = & $getEnv 'RINNE_SELFTEST_PAT'; Credentials = & $getEnv 'RINNE_SELFTEST_CREDENTIALS'; ApiKey = & $getEnv 'RINNE_SELFTESTAPIKEY'
                        Url = & $getEnv 'RINNE_SELFTEST_URL'; SlotHeld = & $getEnv 'RINNE_BUILD_SLOT_HELD'
                        Held = @($held); Cwd = $opts.Cwd; Timeout = $opts.TimeoutSeconds
                        Latch = $(try { Read-RinneLatchText $latchFile } catch { $null })
                    })
                $cfg = $integrity.Configs['.git/config']
                switch ($tamper.What) {
                    'config' { Add-Content -LiteralPath $cfg "[remote `"x`"]`n`turl = https://example.invalid/x" }
                    'user' { Add-Content -LiteralPath $integrity.Configs['用户级 git 配置 ~/.gitconfig'] "[core]`n`tautocrlf = input" }
                    'hook' { Add-Content -LiteralPath (Join-Path $integrity.Hooks 'pre-push') 'exit 0' }
                    'hidden-hook' { $f = Join-Path $integrity.Hooks 'post-checkout'; Set-Content -LiteralPath $f 'exit 0'; (Get-Item -LiteralPath $f).Attributes = 'Hidden' }
                    # 内容变了、仍是合法 JSON（后面的核对还要读它们）
                    'records' { Set-Content -LiteralPath $integrity.Records ((Get-Content -Raw -LiteralPath $integrity.Records).Trim() + ' ') }
                    'cards' { Set-Content -LiteralPath $integrity.Cards ((Get-Content -Raw -LiteralPath $integrity.Cards).Trim() + ' ') }
                    'node' { Add-Content -LiteralPath $integrity.Node 'x' }
                    # 合法的并发写：带 track 的 worktree add、git branch -u 只写 branch 段
                    'branch' { Add-Content -LiteralPath $cfg "[branch `"feat/x`"]`n`tremote = origin`n`tmerge = refs/heads/feat/x" }
                    'dirty' { $tree.Dirty = $true }
                    'head' { $tree.Sha = 'e' * 40 }
                    'exclude' { Add-Content -LiteralPath $integrity.Info['.git/info/exclude'] 'stray.txt' }
                    'global-ignore' { Add-Content -LiteralPath $integrity.Info['用户级 git/ignore ~/.config/git/ignore'] 'stray.txt' }
                    # 构建期间有人改了 PR 标题（-Merge 用的是构建前生成的 squash 消息）
                    'title' { $prFixture.title = "$($prFixture.title)（改过）" }
                    # 构建删掉、换成目录、改写进行中的锁存标记（握着句柄时做不到，记下有没有被拒）；构建中途出错（同父进程被结束一样，
                    # 走不到核对）
                    'latch-delete' { $tamper.Refused = Test-Throws { Remove-Item -LiteralPath $latchFile -ErrorAction Stop } }
                    'latch-dir' { $tamper.Refused = Test-Throws { Remove-Item -LiteralPath $latchFile -ErrorAction Stop; $null = New-Item -ItemType Directory -Path $latchFile } }
                    'latch-edit' { $tamper.Refused = Test-Throws { [IO.File]::WriteAllText($latchFile, ((Read-RinneLatchText $latchFile) -replace '"nonce":"[0-9a-f]+"', '"nonce":"0"')) } }
                    'throw' { throw '构建中途出错' }
                    # 构建留下的进程等核对过了再删标记：下一次查主 clone 的工作区（核对之后、清除之前）时删
                    'latch-late' { $tamper.Late = $true }
                    # 核对途中删标记：构建之后算指纹、解析用户级配置的那一条 git 时删（这一条之前的检查已经过了）
                    'latch-mid' { $tamper.Mid = $true }
                }
                $buildResult
            }
            'check-commits\.mjs contract-guard' = { @{ Code = 0; Out = 'check-commits contract-guard：通过'; Err = '' } }
            # 解析用户级配置照常真跑 git；用例要它在构建前算指纹的这一步写下别的进程的标记
            '^git config --file .*user\.gitconfig' = {
                param($argv)
                if ($plant.Config) { $plant.Config = $false; & $putMarker }
                if ($tamper.Mid) { $tamper.Mid = $false; Remove-Item -LiteralPath $latchFile -ErrorAction SilentlyContinue }
                $o = @(& git @argv 2>$null) -join "`n"
                @{ Code = $LASTEXITCODE; Out = $o; Err = '' }
            }
        }
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Set-RinneLockImpl {
            param($name, $sb)
            if ($concur.Signal -and $name -like 'build-slot-*') { Set-Content -LiteralPath $concur.Signal -Value 'x' }
            $lockLog.Add($name); $held.Add($name); try { & $sb } finally { $held.RemoveAt($held.Count - 1) }
        }
        # 定档用仓里真的 scripts/lib/Tiers.psm1 与 tier-rules.json；只有主 clone 的工作区状态由 $tree 给出
        # Calls 数主 clone 的工作区状态被查了几次（每次都要跑 git）
        $tree = @{ Sha = $tipSha; Dirty = $false; Ignored = @(); Calls = 0 }
        Set-RinneCloneStateImpl {
            param($root)
            $tree.Calls++
            if ($tamper.Late) { $tamper.Late = $false; Remove-Item -LiteralPath $latchFile -ErrorAction SilentlyContinue }
            if ($plant.Clone -and @($tipOps | Where-Object { $_.Line -match 'merge --no-ff' }).Count) { $plant.Clone = $false; & $putMarker }
            [pscustomobject]@{ Sha = $tree.Sha; Dirty = $tree.Dirty; Ignored = $tree.Ignored }
        }
        # 基础设施清单与匹配规则都用仓里真的 scripts/lib/infra-paths.mjs
        $base = Get-MergeFacts -Pr 3 -CardId $cid
        # 主 clone 的判据文件在预期位置：hooksPath 指向主 clone 的 scripts/hooks，记录文件存在、在主 clone 与 wt/ 之外
        $ti = { param($patch) $t = [pscustomobject]@{ Root = $integrity.Root; Configs = $integrity.Configs; Hooks = $integrity.Hooks; Records = $integrity.Records }
            foreach ($k in $patch.Keys) { $t.$k = $patch[$k] }; @(Test-CloneIntegrity $t $tmp) }
        Add-RinneCase $cases '判据文件位置：hooksPath 与记录文件都对' $false (@(& $ti @{}).Count -ne 0)
        Add-RinneCase $cases '判据文件位置：hooksPath 指到别处' $true ((@(& $ti @{ Hooks = $tmp }) -match 'core\.hooksPath').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：hooksPath 是相对路径' $true ((@(& $ti @{ Hooks = 'scripts/hooks' }) -match '绝对路径').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：rinne.waivers 是相对路径' $true ((@(& $ti @{ Records = 'records.json' }) -match '绝对路径').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：rinne.waivers 是盘符相对或当前盘根的写法' $true ((@(& $ti @{ Records = 'C:records.json' }) -match '绝对路径').Count -eq 1 -and (@(& $ti @{ Records = '\records.json' }) -match '绝对路径').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：没设 rinne.waivers' $true ((@(& $ti @{ Records = $null }) -match 'rinne\.waivers').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：记录文件不存在' $true ((@(& $ti @{ Records = (Join-Path $tmp 'missing.json') }) -match '不存在').Count -eq 1)
        Set-Content -LiteralPath (Join-Path $tmp 'wt/records.json') -Value '{}'
        Set-Content -LiteralPath (Join-Path $fakeClone 'records.json') -Value '{}'
        Add-RinneCase $cases '判据文件位置：记录文件在 wt/ 之下' $true ((@(& $ti @{ Records = (Join-Path $tmp 'wt/records.json') }) -match 'wt/ 之外').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：记录文件在主 clone 里' $true ((@(& $ti @{ Records = (Join-Path $fakeClone 'records.json') }) -match 'wt/ 之外').Count -eq 1)
        # 指向 wt/ 里那份的各种写法：设备路径、UNC、junction、硬链接（路径在运行时拼出来）
        $inWt = [IO.Path]::GetFullPath((Join-Path $tmp 'wt/records.json'))
        $bs = [string][char]92
        foreach ($p in @(($bs * 2 + '?' + $bs + $inWt), ($bs * 2 + '.' + $bs + $inWt), ('//?/' + ($inWt -replace '\\', '/')), ($bs * 2 + 'localhost' + $bs + ($inWt -replace '^([A-Za-z]):', '$1$')))) {
            Add-RinneCase $cases "判据文件位置：记录文件写成设备路径或 UNC（$($p.Substring(0, 4))…）" $true ((@(& $ti @{ Records = $p }) -match '设备路径或 UNC').Count -eq 1)
        }
        $null = New-Item -ItemType Directory -Path (Join-Path $tmp 'elsewhere'), (Join-Path $tmp 'realwt'), (Join-Path $tmp 'wr')
        Set-Content -LiteralPath (Join-Path $tmp 'elsewhere/records.json') -Value '{}'
        Set-Content -LiteralPath (Join-Path $tmp 'realwt/records.json') -Value '{}'
        $null = New-Item -ItemType Junction -Path (Join-Path $tmp 'outside/j') -Target (Join-Path $tmp 'wt')
        $null = New-Item -ItemType Junction -Path (Join-Path $tmp 'outside/k') -Target (Join-Path $tmp 'elsewhere')
        $null = New-Item -ItemType Junction -Path (Join-Path $tmp 'wr/wt') -Target (Join-Path $tmp 'realwt')
        $null = New-Item -ItemType HardLink -Path (Join-Path $tmp 'outside/hard.json') -Target $inWt
        Add-RinneCase $cases '判据文件位置：经 junction 指到 wt/ 里' $true (@(& $ti @{ Records = (Join-Path $tmp 'outside/j/records.json') }).Count -ge 1)
        Add-RinneCase $cases '判据文件位置：路径上有 junction（哪怕指到仓外）也判红' $true ((@(& $ti @{ Records = (Join-Path $tmp 'outside/k/records.json') }) -match 'reparse point').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：硬链接到 wt/ 里的文件' $true ((@(& $ti @{ Records = (Join-Path $tmp 'outside/hard.json') }) -match '硬链接').Count -eq 1)
        Add-RinneCase $cases '判据文件位置：仓外的普通文件（经过的目录都不是 reparse point）放行' $false (@(& $ti @{ Records = (Join-Path $tmp 'elsewhere/records.json') }).Count -ne 0)
        $wr = [pscustomobject]@{ Root = $integrity.Root; Configs = $integrity.Configs; Hooks = $integrity.Hooks; Records = (Join-Path $tmp 'realwt/records.json') }
        Add-RinneCase $cases '判据文件位置：wt/ 本身是 junction，记录文件放在它指向的目录里也认得出' $true ((@(Test-CloneIntegrity $wr (Join-Path $tmp 'wr')) -match 'wt/ 之外').Count -eq 1)
        # 盘符是指向本机共享的别名时，记录文件的最终路径是 UNC（这里用桩替掉 Get-PathIdentity，只改记录文件那一项）
        $uncRecords = Join-Path $tmp 'elsewhere/records.json'
        $unc = @(& {
            function Get-PathIdentity {
                param([string]$Path)
                $final = [IO.Path]::GetFullPath($Path)
                if ($final -ieq [IO.Path]::GetFullPath($uncRecords)) { $final = '\' + '\localhost\C$\' + $final.Substring(3) }
                [pscustomobject]@{ Final = $final; Links = 1 }
            }
            & $ti @{ Records = $uncRecords }
        })
        Add-RinneCase $cases '判据文件位置：最终路径是 UNC（盘符映射到共享）也判红' $true (@($unc -match '不在本机卷上').Count -eq 1)
        $integrity.Hooks = $tmp
        $vh = Get-MergeVerdict (Get-MergeFacts -Pr 3 -CardId $cid)
        $integrity.Hooks = Join-Path $fakeClone 'scripts/hooks'
        Add-RinneCase $cases '-Check：hooksPath 不对就判红' $true ($vh.Code -eq 'not-ready' -and @($vh.Failures | Where-Object { $_ -match 'core\.hooksPath' }).Count -eq 1)
        Add-RinneCase $cases 'PowerShell 低于 7.5 拒绝运行（-DateKind）' $true (-not (Test-RinnePwshVersion 7 4))
        Add-RinneCase $cases 'PowerShell 7.5 及以上照常运行' $false (-not ((Test-RinnePwshVersion 7 5) -and (Test-RinnePwshVersion 8 0)))
        # 导入时的版本断言：子进程导入一份把当前版本换成 7.4 的模块副本，导入必须失败；原样的副本照常导入
        $ghText = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'lib/RinneGh.psm1')
        $pinned = '$PSVersionTable.PSVersion.Major $PSVersionTable.PSVersion.Minor'
        # 副本放在同样的目录结构里（模块按相对路径导入 scripts/lib 下的 StdHandles.psm1 与 PremergeLatch.psm1）
        $import = { param($text) $d = Join-Path $tmp "gh-$(Get-Random)"; $f = Join-Path $d 'scripts/maintainer/lib/RinneGh.psm1'
            $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $f), (Join-Path $d 'scripts/lib')
            foreach ($m in 'StdHandles.psm1', 'PremergeLatch.psm1') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../lib/$m") -Destination (Join-Path $d 'scripts/lib') }
            Set-Content -LiteralPath $f -Value $text
            $o = & ([Environment]::ProcessPath) -NoProfile -NonInteractive -Command "Import-Module '$f'" 2>&1 | Out-String; @($LASTEXITCODE, $o) }
        $old, $cur = (& $import $ghText.Replace($pinned, '7 4')), (& $import $ghText)
        Add-RinneCase $cases '导入维护者模块：PowerShell 按 7.4 判时导入就失败' $true ($ghText.Contains($pinned) -and $old[0] -ne 0 -and $old[1] -match '需要 PowerShell 7\.5')
        Add-RinneCase $cases '导入维护者模块：当前的 PowerShell 照常导入' $false ($cur[0] -ne 0)
        # 判据文件的清单问 git 自己（真跑 git，临时仓）：用户级配置跟着 GIT_CONFIG_GLOBAL 与 HOME 走，还不存在的也列上；
        # include 进来的文件与 include 指着、眼下还不存在的文件都在
        $repo = Join-Path $tmp 'trepo'; $tHome = Join-Path $tmp 'thome'
        $null = New-Item -ItemType Directory -Path $tHome
        $null = git init -q $repo 2>&1
        Set-Content -LiteralPath (Join-Path $tmp 'g.gitconfig') -Value "[include]`n`tpath = inc.gitconfig`n`tpath = later.gitconfig"
        Set-Content -LiteralPath (Join-Path $tmp 'inc.gitconfig') -Value "[user]`n`tname = x"
        Set-Content -LiteralPath (Join-Path $tHome '.gitconfig') -Value "[user]`n`tname = y"
        $savedCfg = @{ G = $env:GIT_CONFIG_GLOBAL; H = $env:HOME; X = $env:XDG_CONFIG_HOME; S = $env:GIT_CONFIG_SYSTEM }
        $paths = { param($t) @($t.Configs.Values | ForEach-Object { [IO.Path]::GetFullPath($_) }) }
        $has = { param($t, $p) (& $paths $t) -contains [IO.Path]::GetFullPath($p) }
        $hasInfo = { param($t, $p) @($t.Info.Values | ForEach-Object { [IO.Path]::GetFullPath($_) }) -contains [IO.Path]::GetFullPath($p) }
        $xdg = Join-Path $tmp 'txdg'
        $null = git -C $repo config core.excludesFile (Join-Path $tmp 'ex.ignore').Replace('\', '/') 2>&1
        $null = git -C $repo config core.attributesFile (Join-Path $tmp 'at.attributes').Replace('\', '/') 2>&1
        Set-RinneFake $null
        $script:IntegrityTargets = $null
        try {
            $env:XDG_CONFIG_HOME = $xdg
            $env:GIT_CONFIG_GLOBAL = Join-Path $tmp 'g.gitconfig'
            $tg = Get-CloneIntegrityTargets -Repo $repo
            # 构建新建 XDG 下的全局忽略文件（git 不用任何配置项就会读它）：指纹里这一项变了
            $fi = { Get-CloneFingerprint ([pscustomobject]@{ Configs = [ordered]@{}; Hooks = $null; Records = $null; Node = $null; Cards = $null; Info = $tg.Info }) }
            $f0 = & $fi
            $null = New-Item -ItemType Directory -Force -Path (Join-Path $xdg 'git')
            Set-Content -LiteralPath (Join-Path $xdg 'git/ignore') -Value 'stray.txt'
            $xdgChanged = @(Compare-CloneFingerprint $f0 (& $fi))
            $env:XDG_CONFIG_HOME = $null
            $env:GIT_CONFIG_GLOBAL = Join-Path $tmp 'missing.gitconfig'
            $env:GIT_CONFIG_SYSTEM = Join-Path $tmp 'missing-system.gitconfig'
            $tm = Get-CloneIntegrityTargets -Repo $repo
            $env:GIT_CONFIG_GLOBAL = $null; $env:HOME = $tHome
            $th = Get-CloneIntegrityTargets -Repo $repo
        } finally {
            $env:GIT_CONFIG_GLOBAL, $env:HOME, $env:XDG_CONFIG_HOME, $env:GIT_CONFIG_SYSTEM = $savedCfg.G, $savedCfg.H, $savedCfg.X, $savedCfg.S
            $script:IntegrityTargets = $integrity
            Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        }
        Add-RinneCase $cases '判据文件清单：GIT_CONFIG_GLOBAL 指的用户级配置、它 include 的文件、include 指着还不存在的文件都在' $false (-not (
                (& $has $tg (Join-Path $tmp 'g.gitconfig')) -and (& $has $tg (Join-Path $tmp 'inc.gitconfig')) -and (& $has $tg (Join-Path $tmp 'later.gitconfig')) -and
                (& $has $tg (Join-Path $repo '.git/config'))))
        Add-RinneCase $cases '判据文件清单：GIT_CONFIG_GLOBAL 指着还不存在的文件，也照 git var 列上' $false (-not (& $has $tm (Join-Path $tmp 'missing.gitconfig')))
        Add-RinneCase $cases '判据文件清单：系统级配置照 git var GIT_CONFIG_SYSTEM 列上（还不存在的也算）' $false (-not (& $has $tm (Join-Path $tmp 'missing-system.gitconfig')))
        Add-RinneCase $cases '判据文件清单：共享 .git 的 info/exclude 与 info/attributes 都在' $false (-not (
                [IO.Path]::GetFullPath($tg.Info['.git/info/exclude']) -eq [IO.Path]::GetFullPath((Join-Path $repo '.git/info/exclude')) -and $tg.Info.Contains('.git/info/attributes')))
        Add-RinneCase $cases '判据文件清单：XDG_CONFIG_HOME 下的 git/ignore 与 git/attributes、core.excludesFile 与 core.attributesFile 指的文件都在' $false (-not (
                (& $hasInfo $tg (Join-Path $xdg 'git/ignore')) -and (& $hasInfo $tg (Join-Path $xdg 'git/attributes')) -and
                (& $hasInfo $tg (Join-Path $tmp 'ex.ignore')) -and (& $hasInfo $tg (Join-Path $tmp 'at.attributes'))))
        Add-RinneCase $cases '判据文件清单：HOME 下的 .config/git/ignore 与 .config/git/attributes（还不存在）也在' $false (-not (
                (& $hasInfo $th (Join-Path $tHome '.config/git/ignore')) -and (& $hasInfo $th (Join-Path $tHome '.config/git/attributes'))))
        # 系统级的属性文件（git var GIT_ATTR_SYSTEM；git 太旧报不出来时这一条不比）
        $sysAttr = "$(git var GIT_ATTR_SYSTEM 2>$null)".Trim()
        Add-RinneCase $cases '判据文件清单：git var 报的系统级属性文件也在' $false ($sysAttr -and -not (& $hasInfo $tg $sysAttr))
        Add-RinneCase $cases '指纹：构建新建 XDG 下的全局忽略文件，这一项变了' $true ($xdgChanged.Count -eq 1 -and $xdgChanged[0] -match '^用户级 git/ignore ')
        # include 查询出错（退出码不是 0 也不是 1）：抛错，不当成没有 include
        Set-RinneFake (New-RinneFake -Routes ([ordered]@{
                    ' rev-parse --path-format=absolute --git-common-dir$' = { (Join-Path $repo '.git') }
                    ' config --local --get core\.hooksPath$'          = { @{ Code = 1; Out = ''; Err = '' } }
                    ' var GIT_CONFIG_'                                 = { @{ Code = 1; Out = ''; Err = '' } }
                    ' config --list --show-origin'                     = { '' }
                    ' --get-regexp '                                   = { @{ Code = 128; Out = ''; Err = 'fatal: bad config' } }
                }))
        $script:IntegrityTargets = $null
        Add-RinneCase $cases '判据文件清单：include 查询出错时抛错' $true (Test-Throws { Get-CloneIntegrityTargets -Repo $repo })
        # core.excludesFile 的查询出错：同样抛错，不当成没设
        Set-RinneFake (New-RinneFake -Routes ([ordered]@{
                    ' rev-parse --path-format=absolute --git-common-dir$' = { (Join-Path $repo '.git') }
                    ' config --local --get core\.hooksPath$'         = { @{ Code = 1; Out = ''; Err = '' } }
                    ' var GIT_CONFIG_'                                 = { @{ Code = 1; Out = ''; Err = '' } }
                    ' config --list --show-origin'                     = { '' }
                    ' --get-regexp '                                   = { @{ Code = 1; Out = ''; Err = '' } }
                    ' config --path --get core\.excludesFile$'        = { @{ Code = 128; Out = ''; Err = 'fatal: bad config' } }
                }))
        $script:IntegrityTargets = $null
        Add-RinneCase $cases '判据文件清单：core.excludesFile 查询出错时抛错' $true (Test-Throws { Get-CloneIntegrityTargets -Repo $repo })
        $script:IntegrityTargets = $integrity
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Add-RinneCase $cases '判据文件清单：用户级配置跟着 HOME 走（含还不存在的 XDG 那一份）' $false (-not (
                (& $has $th (Join-Path $tHome '.gitconfig')) -and (& $has $th (Join-Path $tHome '.config/git/config')) -and -not (& $has $th (Join-Path $tmp 'g.gitconfig'))))
        # 指纹按 git 的解析算：只加 branch 段不变。下面两份按文本看只有「branch 段」里那一行不同，git 按续行解析，那一行
        # 却是 core 的键（第二份是 core.hooksPath）：按文本剔除 branch 段的话两份一样，按 git 的解析不一样
        $fp = { param($text) $f = Join-Path $tmp "fp-$(Get-Random).config"; Set-Content -LiteralPath $f -Value $text
            (Get-CloneFingerprint ([pscustomobject]@{ Configs = [ordered]@{ c = $f }; Hooks = $null; Records = $null; Node = $null; Cards = $null; Info = $null })).c }
        $c0 = "[core]`n`tbare = false"
        Add-RinneCase $cases '指纹：只多了 branch 段，git 配置的指纹不变' $false ((& $fp $c0) -ne (& $fp "$c0`n[branch `"feat/x`"]`n`tremote = origin"))
        $hidden = { param($line) "$c0`n`teditor = vi \`n[branch `"y`"]`n`t$line" }
        Add-RinneCase $cases '指纹：续行把 hooksPath 藏在「branch 段」里，指纹照样变' $true ((& $fp (& $hidden 'remote = origin')) -ne (& $fp (& $hidden 'hooksPath = /elsewhere')))
        # mergeable 先读到 null（GitHub 还在算）：稍等再读；一直是 null 就判不可合并
        $MergeablePoll.Seconds = 0
        $pullState.Nulls = 2
        $vm = Get-MergeVerdict (Get-MergeFacts -Pr 3 -CardId $cid)
        Add-RinneCase $cases 'mergeable 先是 null、再读到 true 放行' $false (-not ($vm.Code -eq 'ready' -and $pullState.Nulls -eq 0))
        $pullState.Nulls = 99
        $vm = Get-MergeVerdict (Get-MergeFacts -Pr 3 -CardId $cid)
        $pullState.Nulls = 0
        Add-RinneCase $cases 'mergeable 一直是 null 判不可合并' $true ($vm.Code -eq 'not-ready' -and @($vm.Failures | Where-Object { $_ -match '不可合并' }).Count -eq 1)
        $v = Get-MergeVerdict $base
        Add-RinneCase $cases '干净的 PR 判 ready' $false ($v.Code -ne 'ready')
        if ($v.Code -ne 'ready') { [Console]::Error.WriteLine((ConvertTo-RinneJsonText $v)) }
        Add-RinneCase $cases '契约守卫对当前 origin/feature/v1 重跑' $false (-not ($log | Where-Object { $_ -match 'contract-guard --base origin/feature/v1 --head refs/remotes/origin/pr/3' }))
        Add-RinneCase $cases '合并前 fetch 了 feature/v1、dev 与 PR head' $false (-not ($log | Where-Object { $_ -match 'fetch --no-tags origin .*feature/v1.*dev.*refs/pull/3/head' }))
        Add-RinneCase $cases '没有用到用户记录时 ReliedOn 为空' $false (@($v.ReliedOn).Count -gt 0)
        Add-RinneCase $cases '运行日志里的发帖记录对得上：不注明未核对哈希' $false (@($v.Warnings | Where-Object { $_ -match '未核对哈希' }).Count -gt 0)
        # 用的是真的模块：定档取自仓里的 scripts/lib/Tiers.psm1；锁是 Locks.psm1 的真锁（自测专用的一组名字）
        $repoLib = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../lib'))
        $tiersMod = @(Get-Module Tiers | Where-Object { [IO.Path]::GetDirectoryName($_.Path) -eq $repoLib })
        Add-RinneCase $cases '定档用的是仓里的 scripts/lib/Tiers.psm1' $false ($tiersMod.Count -ne 1)
        $lockName = "Local\Rinne-selftest-$PID-git"
        $probe = { $m = $null; $ok = [System.Threading.Mutex]::TryOpenExisting($lockName, [ref]$m); if ($m) { $m.Dispose() }; $ok }
        $inside = Invoke-RinneGitLocked { & $probe }
        Add-RinneCase $cases 'git 锁是 Locks.psm1 的真锁（块里能看到持有的互斥体，块外没有）' $false (-not ($inside -eq $true -and -not (& $probe)))
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
        $vu = & $copy
        $vu.Posted = $null
        $vu = Get-MergeVerdict $vu
        Add-RinneCase $cases '老运行没有发帖记录：只核对 updated_at，-Check 注明未核对哈希' $false (
            -not ($vu.Code -eq 'ready' -and @($vu.Warnings | Where-Object { $_ -match '未核对哈希' }).Count -eq 1))
        # 评论表重新从（改过的）原始来源生成，连分类一起测
        $reitem = { param($f, [scriptblock]$edit) $a = Copy-RinneObject $f.Activity; & $edit $a; $f.Activity = $a; $f.Items = @(Get-RinneCommentItems -Activity $a -State $f.State) }
        $user = { param($id, $login, $body, $created, $updated) [pscustomobject]@{ id = [long]$id; user = $(if ($login) { [pscustomobject]@{ login = $login; type = 'User' } } else { $null }); author_association = 'NONE'; created_at = $created; updated_at = $updated; body = $body; html_url = "https://github.com/x/y/pull/3#issuecomment-$id" } }
        $reply = { param($id, $to, $created, $body = '已改。') & $user $id 'DLsnows' "<!-- rinne-agent v1 role=review kind=reply round=3 head=f35523c$(if ($to) { " in-reply-to=$to" }) -->`n$body" $created $created }

        & $bad '主 clone 不在最新的 feature 线上' { param($f) $f.MainClone.Head = 'd' * 40 }
        & $bad '主 clone 的工作树不干净' { param($f) $f.MainClone.Dirty = $true }
        & $bad '主 clone 有被忽略却会被构建读到的文件' { param($f) $f.MainClone.Dirty = $true; $f.MainClone.Ignored = @('CMakeUserPresets.json') } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match 'CMakeUserPresets\.json' }).Count -eq 1 }
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
        # SEC：bot 总结只建不改，updated_at 与 created_at 不同说明贴出之后被改过；发帖记录里的编号与正文哈希也要对得上
        & $bad 'bot 总结在贴出之后被改过（updated_at 变了）' { param($f) $f.State.summary.updatedAt = '2026-10-12T06:00:00Z' } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '被改过' }).Count -eq 1 }
        & $bad '发帖记录里的正文哈希与总结对不上' { param($f) $f.State.summary.sha256 = '0' * 64 } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '发帖记录' }).Count -eq 1 }
        & $bad '发帖记录里的评论编号与总结对不上' { param($f) $f.Posted.Id = '1' } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '发帖记录' }).Count -eq 1 }
        # pr-agent 的 check：head 上有时要求它 completed（任何结论都接受）；head 上没有时不要求
        & $bad 'pr-agent 的 check 还没跑完' { param($f) $f.Checks['pr-agent-claude / pr-agent'] = [pscustomobject]@{ name = 'pr-agent-claude / pr-agent'; status = 'in_progress'; conclusion = $null } } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match 'pr-agent-claude / pr-agent' -and $_ -match '没跑完' }).Count -eq 1 }
        & $bad 'pr-agent 的 check 跑完了（failure 也接受）' { param($f) $f.Checks['pr-agent-claude / pr-agent'] = [pscustomobject]@{ name = 'pr-agent-claude / pr-agent'; status = 'completed'; conclusion = 'failure' } } 'ready'
        & $bad 'head 上没有 pr-agent 的 check：不要求' { param($f) $null } 'ready'
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
        # pr-agent 的审查输出单独一类：未回应的按「没有回应的评论」判 not-ready，不是外部评论（不进 Needs）；
        # 回应只认 in-reply-to 且晚于 updated_at；两种通知仍属免回应的 bot 类；标记在前 5 行之外不算
        $prAgentComment = { param($a, $id, $updated = '2026-10-12T05:00:00Z') $a.Issue += & $user $id 'github-actions[bot]' "<!-- pr-agent:review:full -->`n## PR Reviewer Guide`n`n**Recommended focus areas**`n`n- Possible Issue: x" '2026-10-12T05:00:00Z' $updated }
        & $bad '未回应的 pr-agent 评论：判 not-ready（不是外部评论）' { param($f) & $reitem $f { param($a) & $prAgentComment $a 1201 } } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match 'pr-agent' }).Count -eq 1 -and @($r.Needs).Count -eq 0 }
        & $bad '回应 pr-agent 评论（晚于 updated_at）放行' { param($f) & $reitem $f { param($a) & $prAgentComment $a 1201; $a.Issue += & $reply 1202 '1201' '2026-10-12T05:10:00Z' } } 'ready'
        & $bad '回应 pr-agent 评论但早于 updated_at：仍算未回应' { param($f) & $reitem $f { param($a) & $prAgentComment $a 1201 '2026-10-12T05:20:00Z'; $a.Issue += & $reply 1202 '1201' '2026-10-12T05:10:00Z' } }
        & $bad 'pr-agent 的两种通知免回应' { param($f) & $reitem $f { param($a) $a.Issue += & $user 1203 'github-actions[bot]' "<!-- pr-agent:review:full -->`n**[Persistent review]（旧）** updated to latest commit abc1234" '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z'; $a.Issue += & $user 1204 'github-actions[bot]' "Failed to generate PR review: timeout`n<!-- pr-agent:improve:summary -->" '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z' } } 'ready'
        & $bad 'pr-agent 标记在前 5 行之外（正文引用）不算，按 bot 免回应' { param($f) & $reitem $f { param($a) $a.Issue += & $user 1205 'github-actions[bot]' "第一行`n第二行`n第三行`n第四行`n第五行`n<!-- pr-agent:review:full -->`n## PR Reviewer Guide" '2026-10-12T05:00:00Z' '2026-10-12T05:00:00Z' } } 'ready'
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
        # 往指定轮次的处置汇总表格里插一行（插在最后一条表格行之后，仍在表内）
        $addRow = {
            param($f, [int]$round, [string]$row)
            $items = @($f.Items | Where-Object { $_.Kind -eq 'agent' -and $_.Source -eq 'issue' })
            $a = @($items | Where-Object { (Get-AgentMarker $_.Body).Round -eq $round })[0]
            $lines = [System.Collections.Generic.List[string]]::new()
            $lines.AddRange([string[]]($a.Body -split "`n"))
            $at = -1
            for ($j = 0; $j -lt $lines.Count; $j++) { if ($lines[$j] -match '^\s*\|') { $at = $j } }
            if ($at -lt 0) { $lines.Add($row) } else { $lines.Insert($at + 1, $row) }
            $a.Body = $lines -join "`n"
        }
        $deferCell = { param($level, $action, $commit = '理由') "| 9 | bot | $level | a.mjs:1 | x | $action | $commit |" }
        & $bad '【重要】判为不改' { param($f) & $deferRow $f } 'needs-user'
        & $bad '挂账的【重要】只有 by=orchestrator 的豁免' { param($f) & $deferRow $f; $f.Records.Waivers = @([pscustomobject]@{ head = $head; by = 'orchestrator'; red = 0; important = 0; deferred = $true; note = '' }) } 'needs-user'
        & $bad '挂账的【重要】有用户对这个 head 的豁免放行' { param($f) & $deferRow $f; $f.Records.Waivers = @([pscustomobject]@{ head = $head; by = 'user'; red = 0; important = 0; deferred = $true; note = '用户同意挂账' }) } 'ready'
        # 处置列的决定词：去掉 markdown 标记后认任意位置，采纳 / 已采纳不是拒绝
        & $bad '【重要】写成 **不改**' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '**不改**') } 'needs-user'
        & $bad '【重要】写成 __挂账__' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '__挂账__') } 'needs-user'
        & $bad '【重要】处置用反引号包着（不采纳）' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '`不采纳`') } 'needs-user'
        & $bad '【重要】处置写不采纳' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '不采纳') } 'needs-user'
        & $bad '【重要】处置写暂不改' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '暂不改') } 'needs-user'
        & $bad '【重要】处置写维持' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '维持') } 'needs-user'
        & $bad '【重要】处置写已采纳（不是拒绝）' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '已采纳' 'd62c5dc') } 'ready'
        & $bad '【重要】处置写采纳（不是拒绝）' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '采纳' 'd62c5dc') } 'ready'
        & $bad '级别列写重要（不带【】）' { param($f) & $addRow $f 1 (& $deferCell '重要' '不改') } 'needs-user'
        # 同一条意见按最后一次处置判：后来已修并附提交的不再要豁免；后来才挂账的仍要
        & $bad '先挂账、后已修（附提交）：不再要豁免' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '挂账'); & $addRow $f 3 (& $deferCell '【重要】' '已修' 'd62c5dc') } 'ready'
        & $bad '先已修、后挂账：仍要豁免' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '已修' 'd62c5dc'); & $addRow $f 3 (& $deferCell '【重要】' '挂账') } 'needs-user'
        & $bad '先挂账、后写已修但没附提交：仍要豁免' { param($f) & $addRow $f 1 (& $deferCell '【重要】' '挂账'); & $addRow $f 3 (& $deferCell '【重要】' '已修') } 'needs-user'
        & $bad 'required check 不是 success' { param($f) $f.Checks['docs-truth'].conclusion = 'failure' }
        & $bad '缺 gates json' { param($f) $f.Gates = $null }
        & $bad 'gates 的 sha 不是 head' { param($f) $f.Gates.sha40 = 'e' * 40 }
        & $bad 'gates 档位不够' { param($f) $f.Card.tier = 'Standard' }
        # 档位按 PR 的文件清单重算，不信 json 自报的；基线必须是当前 feature 线
        $srcChange = { param($f, $name = 'src/core/loop/Ring.cpp', $prev = $null) $f.Card.owns = @('**'); $f.Files = @([pscustomobject]@{ filename = $name; status = $(if ($prev) { 'renamed' } else { 'modified' }); previous_filename = $prev }) }
        & $bad 'gates 自报 Quick，改的却是 src/' { param($f) & $srcChange $f } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '低于按改动重算的最低档 Standard' }).Count -eq 1 }
        & $bad 'gates 自报 Quick，改的是大小写混写的 Src/' { param($f) & $srcChange $f 'Src/Core/Loop/Ring.cpp' } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '最低档 Standard' }).Count -eq 1 }
        & $bad 'gates 自报 Quick，把 src/ 的文件改名挪进 docs/' { param($f) & $srcChange $f 'docs/Ring.md' 'src/core/loop/Ring.cpp' } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '最低档 Standard' }).Count -eq 1 }
        & $bad 'src/ 改动、gates 跑的是 Standard 放行' { param($f) & $srcChange $f; $f.Gates.tier = 'Standard' } 'ready'
        & $bad 'Src/ 改动、gates 跑的是 Standard 放行' { param($f) & $srcChange $f 'Src/Core/Loop/Ring.cpp'; $f.Gates.tier = 'Standard' } 'ready'
        & $bad 'web/ 改动要 Full，gates 跑的是 Standard' { param($f) & $srcChange $f 'web/app/main.js'; $f.Gates.tier = 'Standard' } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '最低档 Full' }).Count -eq 1 }
        & $bad 'gates json 的档位不认识' { param($f) $f.Gates.tier = 'quick' }
        & $bad 'gates json 没有 baseSha' { param($f) $f.Gates.PSObject.Properties.Remove('baseSha') } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '没有 baseSha' }).Count -eq 1 }
        # 基线：baseSha 必须等于 PR head 与当前 feature 线的共同祖先（$F.PrBase），不要求等于 feature 线的 tip
        & $bad 'gates 的基线与共同祖先对不上' { param($f) $f.Gates.baseSha = 'e' * 40 } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '^gates 的基线 eeeeeee 不是 PR head 与当前 origin/feature/v1 的共同祖先 ccccccc：请在 PR head 上重跑 gates$' }).Count -eq 1 }
        & $bad 'feature/v1 往前走了、子支线没同步，共同祖先没变，gates 不必重跑' { param($f) $f.MainClone.Head = 'd' * 40; $f.MainClone.Tip = 'd' * 40 } 'ready'
        & $bad 'feature/v1 往前走了，gates 却按新的 tip 自报基线' { param($f) $f.MainClone.Head = 'd' * 40; $f.MainClone.Tip = 'd' * 40; $f.Gates.baseSha = 'd' * 40 } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '请在 PR head 上重跑 gates' }).Count -eq 1 }
        & $bad '子支线同步过 feature/v1，gates 也在同步后重跑' { param($f) $f.MainClone.Head = 'd' * 40; $f.MainClone.Tip = 'd' * 40; $f.PrBase = 'd' * 40; $f.Gates.baseSha = 'd' * 40 } 'ready'
        & $bad '子支线同步过 feature/v1，gates 还是同步前跑的' { param($f) $f.MainClone.Head = 'd' * 40; $f.MainClone.Tip = 'd' * 40; $f.PrBase = 'd' * 40 } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '请在 PR head 上重跑 gates' }).Count -eq 1 }
        & $bad '取不到 PR head 与 feature/v1 的共同祖先' { param($f) $f.PrBase = '' } 'not-ready' { param($r) @($r.Failures | Where-Object { $_ -match '核对不了 gates 的基线' }).Count -eq 1 }
        & $bad 'gates 一项也没通过（pass=0）' { param($f) $f.Gates.pass = 0 }
        & $bad 'gates 没通过' { param($f) $f.Gates.exit = 1 }
        & $bad 'gates 跑在脏工作树上' { param($f) $f.Gates.dirty = $true }
        & $bad 'PR 正文没过检查' { param($f) $f.PrText = @{ Code = 1; Out = '正文最后一行必须是签名'; Err = '' } }
        & $bad 'PR 正文含 Co-Authored-By' { param($f) $f.Pull.body = ($f.Pull.body -replace '(?m)^## DCO', "## DCO`nCo-Authored-By: a <a@b.c>") }
        & $bad '我方评论里有本机路径' { param($f) $i = $f.Items | Where-Object Kind -eq 'agent' | Select-Object -First 1; $i.Body += "`n日志在 C:" + '\Users\someone\x.log' }
        & $bad '我方评论里有个人针' { param($f) $i = $f.Items | Where-Object Kind -eq 'agent' | Select-Object -First 1; $i.Body += "`nrinne-selftest-fake-needle-7c1e" }
        & $bad '越界改动' { param($f) $f.Gates.tier = 'Standard'; $f.Files = @($f.Files) + @([pscustomobject]@{ filename = 'src/core/x.cpp'; status = 'added' }) } 'needs-user' { param($r) $r.Level -eq 'orchestrator' -and $r.OutOfScope -contains 'src/core/x.cpp' }
        & $bad '从范围外改名挪进来也算越界' { param($f) $f.Gates.tier = 'Standard'; $f.Files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'renamed'; previous_filename = 'src/core/other/Thing.cpp' }) } 'needs-user' { param($r) $r.OutOfScope -contains 'src/core/other/Thing.cpp' }
        & $bad '非基础设施卡改了 .github/workflows/x.yml' { param($f) $f.Gates.tier = 'Standard'; $f.Card.owns = @($f.Card.owns) + @('.github/workflows/x.yml'); $f.Files = @($f.Files) + @([pscustomobject]@{ filename = '.github/workflows/x.yml'; status = 'modified' }) } 'needs-user' { param($r) $r.Level -eq 'user' -and $r.Infra.Count -gt 0 }
        & $bad '把 workflow 改名挪走也算' { param($f) $f.Gates.tier = 'Standard'; $f.Card.owns = @('**'); $f.Files = @([pscustomobject]@{ filename = 'old/x.yml'; status = 'renamed'; previous_filename = '.github/workflows/x.yml' }) } 'needs-user' { param($r) $r.Infra.Count -gt 0 }
        & $bad 'PR #3 原样的改动（含 workflow）交用户批准' { param($f) $f.Gates.tier = 'Standard'; $f.Card.owns = @('**'); $f.Files = Read-RinneFixture 'pr3-files.json' } 'needs-user' { param($r) $r.Level -eq 'user' -and @($r.Infra | Where-Object { $_ -like '.github/workflows/*' }).Count -ge 3 }
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
        # 命令自己马上退出，却留下一个继承了输出管道、活 40 秒的子进程：读管道只再等一个有上限的时间
        $linger = "Start-Process -FilePath '$([Environment]::ProcessPath)' -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 40' -NoNewWindow; 'before-exit'; exit 0"
        $sw2 = [Diagnostics.Stopwatch]::StartNew()
        $orphan = Invoke-RinneExternal ([Environment]::ProcessPath) @('-NoProfile', '-NonInteractive', '-Command', $linger) -TimeoutSeconds 60 -AllowFail
        $sw2.Stop()
        # 超时被结束的命令：结束前写出的输出也留着
        $partial = Invoke-RinneExternal ([Environment]::ProcessPath) @('-NoProfile', '-NonInteractive', '-Command', "'partial-out'; Start-Sleep -Seconds 120") -TimeoutSeconds 10 -AllowFail
        # 一直刷屏的命令：超时照样生效，输出只留最后一段（上限临时调小，finally 里还原成原来的值）
        $savedPump = & (Get-Module RinneGh) { $script:PumpMaxChars }
        & (Get-Module RinneGh) { $script:PumpMaxChars = 200000 }
        try {
            $sw3 = [Diagnostics.Stopwatch]::StartNew()
            $flood = Invoke-RinneExternal ([Environment]::ProcessPath) @('-NoProfile', '-NonInteractive', '-Command', "`$s = 'x' * 65536; while (`$true) { [Console]::Out.Write(`$s) }") -TimeoutSeconds 5 -AllowFail
            $sw3.Stop()
        } finally {
            & (Get-Module RinneGh) { param($v) $script:PumpMaxChars = $v } $savedPump
        }
        # 带超时的外部命令（合并后构建就这样起构建脚本）：起进程那一刻本进程的输出管道不可继承，构建留下的进程拖不住
        # pr-merge 的调用方。真起一个中间层 pwsh（输出接管道），它导入本模块、用 Invoke-RinneTimed 起一个桩；桩只取
        # scripts/build.ps1 的函数，留下一个最多活 12 秒的孙进程就退出。判据看的是「泄漏用例」比「同样启动、但不留孙进程」
        # 的基线多出多少秒：机器整体慢下来时两边一起慢，差值不受负载影响。再拿一份不调 Protect-StdHandles 的桩做反例，
        # 它必须被判成泄漏——不用绝对秒数也抓得到原来的问题。量完写停止文件，孙进程退出、放开 eof.log，临时目录最后才删得掉。
        $stub, $stubBase, $stubBare, $middle, $eofStop = (Join-Path $tmp 'eof-stub.ps1'), (Join-Path $tmp 'eof-stub-base.ps1'),
        (Join-Path $tmp 'eof-stub-bare.ps1'), (Join-Path $tmp 'eof-middle.ps1'), (Join-Path $tmp 'eof-stop')
        $pw = [Environment]::ProcessPath
        $build = Join-Path (Get-RinneRepoRoot) 'scripts/build.ps1'
        $linger = Get-RinneLingerCommand -Seconds 12 -StopFile $eofStop
        $runStub = "exit (Invoke-Logged 'cmd.exe' @('/c', 'start', '/b', '', '$pw', '-NoProfile', '-EncodedCommand', '$linger') '$tmp\eof.log' 60)"
        Set-Content -LiteralPath $stubBase -Value (". '$build'; Protect-StdHandles; exit (Invoke-Logged 'cmd.exe' @('/c', 'exit', '0') '$tmp\eof-base.log' 60)")
        Set-Content -LiteralPath $stub -Value (". '$build'; Protect-StdHandles; $runStub")
        Set-Content -LiteralPath $stubBare -Value (". '$build'; $runStub")
        Set-Content -LiteralPath $middle -Value ("param([string]`$Stub); Import-Module '$(Join-Path $PSScriptRoot 'lib/RinneGh.psm1')'; " +
            "`$null = Invoke-RinneTimed -Exe '$pw' -Arguments @('-NoProfile', '-NonInteractive', '-File', `$Stub) -TimeoutSeconds 60; 'middle-done'")
        $measureEof = {
            param([string]$StubPath)
            Remove-Item -LiteralPath $eofStop -ErrorAction SilentlyContinue
            $psi = [Diagnostics.ProcessStartInfo]::new($pw)
            foreach ($x in '-NoProfile', '-NonInteractive', '-File', $middle, '-Stub', $StubPath) { $psi.ArgumentList.Add($x) }
            $psi.UseShellExecute, $psi.RedirectStandardOutput, $psi.RedirectStandardError = $false, $true, $true
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $mp = [Diagnostics.Process]::Start($psi)
            $o, $e = $mp.StandardOutput.ReadToEndAsync(), $mp.StandardError.ReadToEndAsync()
            # 40 秒还读不到 EOF 就结束中间层（连同子孙）、判没跑完；没读完的 Result 不去碰，免得一直阻塞
            $ok = $o.Wait(40000) -and $e.Wait(40000)
            $sw.Stop()
            if (-not $ok) { try { $mp.Kill($true) } catch [InvalidOperationException] { $null = $_ } }
            $done = $ok -and $o.Result -match 'middle-done'
            $mp.Dispose()
            Set-Content -LiteralPath $eofStop -Value 'x'
            [pscustomobject]@{ Done = [bool]$done; Seconds = $sw.Elapsed.TotalSeconds }
        }
        $baseRun = & $measureEof $stubBase
        $leakRun = & $measureEof $stub
        # 反例：不调 Protect-StdHandles，孙进程继承桩的输出管道，中间层要等 Invoke-RinneTimed 的 drain 上限才回来
        $bareRun = & $measureEof $stubBare
        $leakDelta = $leakRun.Seconds - $baseRun.Seconds
        $bareDelta = $bareRun.Seconds - $baseRun.Seconds
        Add-RinneCase $cases ('带超时的外部命令：构建留下的进程不拖住 pr-merge 的调用方（基线 {0:N1} 秒、泄漏用例 {1:N1} 秒，差 {2:N1} 秒）' -f $baseRun.Seconds, $leakRun.Seconds, $leakDelta) $false (
            -not ($baseRun.Done -and $leakRun.Done -and $leakDelta -lt 5))
        Add-RinneCase $cases ('带超时的外部命令：不改继承（反例）时差值超过阈值，判据仍能抓到泄漏（基线 {0:N1} 秒、反例 {1:N1} 秒，差 {2:N1} 秒）' -f $baseRun.Seconds, $bareRun.Seconds, $bareDelta) $true (
            $baseRun.Done -and $bareRun.Done -and $bareDelta -ge 5)
        Add-RinneCase $cases '刷屏的用例之后，读取器的上限还原成原来的值' $false ((& (Get-Module RinneGh) { $script:PumpMaxChars }) -ne $savedPump)
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Add-RinneCase $cases '外部命令超时：到点结束进程、判超时' $true ($slow.TimedOut -and $slow.Code -ne 0 -and $sw.Elapsed.TotalSeconds -lt 30)
        Add-RinneCase $cases '外部命令没超时：照常返回退出码' $false (-not ($quick.Code -eq 3 -and -not $quick.TimedOut))
        Add-RinneCase $cases ('外部命令退出后，留下的子进程握着管道也不无限等（{0:N0} 秒）' -f $sw2.Elapsed.TotalSeconds) $true (
            $orphan.Code -eq 0 -and -not $orphan.TimedOut -and $sw2.Elapsed.TotalSeconds -lt 30 -and $orphan.Err -match '不再等')
        Add-RinneCase $cases '外部命令退出后没等到 EOF：已经读到的输出照样留着' $true ($orphan.Out -match 'before-exit')
        Add-RinneCase $cases '外部命令超时：结束前写出的输出照样留着' $true ($partial.TimedOut -and $partial.Out -match 'partial-out')
        # 读取器一次只收有限的块：数据一直现成可读（每次读都立刻完成）时也要回到调用方
        $ready = & (Get-Module RinneGh) {
            $pump = New-RinneStreamPump ([IO.StreamReader]::new([IO.MemoryStream]::new([Text.Encoding]::ASCII.GetBytes('x' * 1000000))))
            Step-RinneStreamPump $pump
            $pump
        }
        # MemoryStream 上每次读都同步完成，所以一次调用恰好收满 64 块
        Add-RinneCase $cases '读取器每次恰好收 64 块：数据一直现成时也按时返回' $true ((-not $ready.Done) -and $ready.Text.Length -eq 64 * 8192)
        Add-RinneCase $cases ('外部命令一直刷屏：超时照样生效（{0:N0} 秒），输出只留最后一段' -f $sw3.Elapsed.TotalSeconds) $true (
            $flood.TimedOut -and $sw3.Elapsed.TotalSeconds -lt 30 -and $flood.Out.Length -le 200000 + 100 -and $flood.Out -match '^（输出过长')

        # 合并后构建：最后一道；只在其余判据都通过、没碰基础设施时跑；子进程看不到凭据与个人针；
        # 每个 PR 自己的 wt/tip-<n>，用前清理；整段持构建槽、构建本身不在 git 锁里；带超时
        $tip = Join-Path (Get-RinneRoot) 'wt/tip-3'
        $slot = Get-PremergeSlot
        $card.tier = 'Standard'; & $writeCard
        $gates.tier = 'Standard'; & $writeGates
        $secretNames = 'GH_TOKEN', 'RINNE_SELFTEST_KEY', 'RINNE_SELFTEST_SECRET', 'RINNE_SELFTEST_PASSWORD', 'RINNE_SELFTEST_PAT', 'RINNE_SELFTEST_PLAIN', 'RINNE_BUILD_SLOT_HELD',
            'RINNE_SELFTEST_CREDENTIALS', 'RINNE_SELFTESTAPIKEY', 'RINNE_SELFTEST_URL'
        $saveTok = @{}; foreach ($k in $secretNames) { $saveTok[$k] = [Environment]::GetEnvironmentVariable($k) }
        try {
            $env:GH_TOKEN = 'tok-selftest'; $env:RINNE_SELFTEST_KEY = 'key-selftest'; $env:RINNE_SELFTEST_PLAIN = 'kept'
            $env:RINNE_SELFTEST_SECRET = 'secret-selftest'; $env:RINNE_SELFTEST_PASSWORD = 'password-selftest'; $env:RINNE_SELFTEST_PAT = 'pat-selftest'
            $env:RINNE_SELFTEST_CREDENTIALS = 'cred-selftest'; $env:RINNE_SELFTESTAPIKEY = 'apikey-selftest'
            $env:RINNE_SELFTEST_URL = 'https://u' + ':p@example.invalid/x'
            $env:RINNE_BUILD_SLOT_HELD = $null
            $lockLog.Clear(); $tipOps.Clear()
            $vc = Invoke-MergeCheck 3 $cid
            $b = $builds | Select-Object -Last 1
            Add-RinneCase $cases '合并后构建：其余都通过时最后才跑' $false (-not ($vc.Code -eq 'ready' -and $builds.Count -eq 1 -and $vc.Premerge.Ok))
            Add-RinneCase $cases '合并后构建：子进程看不到 GH_TOKEN、*_KEY、*_SECRET、*_PASSWORD、*_PAT、*_CREDENTIALS、*APIKEY*、带口令的 URL 与个人针，其他变量照常' $false (-not (
                    $b -and -not $b.GhToken -and -not $b.OtherKey -and -not $b.Secret -and -not $b.Password -and -not $b.Pat -and -not $b.Credentials -and -not $b.ApiKey -and
                    -not $b.Url -and -not $b.Needles -and $b.Plain -eq 'kept'))
            # 凭据判据只有一份：取自 scripts/build.ps1 那两条赋值（与按文本读出来的一致）；文件里没有或有两条时抛错，宁可不跑
            $buildText = Get-Content -Raw -LiteralPath (Join-Path (Get-RinneRepoRoot) 'scripts/build.ps1')
            $rules = Get-RinneSecretEnv
            Add-RinneCase $cases '凭据判据取自 scripts/build.ps1 的 $SecretEnv 与 $SecretEnvValue' $false (-not (
                    $buildText.Contains("`$SecretEnv = '$($rules.Name)'") -and $buildText.Contains("`$SecretEnvValue = '$($rules.Value)'")))
            $noRule = Join-Path $tmp 'no-rule.ps1'
            Set-Content -LiteralPath $noRule -Value "`$SecretEnv = 'x'"
            $twoRules = Join-Path $tmp 'two-rules.ps1'
            Set-Content -LiteralPath $twoRules -Value "`$SecretEnv = 'x'`n`$SecretEnvValue = 'y'`n`$SecretEnv = 'z'"
            Add-RinneCase $cases '凭据判据：build.ps1 里缺一条或有两条时抛错' $true ((Test-Throws { Get-RinneSecretEnv -Path $noRule }) -and (Test-Throws { Get-RinneSecretEnv -Path $twoRules }))
            Add-RinneCase $cases '合并后构建：构建脚本拿到持槽方（本进程）的 pid，不再自己取槽' $false (-not ($b -and $b.SlotHeld -eq "$PID"))
            Add-RinneCase $cases '合并后构建：跑完还原环境' $false ($env:GH_TOKEN -ne 'tok-selftest' -or $env:RINNE_SELFTEST_KEY -ne 'key-selftest' -or $env:RINNE_SELFTEST_SECRET -ne 'secret-selftest' -or $env:RINNE_SELFTEST_PASSWORD -ne 'password-selftest' -or $env:RINNE_SELFTEST_PAT -ne 'pat-selftest' -or $env:RINNE_SELFTEST_CREDENTIALS -ne 'cred-selftest' -or -not $env:RINNE_SELFTEST_URL -or $env:RINNE_PRIVACY_LOCAL -ne $needles.File -or $env:RINNE_BUILD_SLOT_HELD)
            Add-RinneCase $cases '合并后构建：在本 PR 自己的 wt/tip-3 里跑，带超时' $false (-not ($b.Cwd -eq $tip -and $b.Timeout -gt 0))
            Add-RinneCase $cases '合并后构建：构建持着构建槽，不在 git 锁里' $false (-not ($b.Held -contains $slot -and $b.Held -notcontains 'git'))
            Add-RinneCase $cases '合并后构建：第一次用时 worktree add 到 wt/tip-3' $false (-not ($tipOps.Count -ge 1 -and $tipOps[0].Line -match "worktree add --detach $([regex]::Escape($tip)) origin/feature/v1$"))
            Add-RinneCase $cases '合并后构建：建树、合并、abort 都在构建槽里且持 git 锁' $false (-not (
                    @($tipOps | Where-Object { $_.Line -match 'merge --no-ff --no-commit refs/remotes/origin/pr/3' }).Count -eq 1 -and
                    @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -ge 1 -and
                    @($tipOps | Where-Object { $_.Held -notcontains $slot -or $_.Held -notcontains 'git' }).Count -eq 0))
            Add-RinneCase $cases '合并后构建：先取构建槽再取 git 锁' $false (-not ($lockLog.IndexOf($slot) -ge 0 -and $lockLog.LastIndexOf('git') -gt $lockLog.IndexOf($slot)))
            # 删掉标记、还原本进程的锁存状态（真跑时就是下一个进程）
            $unlatch = { Remove-Item -LiteralPath $latchFile -Force -ErrorAction SilentlyContinue; Reset-RinneLatchProcess }
            # 构建脚本顺手改了主 clone 的判据文件或状态：判红，Reason 写明是哪一项（每一项单独改、改完还原）
            $snap = @{}
            foreach ($f in @($integrity.Configs.Values) + @($integrity.Info.Values) + @($integrity.Records, $integrity.Node, $integrity.Cards)) { $snap[$f] = Get-Content -Raw -LiteralPath $f }
            $hit = {
                param($what, $pattern)
                $tamper.What = $what
                try { $v = Invoke-MergeCheck 3 $cid } finally {
                    $tamper.What = ''
                    foreach ($f in $snap.Keys) { Set-Content -LiteralPath $f -Value $snap[$f] -NoNewline }
                    $tree.Dirty = $false; $tree.Sha = $tipSha
                    Get-ChildItem -LiteralPath $integrity.Hooks -Force | Where-Object Name -ne 'pre-push' | Remove-Item -Force
                    Set-Content -LiteralPath (Join-Path $integrity.Hooks 'pre-push') -Value '#!/bin/sh'
                    & $unlatch
                }
                $v.Code -eq 'not-ready' -and @($v.Failures | Where-Object { $_ -match "判据文件：[^（]*$pattern" }).Count -eq 1
            }
            Add-RinneCase $cases '合并后构建：构建改了主 clone 的 .git/config，判红并写明' $true (& $hit 'config' '\.git/config')
            Add-RinneCase $cases '合并后构建：构建改了用户级 git 配置，判红并写明' $true (& $hit 'user' '用户级 git 配置 ~/\.gitconfig')
            Add-RinneCase $cases '合并后构建：构建改了 hooks 目录里的文件，判红并写明' $true (& $hit 'hook' 'hooks/pre-push')
            Add-RinneCase $cases '合并后构建：构建在 hooks 目录里加了隐藏文件，判红并写明' $true (& $hit 'hidden-hook' 'hooks/post-checkout')
            Add-RinneCase $cases '合并后构建：构建改了记录文件，判红并写明' $true (& $hit 'records' '记录文件')
            Add-RinneCase $cases '合并后构建：构建改了之后还要用的 node，判红并写明' $true (& $hit 'node' 'node')
            Add-RinneCase $cases '合并后构建：构建改了卡片清单，判红并写明' $true (& $hit 'cards' '卡片清单')
            Add-RinneCase $cases '合并后构建：构建之后主 clone 的工作树不干净，判红并写明' $true (& $hit 'dirty' '主 clone 的工作树')
            Add-RinneCase $cases '合并后构建：构建之后主 clone 的 HEAD 变了，判红并写明' $true (& $hit 'head' '主 clone 的 HEAD')
            Add-RinneCase $cases '合并后构建：构建改了共享 .git 里的 info/exclude，判红并写明' $true (& $hit 'exclude' '\.git/info/exclude')
            Add-RinneCase $cases '合并后构建：构建改了用户级的全局忽略文件，判红并写明' $true (& $hit 'global-ignore' '用户级 git/ignore')
            # 判据文件变了（例如 git 配置里多了会执行代码的项）：之后不再跑 git——不查主 clone 的工作区、不 abort wt/tip-3 的合并
            $c0 = $tree.Calls; $tipOps.Clear()
            $null = Invoke-MergeCheck 3 $cid
            $plain, $plainAbort = ($tree.Calls - $c0), @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count
            $c1 = $tree.Calls; $tipOps.Clear()
            $tamper.What = 'config'
            try { $vz = Invoke-MergeCheck 3 $cid } finally { $tamper.What = ''; foreach ($f in $snap.Keys) { Set-Content -LiteralPath $f -Value $snap[$f] -NoNewline } }
            Add-RinneCase $cases '合并后构建：判据文件变了之后不再跑 git（不查主 clone 的工作区、不 abort），Reason 写明' $true (
                $vz.Code -eq 'not-ready' -and $plainAbort -ge 1 -and ($tree.Calls - $c1) -eq $plain - 1 -and
                @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -eq 0 -and @($vz.Failures | Where-Object { $_ -match '之后没有再跑 git' }).Count -eq 1)
            # 锁存：判据文件变了，构建前写下的进行中标记改写成「已篡改」（PR 号、时间、哪一项变了）；下一次 -Check / -Merge / -After
            # 在跑任何 git 与 gh 之前就拒绝，维护者删掉标记才放行。包装脚本与 gates 的入口由 PremergeLatch.psm1 的自测起子进程核对
            $readLatch = { $t = $(try { Read-RinneLatchText $latchFile } catch { $null }); $t ? ($t | ConvertFrom-Json -AsHashtable) : $null }
            $gitSince = { param($n) @($log | Select-Object -Skip $n | Where-Object { $_ -match '^(git|gh) ' }).Count }
            $thrown = { param($sb) try { & $sb | Out-Null; '' } catch { $_.Exception.Message } }
            $note = & $readLatch
            Add-RinneCase $cases '合并后构建：判据文件变了，锁存标记改写成已篡改，记下 PR 号、时间与变了的项' $true ($note -and $note.state -eq 'tampered' -and $note.pr -eq 3 -and $note.at -and @($note.changed) -contains '.git/config')
            Reset-RinneLatchProcess
            $n0 = $log.Count
            $latchCheck, $latchMerge, $latchAfter = (& $thrown { Invoke-MergeCheck 3 $cid }), (& $thrown { Invoke-MergeUnlocked -Pr 3 -CardId $cid -SummaryFile 'x' }), (& $thrown { Invoke-AfterUnlocked -Pr 3 -CardId $cid })
            Add-RinneCase $cases '锁存标记在：下一次 -Check / -Merge / -After 一条 git、gh 都不跑就拒绝' $true (
                $latchCheck -match '^已锁存' -and $latchMerge -match '^已锁存' -and $latchAfter -match '^已锁存' -and (& $gitSince $n0) -eq 0)
            & $unlatch
            $vr = Invoke-MergeCheck 3 $cid
            $bn = (($builds | Select-Object -Last 1).Latch ?? '{}') | ConvertFrom-Json -AsHashtable
            Add-RinneCase $cases '维护者删掉锁存标记之后照常核对' $false ($vr.Code -ne 'ready')
            Add-RinneCase $cases '合并后构建：构建开始时进行中标记已经写好（PR 号、pid、构建前指纹摘要、随机值），核对通过后清掉' $false (-not (
                    $bn.state -eq 'building' -and $bn.pr -eq 3 -and $bn.pid -eq $PID -and "$($bn.fingerprint)" -match '^[0-9a-f]{64}$' -and "$($bn.nonce)" -match '^[0-9a-f]{32}$' -and
                    -not (Test-Path -LiteralPath $latchFile)))
            # 父进程中途出事（这里是构建中途抛错，同被结束一样走不到核对）：进行中标记留着、不 abort；下一次 -Check 不跑 git、gh 就拒绝
            $tipOps.Clear()
            $tamper.What = 'throw'
            try { $threw = Test-Throws { Invoke-MergeCheck 3 $cid } } finally { $tamper.What = '' }
            $left = & $readLatch
            Reset-RinneLatchProcess
            $n0 = $log.Count
            $again = & $thrown { Invoke-MergeCheck 3 $cid }
            Add-RinneCase $cases '合并后构建：构建中途出错，进行中标记留着、不 abort；下一次 -Check 不跑 git、gh 就拒绝' $true (
                $threw -and $left -and $left.state -eq 'building' -and @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -eq 0 -and
                $again -match '^已锁存.*进行中' -and (& $gitSince $n0) -eq 0)
            & $unlatch
            # 握着句柄（Windows）：构建删不掉、换不成目录、改不了标记，核对照常通过、标记清掉
            if ($IsWindows) {
                foreach ($w in 'latch-delete', 'latch-dir', 'latch-edit') {
                    $tamper.What, $tamper.Refused = $w, $null
                    try { $vh = Invoke-MergeCheck 3 $cid } finally { $tamper.What = '' }
                    Add-RinneCase $cases "合并后构建：握着进行中标记，构建想 $w 也做不到" $true ($tamper.Refused -eq $true -and $vh.Code -eq 'ready' -and -not (Test-Path -LiteralPath $latchFile))
                }
            }
            # 握不住句柄的平台（这里关掉握持来模拟）：标记被删掉、换成目录、内容改过（随机值不同），都判红，下一次调用（新进程）都
            # 拒绝。标记先查：不在原样就不再算指纹、不查主 clone 的工作区（比平常少查一次），Failures 写的是「构建期间被动过」
            $byBuild = '判据文件：锁存标记（构建期间被删掉、换掉或改过）'
            Set-RinneLatchHold $false
            try {
                $tamper.What = 'latch-delete'
                $s0 = $tree.Calls
                try { $vd = Invoke-MergeCheck 3 $cid } finally { $tamper.What = '' }
                $ds = $tree.Calls - $s0
                $dn = & $readLatch
                Reset-RinneLatchProcess
                $n0 = $log.Count
                $again = & $thrown { Invoke-MergeCheck 3 $cid }
                Add-RinneCase $cases '合并后构建：构建删掉了进行中标记，判红（不再算指纹、不查工作区），标记写回成已篡改，下一次拒绝' $true (
                    $vd.Code -eq 'not-ready' -and @($vd.Failures | Where-Object { $_.Contains($byBuild) }).Count -eq 1 -and $ds -eq $plain - 1 -and
                    $dn -and $dn.state -eq 'tampered' -and $again -match '^已锁存' -and (& $gitSince $n0) -eq 0)
                & $unlatch
                $tamper.What = 'latch-edit'
                $s0 = $tree.Calls
                try { $ve = Invoke-MergeCheck 3 $cid } finally { $tamper.What = '' }
                $es = $tree.Calls - $s0
                $en = & $readLatch
                Reset-RinneLatchProcess
                $n0 = $log.Count
                $again = & $thrown { Invoke-MergeCheck 3 $cid }
                Add-RinneCase $cases '合并后构建：构建改了进行中标记（随机值不同），判红（不再算指纹、不查工作区），改写成已篡改，下一次拒绝' $true (
                    $ve.Code -eq 'not-ready' -and @($ve.Failures | Where-Object { $_.Contains($byBuild) }).Count -eq 1 -and $es -eq $plain - 1 -and
                    $en -and $en.state -eq 'tampered' -and $again -match '^已锁存' -and (& $gitSince $n0) -eq 0)
                & $unlatch
                # 换成同名目录：写不回去，但目录留在原处，下一次照样拒绝
                $tamper.What = 'latch-dir'
                $s0 = $tree.Calls
                try { $vdir = Invoke-MergeCheck 3 $cid } finally { $tamper.What = '' }
                $dirs = $tree.Calls - $s0
                $dirLeft = Test-Path -LiteralPath $latchFile -PathType Container
                Reset-RinneLatchProcess
                $n0 = $log.Count
                $again = & $thrown { Invoke-MergeCheck 3 $cid }
                Add-RinneCase $cases '合并后构建：构建把进行中标记换成同名目录，判红（不再算指纹、不查工作区）；目录留着，下一次拒绝' $true (
                    $vdir.Code -eq 'not-ready' -and @($vdir.Failures | Where-Object { $_.Contains($byBuild) -and $_ -match '照样锁存' }).Count -eq 1 -and $dirs -eq $plain - 1 -and
                    $dirLeft -and $again -match '^已锁存' -and (& $gitSince $n0) -eq 0)
                # 只是测试建的空目录：不递归删
                Remove-Item -LiteralPath $latchFile -Force
                Reset-RinneLatchProcess
                # 核对全部通过之后、清除之前标记被删掉（构建留下的进程还活着）：照样写回成已篡改、不 abort，下一次拒绝
                $tipOps.Clear()
                $tamper.What = 'latch-late'
                try { $vl = Invoke-MergeCheck 3 $cid } finally { $tamper.What = ''; $tamper.Late = $false }
                $ln = & $readLatch
                Reset-RinneLatchProcess
                $n0 = $log.Count
                $again = & $thrown { Invoke-MergeCheck 3 $cid }
                Add-RinneCase $cases '合并后构建：核对通过之后、清除之前标记被删掉，写回成已篡改、不 abort，下一次拒绝' $true (
                    $vl.Code -eq 'not-ready' -and @($vl.Failures | Where-Object { $_ -match '判据文件：锁存标记（核对之后' }).Count -eq 1 -and $ln -and $ln.state -eq 'tampered' -and
                    @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -eq 0 -and $again -match '^已锁存' -and (& $gitSince $n0) -eq 0)
                & $unlatch
                # 核对途中抛错：算指纹时标记被删掉，下一条外部命令之前的检查抛错。照样写回成已篡改、写明核对没有走完；不 abort、
                # 不再查工作区，下一次拒绝（不接住这个抛错时，标记就一直是删掉的，下一次放行）
                $tipOps.Clear()
                $tamper.What = 'latch-mid'
                $s0 = $tree.Calls
                try { $vm = Invoke-MergeCheck 3 $cid } finally { $tamper.What = ''; $tamper.Mid = $false }
                $ms = $tree.Calls - $s0
                $mn = & $readLatch
                Reset-RinneLatchProcess
                $n0 = $log.Count
                $again = & $thrown { Invoke-MergeCheck 3 $cid }
                Add-RinneCase $cases '合并后构建：核对途中抛错（算指纹时标记被删掉），写回成已篡改、写明核对没有走完，不 abort、不查工作区，下一次拒绝' $true (
                    $vm.Code -eq 'not-ready' -and @($vm.Failures | Where-Object { $_ -match '判据文件：核对没有走完' }).Count -eq 1 -and $ms -eq $plain - 1 -and
                    $mn -and $mn.state -eq 'tampered' -and @($mn.changed | Where-Object { $_ -match '^核对没有走完' }).Count -eq 1 -and
                    @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -eq 0 -and $again -match '^已锁存' -and (& $gitSince $n0) -eq 0)
                & $unlatch
            } finally { Set-RinneLatchHold $true }
            # 查工作区才发现不干净（指纹已经算过）：同样改写成已篡改；finally 里看到本进程已锁存，不 abort，Reason 写明
            $tipOps.Clear()
            $tamper.What = 'dirty'
            try { $v5 = Invoke-MergeCheck 3 $cid } finally { $tamper.What = ''; $tree.Dirty = $false }
            $n5 = & $readLatch
            Add-RinneCase $cases '合并后构建：查工作区才发现不干净，改写成已篡改、不 abort，Reason 写明合并没有 abort' $true (
                $v5.Code -eq 'not-ready' -and $n5 -and $n5.state -eq 'tampered' -and @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count -eq 0 -and
                @($v5.Failures | Where-Object { $_ -match '判据文件：主 clone 的工作树' -and $_ -match '之后没有再跑 git：wt/tip-3 的合并没有 abort' }).Count -eq 1)
            & $unlatch
            # 在途的调用撞上之后才写下的标记（别的进程这时写下进行中或已篡改的标记）：写下之后一条 git、gh 都不再跑，finally 里
            # 也不 abort。Result 是返回值或抛出的消息；After 是写下之后日志里 git、gh 的条数；Calls 是查主 clone 工作区的次数
            $other = '{"state":"building","pr":4,"at":"2026-01-01T00:00:00Z","pid":1,"fingerprint":"x","nonce":"y"}'
            $otherTampered = '{"state":"tampered","pr":4,"at":"2026-01-01T00:00:00Z","changed":[".git/config"]}'
            $midCall = {
                param($mcSet)
                $tipOps.Clear(); $plant.At = -1; $plant.Text = $mcSet.Text
                foreach ($k in 'Tip', 'Facts', 'Clone', 'Config') { if ($mcSet.ContainsKey($k)) { $plant[$k] = $mcSet[$k] } }
                $mcN, $mcS = $builds.Count, $tree.Calls
                $mcR = try { Invoke-MergeCheck 3 $cid } catch { $_.Exception.Message }
                $mcO = [pscustomobject]@{
                    Result = $mcR; Thrown = $mcR -is [string]; Failures = @(($mcR -is [string]) ? @() : $mcR.Failures)
                    After = ($plant.At -ge 0) ? @($log | Select-Object -Skip $plant.At | Where-Object { $_ -match '^(git|gh) ' }).Count : -1
                    Builds = $builds.Count - $mcN; Calls = $tree.Calls - $mcS; Left = $(try { Read-RinneLatchText $latchFile } catch { $null })
                    Aborts = @($tipOps | Where-Object { $_.Line -match 'merge --abort' }).Count; Ops = @($tipOps)
                }
                $plant.Tip = ''; $plant.Facts = $false; $plant.Clone = $false; $plant.Config = $false
                & $unlatch
                $mcO
            }
            $f1 = & $midCall @{ Text = $other; Tip = 'reset' }
            Add-RinneCase $cases '在途：wt/tip-3 的 reset 途中别的进程写下进行中标记，之后一条 git、gh 都不跑（不 abort、不构建），Reason 写明停在半路' $true (
                $f1.Ops.Count -eq 1 -and $f1.Ops[0].Line -match 'reset --hard' -and $f1.After -eq 0 -and $f1.Builds -eq 0 -and $f1.Left -ceq $other -and
                @($f1.Failures | Where-Object { $_ -match '已锁存' -and $_ -match '停在半路' }).Count -eq 1)
            $f2 = & $midCall @{ Text = $otherTampered; Facts = $true }
            Add-RinneCase $cases '在途：拉 PR 信息途中别的进程写下已篡改标记，之后一条 git、gh 都不跑' $true (
                $f2.After -eq 0 -and $f2.Builds -eq 0 -and $f2.Left -ceq $otherTampered -and
                (($f2.Thrown -and $f2.Result -match '^已锁存') -or @($f2.Failures | Where-Object { $_ -match '已锁存' }).Count -eq 1))
            $f3 = & $midCall @{ Text = $other; Clone = $true }
            Add-RinneCase $cases '在途：构建前查主 clone 工作区时别的进程写下标记，写自己的标记时认出是锁存（不是「写不进去」），不构建、不 abort' $true (
                $f3.After -eq 0 -and $f3.Builds -eq 0 -and $f3.Aborts -eq 0 -and $f3.Left -ceq $other -and
                @($f3.Failures | Where-Object { $_ -match '另一次合并后构建进行中或已锁存' -and $_ -notmatch '写不进去' }).Count -eq 1)
            $f4 = & $midCall @{ Text = $other; Config = $true }
            Add-RinneCase $cases '在途：构建前算指纹（git 解析用户级配置）时别的进程写下标记，之后不查工作区、不写标记、不构建、不 abort' $true (
                $f4.Calls -eq $plain - 2 -and $f4.After -eq 0 -and $f4.Builds -eq 0 -and $f4.Aborts -eq 0 -and $f4.Left -ceq $other -and
                @($f4.Failures | Where-Object { $_ -match '已锁存' -and $_ -match '停在半路' }).Count -eq 1)
            # 并发：另一个进程先持着 build-slot-1；这一次在它写下标记之前已经走到取槽，就在槽上等它整段结束。它清掉了标记：照常
            # 建树、构建，第一条 git 在它放开槽之后；它改写成已篡改：拿到槽之后的第一条 git 就拒绝，一条都不跑
            $conc = Join-Path $tmp 'conc'
            $null = New-Item -ItemType Directory -Path $conc
            Set-Content -LiteralPath (Join-Path $conc 'building.json') -Value $other -NoNewline
            Set-Content -LiteralPath (Join-Path $conc 'tampered.json') -Value $otherTampered -NoNewline
            $child = Join-Path $tmp 'conc-child.ps1'
            Set-Content -LiteralPath $child -Value @(
                'param($Locks, $Scope, $Dir, $Latch, $Mode)'
                '$ErrorActionPreference = ''Stop'''
                'Import-Module $Locks'
                '& (Get-Module Locks) { param($s) $script:Scope = $s } $Scope'
                '$l = Enter-RinneLock ''build-slot-1'' -TimeoutSeconds 30 -Quiet'
                'if (-not $l) { exit 2 }'
                'try {'
                '    Set-Content -LiteralPath (Join-Path $Dir ''holding'') -Value ''x'''
                '    $d = [datetime]::UtcNow.AddSeconds(30)'
                '    while (-not (Test-Path -LiteralPath (Join-Path $Dir ''waiting'')) -and [datetime]::UtcNow -lt $d) { Start-Sleep -Milliseconds 50 }'
                '    Copy-Item -LiteralPath (Join-Path $Dir ''building.json'') -Destination $Latch'
                '    Start-Sleep -Milliseconds 800'
                '    if ($Mode -eq ''clear'') { Remove-Item -LiteralPath $Latch } else { Copy-Item -LiteralPath (Join-Path $Dir ''tampered.json'') -Destination $Latch -Force }'
                '    Set-Content -LiteralPath (Join-Path $Dir ''released'') -Value ([datetime]::UtcNow.Ticks)'
                '} finally { Exit-RinneLock $l }'
            )
            $runConc = {
                param($mode)
                foreach ($f in 'holding', 'waiting', 'released') { Remove-Item -LiteralPath (Join-Path $conc $f) -ErrorAction SilentlyContinue }
                $psi = [Diagnostics.ProcessStartInfo]::new([Environment]::ProcessPath)
                $scope = & (Get-Module Locks) { $script:Scope }
                foreach ($x in '-NoProfile', '-NonInteractive', '-File', $child, (Join-Path $PSScriptRoot '../lib/Locks.psm1'), $scope, $conc, $latchFile, $mode) { $psi.ArgumentList.Add($x) }
                $psi.UseShellExecute, $psi.CreateNoWindow, $psi.RedirectStandardOutput, $psi.RedirectStandardError = $false, $true, $true, $true
                $p = [Diagnostics.Process]::Start($psi)
                try {
                    $pOut, $pErr = $p.StandardOutput.ReadToEndAsync(), $p.StandardError.ReadToEndAsync()
                    $d = [datetime]::UtcNow.AddSeconds(30)
                    while (-not (Test-Path -LiteralPath (Join-Path $conc 'holding')) -and -not $p.HasExited -and [datetime]::UtcNow -lt $d) { Start-Sleep -Milliseconds 50 }
                    $tipOps.Clear(); $n = $builds.Count
                    $concur.Signal = Join-Path $conc 'waiting'
                    try { $r = try { Invoke-MergeCheck 3 $cid } catch { $_.Exception.Message } } finally { $concur.Signal = $null }
                    $exited = $p.WaitForExit(30000)
                    $rel = Join-Path $conc 'released'
                    [pscustomobject]@{
                        Exit = $exited ? $p.ExitCode : -1; Err = $exited ? $pErr.Result : ''; Result = $r; Builds = $builds.Count - $n; Ops = @($tipOps)
                        Released = (Test-Path -LiteralPath $rel) ? [long](Get-Content -Raw -LiteralPath $rel).Trim() : 0
                        Left = $(try { Read-RinneLatchText $latchFile } catch { $null })
                    }
                    $null = $pOut
                } finally {
                    if (-not $p.HasExited) { try { $p.Kill($true) } catch [InvalidOperationException] { $null = $_ } }
                    $p.Dispose()
                }
            }
            $ci = & $runConc 'clear'
            Add-RinneCase $cases '并发：另一个进程持着 build-slot-1，这一次在槽上等它整段结束；它清掉了标记，照常构建' $false (-not (
                    $ci.Exit -eq 0 -and $ci.Result -isnot [string] -and $ci.Result.Code -eq 'ready' -and $ci.Builds -eq 1 -and $ci.Released -gt 0 -and
                    $ci.Ops.Count -ge 1 -and $ci.Ops[0].At -gt $ci.Released))
            $ct = & $runConc 'tamper'
            Add-RinneCase $cases '并发：在槽上等到的是改写成已篡改的标记，拿到槽之后一条 git 都不跑、不构建' $true (
                $ct.Exit -eq 0 -and $ct.Result -isnot [string] -and $ct.Result.Code -eq 'not-ready' -and $ct.Ops.Count -eq 0 -and $ct.Builds -eq 0 -and
                $ct.Left -ceq $otherTampered -and @($ct.Result.Failures | Where-Object { $_ -match '已锁存' }).Count -eq 1)
            & $unlatch
            # 标记写不进去（目录不在之类；位置上已经有东西的见上面）：不跑构建
            Set-RinneLatchOverride (Join-Path $tmp 'no-such-dir/latch.json')
            $n = $builds.Count
            try { $vw = Invoke-MergeCheck 3 $cid } finally { Set-RinneLatchOverride $latchFile }
            Add-RinneCase $cases '合并后构建：锁存标记写不进去时不跑构建、判红' $true ($vw.Code -eq 'not-ready' -and $builds.Count -eq $n -and @($vw.Failures | Where-Object { $_ -match '锁存标记写不进去' }).Count -eq 1)
            $tamper.What = 'branch'
            $vb = Invoke-MergeCheck 3 $cid
            $tamper.What = ''
            foreach ($f in $snap.Keys) { Set-Content -LiteralPath $f -Value $snap[$f] -NoNewline }
            Add-RinneCase $cases '合并后构建：构建期间别的进程只往 .git/config 写了 branch 段，照常通过' $false ($vb.Code -ne 'ready')
            $vu = Invoke-MergeCheck 3 $cid
            Add-RinneCase $cases '合并后构建：没改判据文件时照常通过' $false ($vu.Code -ne 'ready')
            $tipOps.Clear()
            $vc = Invoke-MergeCheck 3 $cid
            $ops = @($tipOps | ForEach-Object { ($_.Line -replace '^-C \S+ ', '') -replace ' (--quiet|origin/feature/v1|--no-commit refs/remotes/origin/pr/3)', '' })
            Add-RinneCase $cases '合并后构建：再用时先 reset --hard、回到 feature/v1、git clean -ffdx，再合并' $false (($ops -join ' | ') -ne 'reset --hard | checkout --detach --force | clean -ffdx | merge --no-ff | merge --abort')
            Add-RinneCase $cases '合并后构建：都用同一把构建槽 build-slot-1（同一时间只跑一个，并发的见上面）' $false ($slot -ne 'build-slot-1')
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
            # 卡片标的是 Quick，但改了 src/：重算的最低档是 Standard，合并后构建照样要跑
            $card.tier = 'Quick'; $card.owns = @('**'); & $writeCard
            $files = @([pscustomobject]@{ filename = 'src/core/loop/Ring.cpp'; status = 'modified' })
            $n = $builds.Count
            $vc = Invoke-MergeCheck 3 $cid
            Add-RinneCase $cases '合并后构建：卡片标 Quick、改了 src/ 也跑（按重算的最低档）' $false (-not ($vc.Code -eq 'ready' -and $builds.Count -eq $n + 1 -and $vc.Tier.Min -eq 'Standard'))
            $files = @([pscustomobject]@{ filename = 'docs/example.md'; status = 'added' })
            $card.tier = 'Standard'; $card.owns = @('docs/example.md'); & $writeCard
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
        # 带合并后构建的 -Merge：squash 消息的生成与检查（node 跑的判据）都在构建之前，构建之后只剩接口核对与合并请求
        $card.tier = 'Standard'; & $writeCard
        $gates.tier = 'Standard'; & $writeGates
        Set-Content -LiteralPath (Join-Path $tip 'scripts/build.ps1') -Value 'exit 0'
        $log.Clear()
        $n = $put.Count
        Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum | Out-Null
        $idx = { param($re, [switch]$Last) $hits = @(for ($k = 0; $k -lt $log.Count; $k++) { if ($log[$k] -match $re) { $k } }); $hits.Count ? ($Last ? $hits[-1] : $hits[0]) : -1 }
        $buildAt = & $idx 'build\.ps1$'
        Add-RinneCase $cases '-Merge：squash 消息与文本检查在合并后构建之前，构建之后不再跑 node，最后才发合并请求' $false (-not (
                $put.Count -eq $n + 1 -and $buildAt -ge 0 -and (& $idx 'maint\.mjs message') -ge 0 -and (& $idx 'maint\.mjs message') -lt $buildAt -and
                (& $idx '\.mjs( |$)' -Last) -lt $buildAt -and (& $idx 'X PUT \S+/pulls/3/merge' -Last) -gt $buildAt))
        # 构建期间 PR 标题改了：构建前生成的 squash 消息已经过期，不合
        $n = $put.Count; $origTitle = $prFixture.title
        $tamper.What = 'title'
        try { $titleThrew = Test-Throws { Invoke-Merge -Pr 3 -CardId $cid -SummaryFile $sum } } finally { $tamper.What = ''; $prFixture.title = $origTitle }
        Add-RinneCase $cases '-Merge：构建期间 PR 标题改了（squash 消息已过期），不合' $true ($titleThrew -and $put.Count -eq $n)
        $card.tier = 'Quick'; & $writeCard
        $gates.tier = 'Quick'; & $writeGates
        # workflow 按规则至少是 Standard：gates 也按 Standard 跑过（基础设施 PR 不在本机做合并后构建）
        $files = @([pscustomobject]@{ filename = '.github/workflows/x.yml'; status = 'modified' })
        $card.owns = @('.github/workflows/x.yml'); & $writeCard
        $gates.tier = 'Standard'; & $writeGates
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
        $gates.tier = 'Quick'; & $writeGates

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
            # 假的 worktree remove：挪进自测的临时目录（不递归删参数给的路径），最后随临时目录一起删。挪之前核对给的路径按真实路径
            # 在这个临时目录里面，不在就抛错、不动它（Get-RinneRoot 的取值以后变了，也挪不到真的 wt/ 下的目录）
            ' worktree remove '              = {
                param($argv)
                if (-not (Test-RinneInTempDir -Path $argv[-1] -Dir $tmp)) { throw "假的 worktree remove 只挪自测临时目录里面的路径：$($argv[-1])" }
                Move-Item -LiteralPath $argv[-1] -Destination (Join-Path $tmp "removed-$([guid]::NewGuid().ToString('N'))"); ''
            }
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
        # 假的 worktree remove 给的路径在自测临时目录外面：直接给、经临时目录里的链接给，都抛错，原样不动。外面那个目录是本进程新建的
        $away = New-RinneTempDir -Prefix 'rinne-merge-selftest-'
        Set-Content -LiteralPath (Join-Path $away 'keep.txt') -Value 'x'
        $toAway = Join-Path $tmp 'wt/to-away'
        $null = New-Item -ItemType ($IsWindows ? 'Junction' : 'SymbolicLink') -Path $toAway -Target $away
        $rm = $after[' worktree remove ']
        $awayThrew = Test-Throws { & $rm @('git', 'worktree', 'remove', '--force', $away) }
        $linkThrew = Test-Throws { & $rm @('git', 'worktree', 'remove', '--force', $toAway) }
        Add-RinneCase $cases '假的 worktree remove：给的路径（按真实路径）在自测临时目录外面就抛错，原样不动' $true (
            $awayThrew -and $linkThrew -and (Test-Path -LiteralPath (Join-Path $away 'keep.txt')) -and (Test-Path -LiteralPath $toAway))
        # 只删链接本身（核对失效时它已经被挪走，不在原处）
        if (Test-Path -LiteralPath $toAway) { Remove-Item -LiteralPath $toAway -Force }

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
        Set-RinneCloneStateImpl $null
        Set-RinneInfraList $null
        Exit-RinneSelfTestNeedles $needles
        $script:IntegrityTargets = $null
        Set-RinneLatchOverride $null
        Set-RinneLatchHold $true
        Reset-RinneLatchProcess
        $env:RINNE_ROOT = $saved.Root
        $env:RINNE_CARDS = $saved.Cards
        $env:RINNE_WAIVERS = $saved.Waivers
        # EOF 用例的孙进程量完就收到停止信号；万一它还握着 eof.log，最多再等 15 秒
        $awayLeft = Remove-RinneTempDir -Path $away
        $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 15
        Add-RinneCase $cases '自测的临时目录删掉了（EOF 用例的孙进程量完就退出）' $false ($left -or $awayLeft -or (Test-Path -LiteralPath $tmp) -or ($away -and (Test-Path -LiteralPath $away)))
    }
    Complete-RinneSelfTest $cases 'pr-merge'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
if (@($Check, $Merge, $After | Where-Object { $_ }).Count -ne 1) { throw '用法：-Check | -Merge | -After，三选一（或 -SelfTest）' }
if ($Pr -le 0 -or $Card -notmatch $CardPattern) { throw '需要 -Pr <n> 与 -Card <卡号>' }
# 锁存标记在任何 git 与 gh 之前看（导入 RinneGh 时已经看过一次，这里再明说一遍）
Assert-RinneNoLatch
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
