# SPDX-License-Identifier: GPL-3.0-or-later
#
# 维护者包装脚本（scripts/maintainer/*.ps1）的公共模块：身份、外部命令入口、公开文本关、agent 隐藏标记、
# 按仓库权限认人、路径通配与基础设施路径、卡片清单、git 锁、自测框架。
# - GitHub 写操作只经这些脚本；身份固定为维护者账号：进程内 GH_TOKEN = gh auth token --user <维护者>，并核对 login，
#   从不 gh auth switch / login。
# - 本机配置只从环境变量或 git config 读（与 hooks 相同），脚本里没有任何本机路径：
#   RINNE_ROOT / rinne.root（工作区根，含 wt/、_logs/、plan/）、RINNE_NODE / rinne.node、
#   RINNE_CARDS / rinne.cards（卡片清单 JSON：每张卡的 owns、takesOver、sections、tier 等）、
#   rinne.waivers（用户记录 JSON，没配置就是没有记录；pr-merge 真跑只认主 clone 的 git config，RINNE_WAIVERS 只给自测用）、
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
  运行 gh / git / node，返回 @{ Code; Out; Err }。自测时 $script:Fake 先接手（第三个参数带上 Cwd 与 TimeoutSeconds）：
  返回 $null 表示放行给真实命令（Node 判据在自测里照常真跑）。非零退出码默认抛错，-AllowFail 时原样返回。
  -TimeoutSeconds 大于 0 时到点结束整棵进程树，返回 Code = -1、TimedOut = $true。
#>
function Invoke-RinneExternal {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @(), [string]$Cwd, [switch]$AllowFail, [int]$TimeoutSeconds = 0)
    $r = $null
    if ($script:Fake) { $r = & $script:Fake $Exe $Arguments ([pscustomobject]@{ Cwd = $Cwd; TimeoutSeconds = $TimeoutSeconds }) }
    if ($null -eq $r -and $TimeoutSeconds -gt 0) { $r = Invoke-RinneTimed -Exe $Exe -Arguments $Arguments -Cwd $Cwd -TimeoutSeconds $TimeoutSeconds }
    elseif ($null -eq $r) {
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

<#
  带超时跑一个进程：输出异步读（免得管道写满卡住），到点 Kill 整棵进程树（构建会起编译器子进程）。
  进程退出后读管道最多再等 DrainSeconds：它留下的常驻子进程（编译器的 PDB 服务、编译缓存的服务）继承了管道时，
  EOF 要等它们退出才来，不设上限的话超时就形同虚设。等不到时用已经读到的输出，并在 Err 里注明。
#>
function Invoke-RinneTimed {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @(), [string]$Cwd, [Parameter(Mandatory)][int]$TimeoutSeconds, [int]$DrainSeconds = 10)
    $psi = [System.Diagnostics.ProcessStartInfo]::new($Exe)
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($Cwd) { $psi.WorkingDirectory = $Cwd }
    $p = [System.Diagnostics.Process]::Start($psi)
    try {
        $out = $p.StandardOutput.ReadToEndAsync()
        $err = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            # 恰好在这之间自己退出了，Kill 会抛错；照样按超时算
            try { $p.Kill($true) } catch [InvalidOperationException] { }
            $null = $p.WaitForExit(5000)
            return @{ Code = -1; Out = (Read-RinneDrained $out $DrainSeconds).Text; Err = "超过 $TimeoutSeconds 秒，已结束整棵进程树"; TimedOut = $true }
        }
        $o, $e = (Read-RinneDrained $out $DrainSeconds), (Read-RinneDrained $err $DrainSeconds)
        $note = ($o.Done -and $e.Done) ? '' : "`n（进程已退出，留下的子进程还握着输出管道，$DrainSeconds 秒后不再等）"
        @{ Code = $p.ExitCode; Out = $o.Text.TrimEnd(); Err = $e.Text + $note; TimedOut = $false }
    } finally {
        $p.Dispose()
    }
}

