# SPDX-License-Identifier: GPL-3.0-or-later
#
# 维护者包装脚本（scripts/maintainer/*.ps1）的公共模块：身份、外部命令入口、公开文本关、agent 隐藏标记、
# 按仓库权限认人、路径通配与基础设施路径、卡片清单、git 锁、自测框架。
# - GitHub 写操作只经这些脚本；身份固定为维护者账号：进程内 GH_TOKEN = gh auth token --user <维护者>，并核对 login，
#   从不 gh auth switch / login。
# - 本机配置只从环境变量或 git config 读（与 hooks 相同），脚本里没有任何本机路径：
#   RINNE_ROOT / rinne.root（工作区根，含 wt/、_logs/、plan/）、RINNE_NODE / rinne.node、
#   RINNE_CARDS / rinne.cards（卡片清单 JSON：每张卡的 owns、takesOver、sections、tier 等）、
#   RINNE_WAIVERS / rinne.waivers（用户豁免记录 JSON，没配置就是没有豁免）、
#   RINNE_GITLEAKS / rinne.gitleaks；个人针由 Node 判据自己读（RINNE_PRIVACY_LOCAL / rinne.privacyLocal）。
# - 判据不在这里重写：隐私、提交消息、尾注与处置汇总都经 lib/maint.mjs 调用仓内已有的 Node 判据。
# - gh、git、node 都经 Invoke-RinneExternal 一处调用；-SelfTest 用 Set-RinneFake 换成读 fixture 的假实现。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$script:Repo = 'synchain-oss/rinne'
$script:Maintainer = 'DLsnows'
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$script:Fake = $null
$script:LockImpl = $null
$script:PermCache = @{}
$script:NeedlesMode = 'local'

function Get-RinneRepo { $script:Repo }
function Get-RinneMaintainer { $script:Maintainer }
function Get-RinneRepoRoot { $script:RepoRoot }

# ---- 本机配置 -------------------------------------------------------------------------------

function Get-RinneSetting {
    param([Parameter(Mandatory)][string]$Env, [Parameter(Mandatory)][string]$GitKey)
    $v = [Environment]::GetEnvironmentVariable($Env)
    if ($v) { return $v }
    $r = Invoke-RinneExternal git @('-C', $script:RepoRoot, 'config', '--get', $GitKey) -AllowFail
    if ($r.Code -eq 0 -and $r.Out.Trim()) { return $r.Out.Trim() }
    return $null
}

function Get-RinneRoot {
    $root = Get-RinneSetting -Env 'RINNE_ROOT' -GitKey 'rinne.root'
    if (-not $root) { throw '未配置工作区根：设置 RINNE_ROOT 或 git config rinne.root' }
    return [IO.Path]::GetFullPath($root)
}

# ---- 外部命令 -------------------------------------------------------------------------------

<#
  运行 gh / git / node，返回 @{ Code; Out; Err }。自测时 $script:Fake 先接手：返回 $null 表示放行给真实命令
  （Node 判据在自测里照常真跑）。非零退出码默认抛错，-AllowFail 时原样返回。
#>
function Invoke-RinneExternal {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @(), [string]$Cwd, [switch]$AllowFail)
    $r = $null
    if ($script:Fake) { $r = & $script:Fake $Exe $Arguments }
    if ($null -eq $r) {
        $errFile = [IO.Path]::GetTempFileName()
        $pushed = $false
        try {
            if ($Cwd) { Push-Location -LiteralPath $Cwd; $pushed = $true }
            $out = & $Exe @Arguments 2> $errFile
            $code = $LASTEXITCODE
        } finally {
            if ($pushed) { Pop-Location }
        }
        $r = @{ Code = $code; Out = (@($out) -join "`n"); Err = [IO.File]::ReadAllText($errFile) }
        Remove-Item -LiteralPath $errFile -ErrorAction SilentlyContinue
    }
    if ($r.Code -ne 0 -and -not $AllowFail) {
        $first = (($r.Err -split "`n") | Where-Object { $_.Trim() } | Select-Object -First 1)
        throw "$Exe $(@($Arguments | Select-Object -First 2) -join ' ') 失败（退出码 $($r.Code)）：$first"
    }
    return $r
}

