#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/tests.json
#
# 断言 ctest 真正吃的那份清单里，每个 add_test 都带 TIMEOUT 属性。
#   pwsh scripts/check-ctest-timeouts.ps1 -SelfTest             判据自测
#   pwsh scripts/check-ctest-timeouts.ps1 -BuildDir <构建目录>   实扫
# 为什么要有它：新加一个测试、忘了写 TIMEOUT，它会静默退回 ctest 默认的 1500 秒，而这在本机与 CI 上都不会红
# （命令行的 --timeout 只对没有属性的测试生效，等于给了一个「看起来有上界」的假象）。
# 为什么读生成物、不读 CMakeLists：属性可以写在任何一个 CMakeLists.txt 里，也可以由开关决定进不进集合，手边的文本回答不了
# 「谁有属性」；构建目录里各处的 CTestTestfile.cmake 才是 ctest 真正读的。
# 判负三条，缺一条门禁就会静默变绿：
#   有 add_test 没有 TIMEOUT  → 判负并点名；
#   一个 CTestTestfile 都找不到 → 判负（没配置过的构建目录会让判据恒真）；
#   找到文件但一个测试都没有   → 判负（空集合会让「全部都有」恒真）。
# 排除 FetchContent 拉下来的第三方（路径里有 _deps 一段）：它的测试不归本仓管、也改不动。