<# 异步读到的文本：最多等 Seconds 秒；没等到 EOF 时返回空串并标 Done = $false。 #>
function Read-RinneDrained {
    param([Parameter(Mandatory)][System.Threading.Tasks.Task[string]]$Task, [int]$Seconds)
    if ($Task.Wait([TimeSpan]::FromSeconds($Seconds))) { return @{ Text = $Task.Result; Done = $true } }
    @{ Text = ''; Done = $false }
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

<# 一次扫多段（同一判据），返回与输入同序的 @{ Ok; Findings }。 #>
function Test-PublicTexts {
    param([AllowEmptyCollection()][string[]]$Texts = @())
    if (-not $Texts.Count) { return }
    $v = Invoke-RinneMaint -Command 'texts' -Content (ConvertTo-RinneJsonText @($Texts)) -Extra @('--needles', $script:NeedlesMode)
    foreach ($r in @($v.results)) { $f = @($r.findings); [pscustomobject]@{ Ok = ($f.Count -eq 0); Findings = $f } }
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
    $extra = @{}
    foreach ($kv in ($m.Groups[5].Value.Trim() -split '\s+' | Where-Object { $_ })) {
        $k, $v = $kv -split '=', 2
        $extra[$k] = $v
    }
    $replyTo = if ($extra.ContainsKey('in-reply-to')) { @($extra['in-reply-to'] -split ',' | Where-Object { $_ }) } else { @() }
    [pscustomobject]@{
        Role = $m.Groups[1].Value; Kind = $m.Groups[2].Value; Round = [int]$m.Groups[3].Value; Head = $m.Groups[4].Value
        Extra = $extra; InReplyTo = $replyTo
    }
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
  Set-RinneInfraList 换一份 glob 清单（匹配规则仍是 infra-paths.mjs 的），自测用它核对「清单取不到时不放行」。
#>
$script:InfraList = $null

function Set-RinneInfraList { param([string]$Path) $script:InfraList = $Path }

<# files：[{ filename; status; previous_filename? }] 涉及的全部路径：改名的新旧路径都算（挪走一个文件也是改了它）。 #>
function Get-RinneChangedPaths {
    param([object[]]$Files = @())
    @(foreach ($f in $Files) {
            $f.filename
            $prev = Get-RinneProp $f 'previous_filename'
            if ($prev) { $prev }
        }) | Select-Object -Unique
}

<# 返回碰到的基础设施路径。 #>
function Get-InfraTouches {
    param([object[]]$Files = @())
    $paths = @(Get-RinneChangedPaths $Files)
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

$script:ReviewEditsQuery = @'
query($owner: String!, $name: String!, $number: Int!, $after: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      reviews(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes { databaseId updatedAt lastEditedAt }
      }
    }
  }
}
'@

<# REST 的 review 对象没有 updated_at：改过的 review 正文要从 GraphQL 的 lastEditedAt / updatedAt 取。返回 id → 两者中较晚的一个。 #>
function Get-RinneReviewEdits {
    param([Parameter(Mandatory)][int]$Pr)
    $owner, $name = $script:Repo.Split('/')
    $edits = @{}
    $after = $null
    do {
        $vars = @{ owner = $owner; name = $name; number = $Pr; after = $after }
        $page = (Invoke-RinneGraphQL -Query $script:ReviewEditsQuery -Variables $vars).data.repository.pullRequest.reviews
        foreach ($n in @($page.nodes)) {
            $times = @($n.updatedAt, (Get-RinneProp $n 'lastEditedAt')) | Where-Object { $_ } | Sort-Object { ConvertTo-RinneUtc $_ }
            $edits[[string]$n.databaseId] = [string]($times | Select-Object -Last 1)
        }
        $after = $page.pageInfo.endCursor
    } while ($page.pageInfo.hasNextPage)
    $edits
}

<#
  issue comments、review comments、reviews、GraphQL reviewThreads，各取全部。review 补上 updated_at：
  提交时间与 GraphQL 修改时间中较晚的一个（回复之后又改过正文的 review 要重新回应）。
#>
function Get-RinnePrActivity {
    param([Parameter(Mandatory)][int]$Pr)
    $r = $script:Repo
    $reviews = @(Get-RinneApi "repos/$r/pulls/$Pr/reviews?per_page=100" -Paginate)
    $edits = Get-RinneReviewEdits $Pr
    foreach ($rv in $reviews) {
        $id = [string]$rv.id
        if (-not $edits.ContainsKey($id)) { throw "GraphQL 里没有 review $id 的修改时间，判不了它改没改过" }
        $updated = @([string]$rv.submitted_at, $edits[$id]) | Where-Object { $_ } | Sort-Object { ConvertTo-RinneUtc $_ } | Select-Object -Last 1
        $rv | Add-Member -NotePropertyName updated_at -NotePropertyValue $updated -Force
    }
    [pscustomobject]@{
        Issue   = Get-RinneApi "repos/$r/issues/$Pr/comments?per_page=100" -Paginate
        Review  = Get-RinneApi "repos/$r/pulls/$Pr/comments?per_page=100" -Paginate
        Reviews = $reviews
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
  维护者本人的意见）、external（既不是 bot 也没有写权限；账号已删除、login 为空的也算）。
  review comment 带上所在线程、是否已 resolve 与所属 review。review 只收 COMMENTED 与 CHANGES_REQUESTED（正文为空的也收：
  inline 评论都挂在一个空正文的 COMMENTED review 下，「Request changes」也可以不写正文）；空正文、只装着我方回复的 review
  算 agent 的。CHANGES_REQUESTED 记下同一作者之后有没有 APPROVED。
  IsNew：updated_at 晚于 Since（追踪评论是原地编辑的，按更新时间判新）。InReplyTo：agent 标记里 in-reply-to 列出的编号。
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
        param($source, $c, $created, $extra)
        $login = [string](Get-RinneProp (Get-RinneProp $c 'user') 'login')
        $body = [string](Get-RinneProp $c 'body')
        $id = [string]$c.id
        $marker = Get-AgentMarker $body
        $kind = if (-not $login) { 'external' }
        elseif ($login -match '\[bot\]$') {
            if ($source -eq 'issue' -and $summaryIds -contains $id) { 'bot-summary' } else { 'bot' }
        } elseif (Test-RepoWriter $login) {
            if ($marker) { 'agent' } else { 'user' }
        } else { 'external' }
        $updated = [string](Get-RinneProp $c 'updated_at') ; if (-not $updated) { $updated = $created }
        $thread = if ($source -eq 'review-comment') { $threadOf[$id] } else { $null }
        $item = [pscustomobject]@{
            Source = $source; Id = $id; Author = $(if ($login) { $login } else { '（已删除的账号）' }); Kind = $kind; Body = $body
            Created = $created; Updated = $updated; Url = [string](Get-RinneProp $c 'html_url')
            IsNew = (-not $Since) -or ((ConvertTo-RinneUtc $updated) -gt (ConvertTo-RinneUtc $Since))
            ThreadId = $(if ($thread) { $thread.id } else { $null })
            Resolved = $(if ($thread) { [bool]$thread.isResolved } else { $null })
            ReviewId = [string](Get-RinneProp $c 'pull_request_review_id')
            InReplyTo = $(if ($marker) { @($marker.InReplyTo) } else { @() })
            State = [string](Get-RinneProp $c 'state'); Comments = @(); ApprovedLater = $false
        }
        if ($extra) { foreach ($k in $extra.Keys) { $item.$k = $extra[$k] } }
        $items.Add($item)
    }
    foreach ($c in @($Activity.Issue)) { & $add 'issue' $c ([string]$c.created_at) $null }
    foreach ($c in @($Activity.Review)) { & $add 'review-comment' $c ([string]$c.created_at) $null }
    $reviews = @($Activity.Reviews)
    foreach ($c in $reviews) {
        $state = [string](Get-RinneProp $c 'state')
        if (@('COMMENTED', 'CHANGES_REQUESTED') -notcontains $state) { continue }
        $rid = [string]$c.id
        $inline = @($items | Where-Object { $_.Source -eq 'review-comment' -and $_.ReviewId -eq $rid })
        $author = [string](Get-RinneProp (Get-RinneProp $c 'user') 'login')
        $approved = $state -eq 'CHANGES_REQUESTED' -and @($reviews | Where-Object {
                (Get-RinneProp $_ 'state') -eq 'APPROVED' -and [string](Get-RinneProp (Get-RinneProp $_ 'user') 'login') -eq $author -and
                (ConvertTo-RinneUtc $_.submitted_at) -gt (ConvertTo-RinneUtc $c.submitted_at) }).Count -gt 0
        & $add 'review' $c ([string]$c.submitted_at) @{ Comments = @($inline | ForEach-Object Id); ApprovedLater = $approved }
        $last = $items[$items.Count - 1]
        if (-not $last.Body.Trim() -and $inline.Count -and @($inline | Where-Object Kind -ne 'agent').Count -eq 0) { $last.Kind = 'agent' }
    }
    $items.ToArray()
}

<#
  一条评论有没有得到我方回应。「之后」一律对比原评论的 updated_at：原评论在我方回复之后又改过，就算没回应。
  - agent 自己的、非总结的 bot 通知（/review 的回执之类）：不需要；
  - bot 审查总结：之后有处置汇总引用它的运行编号；
  - review comment：线程里有我方回复的，要有一条晚于它的最后一次修改；维护者本人的 inline 评论必须有这样一条回复，
    只 resolve 不算；bot 的 inline 评论在处置汇总里逐条处置过，线程里没有我方回复的，线程已 resolve 也算；
  - 其余（issue comment、review 正文、CHANGES_REQUESTED）：我方评论的 agent 标记里用 in-reply-to 点名了它
    （-Reply 自动带上），不认正文里碰巧出现的数字。空正文、只装着 inline 评论的 review 看它那些评论；
    CHANGES_REQUESTED 也可以由同一作者之后的 APPROVED 解除（驳回的 review 不再出现在表里）。
#>
function Test-RinneResponded {
    param([Parameter(Mandatory)]$Item, [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Items, $State)
    $edited = ConvertTo-RinneUtc $Item.Updated
    $explicit = @($Items | Where-Object {
            $_.Kind -eq 'agent' -and @($_.InReplyTo) -contains $Item.Id -and (ConvertTo-RinneUtc $_.Created) -gt $edited }).Count -gt 0
    switch ($Item.Kind) {
        'agent' { return $true }
        'bot-summary' {
            $s = @($State.summaries) | Where-Object { [string]$_.id -eq $Item.Id } | Select-Object -First 1
            return [bool]($s -and $s.runId -and (@($State.dispositionRuns) -contains [string]$s.runId))
        }
    }
    if ($Item.Source -eq 'review-comment') {
        $replies = @($Items | Where-Object { $_.Kind -eq 'agent' -and $_.ThreadId -and $_.ThreadId -eq $Item.ThreadId -and $_.Id -ne $Item.Id })
        $later = @($replies | Where-Object { (ConvertTo-RinneUtc $_.Created) -gt $edited }).Count -gt 0
        if ($replies.Count -or $Item.Kind -ne 'bot') { return $explicit -or $later }
        return $explicit -or [bool]$Item.Resolved
    }
    if ($Item.Kind -eq 'bot') { return $true }
    if ($Item.Source -eq 'review') {
        if ($Item.State -eq 'CHANGES_REQUESTED') { return $explicit -or [bool]$Item.ApprovedLater }
        if (-not $Item.Body.Trim() -and @($Item.Comments).Count) {
            foreach ($cid in @($Item.Comments)) {
                $c = $Items | Where-Object { $_.Source -eq 'review-comment' -and $_.Id -eq $cid } | Select-Object -First 1
                if ($c -and -not (Test-RinneResponded -Item $c -Items $Items -State $State)) { return $false }
            }
            return $true
        }
    }
    return $explicit
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

# ---- 锁 -------------------------------------------------------------------------------------

<#
  锁由本地 gates 的 scripts/lib/Locks.psm1 提供（Local\ 命名互斥体，在 finally 里释放；超时抛 TimeoutException）：
  - git 锁：所有 fetch / merge / worktree 操作包在 Invoke-WithGitLock 里；
  - merge 锁：pr-merge 的 -Merge 与 -After 整段持有 Invoke-WithRinneLock -Name merge，同一时间只合一个 PR；
  - 构建槽：合并后构建（建树、清理、合并、构建、abort）整段持有一把 build-slot-1 / build-slot-2，与 gates 的编译共用。
  顺序固定为 merge → 构建槽 → git；gates 持构建槽时不取 git 锁，所以不会互等。块里的 git 失败由 Invoke-RinneGit 自己按退出码抛错。
#>
function Import-RinneLocks {
    Import-Module (Join-Path $PSScriptRoot '../../lib/Locks.psm1') -ErrorAction Stop -Global
}

<#
  持一把锁跑一段代码：git 锁经 Invoke-WithGitLock，其余（merge、build-slot-1/2）经 Invoke-WithRinneLock。
  自测用 Set-RinneLockImpl 挂一个观察者：它拿到锁名与「持真锁跑这段代码」的块，记下顺序后照样执行，锁还是真的锁
  （自测期间换成自测专用的一组锁名，与真在跑的 gates、合并互不等待）。
#>
function Invoke-RinneLocked {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][scriptblock]$Script, [double]$TimeoutSeconds = 0)
    Import-RinneLocks
    $opts = @{ ScriptBlock = $Script }
    if ($TimeoutSeconds -gt 0) { $opts.TimeoutSeconds = $TimeoutSeconds }
    $real = if ($Name -eq 'git') { { Invoke-WithGitLock @opts }.GetNewClosure() }
    else { $opts.Name = $Name; { Invoke-WithRinneLock @opts }.GetNewClosure() }
    if ($script:LockImpl) { return & $script:LockImpl $Name $real }
    & $real
}

function Invoke-RinneGitLocked {
    param([Parameter(Mandatory)][scriptblock]$Script)
    Invoke-RinneLocked -Name git -Script $Script
}

function Invoke-RinneMergeLocked {
    param([Parameter(Mandatory)][scriptblock]$Script)
    Invoke-RinneLocked -Name merge -Script $Script
}

function Invoke-RinneBuildSlotLocked {
    param(
        [Parameter(Mandatory)][ValidateSet('build-slot-1', 'build-slot-2')][string]$Slot,
        [Parameter(Mandatory)][scriptblock]$Script, [double]$TimeoutSeconds = 2700
    )
    Invoke-RinneLocked -Name $Slot -Script $Script -TimeoutSeconds $TimeoutSeconds
}

# ---- 定档与工作区状态 -----------------------------------------------------------------------

<#
  定档规则与判定只有一份：本地 gates 的 scripts/lib/Tiers.psm1 与 tier-rules.json（取主 clone 里、也就是 feature 线上的
  版本，不区分大小写）。gates 的结果 json 是自报的，档位够不够以合并时按 PR 文件清单重算的为准。
  自测只换掉「主 clone 的工作区状态」（Set-RinneCloneStateImpl），定档照常用真的 Tiers.psm1 与规则。
#>
$script:CloneStateImpl = $null

function Set-RinneCloneStateImpl { param([scriptblock]$Impl) $script:CloneStateImpl = $Impl }

function Import-RinneTiers {
    Import-Module (Join-Path $PSScriptRoot '../../lib/Tiers.psm1') -ErrorAction Stop -Global
}

<# 这些文件（改名的新旧路径都要给）加上卡片规定的档，最低要跑哪一档：{ Auto（只按文件）; Min（与卡片取高）; … }。 #>
function Resolve-RinneTierFloor {
    param([AllowEmptyCollection()][string[]]$Files = @(), [string]$CardTier = '')
    Import-RinneTiers
    Resolve-RinneTier -Files $Files -CardTier $CardTier
}

<# 档位的序号（Quick 0、Standard 1、Full 2）；不认识的档位抛错。 #>
function Get-RinneTierOrder {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Tier)
    Import-RinneTiers
    Get-RinneTierRank $Tier
}

