# SPDX-License-Identifier: GPL-3.0-or-later
#
# 页面冒烟，由 scripts/gates.ps1 加载：G9-web 与 G9-web-port（都是 Full 档，都持 gui 锁：浏览器冒烟与固定端口本机同时只能一个）。
# G9-web 跑三件事：
#   1. web-preview/serve.ps1 -SmokeTest：起服务、自己发请求、核对状态码与 no-store、停服。首选固定端口 8833；它被别的进程占着时
#      serve.ps1 打出占用者（PID 与进程名）、改用空闲端口照常跑完，Reason 里写明这次用的端口与占用者；
#   2. web-preview/build-static.mjs --check：预览的静态导出自检，纯本地、无外链、无网络调用；
#   3. web-preview/tests/run-all.mjs：全部 smoke-*.mjs（页面级的用 tests/lib/static-server.mjs 的临时端口，不占 8833；无头浏览器
#      那几个在没有浏览器时跳过）。
# 有冒烟整套跳过，或套内某一项跳过（例如还没入库的 C++ 侧对拍项）时，整条 gate 记 SKIP 并写明是哪几个，不记 PASS：跳过不是通过。
# G9-web-port 是「固定端口 8833 自检」，单独一项：serve.ps1 -SmokeTest -StrictPort，只在 8833 上起服自检。8833 被别的进程占着
# （本机的出站连接也会随机拿到它当源端口）记 SKIP，Reason 写明占用者，不判红：那是本机别的程序，不是改动的问题；不杀进程、不改系统的
# 端口设置，它放开之后重跑就是 PASS。起不来又不是被占（例如没有权限）、或者起来了但请求不符合预期，照常 FAIL。
# Node 用钉版：先找环境变量 RINNE_NODE，再找 git config rinne.node，最后退回 PATH；主版本必须等于 .node-version。

$script:FixedPort = 8833

function Get-WebNode($Ctx) {
    $p = & $Ctx.Setting 'RINNE_NODE' 'rinne.node'
    $p ? $p : 'node'
}

# 跑一条原生命令，返回 Code 与输出行；输出里的 ANSI 颜色码去掉。
function Invoke-WebNative([string]$Exe, [string[]]$Arguments, [string]$Cwd) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    Push-Location -LiteralPath $Cwd
    try {
        $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" -replace '\x1b\[[0-9;]*m', '' })
        $code = $LASTEXITCODE
    } catch {
        $lines, $code = @("$Exe：$($_.Exception.Message)"), 127
    } finally {
        Pop-Location
    }
    [pscustomobject]@{ Code = $code; Lines = $lines }
}

# run-all 的最后一行 `run-all：通过 n / 跳过 k / 失败 f（内部跳过项 m）` 折成四个数；读不到时返回 $null。
function Get-WebSmokeCounts([string[]]$Lines) {
    $last = @($Lines | Where-Object { $_ -cmatch '^run-all：' } | Select-Object -Last 1)[0]
    $m = [regex]::Match("$last", '通过 (\d+) / 跳过 (\d+) / 失败 (\d+)（内部跳过项 (\d+)）')
    if (-not $m.Success) { return $null }
    [pscustomobject]@{ Pass = [int]$m.Groups[1].Value; Skip = [int]$m.Groups[2].Value; Fail = [int]$m.Groups[3].Value; Inner = [int]$m.Groups[4].Value }
}

# serve.ps1 输出里的两行机读信息：`serve.ps1 端口 <n> 被占：<占用者>` 与 `serve.ps1 端口：<实际端口>`。返回 @{ Port; Busy; Owner }，
# Port 读不到时为 0；Busy 是被占的那个端口（没被占为 0），Owner 是占用者的说明。
function Get-ServePortInfo([string[]]$Lines) {
    $busy = @($Lines | ForEach-Object { [regex]::Match("$_", '^serve\.ps1 端口 (\d+) 被占：(.*)$') } | Where-Object Success | Select-Object -Last 1)[0]
    $used = @($Lines | ForEach-Object { [regex]::Match("$_", '^serve\.ps1 端口：(\d+)$') } | Where-Object Success | Select-Object -Last 1)[0]
    [pscustomobject]@{
        Port = $used ? [int]$used.Groups[1].Value : 0
        Busy = $busy ? [int]$busy.Groups[1].Value : 0
        Owner = $busy ? $busy.Groups[2].Value.Trim() : ''
    }
}

