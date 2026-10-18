# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web-preview.json
#
# web-preview 的本地静态服务器：端口固定 8833，只监听 127.0.0.1，纯本地，不发任何网络请求。
#   pwsh web-preview/serve.ps1 [-Port 8833] [-Open]
#   pwsh web-preview/serve.ps1 -SmokeTest      起服、自己发几条请求、核对后停服，退出码 0 或 1
# 站点根是仓库根而不是 web-preview/：壳页用 ../web/<角色>/index.html 引用真源，两棵目录要在同一个源下，
# mock 才能经同源 iframe 注入。不能用 file:// 直开：ES module 在 file:// 下被 CORS 拒绝，iframe 也会变跨源。
# 所有响应都带 no-store，页面永远拿最新的 web/ 真源。端口被占用时直接报错退出，不静默换端口：
# 地址要能写进文档当固定入口。
param(
    [int]$Port = 8833,
    [switch]$Open,
    [switch]$SmokeTest
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BaseUrl = "http://127.0.0.1:$Port"
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

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("$BaseUrl/")
try {
    $listener.Start()
} catch {
    Write-Host ("serve.ps1: 端口 {0} 起不来：{1}" -f $Port, $_.Exception.Message) -ForegroundColor Red
    Write-Host '  端口固定是有意的。占用方多半是上一次没停掉的 serve.ps1。' -ForegroundColor Red
    exit 1
}

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
