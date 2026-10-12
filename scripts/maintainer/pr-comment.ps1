#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PR 评论的读与写。写：我方评论、线程回复、处置汇总只经这里发出，一律过公开文本检查并在首行加 agent 隐藏标记。
#   -List -Pr <n> [-Since <ISO>] [-Json]
#        四个来源（issue comments、review comments、reviews、GraphQL reviewThreads）摊成一张表：作者分类
#        （agent / bot-summary / bot / user / external，按仓库权限认人）、按 updated_at 判新、是否已回应；
#        另列出每条 bot 总结里的逐条意见（含没开 thread 的）
#   -Footer -Pr <n> [-Head <sha40>]
#        最近一条 bot 总结的机读尾注；缺失、多于一行、条数与正文不符或审的不是该 head → 「bot 结论不可用」，退出 2
#   -Wait -Pr <n> -Head <sha40> [-TimeoutMin 9]
#        等 feature 线的 required checks 与 claude-pr-review 在该 head 上都跑完；单次最多 9 分钟，没跑完退出 3，
#        head 变了退出 4
#   -WaitMerged -Pr <n> [-TimeoutMin 9]        等 PR 合入；单次最多 9 分钟，没合入退出 3
#   -Sweep -Since <ISO>
#        全仓（含已合并 / 已关闭的 PR 与 issue）晚于该时间、我方还没回应的评论
#   -Post -Pr <n> -BodyFile <f> -Role <impl|review|orchestrator> -Kind <…> -Round <n> -Head <sha> [-BotRun <id>]
#        发一条 issue comment；-Kind disposition（评审处置汇总）必须带 -BotRun
#   -Reply -Pr <n> -To <评论 id> -BodyFile <f> -Role -Round -Head
#        回复：review comment 在原线程里回；issue comment 与 review 另发一条，正文开头链接到原评论
#   -Resolve -Pr <n> -Thread <threadId> -BodyFile <f> -Role -Round -Head
#        在线程里回复，然后 resolveReviewThread
#   -SelfTest