# G9-web 的 Reason 里要带的端口说明：serve 自检这次没用上固定端口时，写明用的是哪个空闲端口、固定端口被谁占着。
function Get-ServePortNote($Info) {
    if (-not $Info.Busy) { return '' }
    "；serve 自检改用空闲端口 $($Info.Port)（$($Info.Busy) 被占：$($Info.Owner)；固定端口另见 G9-web-port）"
}

# 三步的结果折成一条 gate 结果。Steps 是 @{ Label; Code; Lines } 的数组，Counts 是 run-all 的计数，PortNote 是 Get-ServePortNote 的结果。
function Resolve-WebResult($Steps, $Counts, [string[]]$SkippedNames, [string]$PortNote = '') {
    $failed = @($Steps | Where-Object { $_.Code -ne 0 } | ForEach-Object { $_.Label })
    $lines = @($Steps | ForEach-Object { "--- $($_.Label)：退出码 $($_.Code)"; $_.Lines })
    if ($failed.Count) { return @{ Status = 'FAIL'; Reason = "失败：$($failed -join '、')$PortNote"; Lines = $lines } }
    if ($null -eq $Counts) { return @{ Status = 'FAIL'; Reason = "run-all 没有给出汇总行$PortNote"; Lines = $lines } }
    if ($Counts.Skip -gt 0 -or $Counts.Inner -gt 0) {
        return @{ Status = 'SKIP'; Reason = "$($Counts.Skip) 个冒烟整套跳过（多半是没有浏览器）、$($Counts.Inner) 个套内项跳过（多半是 C++ 侧对拍项还没入库）：$($SkippedNames -join '、')；其余 $($Counts.Pass) 个通过$PortNote"; Lines = $lines }
    }
    @{ Status = 'PASS'; Reason = "serve 自检、静态导出自检与 $($Counts.Pass) 个冒烟全过$PortNote"; Lines = $lines }
}

# 固定端口自检（serve.ps1 -SmokeTest -StrictPort）的退出码折成 gate 结果：0 PASS；3 是端口被占，有占用者那一行才记 SKIP（写明是谁），
# 没有那一行说明 serve.ps1 不是因为被占才退出 3，记 FAIL；其余 FAIL。
function Resolve-PortResult([int]$Code, [string[]]$Lines, [int]$Port) {
    $info = Get-ServePortInfo $Lines
    switch ($Code) {
        0 { @{ Status = 'PASS'; Reason = "固定端口 $Port 上起服，全部请求符合预期"; Lines = $Lines } }
        3 {
            if ($info.Busy -eq $Port -and $info.Owner) { @{ Status = 'SKIP'; Reason = "固定端口 $Port 被占：$($info.Owner)。没杀进程、没改系统端口设置，冒烟已改用空闲端口照跑（见 G9-web）；放开后重跑即可"; Lines = $Lines } }
            else { @{ Status = 'FAIL'; Reason = "serve.ps1 -StrictPort 退出 3，却没写端口 $Port 的占用者"; Lines = $Lines } }
        }
        default { @{ Status = 'FAIL'; Reason = "固定端口 $Port 自检失败（serve.ps1 退出码 $Code）"; Lines = $Lines } }
    }
}

# 起一次 serve.ps1 -SmokeTest（-Strict 时带 -StrictPort），返回 @{ Code; Lines }。
function Invoke-ServeSmoke([string]$Root, [int]$Port, [switch]$Strict) {
    $pwsh = (Get-Process -Id $PID).Path
    $a = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $Root 'web-preview/serve.ps1'), '-SmokeTest', '-Port', "$Port")
    if ($Strict) { $a += '-StrictPort' }
    Invoke-WebNative $pwsh $a $Root
}

function Invoke-WebPortCheck($Ctx) {
    $r = Invoke-ServeSmoke $Ctx.Root $script:FixedPort -Strict
    Resolve-PortResult $r.Code $r.Lines $script:FixedPort
}