<# 仓库的 HEAD 与工作区是否干净（改动、未跟踪的文件，以及被忽略却会被构建读到的文件都算不干净）：{ Sha; Dirty; Ignored }。 #>
function Get-RinneCloneState {
    param([string]$Root = $script:RepoRoot)
    if ($script:CloneStateImpl) { return & $script:CloneStateImpl $Root }
    Import-RinneTiers
    Get-RinneTreeState -Root $Root
}

# ---- 不带凭据跑不可信代码 -------------------------------------------------------------------

<#
  合并后构建跑的是 PR 树里的脚本：子进程启动前从本进程环境里拿掉凭据与个人针（GH_TOKEN、GITHUB_TOKEN、
  GH_ENTERPRISE_TOKEN、PRIVACY_NEEDLES、RINNE_PRIVACY_LOCAL，以及任何 *_TOKEN、*_KEY、*_SECRET、*_PASSWORD、*_PAT），
  结束后在 finally 里还原。这只挡住环境变量；本机 gh 的登录凭据与 git config 仍在，所以合并后构建只在其他判据都通过、
  且没碰基础设施时才跑。
#>
$script:SecretEnv = '^(?:GH_TOKEN|GITHUB_TOKEN|GH_ENTERPRISE_TOKEN|PRIVACY_NEEDLES|RINNE_PRIVACY_LOCAL|.+_TOKEN|.+_KEY|.+_SECRET|.+_PASSWORD|.+_PAT)$'

