#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/tests.json
#
# 断言 ctest 会跑的每一条测试都带正数的 TIMEOUT 属性，且不超过上限。
#   pwsh scripts/check-ctest-timeouts.ps1 -SelfTest                                      判据自测
#   pwsh scripts/check-ctest-timeouts.ps1 -BuildDir <构建目录> [-Config Release] [-Ctest <ctest>]   实扫
# 为什么要有它：新加一个测试、忘了写 TIMEOUT，它会静默退回 ctest 默认的 1500 秒；写成 0 更糟，CMake 文档写明显式的 0 就是
# 不设上限。这两种在本机与 CI 上都不会红（命令行的 --timeout 只对没有属性的测试生效）。
# 测试全集取自 `ctest --show-only=json-v1`，也就是 ctest 真正会跑的那一份：经 TEST_INCLUDE_FILES 注册的测试
# （catch_discover_tests 就是这样来的）、名字里带方括号的测试，都在里面；不读 CMakeLists，也不扒生成文件的文本。
# 判负几条，缺一条门禁就会静默变绿：
#   某条测试没有 TIMEOUT、TIMEOUT 不是数、不大于 0、或超过上限  → 判负并点名；
#   ctest 本身失败、输出不是 json-v1                            → 判负；
#   一条测试都没有                                                → 判负（空集合会让「全部都有」恒真）。
# 第三方（FetchContent 拉下来的）测试若有，同样要求 TIMEOUT：改不动它就别让它进 ctest。