function Invoke-WebSmoke($Ctx) {
    $node = Get-WebNode $Ctx
    $want = (Get-Content -LiteralPath (Join-Path $Ctx.Root '.node-version') -Raw).Trim().Split('.')[0]
    $v = Invoke-WebNative $node @('--version') $Ctx.Root
    $have = [regex]::Match(($v.Lines -join ''), '^v(\d+)').Groups[1].Value
    if ($v.Code -ne 0 -or $have -cne $want) {
        return @{ Status = 'FAIL'; Reason = "需要 Node $want（.node-version），得到 $($have ? $have : '找不到')；用 RINNE_NODE 或 git config rinne.node 指定钉版" }
    }
    $steps = [System.Collections.Generic.List[object]]::new()
    $serve = Invoke-ServeSmoke $Ctx.Root $script:FixedPort
    $steps.Add([pscustomobject]@{ Label = 'serve.ps1 -SmokeTest'; Code = $serve.Code; Lines = $serve.Lines })
    $static = Invoke-WebNative $node @('web-preview/build-static.mjs', '--check') $Ctx.Root
    $steps.Add([pscustomobject]@{ Label = 'build-static --check'; Code = $static.Code; Lines = $static.Lines })
    $all = Invoke-WebNative $node @('web-preview/tests/run-all.mjs', $Ctx.Root) $Ctx.Root
    $steps.Add([pscustomobject]@{ Label = '冒烟 run-all'; Code = $all.Code; Lines = $all.Lines })
    $skipped = @($all.Lines | Where-Object { $_ -cmatch '^\[跳过(项)?\] ' } | ForEach-Object { ($_ -replace '^\[跳过(项)?\] ', '') })
    Resolve-WebResult $steps (Get-WebSmokeCounts $all.Lines) $skipped (Get-ServePortNote (Get-ServePortInfo $serve.Lines))
}