function Invoke-RinneGh {
    param([Parameter(Mandatory)][string[]]$Arguments, [switch]$AllowFail)
    Invoke-RinneExternal gh $Arguments -AllowFail:$AllowFail
}

function Invoke-RinneGit {
    param([Parameter(Mandatory)][string[]]$Arguments, [string]$Repo = $script:RepoRoot, [switch]$AllowFail)
    Invoke-RinneExternal git (@('-C', $Repo) + $Arguments) -AllowFail:$AllowFail
}

<# REST GET。-Paginate 时用 --slurp 收齐各页再摊平成一个数组；取不到且 -AllowFail 时返回 $null。 #>
function Get-RinneApi {
    param([Parameter(Mandatory)][string]$Path, [switch]$Paginate, [switch]$AllowFail)
    $argv = @('api') + $(if ($Paginate) { @('--paginate', '--slurp') } else { @() }) + @($Path)
    $r = Invoke-RinneGh $argv -AllowFail:$AllowFail
    if ($r.Code -ne 0) { return $null }
    if ($Paginate) { return (ConvertFrom-Json -InputObject $r.Out -Depth 100 -DateKind String -NoEnumerate | ForEach-Object { $_ } | ForEach-Object { $_ }) }
    return ConvertFrom-Json -InputObject $r.Out -Depth 100 -DateKind String
}

<# 接口里的时间（ISO 8601，带 Z 或偏移）统一换成 UTC 再比较；JSON 里的时间保持字符串（-DateKind String）。 #>
function ConvertTo-RinneUtc {
    param([Parameter(Mandatory)][string]$Value)
    [datetimeoffset]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture).UtcDateTime
}

<# StrictMode 下读可能不存在的属性。 #>
function Get-RinneProp {
    param($Object, [Parameter(Mandatory)][string]$Name)
    if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $null
}

<# 写操作：请求体写进临时 JSON 文件经 --input 交给 gh，正文不经过命令行参数。 #>
function Invoke-RinneApiWrite {
    param([Parameter(Mandatory)][string]$Method, [Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][hashtable]$Body)
    $file = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($file, (ConvertTo-Json -InputObject $Body -Depth 20 -Compress), [Text.UTF8Encoding]::new($false))
        $r = Invoke-RinneGh @('api', '-X', $Method, $Path, '--input', $file)
        if ($r.Out.Trim()) { return ConvertFrom-Json -InputObject $r.Out -Depth 100 -DateKind String }
        return $null
    } finally {
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    }
}

function Invoke-RinneGraphQL {
    param([Parameter(Mandatory)][string]$Query, [hashtable]$Variables = @{})
    $data = Invoke-RinneApiWrite -Method POST -Path 'graphql' -Body @{ query = $Query; variables = $Variables }
    if ($data.PSObject.Properties.Name -contains 'errors' -and $data.errors) {
        throw "GraphQL 出错：$(@($data.errors | ForEach-Object { $_.message }) -join '；')"
    }
    return $data
}

function Get-RinneNode {
    $n = Get-RinneSetting -Env 'RINNE_NODE' -GitKey 'rinne.node'
    if ($n) { return $n }
    return 'node'
}

<# 跑仓内的 Node 脚本（仓根相对路径），在仓根下执行：git config 的本机设置按仓库找。 #>
function Invoke-RinneNodeScript {
    param([Parameter(Mandatory)][string]$Script, [string[]]$Arguments = @(), [switch]$AllowFail)
    $path = Join-Path $script:RepoRoot $Script
    Invoke-RinneExternal (Get-RinneNode) (@($path) + $Arguments) -Cwd $script:RepoRoot -AllowFail:$AllowFail
}