param(
    [string]$BuildDir = 'build',
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# 路径里有没有 _deps 这一段。先把两种分隔符统一成 /，再用通配比较，不用正则，免得转义和反斜杠的坑；
# 两侧都要求分隔符，my_deps_here 这种同名子串不算第三方。
function Test-ThirdPartyPath {
    param([string]$FullName)
    $norm = $FullName.Replace([char]92, '/')
    return ($norm -like '*/_deps/*')
}

# 判据本体：生产路径与 -SelfTest 共用这一个函数，自测证明的才是门禁本身。
function Get-CtestTimeoutReport {
    param([string[]]$Contents)

    $declared = New-Object 'System.Collections.Generic.HashSet[string]'
    $withTimeout = New-Object 'System.Collections.Generic.HashSet[string]'

    foreach ($text in $Contents) {
        foreach ($m in [regex]::Matches($text, 'add_test\(\[=\[([^\]]+)\]=\]')) {
            [void]$declared.Add($m.Groups[1].Value)
        }
        foreach ($m in [regex]::Matches($text, 'set_tests_properties\(\[=\[([^\]]+)\]=\]\s+PROPERTIES\s')) {
            $name = $m.Groups[1].Value
            # 属性尾巴按「名 值」逐对读，只认落在属性名位置上的 TIMEOUT：同一行里 _BACKTRACE_TRIPLES 的值是带引号的路径，
            # 路径里恰好有 timeout 字样（工作目录名）时，整行 grep 会把没有上界的测试判成有。
            $tail = $text.Substring($m.Index + $m.Length)
            $pos = 0
            while ($pos -lt $tail.Length) {
                $pair = [regex]::Match($tail.Substring($pos), '^\s*([A-Za-z_][A-Za-z0-9_]*)\s+("(?:[^"\\]|\\.)*"|[^\s)]+)')
                if (-not $pair.Success) { break }
                if ($pair.Groups[1].Value -ceq 'TIMEOUT') { [void]$withTimeout.Add($name); break }
                $pos += $pair.Length
            }
        }
    }

    [pscustomobject]@{
        Declared    = @($declared | Sort-Object)
        WithTimeout = @($withTimeout | Sort-Object)
        Missing     = @($declared | Where-Object { -not $withTimeout.Contains($_) } | Sort-Object)
    }
}

if ($SelfTest) {
    # 夹具现场拼装，不读仓、不写进任何被扫描的路径
    $q = [char]34
    $mk = {
        param([string]$name, [string]$props)
        ('add_test([=[{0}]=] "C:/x/{0}.exe")' -f $name) + "`n" +
        $(if ($props) { ('set_tests_properties([=[{0}]=] PROPERTIES  {1})' -f $name, $props) + "`n" } else { '' })
    }
    $fixOk = & $mk 'fix_has_timeout' ('TIMEOUT {0}30{0} _BACKTRACE_TRIPLES {0}C:/x/CMakeLists.txt;1;add_test;{0}' -f $q)
    $fixMissing = & $mk 'fix_no_timeout' ('WILL_FAIL {0}FALSE{0} _BACKTRACE_TRIPLES {0}C:/x/CMakeLists.txt;2;add_test;{0}' -f $q)
    $fixNoProps = & $mk 'fix_no_properties' ''
    # TIMEOUT 只出现在 _BACKTRACE_TRIPLES 的值里
    $fixInValue = & $mk 'fix_timeout_in_path' ('WILL_FAIL {0}FALSE{0} _BACKTRACE_TRIPLES {0}C:/agents/TIMEOUT-repro/CMakeLists.txt;3;add_test;{0}' -f $q)
    # 属性名大小写不对不算（ctest 的属性名区分大小写）
    $fixLower = & $mk 'fix_lowercase' ('timeout {0}30{0}' -f $q)

    $failures = @()
    $r1 = Get-CtestTimeoutReport -Contents @($fixOk)
    if ($r1.Missing.Count -ne 0) { $failures += ('有 TIMEOUT 的夹具被误判成缺失：{0}' -f ($r1.Missing -join ', ')) }
    $r2 = Get-CtestTimeoutReport -Contents @($fixMissing)
    if ($r2.Missing -notcontains 'fix_no_timeout') { $failures += '有属性但没有 TIMEOUT 的夹具没被抓到，判据没有牙' }
    $r3 = Get-CtestTimeoutReport -Contents @($fixNoProps)
    if ($r3.Missing -notcontains 'fix_no_properties') { $failures += '完全没有属性的夹具没被抓到，判据没有牙' }
    $r4 = Get-CtestTimeoutReport -Contents @($fixInValue)
    if ($r4.Missing -notcontains 'fix_timeout_in_path') { $failures += 'TIMEOUT 出现在属性值里被当成属性名，逐对读失效' }
    $r5 = Get-CtestTimeoutReport -Contents @($fixLower)
    if ($r5.Missing -notcontains 'fix_lowercase') { $failures += '小写的 timeout 被当成了 TIMEOUT' }
    # 混合：只有该红的那个红，免得「一红全红」蒙混过关；声明在不同文件里也要合并判断
    $r6 = Get-CtestTimeoutReport -Contents @($fixOk, $fixMissing)
    if (($r6.Missing -join ',') -ne 'fix_no_timeout') { $failures += ('混合夹具的缺失集合不对：[{0}]' -f ($r6.Missing -join ', ')) }
    $r7 = Get-CtestTimeoutReport -Contents @()
    if ($r7.Declared.Count -ne 0) { $failures += '空输入应当没有任何声明' }

    # 第三方路径判定四格；反斜杠那格是旧写法漏掉的
    $bs = [char]92
    $pathCases = @(
        @{ p = ('C:' + $bs + 'x' + $bs + 'build' + $bs + '_deps' + $bs + 'catch2-build' + $bs + 'CTestTestfile.cmake'); want = $true; why = '反斜杠路径下的 _deps 必须被排除' },
        @{ p = 'C:/x/build/_deps/catch2-build/CTestTestfile.cmake'; want = $true; why = '正斜杠路径下的 _deps 必须被排除' },
        @{ p = ('C:' + $bs + 'x' + $bs + 'build' + $bs + 'tests' + $bs + 'CTestTestfile.cmake'); want = $false; why = '本仓自己的测试目录不得被排除' },
        @{ p = ('C:' + $bs + 'x' + $bs + 'my_deps_here' + $bs + 'CTestTestfile.cmake'); want = $false; why = '同名子串（两侧无分隔符）不得被排除' }
    )
    foreach ($c in $pathCases) {
        $got = Test-ThirdPartyPath $c.p
        if ($got -ne $c.want) { $failures += ('第三方路径判定错：{0}（期望 {1}，实得 {2}）：{3}' -f $c.p, $c.want, $got, $c.why) }
    }

    if ($failures.Count -gt 0) {
        Write-Host '[FAIL] check-ctest-timeouts -SelfTest：' -ForegroundColor Red
        $failures | ForEach-Object { Write-Host ('  ' + $_) -ForegroundColor Red }
        exit 1
    }
    Write-Host 'check-ctest-timeouts -SelfTest：全部通过（有属性不误报、缺属性抓得到、值里的 TIMEOUT 不算、小写不算、混合只红该红的、_deps 两种分隔符都排除、本仓目录与同名子串不排除）'
    exit 0
}

$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
if (-not (Test-Path -LiteralPath $buildPath)) {
    Write-Host ('[FAIL] 构建目录不存在：{0}。这不是跳过：没有生成物就无从判定谁有上界' -f $buildPath) -ForegroundColor Red
    exit 1
}

$files = @(Get-ChildItem -LiteralPath $buildPath -Filter 'CTestTestfile.cmake' -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { -not (Test-ThirdPartyPath $_.FullName) })
if ($files.Count -eq 0) {
    Write-Host ('[FAIL] {0} 下找不到 CTestTestfile.cmake，先配置一次' -f $buildPath) -ForegroundColor Red
    exit 1
}

$report = Get-CtestTimeoutReport -Contents @($files | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw })
if ($report.Declared.Count -eq 0) {
    Write-Host ('[FAIL] 读了 {0} 份 CTestTestfile.cmake，一个 add_test 都没有。空集合会让本判据恒真' -f $files.Count) -ForegroundColor Red
    exit 1
}
if ($report.Missing.Count -gt 0) {
    Write-Host '[FAIL] 下列测试没有 TIMEOUT 属性，会静默吃 ctest 默认的 1500 秒：' -ForegroundColor Red
    $report.Missing | ForEach-Object { Write-Host ('  ' + $_) -ForegroundColor Red }
    Write-Host '修法：在它的 add_test 旁写 set_tests_properties(<名> PROPERTIES TIMEOUT <秒>)；套件走 tests/CMakeLists.txt 的 rinne_add_test_suite 会自带。' -ForegroundColor Yellow
    exit 1
}

Write-Host ('ctest 上界属性完备：{0} 条测试全部有 TIMEOUT（{1}）' -f $report.Declared.Count, ($report.Declared -join '、'))
exit 0
