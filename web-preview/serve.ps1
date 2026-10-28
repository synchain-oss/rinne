# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web-preview.json
#
# web-preview 的本地静态服务器：首选端口 8833，只监听 127.0.0.1，纯本地，不发任何网络请求。
#   pwsh web-preview/serve.ps1 [-Port 8833] [-Open] [-StrictPort]
#   pwsh web-preview/serve.ps1 -SmokeTest [-Port 8833] [-StrictPort]   起服、自己发几条请求、核对后停服
# 站点根是仓库根而不是 web-preview/：壳页用 ../web/<角色>/index.html 引用真源，两棵目录要在同一个源下，
# mock 才能经同源 iframe 注入。不能用 file:// 直开：ES module 在 file:// 下被 CORS 拒绝，iframe 也会变跨源。
# 所有响应都带 no-store，页面永远拿最新的 web/ 真源。
# 端口：8833 是写进文档的固定入口。它被别的进程占着时（本机的出站连接也可能随机拿到它当源端口），先把占用者的 PID 与进程名
# 打出来，再挑一个空闲端口照常起服，实际地址照常打印；不杀任何进程，也不改系统的端口设置。-StrictPort 不换端口：被占就退出 3
# （gates 的「固定端口自检」用它）。机读的两行：`serve.ps1 端口 <n> 被占：<占用者>` 与 `serve.ps1 端口：<实际端口>`。
# 退出码：0 正常（-SmokeTest 全部请求符合预期）；1 失败；3 -StrictPort 下端口被占。
param(
    [int]$Port = 8833,
    [switch]$Open,
    [switch]$SmokeTest,
    [switch]$StrictPort
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$EntryPath = '/web-preview/input.html'

# 只列预览页会取到的类型，缺项回落 octet-stream。.js 必须是 text/javascript，否则 module script 会被拒。
$MimeTypes = @{
    '.html' = 'text/html; charset=utf-8'
    '.js' = 'text/javascript; charset=utf-8'
    '.mjs' = 'text/javascript; charset=utf-8'
    '.css' = 'text/css; charset=utf-8'
    '.json' = 'application/json; charset=utf-8'
    '.svg' = 'image/svg+xml'
    '.woff2' = 'font/woff2'
    '.png' = 'image/png'
    '.md' = 'text/markdown; charset=utf-8'
    '.txt' = 'text/plain; charset=utf-8'
}

# 站点根是仓库根，但只放行预览要用到的两棵目录：/web/ 与 /web-preview/；别的一律 404（含 /.git/、/scripts/、/_logs/ 等）。
# 以点开头的路径段（.git、.shots、.env 之类）也一律 404。
function Test-AllowedUrlPath([string]$UrlPath) {
    $decoded = [System.Uri]::UnescapeDataString($UrlPath)
    if ($decoded -notmatch '^/(web|web-preview)/') { return $false }
    foreach ($seg in ($decoded -split '[/\\]')) { if ($seg.StartsWith('.')) { return $false } }
    return $true
}

# URL 路径转磁盘路径并挡掉目录穿越：先查前缀白名单，再解码、拼到站点根上归一化，最后断言仍在站点根之内。
function Get-SafeLocalPath([string]$UrlPath, [string]$Root) {
    if (-not (Test-AllowedUrlPath $UrlPath)) { return $null }
    $decoded = [System.Uri]::UnescapeDataString($UrlPath).TrimStart('/')
    $decoded = $decoded -replace '/', [System.IO.Path]::DirectorySeparatorChar
    $full = [System.IO.Path]::GetFullPath((Join-Path $Root $decoded))
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    if ($full -ne $rootFull -and -not $full.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }
    if (Test-Path -LiteralPath $full -PathType Container) { $full = Join-Path $full 'index.html' }
    return $full
}

# 处理一条请求，返回状态码。根路径跳到入口页。
function Send-StaticFile([System.Net.HttpListenerContext]$Context, [string]$Root) {
    $req = $Context.Request
    $res = $Context.Response
    $status = 200
    try {
        if ($req.Url.AbsolutePath -eq '/') {
            $status = 302
            $res.Redirect($EntryPath)
        } else {
            $local = Get-SafeLocalPath -UrlPath $req.Url.AbsolutePath -Root $Root
            if ($null -ne $local -and (Test-Path -LiteralPath $local -PathType Leaf)) {
                $ext = [System.IO.Path]::GetExtension($local).ToLowerInvariant()
                $res.ContentType = if ($MimeTypes.ContainsKey($ext)) { $MimeTypes[$ext] } else { 'application/octet-stream' }
                $bytes = [System.IO.File]::ReadAllBytes($local)
            } else {
                $status = 404
                $res.ContentType = 'text/plain; charset=utf-8'
                $bytes = [System.Text.Encoding]::UTF8.GetBytes("404 Not Found: $($req.Url.AbsolutePath)")
            }
            $res.StatusCode = $status
            $res.Headers['Cache-Control'] = 'no-store, must-revalidate'
            $res.ContentLength64 = $bytes.Length
            if ($req.HttpMethod -ne 'HEAD') { $res.OutputStream.Write($bytes, 0, $bytes.Length) }
        }
    } catch {
        $status = 500
        try {
            $res.StatusCode = 500
            $msg = [System.Text.Encoding]::UTF8.GetBytes("500 $($_.Exception.Message)")
            $res.ContentLength64 = $msg.Length
            $res.OutputStream.Write($msg, 0, $msg.Length)
        } catch { $null = $_ }
    } finally {
        try { $res.OutputStream.Close() } catch { $null = $_ }
    }
    Write-Host ('  {0} {1} {2}' -f $status, $req.HttpMethod, $req.Url.AbsolutePath) -ForegroundColor ($status -lt 400 ? 'DarkGray' : 'Yellow')
    return $status
}

# 在 127.0.0.1:<P> 上起 HttpListener。返回 @{ Listener; Error }：起来了 Error 为空，起不来 Listener 为空、Error 是异常。
function Start-PreviewListener([int]$P) {
    $l = [System.Net.HttpListener]::new()
    $l.Prefixes.Add("http://127.0.0.1:$P/")
    try {
        $l.Start()
        return @{ Listener = $l; Error = $null }
    } catch {
        $l.Close()
        $e = $_.Exception
        while ($e.InnerException -and $e -isnot [System.Net.HttpListenerException]) { $e = $e.InnerException }
        return @{ Listener = $null; Error = $e }
    }
}

# 占着这个 TCP 端口的进程，逐个写成「PID <n> <进程名>（<本地地址>:<端口> <状态>）」。状态不限：只绑了端口、还没连上或不在监听的
# 出站连接（Bound）也占着它，netstat 的 LISTENING 里看不到。PID 4 是 System，端口在 http.sys 手里，是另一个 HttpListener 服务
# （多半是上一次没停掉的 serve.ps1）。TimeWait 不算：那是已经关掉的连接留下的记录（属主是 PID 0），不占着端口，写进说明只会误导。
# 这里只给说明，被不被占由 Test-PortTaken 按错误码判。查不到时返回 $null。
function Get-PortOwner([int]$P) {
    if (-not (Get-Command Get-NetTCPConnection -ErrorAction Ignore)) { return $null }
    $conns = @(Get-NetTCPConnection -LocalPort $P -ErrorAction Ignore | Where-Object { "$($_.State)" -cne 'TimeWait' } | Sort-Object OwningProcess, State -Unique)
    if (-not $conns.Count) { return $null }
    @($conns | ForEach-Object {
            $name = (Get-Process -Id $_.OwningProcess -ErrorAction Ignore).ProcessName
            $sys = ($_.OwningProcess -eq 4) ? '，http.sys：另一个 HttpListener 服务，多半是没停掉的 serve.ps1' : ''
            "PID $($_.OwningProcess) $($name ? $name : '?')（$($_.LocalAddress):$P $($_.State)$sys）"
        }) -join '；'
}

# 端口被占只认两种证据：HttpListener 报的就是「被另一个进程占着」（32）/「这个地址已经有人注册」（183）；或者在 127.0.0.1 的这个
# 端口上连普通的 TCP 监听也绑不上，而且报的是地址已被使用（没有 Get-NetTCPConnection 的平台也靠这一条）。查到的占用者只用来说明
# 是谁占着，不当作被占的证据：别的本地地址上的连接未必挡得住 127.0.0.1。别的起不来（例如没有权限、端口在系统保留段里、探测时
# 报了别的套接字错误）都不算被占：换端口也不会好，照常判失败，-StrictPort 也就不会把它记成跳过。
# $Address 只给自测换探测地址用（换成本机没有的地址，探测就会报别的套接字错误）；serve.ps1 自己一律探测 127.0.0.1。
function Test-PortTaken($Err, [int]$P, [System.Net.IPAddress]$Address = [System.Net.IPAddress]::Loopback) {
    if ($Err -is [System.Net.HttpListenerException] -and $Err.ErrorCode -in 32, 183) { return $true }
    $probe = [System.Net.Sockets.TcpListener]::new($Address, $P)
    try { $probe.Start(); $false } catch { Test-AddressInUse $_.Exception } finally { $probe.Stop() }
}

# 探测绑不上时报的是不是「地址已被使用」：剥掉 PowerShell 包的外层异常找到套接字异常再看错误码；不是套接字异常、或是别的错误码都不算。
function Test-AddressInUse($Exception) {
    $e = $Exception
    while ($e.InnerException -and $e -isnot [System.Net.Sockets.SocketException]) { $e = $e.InnerException }
    $e -is [System.Net.Sockets.SocketException] -and $e.SocketErrorCode -eq [System.Net.Sockets.SocketError]::AddressAlreadyInUse
}

# 本机挑一个空闲端口：让系统给 127.0.0.1 分配一个，记下号码就放开，紧接着在它上面起服（中间被别人抢走就再挑）。
function Get-FreePort {
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    try { $l.LocalEndpoint.Port } finally { $l.Stop() }
}

$started = Start-PreviewListener $Port
if (-not $started.Listener) {
    $owner = Get-PortOwner $Port
    if (-not (Test-PortTaken $started.Error $Port)) {
        Write-Host ("serve.ps1: 端口 {0} 起不来：{1}" -f $Port, $started.Error.Message) -ForegroundColor Red
        exit 1
    }
    Write-Host ("serve.ps1 端口 {0} 被占：{1}" -f $Port, ($owner ? $owner : "查不到占用者（{0}）" -f $started.Error.Message)) -ForegroundColor Yellow
    if ($StrictPort) {
        Write-Host '  -StrictPort：不换端口，退出 3。不杀占用者、不改系统端口设置；它放开之后再跑。' -ForegroundColor Yellow
        exit 3
    }
    $wanted = $Port
    for ($i = 0; $i -lt 5 -and -not $started.Listener; $i++) {
        $Port = Get-FreePort
        $started = Start-PreviewListener $Port
    }
    if (-not $started.Listener) {
        Write-Host ("serve.ps1: 换了 5 个空闲端口也起不来：{0}" -f $started.Error.Message) -ForegroundColor Red
        exit 1
    }
    Write-Host ("  改用空闲端口 {0}：这次的地址以它为准（{1} 放开之后再起就回到它）" -f $Port, $wanted) -ForegroundColor Yellow
}
$listener = $started.Listener
$BaseUrl = "http://127.0.0.1:$Port"
Write-Host ("serve.ps1 端口：{0}" -f $Port)

Write-Host ''
Write-Host 'Rinne web-preview：本地静态服务器已启动' -ForegroundColor Green
Write-Host ("  站点根 : {0}" -f $RepoRoot)
Write-Host ("  Input  : {0}/web-preview/input.html?scenario=in-playing" -f $BaseUrl) -ForegroundColor Cyan
Write-Host ("  Output : {0}/web-preview/output.html?scenario=out-connected" -f $BaseUrl) -ForegroundColor Cyan
Write-Host ''

if ($SmokeTest) {
    # 自检：自己当客户端发请求，单线程一收一发。根路径应当 302 到入口页，其余应当 200，穿越路径应当 404。
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $http = [System.Net.Http.HttpClient]::new($handler)
    $cases = @(
        @{ Path = '/'; Want = 302 }
        @{ Path = '/web-preview/input.html'; Want = 200 }
        @{ Path = '/web-preview/output.html'; Want = 200 }
        @{ Path = '/web-preview/shell.js'; Want = 200 }
        @{ Path = '/web/input/index.html'; Want = 200 }
        @{ Path = '/web/shared/bridge.js'; Want = 200 }
        @{ Path = '/web-preview/%2e%2e/%2e%2e/etc/passwd'; Want = 404 }
        @{ Path = '/web-preview/no-such-file.js'; Want = 404 }
        @{ Path = '/.git/config'; Want = 404 }
        @{ Path = '/scripts/gates.ps1'; Want = 404 }
        @{ Path = '/CLAUDE.md'; Want = 404 }
        @{ Path = '/web-preview/.shots/x.png'; Want = 404 }
        @{ Path = '/web/%2e%67it/config'; Want = 404 }
        @{ Path = '/web/../.git/config'; Want = 404 }
    )
    $fail = 0
    foreach ($c in $cases) {
        $pending = $http.GetAsync("$BaseUrl$($c.Path)")
        $ctxTask = $listener.GetContextAsync()
        if (-not $ctxTask.Wait(10000)) {
            Write-Host ("  [超时] {0}" -f $c.Path) -ForegroundColor Red
            $fail++
            continue
        }
        [void](Send-StaticFile -Context $ctxTask.Result -Root $RepoRoot)
        $resp = $pending.GetAwaiter().GetResult()
        $got = [int]$resp.StatusCode
        $ok = $got -eq $c.Want
        if ($ok -and $c.Want -eq 200) {
            $ok = $resp.Content.Headers.ContentLength -gt 0 -and $resp.Headers.CacheControl.NoStore
        }
        if ($ok -and $c.Want -eq 302) { $ok = "$($resp.Headers.Location)" -eq $EntryPath }
        if (-not $ok) { $fail++ }
        Write-Host ("  [{0}] {1} 期望 {2}，得到 {3}" -f ($ok ? '通过' : '失败'), $c.Path, $c.Want, $got)
    }
    $http.Dispose()
    $listener.Stop()
    $listener.Close()
    if ($fail) {
        Write-Host ("serve.ps1 -SmokeTest：{0} 条失败" -f $fail) -ForegroundColor Red
        exit 1
    }
    Write-Host 'serve.ps1 -SmokeTest：起服、全部请求符合预期、已停服' -ForegroundColor Green
    exit 0
}

if ($Open) { Start-Process "$BaseUrl$EntryPath" }
Write-Host 'Ctrl+C 停止。' -ForegroundColor DarkGray

try {
    while ($listener.IsListening) {
        # 用 GetContextAsync 加短等待轮询：阻塞在 .NET 调用里时 PowerShell 收不到 Ctrl+C。
        $ctxTask = $listener.GetContextAsync()
        while (-not $ctxTask.Wait(200)) { $null = $null }
        [void](Send-StaticFile -Context $ctxTask.Result -Root $RepoRoot)
    }
} finally {
    try { $listener.Stop(); $listener.Close() } catch { $null = $_ }
    Write-Host 'serve.ps1: 已停止。' -ForegroundColor DarkGray
}