param(
    [string]$BuildDir = 'build',
    [string]$Config = 'Release',
    [string]$Ctest = 'ctest',
    [int]$MaxTimeout = 1800,
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# 判据本体：实扫与 -SelfTest 共用这一个函数，自测证明的才是门禁本身。
# 参数是 ctest --show-only=json-v1 的输出文本与 ctest 的退出码；返回 Ok、点名的 Lines 与测试条数 Count。
function Test-CtestTimeouts {
    param([string]$Json, [int]$ExitCode, [int]$Max)

    $fail = { param($msg) [pscustomobject]@{ Ok = $false; Lines = @($msg); Count = 0 } }
    if ($ExitCode -ne 0) { return (& $fail "ctest --show-only=json-v1 失败，退出码 $ExitCode") }
    try { $doc = $Json | ConvertFrom-Json -ErrorAction Stop } catch { return (& $fail "ctest 的输出不是 JSON：$($_.Exception.Message)") }
    if ($null -eq $doc -or $null -eq $doc.PSObject.Properties['tests']) { return (& $fail 'ctest 的输出里没有 tests，不是 json-v1') }
    $tests = @($doc.tests)
    if ($tests.Count -eq 0) { return (& $fail 'ctest 一条测试都没有。空集合会让本判据恒真') }

    $lines = @()
    foreach ($t in $tests) {
        $name = [string]$t.name
        $prop = $null
        if ($t.PSObject.Properties['properties']) { $prop = @($t.properties) | Where-Object { $_.name -ceq 'TIMEOUT' } | Select-Object -First 1 }
        if ($null -eq $prop) { $lines += "${name}：没有 TIMEOUT，会静默吃 ctest 默认的 1500 秒"; continue }
        $value = 0.0
        if (-not [double]::TryParse([string]$prop.value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$value)) {
            $lines += "${name}：TIMEOUT 的值不是数：$($prop.value)"
        } elseif ($value -le 0) {
            $lines += "${name}：TIMEOUT 是 $($prop.value)，必须大于 0（0 就是不设上限）"
        } elseif ($value -gt $Max) {
            $lines += "${name}：TIMEOUT 是 $($prop.value)，超过上限 $Max"
        }
    }
    [pscustomobject]@{ Ok = ($lines.Count -eq 0); Lines = $lines; Count = $tests.Count; Names = @($tests | ForEach-Object { [string]$_.name }) }
}

if ($SelfTest) {
    # 夹具现场拼装，不读仓、不碰任何被扫描的路径
    $doc = {
        param([object[]]$tests)
        (@{ kind = 'ctestInfo'; version = @{ major = 1; minor = 0 }; tests = @($tests) } | ConvertTo-Json -Depth 8)
    }
    $t = {
        param([string]$name, $timeout, [switch]$noProps, [switch]$otherProps, [string]$propName = 'TIMEOUT')
        $o = @{ name = $name; config = 'Release' }
        if (-not $noProps) {
            $props = @()
            if ($otherProps) { $props += @{ name = 'WILL_FAIL'; value = $false }; $props += @{ name = 'ENVIRONMENT'; value = @('TIMEOUT=30') } }
            if ($null -ne $timeout) { $props += @{ name = $propName; value = $timeout } }
            $o.properties = $props
        }
        $o
    }
    $judge = { param($json, [int]$code = 0) Test-CtestTimeouts -Json $json -ExitCode $code -Max 1800 }

    $script:failures = @()
    $expectRed = {
        param($label, $r, $needle)
        if ($r.Ok) { $script:failures += "$label：应当判红却通过了，判据没有牙" }
        elseif ($needle -and -not ($r.Lines -join "`n").Contains($needle)) { $script:failures += "$label：红了，但没有点名「$needle」：$($r.Lines -join ' / ')" }
    }

    $r = & $judge (& $doc @((& $t 'good' 30)))
    if (-not $r.Ok) { $script:failures += "有 TIMEOUT 的测试被误判：$($r.Lines -join ' / ')" }
    $r = & $judge (& $doc @((& $t 'a' 30), (& $t 'b' 1800)))
    if (-not $r.Ok -or $r.Count -ne 2) { $script:failures += '两条都合格的测试被误判，或条数不对' }

    & $expectRed '没有任何属性' (& $judge (& $doc @((& $t 'plain' $null -noProps)))) 'plain'
    & $expectRed '有别的属性但没有 TIMEOUT' (& $judge (& $doc @((& $t 'other' $null -otherProps)))) 'other'
    # TIMEOUT 字样只出现在别的属性的值里（环境变量）：不能算
    & $expectRed 'TIMEOUT 只出现在属性值里' (& $judge (& $doc @((& $t 'invalue' $null -otherProps)))) 'invalue'
    & $expectRed '属性名大小写不对（ctest 的属性名区分大小写）' (& $judge (& $doc @((& $t 'lower' 30 -propName 'timeout')))) 'lower'
    & $expectRed 'TIMEOUT 为 0' (& $judge (& $doc @((& $t 'zero' 0)))) 'zero'
    & $expectRed 'TIMEOUT 为负' (& $judge (& $doc @((& $t 'neg' -5)))) 'neg'
    & $expectRed 'TIMEOUT 超过上限' (& $judge (& $doc @((& $t 'huge' 1801)))) 'huge'
    & $expectRed 'TIMEOUT 不是数' (& $judge (& $doc @((& $t 'text' 'soon')))) 'text'
    # 名字里带方括号：旧的按文本扒 add_test([=[名]=] 的写法在这里读漏
    & $expectRed '名字里带方括号且没有 TIMEOUT' (& $judge (& $doc @((& $t 'bracket[1]' $null -noProps)))) 'bracket[1]'
    $r = & $judge (& $doc @((& $t 'bracket[2]' 60)))
    if (-not $r.Ok) { $script:failures += "名字里带方括号且有 TIMEOUT 的测试被误判：$($r.Lines -join ' / ')" }
    # 混合：只点名该红的
    $r = & $judge (& $doc @((& $t 'fine' 30), (& $t 'bad_zero' 0), (& $t 'bad_none' $null -noProps)))
    if ($r.Ok -or $r.Lines.Count -ne 2 -or ($r.Lines -join ' ').Contains('fine')) { $script:failures += "混合夹具应当恰好点名 2 条：$($r.Lines -join ' / ')" }
    # 主流程里的三条判负
    & $expectRed '一条测试都没有' (& $judge (& $doc @())) '一条测试都没有'
    & $expectRed 'ctest 本身失败' (& $judge (& $doc @((& $t 'x' 30))) 1) '退出码 1'
    & $expectRed '输出不是 JSON' (& $judge 'not json') 'JSON'
    & $expectRed '输出不是 json-v1（没有 tests）' (& $judge '{"kind":"other"}') 'tests'

    if ($script:failures.Count -gt 0) {
        Write-Host '[FAIL] check-ctest-timeouts -SelfTest：' -ForegroundColor Red
        $script:failures | ForEach-Object { Write-Host ('  ' + $_) -ForegroundColor Red }
        exit 1
    }
    Write-Host 'check-ctest-timeouts -SelfTest：全部通过（缺属性、0、负数、超上限、非数、值里的 TIMEOUT、方括号名字、混合、空集合、ctest 失败、非 JSON、非 json-v1 都判得出）'
    exit 0
}

$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
if (-not (Test-Path -LiteralPath $buildPath)) {
    Write-Host ('[FAIL] 构建目录不存在：{0}。这不是跳过：没有生成物就无从判定谁有上界' -f $buildPath) -ForegroundColor Red
    exit 1
}

$json = (& $Ctest --test-dir $buildPath -C $Config --show-only=json-v1 2>&1 | Out-String)
$report = Test-CtestTimeouts -Json $json -ExitCode $LASTEXITCODE -Max $MaxTimeout
if (-not $report.Ok) {
    Write-Host '[FAIL] ctest 的 TIMEOUT 门禁不通过：' -ForegroundColor Red
    $report.Lines | ForEach-Object { Write-Host ('  ' + $_) -ForegroundColor Red }
    Write-Host '修法：在 add_test 旁写 set_tests_properties(<名> PROPERTIES TIMEOUT <秒>)；套件走 tests/CMakeLists.txt 的 rinne_add_test_suite 会自带。' -ForegroundColor Yellow
    exit 1
}
Write-Host ('ctest 上界属性完备：{0} 条测试全部有正数的 TIMEOUT（{1}）' -f $report.Count, ($report.Names -join '、'))
exit 0