function Test-RinneGates([string]$Root) {
    $ok = [pscustomobject]@{ Label = 'x'; Code = 0; Lines = @() }
    $bad = [pscustomobject]@{ Label = '冒烟 run-all'; Code = 1; Lines = @() }
    $counts = { param($p, $s, $f, $i = 0) [pscustomobject]@{ Pass = $p; Skip = $s; Fail = $f; Inner = $i } }
    $parsed = Get-WebSmokeCounts @('噪声', 'run-all：通过 7 / 跳过 2 / 失败 0（内部跳过项 3）')
    [pscustomobject]@{ Label = '能解析 run-all 的汇总行'; Bad = $false; Passed = $parsed -and $parsed.Pass -eq 7 -and $parsed.Skip -eq 2 -and $parsed.Fail -eq 0 -and $parsed.Inner -eq 3 }
    [pscustomobject]@{ Label = '没有汇总行时解析不出来'; Bad = $true; Passed = $null -eq (Get-WebSmokeCounts @('run-all 崩了')) }
    [pscustomobject]@{ Label = '三步全过、没有跳过：PASS'; Bad = $false; Passed = (Resolve-WebResult @($ok) (& $counts 6 0 0) @()).Status -ceq 'PASS' }
    [pscustomobject]@{ Label = '有一步退出码非零：FAIL'; Bad = $true; Passed = (Resolve-WebResult @($ok, $bad) (& $counts 6 0 0) @()).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = '有冒烟跳过：SKIP，不是 PASS'; Bad = $true; Passed = (Resolve-WebResult @($ok) (& $counts 4 2 0) @('a.mjs', 'b.mjs')).Status -ceq 'SKIP' }
    [pscustomobject]@{ Label = '只有套内项跳过：SKIP，不是 PASS'; Bad = $true; Passed = (Resolve-WebResult @($ok) (& $counts 8 0 0 5) @('x')).Status -ceq 'SKIP' }
    [pscustomobject]@{ Label = '没有汇总行：FAIL'; Bad = $true; Passed = (Resolve-WebResult @($ok) $null @()).Status -ceq 'FAIL' }
    # 固定端口：serve.ps1 的两行机读信息、G9-web 的端口说明、G9-web-port 的退出码映射
    $busyLines = @('x', 'serve.ps1 端口 8833 被占：PID 9632 SomeDaemon（0.0.0.0:8833 Bound）', '  改用空闲端口 12350', 'serve.ps1 端口：12350')
    $info = Get-ServePortInfo $busyLines
    [pscustomobject]@{ Label = '能读出 serve.ps1 的实际端口、被占的端口与占用者'; Bad = $false; Passed = $info.Port -eq 12350 -and $info.Busy -eq 8833 -and $info.Owner -ceq 'PID 9632 SomeDaemon（0.0.0.0:8833 Bound）' }
    $note = Get-ServePortNote $info
    [pscustomobject]@{ Label = '8833 被占时 G9-web 的 Reason 写明改用的端口与占用者'; Bad = $true; Passed = (Resolve-WebResult @($ok) (& $counts 6 0 0) @() $note).Reason -match '改用空闲端口 12350（8833 被占：PID 9632 SomeDaemon' }
    [pscustomobject]@{ Label = '没被占时没有端口说明'; Bad = $false; Passed = (Get-ServePortNote (Get-ServePortInfo @('serve.ps1 端口：8833'))) -ceq '' }
    $port = { param($code, $lines) (Resolve-PortResult $code $lines 8833) }
    [pscustomobject]@{ Label = '固定端口自检通过：PASS'; Bad = $false; Passed = (& $port 0 @('serve.ps1 端口：8833')).Status -ceq 'PASS' }
    $taken = & $port 3 $busyLines[0..1]
    [pscustomobject]@{ Label = '固定端口被占：SKIP（不是 PASS），Reason 写明占用者'; Bad = $true; Passed = $taken.Status -ceq 'SKIP' -and $taken.Reason -match 'PID 9632 SomeDaemon' }
    [pscustomobject]@{ Label = '退出 3 却没写占用者：FAIL'; Bad = $true; Passed = (& $port 3 @('x')).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = '固定端口起服后请求不符合预期（退出 1）：FAIL'; Bad = $true; Passed = (& $port 1 @('serve.ps1 端口：8833')).Status -ceq 'FAIL' }
    # 真起 serve.ps1：本进程占住一个临时端口，冒烟要换空闲端口跑完并写明占用者（本进程），-StrictPort 要退出 3 记 SKIP；放开之后照常 PASS
    $held = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $held.Start()
    $p = $held.LocalEndpoint.Port
    try {
        $moved = Invoke-ServeSmoke $Root $p
        $mi = Get-ServePortInfo $moved.Lines
        $strict = Invoke-ServeSmoke $Root $p -Strict
    } finally { $held.Stop() }
    $free = Invoke-ServeSmoke $Root $p -Strict
    [pscustomobject]@{ Label = "serve.ps1：端口 $p 被本进程占着时换空闲端口 $($mi.Port) 跑完自检，并写明占用者是本进程"; Bad = $true
        Passed = $moved.Code -eq 0 -and $mi.Busy -eq $p -and $mi.Port -gt 0 -and $mi.Port -ne $p -and $mi.Owner -match "^PID $PID " }
    $sr = Resolve-PortResult $strict.Code $strict.Lines $p
    [pscustomobject]@{ Label = 'serve.ps1 -StrictPort：端口被占时不换端口、退出 3，记 SKIP 并写明占用者'; Bad = $true; Passed = $strict.Code -eq 3 -and $sr.Status -ceq 'SKIP' -and $sr.Reason -match "PID $PID " }
    [pscustomobject]@{ Label = 'serve.ps1 -StrictPort：端口放开之后照常 PASS'; Bad = $false; Passed = (Resolve-PortResult $free.Code $free.Lines $p).Status -ceq 'PASS' -and (Get-ServePortInfo $free.Lines).Port -eq $p }
}

function Register-RinneGates {
    @{ Id = 'G9-web'; Tier = 'Full'; Name = '页面冒烟'; Lock = 'gui'; Run = ${function:Invoke-WebSmoke} }
    @{ Id = 'G9-web-port'; Tier = 'Full'; Name = '固定端口 8833 自检'; Lock = 'gui'; Run = ${function:Invoke-WebPortCheck} }
}
