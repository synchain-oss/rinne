# SPDX-License-Identifier: GPL-3.0-or-later
#
# 页面冒烟，由 scripts/gates.ps1 加载：G9-web（Full 档）。一条 gate 跑三件事，持 gui 锁（浏览器冒烟本机同时只能一个）：
#   1. web-preview/serve.ps1 -SmokeTest：起服务（固定端口 8833）、自己发请求、核对状态码与 no-store、停服；
#   2. web-preview/build-static.mjs --check：预览的静态导出自检，纯本地、无外链、无网络调用；
#   3. web-preview/tests/run-all.mjs：全部 smoke-*.mjs（无头浏览器那几个在没有浏览器时跳过）。
# 有冒烟整套跳过，或套内某一项跳过（例如还没入库的 C++ 侧对拍项）时，整条 gate 记 SKIP 并写明是哪几个，不记 PASS：跳过不是通过。端口 8833 被占用记 FAIL，不换端口。
# Node 用钉版：先找环境变量 RINNE_NODE，再找 git config rinne.node，最后退回 PATH；主版本必须等于 .node-version。

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

# 三步的结果折成一条 gate 结果。Steps 是 @{ Label; Code; Lines } 的数组，Counts 是 run-all 的计数。
function Resolve-WebResult($Steps, $Counts, [string[]]$SkippedNames) {
    $failed = @($Steps | Where-Object { $_.Code -ne 0 } | ForEach-Object { $_.Label })
    $lines = @($Steps | ForEach-Object { "--- $($_.Label)：退出码 $($_.Code)"; $_.Lines })
    if ($failed.Count) { return @{ Status = 'FAIL'; Reason = "失败：$($failed -join '、')"; Lines = $lines } }
    if ($null -eq $Counts) { return @{ Status = 'FAIL'; Reason = 'run-all 没有给出汇总行'; Lines = $lines } }
    if ($Counts.Skip -gt 0 -or $Counts.Inner -gt 0) {
        return @{ Status = 'SKIP'; Reason = "$($Counts.Skip) 个冒烟整套跳过（多半是没有浏览器）、$($Counts.Inner) 个套内项跳过（多半是 C++ 侧对拍项还没入库）：$($SkippedNames -join '、')；其余 $($Counts.Pass) 个通过"; Lines = $lines }
    }
    @{ Status = 'PASS'; Reason = "serve 自检、静态导出自检与 $($Counts.Pass) 个冒烟全过"; Lines = $lines }
}

function Invoke-WebSmoke($Ctx) {
    $node = Get-WebNode $Ctx
    $want = (Get-Content -LiteralPath (Join-Path $Ctx.Root '.node-version') -Raw).Trim().Split('.')[0]
    $v = Invoke-WebNative $node @('--version') $Ctx.Root
    $have = [regex]::Match(($v.Lines -join ''), '^v(\d+)').Groups[1].Value
    if ($v.Code -ne 0 -or $have -cne $want) {
        return @{ Status = 'FAIL'; Reason = "需要 Node $want（.node-version），得到 $($have ? $have : '找不到')；用 RINNE_NODE 或 git config rinne.node 指定钉版" }
    }
    $pwsh = (Get-Process -Id $PID).Path
    $steps = [System.Collections.Generic.List[object]]::new()
    $serve = Invoke-WebNative $pwsh @('-NoProfile', '-NonInteractive', '-File', (Join-Path $Ctx.Root 'web-preview/serve.ps1'), '-SmokeTest') $Ctx.Root
    $steps.Add([pscustomobject]@{ Label = 'serve.ps1 -SmokeTest'; Code = $serve.Code; Lines = $serve.Lines })
    $static = Invoke-WebNative $node @('web-preview/build-static.mjs', '--check') $Ctx.Root
    $steps.Add([pscustomobject]@{ Label = 'build-static --check'; Code = $static.Code; Lines = $static.Lines })
    $all = Invoke-WebNative $node @('web-preview/tests/run-all.mjs', $Ctx.Root) $Ctx.Root
    $steps.Add([pscustomobject]@{ Label = '冒烟 run-all'; Code = $all.Code; Lines = $all.Lines })
    $skipped = @($all.Lines | Where-Object { $_ -cmatch '^\[跳过(项)?\] ' } | ForEach-Object { ($_ -replace '^\[跳过(项)?\] ', '') })
    Resolve-WebResult $steps (Get-WebSmokeCounts $all.Lines) $skipped
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
}

function Register-RinneGates {
    @{ Id = 'G9-web'; Tier = 'Full'; Name = '页面冒烟'; Lock = 'gui'; Run = ${function:Invoke-WebSmoke} }
}