function Invoke-RinneWithoutSecrets {
    param([Parameter(Mandatory)][scriptblock]$Script)
    $saved = @{}
    foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
        if ($name -match $script:SecretEnv) {
            $saved[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, $null)
        }
    }
    try { & $Script } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

# ---- 自测框架 -------------------------------------------------------------------------------

<# 自测用：个人针改读一份临时的假针文件，不依赖本机配置。返回的状态交给 Exit-RinneSelfTestNeedles 还原。 #>
function Enter-RinneSelfTestNeedles {
    $file = [IO.Path]::GetTempFileName()
    [IO.File]::WriteAllText($file, "rinne-selftest-fake-needle-7c1e`n", [Text.UTF8Encoding]::new($false))
    $state = [pscustomobject]@{ File = $file; Local = $env:RINNE_PRIVACY_LOCAL; Ci = $env:PRIVACY_NEEDLES; Mode = $script:NeedlesMode }
    $env:RINNE_PRIVACY_LOCAL = $file
    $env:PRIVACY_NEEDLES = $null
    $script:NeedlesMode = 'local'
    $state
}

function Exit-RinneSelfTestNeedles {
    param($State)
    if (-not $State) { return }
    $env:RINNE_PRIVACY_LOCAL = $State.Local
    $env:PRIVACY_NEEDLES = $State.Ci
    $script:NeedlesMode = $State.Mode
    Remove-Item -LiteralPath $State.File -ErrorAction SilentlyContinue
}