<# lib/maint.mjs 的一个子命令，输入写进临时文件，返回解析后的 JSON。 #>
function Invoke-RinneMaint {
    param([Parameter(Mandatory)][string]$Command, [Parameter(Mandatory)][AllowEmptyString()][string]$Content, [string[]]$Extra = @())
    $file = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($file, $Content, [Text.UTF8Encoding]::new($false))
        $r = Invoke-RinneNodeScript 'scripts/maintainer/lib/maint.mjs' (@($Command, '--file', $file) + $Extra) -AllowFail
        $last = (($r.Out -split "`n") | Where-Object { $_.Trim() } | Select-Object -Last 1)
        if (-not $last) { throw "maint.mjs $Command 没有输出：$($r.Err.Trim())" }
        $v = ConvertFrom-Json -InputObject $last -Depth 100 -DateKind String
        if ($v.PSObject.Properties.Name -contains 'error') { throw "maint.mjs $Command：$($v.error)" }
        return $v
    } finally {
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    }
}

function ConvertTo-RinneJsonText {
    param($Value)
    ConvertTo-Json -InputObject $Value -Depth 100 -Compress
}

# ---- 身份 -----------------------------------------------------------------------------------

<# 进程内 GH_TOKEN 取维护者账号的 token，并核对它确实解析为维护者；不切换 gh 的全局账号。 #>
function Initialize-RinneIdentity {
    $r = Invoke-RinneGh @('auth', 'token', '--user', $script:Maintainer) -AllowFail
    $token = $r.Out.Trim()
    if ($r.Code -ne 0 -or -not $token) { throw "取不到 $($script:Maintainer) 的 gh 凭据（先在本机 gh auth login 该账号）" }
    $env:GH_TOKEN = $token
    $login = (Invoke-RinneGh @('api', 'user', '--jq', '.login')).Out.Trim()
    if ($login -ne $script:Maintainer) { throw "gh 凭据解析为 $login，不是 $($script:Maintainer)" }
}

# ---- 公开文本 -------------------------------------------------------------------------------

function Set-RinneNeedlesMode { param([ValidateSet('local', 'ci', 'ci-optional')][string]$Mode) $script:NeedlesMode = $Mode }

<# 通用规则、个人针、AI 署名、卡号：命中只返回打码后的片段。 #>
function Test-PublicText {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $v = Invoke-RinneMaint -Command 'text' -Content $Text -Extra @('--needles', $script:NeedlesMode)
    $findings = @($v.findings)
    [pscustomobject]@{ Ok = ($findings.Count -eq 0); Findings = $findings }
}

function Assert-PublicText {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text, [string]$Label = '文本')
    $t = Test-PublicText $Text
    if (-not $t.Ok) {
        $list = @($t.Findings | ForEach-Object { "第 $($_.line) 行 [$($_.rule)] $($_.text)" }) -join '；'
        throw "$Label 没过公开文本检查，不发出：$list"
    }
}

# ---- agent 隐藏标记 -------------------------------------------------------------------------

$script:MarkerPattern = '^<!-- rinne-agent v1 role=(impl|review|orchestrator) kind=([a-z][a-z-]*) round=(\d+) head=([0-9a-f]{7,40})((?: [a-z-]+=[^\s>]+)*) -->$'