[CmdletBinding()]
param(
    [switch]$List, [switch]$Footer, [switch]$Wait, [switch]$WaitMerged, [switch]$Sweep,
    [switch]$Post, [switch]$Reply, [switch]$Resolve, [switch]$SelfTest,
    [int]$Pr, [string]$Head, [string]$Since, [switch]$Json, [int]$TimeoutMin = 9,
    [string]$BodyFile, [string]$Role, [string]$Kind, [int]$Round = -1, [string]$BotRun,
    [string]$To, [string]$Thread
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

$PollSeconds = 20

# ---- 读 -------------------------------------------------------------------------------------

function Get-ListReport {
    param([int]$Pr, [string]$Since)
    $activity = Get-RinnePrActivity $Pr
    $state = Get-RinneReviewState $activity
    $items = @(Get-RinneCommentItems -Activity $activity -State $state -Since $Since)
    $rows = foreach ($i in $items) {
        [pscustomobject]@{
            Source = $i.Source; Id = $i.Id; Author = $i.Author; Kind = $i.Kind; Updated = $i.Updated; IsNew = $i.IsNew
            Responded = Test-RinneResponded -Item $i -Items $items -State $state; Thread = $i.ThreadId; Resolved = $i.Resolved
            Url = $i.Url; Body = $i.Body
        }
    }
    $findings = foreach ($s in @($state.summaries)) {
        foreach ($f in @($s.items)) { [pscustomobject]@{ Summary = [string]$s.id; Run = $s.runId; Head = $s.head; Level = $f.level; Text = $f.text } }
    }
    [pscustomobject]@{
        Pr = $Pr; Items = @($rows); BotFindings = @($findings)
        OpenThreads = @($activity.Threads | Where-Object { -not $_.isResolved }).Count
        External = @($rows | Where-Object Kind -eq 'external').Count
        Unanswered = @($rows | Where-Object { -not $_.Responded }).Count
    }
}

<# 最近一条 bot 总结的尾注。可用返回 @{ Ok = $true; Red; Important; Suggest; Run }，否则 @{ Ok = $false; Reason }。 #>
function Get-FooterResult {
    param([int]$Pr, [string]$Head)
    $activity = Get-RinnePrActivity $Pr
    $s = (Get-RinneReviewState $activity).summary
    if (-not $s) { return @{ Ok = $false; Reason = '还没有 bot 审查总结' } }
    if ($Head -and $s.head -ne $Head) { return @{ Ok = $false; Reason = "最近一条总结审的不是 $($Head.Substring(0, 7))" } }
    if (-not $s.ok) { return @{ Ok = $false; Reason = $s.reason } }
    @{ Ok = $true; Red = $s.red; Important = $s.important; Suggest = $s.suggest; Run = $s.runId; Head = $s.head }
}

function Get-WaitState {
    param([int]$Pr, [string]$Head)
    $pull = Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr"
    if ($pull.head.sha -ne $Head) { return [pscustomobject]@{ State = 'moved'; Head = $pull.head.sha; Checks = @() } }
    $runs = Get-RinneCheckRuns $Head
    $required = Get-RinneRequiredChecks
    $want = @($required) + @('claude-pr-review')
    $checks = foreach ($n in $want) {
        $c = if ($runs.ContainsKey($n)) { $runs[$n] } else { $null }
        [pscustomobject]@{ Name = $n; Status = $(if ($c) { $c.status } else { 'missing' }); Conclusion = $(if ($c) { $c.conclusion } else { $null }) }
    }
    $done = @($checks | Where-Object Status -ne 'completed').Count -eq 0
    [pscustomobject]@{ State = $(if ($done) { 'done' } else { 'pending' }); Head = $Head; Checks = @($checks) }
}

function Wait-Until {
    param([scriptblock]$Probe, [scriptblock]$Done, [int]$TimeoutMin)
    $deadline = [datetime]::UtcNow.AddMinutes([Math]::Min([Math]::Max($TimeoutMin, 0), 9))
    while ($true) {
        $r = & $Probe
        if ((& $Done $r) -or [datetime]::UtcNow -ge $deadline) { return $r }
        Start-Sleep -Seconds $PollSeconds
    }
}

<# 全仓扫一遍：since 之后有动静的 PR 与 issue（含已合并、已关闭），列出我方还没回应的评论。 #>
function Get-SweepReport {
    param([string]$Since)
    $repo = Get-RinneRepo
    $issues = Get-RinneApi "repos/$repo/issues?state=all&since=$([uri]::EscapeDataString($Since))&per_page=100" -Paginate
    $out = foreach ($is in @($issues)) {
        $isPr = [bool](Get-RinneProp $is 'pull_request')
        if ($isPr) {
            $activity = Get-RinnePrActivity ([int]$is.number)
            $state = Get-RinneReviewState $activity
        } else {
            $activity = [pscustomobject]@{ Issue = Get-RinneApi "repos/$repo/issues/$($is.number)/comments?per_page=100" -Paginate; Review = @(); Reviews = @(); Threads = @() }
            $state = $null
        }
        $items = @(Get-RinneCommentItems -Activity $activity -State $state -Since $Since)
        foreach ($i in @($items | Where-Object IsNew)) {
            if ($i.Kind -eq 'external' -or -not (Test-RinneResponded -Item $i -Items $items -State $state)) {
                [pscustomobject]@{ Number = [int]$is.number; IsPr = $isPr; State = $is.state; Source = $i.Source; Id = $i.Id; Author = $i.Author; Kind = $i.Kind; Updated = $i.Updated; Url = $i.Url; Body = $i.Body }
            }
        }
    }
    [pscustomobject]@{ Since = $Since; Unanswered = @($out) }
}

# ---- 写 -------------------------------------------------------------------------------------

function Read-Body {
    param([string]$BodyFile)
    if (-not $BodyFile -or -not (Test-Path -LiteralPath $BodyFile)) { throw '需要 -BodyFile（UTF-8 文本）' }
    $text = [IO.File]::ReadAllText($BodyFile, [Text.UTF8Encoding]::new($false))
    if (-not $text.Trim()) { throw '正文为空' }
    $text
}

function Assert-WriteArgs {
    param([string]$Role, [int]$Round, [string]$Head)
    if (-not $Role -or $Round -lt 0 -or $Head -notmatch '^[0-9a-f]{7,40}$') { throw '写评论需要 -Role、-Round 与 -Head（标记里的 head 是当前 head）' }
}

function Invoke-Post {
    param([int]$Pr, [string]$Text, [string]$Role, [string]$Kind, [int]$Round, [string]$Head, [string]$BotRun)
    $extra = $null
    if ($Kind -eq 'disposition') {
        if ($BotRun -notmatch '^\d+$') { throw '处置汇总必须用 -BotRun 引用本轮 bot 运行' }
        $extra = [ordered]@{ 'bot-run' = $BotRun }
    }
    $body = Format-AgentText -Text $Text -Role $Role -Kind $Kind -Round $Round -Head $Head -Extra $extra
    $r = Invoke-RinneApiWrite -Method POST -Path "repos/$(Get-RinneRepo)/issues/$Pr/comments" -Body @{ body = $body }
    Write-Output "已发评论 $($r.html_url)"
}

function Invoke-Reply {
    param([int]$Pr, [string]$To, [string]$Text, [string]$Role, [int]$Round, [string]$Head)
    if ($To -notmatch '^\d+$') { throw '-To 是评论的数字编号' }
    $repo = Get-RinneRepo
    $rc = Get-RinneApi "repos/$repo/pulls/comments/$To" -AllowFail
    if ($rc) {
        $body = Format-AgentText -Text $Text -Role $Role -Kind 'reply' -Round $Round -Head $Head
        $r = Invoke-RinneApiWrite -Method POST -Path "repos/$repo/pulls/$Pr/comments/$To/replies" -Body @{ body = $body }
        return "已在线程里回复 $($r.html_url)"
    }
    $ic = Get-RinneApi "repos/$repo/issues/comments/$To" -AllowFail
    $url = if ($ic) { $ic.html_url } else {
        $all = Get-RinneApi "repos/$repo/pulls/$Pr/reviews?per_page=100" -Paginate
        $rv = $all | Where-Object { [string]$_.id -eq $To } | Select-Object -First 1
        if (-not $rv) { throw "#$Pr 上找不到评论 $To" }
        $rv.html_url
    }
    # 正文开头的链接带着原评论的编号：pr-merge 与 -Sweep 靠它认定「已回应」
    $body = Format-AgentText -Text "> 回复 $url`n`n$Text" -Role $Role -Kind 'reply' -Round $Round -Head $Head
    $r = Invoke-RinneApiWrite -Method POST -Path "repos/$repo/issues/$Pr/comments" -Body @{ body = $body }
    "已回复 $($r.html_url)"
}

$ReplyMutation = 'mutation($thread: ID!, $body: String!) { addPullRequestReviewThreadReply(input: { pullRequestReviewThreadId: $thread, body: $body }) { comment { url } } }'
$ResolveMutation = 'mutation($thread: ID!) { resolveReviewThread(input: { threadId: $thread }) { thread { id isResolved } } }'

function Invoke-Resolve {
    param([string]$Thread, [string]$Text, [string]$Role, [int]$Round, [string]$Head)
    if ($Thread -notmatch '^[A-Za-z0-9_=-]+$') { throw '-Thread 是 reviewThreads 的节点 id' }
    $body = Format-AgentText -Text $Text -Role $Role -Kind 'reply' -Round $Round -Head $Head
    $r = Invoke-RinneGraphQL -Query $ReplyMutation -Variables @{ thread = $Thread; body = $body }
    $done = Invoke-RinneGraphQL -Query $ResolveMutation -Variables @{ thread = $Thread }
    if (-not $done.data.resolveReviewThread.thread.isResolved) { throw "线程 $Thread 没有关上" }
    "已回复并关闭线程 $($r.data.addPullRequestReviewThreadReply.comment.url)"
}

# ---- 自测 -----------------------------------------------------------------------------------

function Invoke-SelfTest {
    $cases = New-RinneCases
    $oldPriv = $env:PRIVACY_NEEDLES
    $oldToken = $env:GH_TOKEN
    try {
        $env:PRIVACY_NEEDLES = 'rinne-selftest-fake-needle-7c1e'
        Set-RinneNeedlesMode 'ci'

        # 身份：进程内 GH_TOKEN 取维护者账号的 token 并核对 login；从不切换 gh 的全局账号
        $id = @{ Token = 'tok-selftest'; Login = 'DLsnows' }
        $idLog = [System.Collections.Generic.List[string]]::new()
        Set-RinneFake (New-RinneFake -Log $idLog -Routes ([ordered]@{
                    '^gh auth token --user DLsnows$' = { @{ Code = $(if ($id.Token) { 0 } else { 1 }); Out = $id.Token; Err = '' } }
                    '^gh api user --jq \.login$'     = { $id.Login }
                }))
        Initialize-RinneIdentity
        Add-RinneCase $cases '身份：GH_TOKEN 取自维护者账号且 login 核对通过' $false ($env:GH_TOKEN -ne 'tok-selftest')
        $id.Login = 'someone-else'
        Add-RinneCase $cases '身份：token 解析成别的账号' $true (Test-Throws { Initialize-RinneIdentity })
        $id.Token = ''
        Add-RinneCase $cases '身份：取不到维护者账号的 token' $true (Test-Throws { Initialize-RinneIdentity })
        Add-RinneCase $cases '身份：从不 gh auth switch / login' $false (@($idLog | Where-Object { $_ -match '^gh auth (switch|login)' }).Count -gt 0)
        Add-RinneCase $cases '标记：格式与 head 截成 7 位' $false ((New-AgentMarker -Role orchestrator -Kind reply -Round 3 -Head ('f' * 40)) -ne '<!-- rinne-agent v1 role=orchestrator kind=reply round=3 head=fffffff -->')
        Add-RinneCase $cases '标记：kind 不合法' $true (Test-Throws { New-AgentMarker -Role review -Kind 'Bad Kind' -Round 1 -Head ('f' * 40) })
        Clear-RinnePermCache
        $issue = Read-RinneFixture 'pr3-issue-comments.json'
        $pull = Read-RinneFixture 'pr3-pull.json'
        $head = $pull.head.sha
        $posted = [System.Collections.Generic.List[object]]::new()
        $log = [System.Collections.Generic.List[string]]::new()
        $checks = Read-RinneFixture 'pr3-check-runs.json'
        $routes = [ordered]@{
            'collaborators/DLsnows/permission' = { 'admin' }
            'collaborators/ghost/permission'   = { @{ Code = 1; Out = ''; Err = 'gh: Not Found (HTTP 404)' } }
            'collaborators/flaky/permission'   = { @{ Code = 1; Out = ''; Err = 'gh: Bad Gateway (HTTP 502)' } }
            'collaborators/[^/]+/permission'   = { @{ Code = 0; Out = 'read'; Err = '' } }
            'pulls/comments/4244522703'        = { (Read-RinneFixture 'pr3-review-comments.json')[0] }
            'pulls/comments/\d+$'              = { @{ Code = 1; Out = ''; Err = 'Not Found' } }
            'issues/comments/6117635288'       = { $issue[1] }
            'api -X POST \S+/(comments|replies) --input' = { param($argv) $posted.Add((Get-Content -Raw ($argv[-1]) | ConvertFrom-Json)); @{ html_url = 'https://github.com/x/y/pull/3#c' } }
            'pulls/3/comments'                 = { Read-RinneFixture 'pr3-review-comments.json' }
            'pulls/3/reviews'                  = { Read-RinneFixture 'pr3-reviews.json' }
            'pulls/3$'                         = { $pull }
            'issues/3/comments'                = { $issue }
            'api -X POST graphql'              = {
                param($argv)
                $q = (Get-Content -Raw ($argv[-1]) | ConvertFrom-Json)
                $posted.Add($q)
                if ($q.query -match 'resolveReviewThread') { return @{ data = @{ resolveReviewThread = @{ thread = @{ id = 'T'; isResolved = $true } } } } }
                if ($q.query -match 'addPullRequestReviewThreadReply') { return @{ data = @{ addPullRequestReviewThreadReply = @{ comment = @{ url = 'u' } } } } }
                Read-RinneFixture 'pr3-threads.json'
            }
            'check-runs'                       = { $checks }
            'repos/\S+/issues\?state=all'      = { @([pscustomobject]@{ number = 3; state = 'closed'; pull_request = @{ url = 'x' } }) }
        }
        Set-RinneFake (New-RinneFake -Routes $routes -Log $log)
        Add-RinneCase $cases '权限：维护者有写权限' $false (-not (Test-RepoWriter 'DLsnows'))
        Add-RinneCase $cases '权限：只读的人不算' $true (-not (Test-RepoWriter 'someone'))
        Add-RinneCase $cases '权限：查无此人（404）不算' $true (-not (Test-RepoWriter 'ghost'))
        Add-RinneCase $cases '权限：接口失败不静默当成没权限' $true (Test-Throws { Test-RepoWriter 'flaky' })

        $rep = Get-ListReport 3 '2026-10-12T04:20:00Z'
        Add-RinneCase $cases '-List 四个来源都在' $false (-not ((@($rep.Items | Where-Object Source -eq 'issue').Count -eq 6) -and (@($rep.Items | Where-Object Source -eq 'review-comment').Count -eq 16)))
        Add-RinneCase $cases '-List 维护者的标记评论认作 agent（按权限，不看 association）' $false (@($rep.Items | Where-Object { $_.Author -eq 'DLsnows' -and $_.Kind -ne 'agent' }).Count -gt 0)
        Add-RinneCase $cases '-List 按 updated_at 判新' $false (-not (@($rep.Items | Where-Object IsNew).Count -gt 0 -and @($rep.Items | Where-Object { -not $_.IsNew }).Count -gt 0))
        Add-RinneCase $cases '-List 列出 bot 总结里的逐条意见' $false (@($rep.BotFindings).Count -ne 8)
        Add-RinneCase $cases '-List 已全部回应、线程全部关闭' $false ($rep.Unanswered -ne 0 -or $rep.OpenThreads -ne 0)

        $f = Get-FooterResult 3 $head
        Add-RinneCase $cases '-Footer 解析尾注' $false (-not ($f.Ok -and $f.Red -eq 0 -and $f.Important -eq 0 -and $f.Suggest -eq 2))
        Add-RinneCase $cases '-Footer 审的不是该 head' $true (-not (Get-FooterResult 3 ('a' * 40)).Ok)
        $last = $issue[4]
        $orig = $last.body
        $last.body = "$orig`n<!-- rinne-review v1 red=0 important=0 suggest=2 -->"
        Add-RinneCase $cases '-Footer 尾注多于一行 → 不可用' $true (-not (Get-FooterResult 3 $head).Ok)
        $last.body = ($orig -split "`n" | Select-Object -SkipLast 1) -join "`n"
        Add-RinneCase $cases '-Footer 缺尾注 → 不可用' $true (-not (Get-FooterResult 3 $head).Ok)
        $last.body = $orig -replace 'suggest=2 -->$', 'suggest=5 -->'
        Add-RinneCase $cases '-Footer 条数与正文不符 → 不可用' $true (-not (Get-FooterResult 3 $head).Ok)
        $last.body = $orig

        $w = Get-WaitState 3 $head
        Add-RinneCase $cases '-Wait 全部 check 已完成' $false ($w.State -ne 'done')
        Add-RinneCase $cases '-Wait head 变了' $true ((Get-WaitState 3 ('b' * 40)).State -eq 'moved')
        $checks.check_runs[0].status = 'in_progress'
        $checks.check_runs = @($checks.check_runs | Where-Object name -ne 'claude-pr-review')
        Add-RinneCase $cases '-Wait bot 还没跑完' $true ((Get-WaitState 3 $head).State -eq 'pending')

        $sw = Get-SweepReport '2026-10-12T04:00:00Z'
        Add-RinneCase $cases '-Sweep 已合并的 PR 也扫，已回应的不列' $false (@($sw.Unanswered).Count -ne 0 -or -not ($log | Where-Object { $_ -match 'issues\?state=all' }))
        $issue += [pscustomobject]@{ id = 7000000001; user = @{ login = 'someone'; type = 'User' }; author_association = 'NONE'; created_at = '2026-10-12T06:00:00Z'; updated_at = '2026-10-12T06:00:00Z'; body = '合并后发现一个问题'; html_url = 'u' }
        $sw = Get-SweepReport '2026-10-12T04:00:00Z'
        Add-RinneCase $cases '-Sweep 列出合并后的外部评论' $true (@($sw.Unanswered | Where-Object Id -eq '7000000001').Count -eq 1)

        Invoke-Post -Pr 3 -Text "## 评审处置汇总`n全部采纳。" -Role review -Kind disposition -Round 2 -Head $head -BotRun '38203700274' | Out-Null
        $b = $posted[-1].body
        Add-RinneCase $cases '-Post 首行自动加 agent 标记（含 bot-run）' $false (-not ($b -match '^<!-- rinne-agent v1 role=review kind=disposition round=2 head=f35523c bot-run=38203700274 -->\n'))
        Add-RinneCase $cases '-Post 处置汇总不带 -BotRun' $true (Test-Throws { Invoke-Post -Pr 3 -Text 'x' -Role review -Kind disposition -Round 2 -Head $head })
        Add-RinneCase $cases '-Post 评论里有 C:\Users\' $true (Test-Throws { Invoke-Post -Pr 3 -Text ('日志见 C:' + '\Users\someone\a.log') -Role review -Kind note -Round 1 -Head $head })
        Add-RinneCase $cases '-Post 评论里有 Co-Authored-By' $true (Test-Throws { Invoke-Post -Pr 3 -Text "ok`nCo-Authored-By: a <a@b.c>" -Role review -Kind note -Round 1 -Head $head })
        Add-RinneCase $cases '-Post 评论里有卡号' $true (Test-Throws { Invoke-Post -Pr 3 -Text ('见 RN' + '-02b') -Role review -Kind note -Round 1 -Head $head })
        Add-RinneCase $cases '-Post 评论里有个人针' $true (Test-Throws { Invoke-Post -Pr 3 -Text 'rinne-selftest-fake-needle-7c1e' -Role review -Kind note -Round 1 -Head $head })
        Add-RinneCase $cases '-Post 正文自带 rinne 标记' $true (Test-Throws { Invoke-Post -Pr 3 -Text '<!-- rinne-review v1 red=0 important=0 suggest=0 -->' -Role review -Kind note -Round 1 -Head $head })
        Add-RinneCase $cases '-Post 角色不合法' $true (Test-Throws { Invoke-Post -Pr 3 -Text 'x' -Role user -Kind note -Round 1 -Head $head })

        $n = $posted.Count
        Invoke-Reply -Pr 3 -To '4244522703' -Text '已修复。' -Role review -Round 2 -Head $head | Out-Null
        Add-RinneCase $cases '-Reply review comment 在原线程里回' $false (-not ($posted.Count -eq $n + 1 -and ($log[-1] -match 'pulls/3/comments/4244522703/replies')))
        Invoke-Reply -Pr 3 -To '6117635288' -Text '收到。' -Role review -Round 2 -Head $head | Out-Null
        Add-RinneCase $cases '-Reply issue comment 另发一条并链接原评论' $false (-not ($posted[-1].body -match "kind=reply" -and $posted[-1].body.Contains('6117635288')))
        Invoke-Resolve -Thread 'PRRT_kwDOtest' -Text '已修复。' -Role review -Round 2 -Head $head | Out-Null
        $mut = @($posted | Select-Object -Last 2)
        Add-RinneCase $cases '-Resolve 先回复再 resolveReviewThread' $false (-not ($mut[0].query -match 'addPullRequestReviewThreadReply' -and $mut[1].query -match 'resolveReviewThread' -and $mut[0].variables.body -match '^<!-- rinne-agent v1 role=review kind=reply'))
        Add-RinneCase $cases '-Resolve 线程 id 不合法' $true (Test-Throws { Invoke-Resolve -Thread 'x y' -Text 'a' -Role review -Round 2 -Head $head })
    } finally {
        Set-RinneFake $null
        Set-RinneNeedlesMode 'local'
        $env:PRIVACY_NEEDLES = $oldPriv
        $env:GH_TOKEN = $oldToken
    }
    Complete-RinneSelfTest $cases 'pr-comment'
}

# ---- 入口 -----------------------------------------------------------------------------------

if ($SelfTest) { exit (Invoke-SelfTest) }
$modes = @($List, $Footer, $Wait, $WaitMerged, $Sweep, $Post, $Reply, $Resolve | Where-Object { $_ })
if ($modes.Count -ne 1) { throw '用法见文件头：-List | -Footer | -Wait | -WaitMerged | -Sweep | -Post | -Reply | -Resolve，选一个' }
if (-not $Sweep -and $Pr -le 0) { throw '需要 -Pr <n>' }
Initialize-RinneIdentity

if ($List) {
    $rep = Get-ListReport $Pr $Since
    if ($Json) { Write-Output (ConvertTo-RinneJsonText $rep); exit 0 }
    foreach ($i in $rep.Items) {
        $flag = @($(if ($i.IsNew) { '新' }), $(if (-not $i.Responded) { '未回应' }), $(if ($i.Kind -eq 'external') { '外部' })) | Where-Object { $_ }
        Write-Output ("{0,-14} {1,-12} {2,-16} {3,-11} {4} {5}" -f $i.Source, $i.Id, $i.Author, $i.Kind, $i.Updated, ($flag -join ' '))
    }
    foreach ($f in $rep.BotFindings) { Write-Output "  bot 意见（运行 $($f.Run)）【$($f.Level)】$($f.Text)" }
    Write-Output "未关线程 $($rep.OpenThreads)，未回应 $($rep.Unanswered)，外部评论 $($rep.External)"
    exit 0
}
if ($Footer) {
    $f = Get-FooterResult $Pr $Head
    if (-not $f.Ok) { Write-Output "bot 结论不可用：$($f.Reason)"; exit 2 }
    Write-Output "red=$($f.Red) important=$($f.Important) suggest=$($f.Suggest) run=$($f.Run)"
    exit 0
}
if ($Wait) {
    if ($Head -notmatch '^[0-9a-f]{40}$') { throw '-Wait 需要 40 位的 -Head' }
    $r = Wait-Until { Get-WaitState $Pr $Head } { param($s) $s.State -ne 'pending' } $TimeoutMin
    Write-Output (ConvertTo-RinneJsonText $r)
    exit $(switch ($r.State) { 'done' { 0 } 'moved' { 4 } default { 3 } })
}
if ($WaitMerged) {
    $r = Wait-Until { Get-RinneApi "repos/$(Get-RinneRepo)/pulls/$Pr" } { param($p) [bool]$p.merged -or $p.state -eq 'closed' } $TimeoutMin
    Write-Output (ConvertTo-RinneJsonText @{ merged = [bool]$r.merged; state = $r.state; mergeCommit = $r.merge_commit_sha })
    exit $(if ($r.merged) { 0 } else { 3 })
}
if ($Sweep) {
    if (-not $Since) { throw '-Sweep 需要 -Since <ISO 时间>' }
    Write-Output (ConvertTo-RinneJsonText (Get-SweepReport $Since))
    exit 0
}
$text = Read-Body $BodyFile
Assert-WriteArgs $Role $Round $Head
if ($Post) {
    if (-not $Kind) { throw '-Post 需要 -Kind（例如 disposition、note、freeze）' }
    Invoke-Post -Pr $Pr -Text $text -Role $Role -Kind $Kind -Round $Round -Head $Head -BotRun $BotRun
} elseif ($Reply) {
    Write-Output (Invoke-Reply -Pr $Pr -To $To -Text $text -Role $Role -Round $Round -Head $Head)
} else {
    Write-Output (Invoke-Resolve -Thread $Thread -Text $text -Role $Role -Round $Round -Head $Head)
}