function Set-RinneFake { param([scriptblock]$Handler) $script:Fake = $Handler }
<#
  挂上锁的观察者时，Locks.psm1 的锁名换成自测专用的一组（和它自己的自测一样，在模块内部改前缀；没有环境变量能换），
  与真在跑的 gates、合并互不等待；摘掉时还原。
#>
$script:SavedLockScope = $null
function Set-RinneLockImpl {
    param([scriptblock]$Impl)
    if ($Impl -and -not $script:LockImpl) {
        Import-RinneLocks
        $script:SavedLockScope = & (Get-Module Locks) { $script:Scope }
        & (Get-Module Locks) { param($s) $script:Scope = $s } "Rinne-selftest-$PID"
    } elseif (-not $Impl -and $script:LockImpl) {
        & (Get-Module Locks) { param($s) $script:Scope = $s } $script:SavedLockScope
    }
    $script:LockImpl = $Impl
}

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
  假的外部命令：routes 是有序表，键是对整条命令行（「exe 参数…」）的正则，值是 scriptblock，参数为 argv 与
  opts（Cwd、TimeoutSeconds），返回对象（转成 JSON）、字符串或 @{ Code; Out; Err }。没有命中的 node 命令与读本机
  git config 的命令放行给真实命令（都不联网）；其余没有命中的一律失败，自测里能看出漏录了哪条。log 记录每条命令。
#>
function New-RinneFake {
    param([Parameter(Mandatory)][System.Collections.Specialized.OrderedDictionary]$Routes, [System.Collections.Generic.List[string]]$Log)
    return {
        param($exe, $argv, $opts)
        $line = "$exe $($argv -join ' ')"
        if ($null -ne $Log) { $Log.Add($line) }
        foreach ($k in $Routes.Keys) {
            if ($line -match $k) {
                $v = & $Routes[$k] $argv $opts
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

Export-ModuleMember -Function *-Rinne*, *-AgentMarker, Format-AgentText, Test-PublicText, Test-PublicTexts, Assert-PublicText,
    Test-RepoWriter, ConvertTo-GlobRegex, Test-GlobMatch, Get-InfraTouches, Get-CardScope,
    Initialize-RinneIdentity, Test-Throws, Add-RinneCase, Complete-RinneSelfTest, ConvertTo-RinneJsonText