<# <!-- rinne-agent v1 role=<impl|review|orchestrator> kind=<…> round=<n> head=<sha7>[ 键=值 …] --> #>
function New-AgentMarker {
    param(
        [Parameter(Mandatory)][ValidateSet('impl', 'review', 'orchestrator')][string]$Role,
        [Parameter(Mandatory)][ValidatePattern('^[a-z][a-z-]*$')][string]$Kind,
        [Parameter(Mandatory)][ValidateRange(0, 99)][int]$Round,
        [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{7,40}$')][string]$Head,
        [System.Collections.Specialized.OrderedDictionary]$Extra
    )
    $tail = ''
    if ($Extra) {
        foreach ($k in $Extra.Keys) {
            $val = [string]$Extra[$k]
            if ($k -notmatch '^[a-z][a-z-]*$' -or $val -notmatch '^[^\s>]+$') { throw "标记字段不合法：$k=$val" }
            $tail += " $k=$val"
        }
    }
    "<!-- rinne-agent v1 role=$Role kind=$Kind round=$Round head=$($Head.Substring(0, 7))$tail -->"
}

function Get-AgentMarker {
    param([AllowEmptyString()][AllowNull()][string]$Body)
    $first = (($Body ?? '') -split "`r?`n", 2)[0]
    $m = [regex]::Match($first, $script:MarkerPattern)
    if (-not $m.Success) { return $null }
    [pscustomobject]@{ Role = $m.Groups[1].Value; Kind = $m.Groups[2].Value; Round = [int]$m.Groups[3].Value; Head = $m.Groups[4].Value }
}

<# agent 发出的文本：首行加隐藏标记，整段过公开文本检查。调用方自己写了标记的拒收，免得标记与身份对不上。 #>
function Format-AgentText {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$Role, [Parameter(Mandatory)][string]$Kind,
        [Parameter(Mandatory)][int]$Round, [Parameter(Mandatory)][string]$Head,
        [System.Collections.Specialized.OrderedDictionary]$Extra, [string]$Label = '评论'
    )
    if ($Text -match '<!--\s*rinne-') { throw "$Label 里不要自己写 rinne- 隐藏标记，由脚本统一加" }
    $marker = New-AgentMarker -Role $Role -Kind $Kind -Round $Round -Head $Head -Extra $Extra
    $full = "$marker`n$($Text.TrimEnd())"
    Assert-PublicText $full $Label
    return $full
}

# ---- 按仓库权限认人 -------------------------------------------------------------------------

<#
  对本仓有 write 及以上权限（collaborators/<login>/permission 为 admin 或 write；maintain 也报成 write）。
  不看 author_association：组织成员身份不公开时它是 NONE，维护者会被当成外人。按 login 缓存。
#>
function Test-RepoWriter {
    param([Parameter(Mandatory)][string]$Login)
    if ($Login -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$') { return $false }
    if (-not $script:PermCache.ContainsKey($Login)) {
        $r = Invoke-RinneGh @('api', "repos/$($script:Repo)/collaborators/$Login/permission", '--jq', '.permission') -AllowFail
        # 404 是「查无此人」；限流、5xx、token 缺权限之类的失败不能静默当成「没有写权限」
        if ($r.Code -ne 0 -and $r.Err -notmatch 'HTTP 404|Not Found') { throw "核对 $Login 的仓库权限失败：$(($r.Err -split "`n")[0])" }
        $script:PermCache[$Login] = ($r.Code -eq 0 -and @('admin', 'write') -contains $r.Out.Trim())
    }
    return $script:PermCache[$Login]
}

function Clear-RinnePermCache { $script:PermCache = @{} }

# ---- 路径通配、基础设施路径与卡片清单 -------------------------------------------------------

<# 仓内相对路径的通配：** 跨目录、* 与 ? 不跨 /、{a,b} 多选一。 #>
function ConvertTo-GlobRegex {
    param([Parameter(Mandatory)][string]$Glob)
    $sb = [Text.StringBuilder]::new('^')
    $i = 0
    while ($i -lt $Glob.Length) {
        $c = $Glob[$i]
        if ($c -eq '*' -and $i + 1 -lt $Glob.Length -and $Glob[$i + 1] -eq '*') {
            if ($i + 2 -lt $Glob.Length -and $Glob[$i + 2] -eq '/') { [void]$sb.Append('(?:.*/)?'); $i += 3 }
            else { [void]$sb.Append('.*'); $i += 2 }
            continue
        }
        if ($c -eq '*') { [void]$sb.Append('[^/]*') }
        elseif ($c -eq '?') { [void]$sb.Append('[^/]') }
        elseif ($c -eq '{') {
            $end = $Glob.IndexOf('}', $i)
            if ($end -lt 0) { throw "通配缺 }：$Glob" }
            $alts = $Glob.Substring($i + 1, $end - $i - 1).Split(',') | ForEach-Object { [regex]::Escape($_) }
            [void]$sb.Append("(?:$($alts -join '|'))")
            $i = $end
        } else { [void]$sb.Append([regex]::Escape([string]$c)) }
        $i++
    }
    [void]$sb.Append('$')
    $sb.ToString()
}

function Test-GlobMatch {
    param([Parameter(Mandatory)][string]$Path, [string[]]$Globs = @())
    foreach ($g in $Globs) { if ($Path -match (ConvertTo-GlobRegex $g)) { return $true } }
    return $false
}

<#
  基础设施路径的清单与匹配规则只有一份：scripts/lib/infra-paths.mjs（经 maint.mjs infra 调用）。这里不另写清单：
  评审 bot 跑的是 PR 自己的 review-bot.mjs，削弱评审逻辑的 PR 能给自己判干净，所以碰到这些路径就不自合。
  pr-merge 从主 clone 运行，读到的是 feature 线上的清单，而不是待合并 PR 里的版本。
  自测用 Set-RinneInfraList 换成 fixture 里的一份 glob（匹配规则仍是 infra-paths.mjs 的）。
#>
$script:InfraList = $null

function Set-RinneInfraList { param([string]$Path) $script:InfraList = $Path }

<# files：[{ filename; status; previous_filename? }]。返回碰到的基础设施路径。改名的新旧路径都算。 #>
function Get-InfraTouches {
    param([object[]]$Files = @())
    $paths = @(foreach ($f in $Files) {
            $f.filename
            $prev = Get-RinneProp $f 'previous_filename'
            if ($prev) { $prev }
        }) | Select-Object -Unique
    if (-not $paths) { return }
    $extra = if ($script:InfraList) { @('--list', $script:InfraList) } else { @() }
    $v = Invoke-RinneMaint -Command 'infra' -Content (ConvertTo-RinneJsonText @($paths)) -Extra $extra
    @($v.hits)
}

function Get-RinneCard {
    param([Parameter(Mandatory)][ValidatePattern('^(?:RN-\d{2}[a-z]?|K\d+)$')][string]$Id)
    $path = Get-RinneSetting -Env 'RINNE_CARDS' -GitKey 'rinne.cards'
    if (-not $path -or -not (Test-Path -LiteralPath $path)) { throw '找不到卡片清单：设置 RINNE_CARDS 或 git config rinne.cards' }
    $card = @(Get-Content -Raw -LiteralPath $path | ConvertFrom-Json -Depth 50) | Where-Object { $_.id -eq $Id }
    if (-not $card) { throw "卡片清单里没有 $Id" }
    return $card
}

<# 卡可以改的路径：owns，加 takesOver 与 sections 里的 path。 #>
function Get-CardScope {
    param([Parameter(Mandatory)]$Card)
    $globs = @($Card.owns)
    foreach ($k in 'takesOver', 'sections') {
        if ($Card.PSObject.Properties.Name -contains $k) { $globs += @($Card.$k | ForEach-Object { $_.path }) }
    }
    $globs | Where-Object { $_ }
}

# ---- PR 上的评论：四个来源 -------------------------------------------------------------------

$script:ThreadsQuery = @'
query($owner: String!, $name: String!, $number: Int!, $after: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      reviewThreads(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id isResolved isOutdated path line
          comments(first: 100) { nodes { databaseId author { login } body createdAt updatedAt url } }
        }
      }
    }
  }
}
'@

function Get-RinneReviewThreads {
    param([Parameter(Mandatory)][int]$Pr)
    $owner, $name = $script:Repo.Split('/')
    $nodes = [System.Collections.Generic.List[object]]::new()
    $after = $null
    do {
        $vars = @{ owner = $owner; name = $name; number = $Pr; after = $after }
        $page = (Invoke-RinneGraphQL -Query $script:ThreadsQuery -Variables $vars).data.repository.pullRequest.reviewThreads
        foreach ($n in @($page.nodes)) { $nodes.Add($n) }
        $after = $page.pageInfo.endCursor
    } while ($page.pageInfo.hasNextPage)
    $nodes.ToArray()
}

<# issue comments、review comments、reviews、GraphQL reviewThreads，各取全部。 #>
function Get-RinnePrActivity {
    param([Parameter(Mandatory)][int]$Pr)
    $r = $script:Repo
    [pscustomobject]@{
        Issue   = Get-RinneApi "repos/$r/issues/$Pr/comments?per_page=100" -Paginate
        Review  = Get-RinneApi "repos/$r/pulls/$Pr/comments?per_page=100" -Paginate
        Reviews = Get-RinneApi "repos/$r/pulls/$Pr/reviews?per_page=100" -Paginate
        Threads = Get-RinneReviewThreads $Pr
    }
}

<# 评论里非 bot 作者中对本仓有写权限的 login（交给 maint.mjs review-state 认处置汇总）。 #>
function Get-RinneWriters {
    param([Parameter(Mandatory)]$Activity)
    $logins = @($Activity.Issue) + @($Activity.Review) + @($Activity.Reviews) |
        ForEach-Object { Get-RinneProp (Get-RinneProp $_ 'user') 'login' } |
        Where-Object { $_ -and $_ -notmatch '\[bot\]$' } | Select-Object -Unique
    $logins | Where-Object { Test-RepoWriter $_ }
}

<# 最近一条 bot 总结、全部总结（尾注、运行编号、逐条意见）与处置汇总，判据在 review-bot.mjs。 #>
function Get-RinneReviewState {
    param([Parameter(Mandatory)]$Activity, [string]$Bot = 'claude')
    $writers = Get-RinneWriters $Activity
    $json = ConvertTo-RinneJsonText @($Activity.Issue)
    Invoke-RinneMaint -Command 'review-state' -Content $json -Extra @('--writers', ($writers -join ','), '--bot', $Bot)
}

<#
  把四个来源摊成一张表。Kind：agent（有写权限且首行带 agent 标记）、bot-summary、bot、user（有写权限、没有标记：
  维护者本人的意见）、external（既不是 bot 也没有写权限）。review comment 带上所在线程与是否已 resolve。
  IsNew：updated_at 晚于 Since（追踪评论是原地编辑的，按更新时间判新）。
#>
function Get-RinneCommentItems {
    param([Parameter(Mandatory)]$Activity, $State, [string]$Since)
    $summaryIds = if ($State) { @($State.summaries | ForEach-Object { [string]$_.id }) } else { @() }
    $threadOf = @{}
    foreach ($t in @($Activity.Threads)) {
        foreach ($c in @($t.comments.nodes)) { $threadOf[[string]$c.databaseId] = $t }
    }
    $items = [System.Collections.Generic.List[object]]::new()
    $add = {
        param($source, $c, $created)
        $login = Get-RinneProp (Get-RinneProp $c 'user') 'login'
        $body = [string](Get-RinneProp $c 'body')
        $id = [string]$c.id
        $kind = if ($login -match '\[bot\]$') {
            if ($source -eq 'issue' -and $summaryIds -contains $id) { 'bot-summary' } else { 'bot' }
        } elseif (Test-RepoWriter $login) {
            if (Get-AgentMarker $body) { 'agent' } else { 'user' }
        } else { 'external' }
        $updated = [string](Get-RinneProp $c 'updated_at') ; if (-not $updated) { $updated = $created }
        $thread = if ($source -eq 'review-comment') { $threadOf[$id] } else { $null }
        $items.Add([pscustomobject]@{
                Source = $source; Id = $id; Author = $login; Kind = $kind; Body = $body
                Created = $created; Updated = $updated; Url = [string](Get-RinneProp $c 'html_url')
                IsNew = (-not $Since) -or ((ConvertTo-RinneUtc $updated) -gt (ConvertTo-RinneUtc $Since))
                ThreadId = $(if ($thread) { $thread.id } else { $null })
                Resolved = $(if ($thread) { [bool]$thread.isResolved } else { $null })
                ReplyTo = [string](Get-RinneProp $c 'in_reply_to_id')
            })
    }
    foreach ($c in @($Activity.Issue)) { & $add 'issue' $c ([string]$c.created_at) }
    foreach ($c in @($Activity.Review)) { & $add 'review-comment' $c ([string]$c.created_at) }
    foreach ($c in @($Activity.Reviews)) {
        if ([string](Get-RinneProp $c 'body')) { & $add 'review' $c ([string]$c.submitted_at) }
    }
    $items.ToArray()
}

<#
  一条评论有没有得到我方回应：
  - agent 自己的、非总结的 bot 通知（/review 的回执之类）：不需要；
  - bot 审查总结：之后有处置汇总引用它的运行编号；
  - review comment（bot 或人）：所在线程已 resolve，或线程里有我方之后的回复；
  - issue comment 与 review（人）：之后有我方评论在正文里引用它的编号（回复链接里带编号）。
#>
function Test-RinneResponded {
    param([Parameter(Mandatory)]$Item, [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Items, $State)
    switch ($Item.Kind) {
        'agent' { return $true }
        'bot-summary' {
            $s = @($State.summaries) | Where-Object { [string]$_.id -eq $Item.Id } | Select-Object -First 1
            return [bool]($s -and $s.runId -and (@($State.dispositionRuns) -contains [string]$s.runId))
        }
    }
    if ($Item.Source -eq 'review-comment') {
        if ($Item.Resolved) { return $true }
        $later = @($Items | Where-Object {
                $_.Kind -eq 'agent' -and $_.ThreadId -and $_.ThreadId -eq $Item.ThreadId -and (ConvertTo-RinneUtc $_.Created) -gt (ConvertTo-RinneUtc $Item.Created) })
        return $later.Count -gt 0
    }
    if ($Item.Kind -eq 'bot') { return $true }
    $refs = @($Items | Where-Object {
            $_.Kind -eq 'agent' -and (ConvertTo-RinneUtc $_.Created) -gt (ConvertTo-RinneUtc $Item.Created) -and $_.Body -match "(?<!\d)$($Item.Id)(?!\d)" })
    return $refs.Count -gt 0
}

<# 当前 head 上各个 check 的最新一次结果：name → { status; conclusion; details_url }。 #>
function Get-RinneCheckRuns {
    param([Parameter(Mandatory)][string]$Sha)
    $pages = Get-RinneApi "repos/$($script:Repo)/commits/$Sha/check-runs?per_page=100" -Paginate
    $latest = @{}
    foreach ($c in @($pages | ForEach-Object { $_.check_runs } | ForEach-Object { $_ })) {
        if (-not $latest.ContainsKey($c.name) -or [long]$c.id -gt [long]$latest[$c.name].id) { $latest[$c.name] = $c }
    }
    return $latest
}

<# feature 线的 required checks（.github/required-checks.json 是单一真源）。 #>
function Get-RinneRequiredChecks {
    $f = Join-Path $script:RepoRoot '.github/required-checks.json'
    (Get-Content -Raw -LiteralPath $f | ConvertFrom-Json -Depth 20).'feature-lines'.contexts
}

# ---- git 锁 ---------------------------------------------------------------------------------

<# 所有 fetch / merge / worktree 操作包在本地 gates 提供的 Invoke-WithGitLock 里（scripts/lib/Locks.psm1）。 #>
function Invoke-RinneGitLocked {
    param([Parameter(Mandatory)][scriptblock]$Script)
    if ($script:LockImpl) { return & $script:LockImpl $Script }
    $locks = Join-Path $script:RepoRoot 'scripts/lib/Locks.psm1'
    if (-not (Test-Path -LiteralPath $locks)) { throw '需要 scripts/lib/Locks.psm1 的 Invoke-WithGitLock（本地 gates 入库后才有）' }
    Import-Module $locks -Force -Global
    Invoke-WithGitLock $Script
}

# ---- 自测框架 -------------------------------------------------------------------------------

function Set-RinneFake { param([scriptblock]$Handler) $script:Fake = $Handler }
function Set-RinneLockImpl { param([scriptblock]$Impl) $script:LockImpl = $Impl }

function Read-RinneFixture {
    param([Parameter(Mandatory)][string]$Name)
    $path = Join-Path $PSScriptRoot "../fixtures/$Name"
    , (ConvertFrom-Json -InputObject (Get-Content -Raw -LiteralPath $path) -Depth 100 -DateKind String -NoEnumerate)
}

<# 深拷贝（自测里改一份 fixture 造反例，不影响原件）。数组保持为数组。 #>
function Copy-RinneObject {
    param($Value)
    , (ConvertFrom-Json -InputObject (ConvertTo-Json -InputObject $Value -Depth 100) -Depth 100 -DateKind String -NoEnumerate)
}

<#
  假的外部命令：routes 是有序表，键是对整条命令行（「exe 参数…」）的正则，值是 scriptblock，参数为 argv，
  返回对象（转成 JSON）、字符串或 @{ Code; Out; Err }。没有命中的 node 命令与读本机 git config 的命令放行给真实命令
  （都不联网）；其余没有命中的一律失败，自测里能看出漏录了哪条。log 记录每条命令。
#>
function New-RinneFake {
    param([Parameter(Mandatory)][System.Collections.Specialized.OrderedDictionary]$Routes, [System.Collections.Generic.List[string]]$Log)
    return {
        param($exe, $argv)
        $line = "$exe $($argv -join ' ')"
        if ($null -ne $Log) { $Log.Add($line) }
        foreach ($k in $Routes.Keys) {
            if ($line -match $k) {
                $v = & $Routes[$k] $argv
                if ($v -is [hashtable] -and $v.ContainsKey('Code')) { return $v }
                $out = if ($v -is [string]) { $v } elseif ($null -eq $v) { '' } else { ConvertTo-Json -InputObject $v -Depth 100 -Compress }
                if ($argv -contains '--slurp') { $out = "[$out]" }
                return @{ Code = 0; Out = $out; Err = '' }
            }
        }
        if ($exe -match '(^|[\\/])node(\.exe)?$' -or $line -match '^git -C \S+ config --get rinne\.') { return $null }
        return @{ Code = 1; Out = ''; Err = "fixture 里没有：$line" }
    }.GetNewClosure()
}

function New-RinneCases { , [System.Collections.Generic.List[object]]::new() }

<# flagged：这一格是否被判为有问题。坏样例必须被判出来，正例必须不被判出来。 #>
function Add-RinneCase {
    param([Parameter(Mandatory)]$Cases, [Parameter(Mandatory)][string]$Label, [Parameter(Mandatory)][bool]$Bad, [Parameter(Mandatory)][bool]$Flagged)
    $Cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = $(if ($Bad) { $Flagged } else { -not $Flagged }) })
}

function Complete-RinneSelfTest {
    param([Parameter(Mandatory)]$Cases, [Parameter(Mandatory)][string]$Name)
    $bad = @($Cases | Where-Object Bad).Count
    $good = $Cases.Count - $bad
    $failed = @($Cases | Where-Object { -not $_.Passed })
    foreach ($f in $failed) { [Console]::Error.WriteLine("自测未通过：$($f.Label)") }
    $ok = $failed.Count -eq 0 -and $bad -gt 0 -and $good -gt 0
    [Console]::Out.WriteLine("$Name -SelfTest：$(if ($ok) { '通过' } else { '失败' })（坏样例 $bad、正例 $good）")
    if ($ok) { return 0 } else { return 1 }
}

<# 自测里跑一段代码，抛错算「判出了问题」。 #>
function Test-Throws { param([scriptblock]$Script) try { & $Script | Out-Null; return $false } catch { return $true } }

Export-ModuleMember -Function *-Rinne*, *-AgentMarker, Format-AgentText, Test-PublicText, Assert-PublicText,
    Test-RepoWriter, ConvertTo-GlobRegex, Test-GlobMatch, Get-InfraTouches, Get-CardScope,
    Initialize-RinneIdentity, Test-Throws, Add-RinneCase, Complete-RinneSelfTest, ConvertTo-RinneJsonText
