#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 压测 rinne_stress 与它的两个变异构建。本机 Full 档 gate（scripts/gates.d/40-stress.ps1）与手动长跑共用这一份：
#   pwsh scripts/stress.ps1 [-BuildDir build] [-Config Release] [-Scale 15] [-Mutations] [-Jobs 2] [-LogDir <目录>] [-Ninja <exe>]
#                           [-RunTimeoutSeconds <秒>] [-NoStampCheck]
#   pwsh scripts/stress.ps1 -Lint        只检查 tests/tsan.supp
#   pwsh scripts/stress.ps1 -SelfTest
# 三件事，按这个顺序，前一件失败也接着做下一件，一次报全：
#   1. tsan.supp 的理由检查：每一条抑制前面紧挨着一行「# 理由：…」，写明库与版本、为什么是误报、对应哪段代码；没写理由的
#      判红（处置是删掉那一条）；类型必须是 TSan 认得的，也不许点到第一方代码（rinne、synchain、src/、tests/、tools/ 的符号或路径）。
#      当前清单没有任何抑制，检查空转通过；它存在是为了让以后加的第一条不能不写理由。
#   2. 跑构建目录里已经建好的 rinne_stress（<构建目录>/tests/<配置>/rinne_stress.exe）。环境变量 RINNE_STRESS_SCALE 设成 -Scale，
#      压测的每一轮最短与封顶时长都乘它（tests/stress/StressScale.h），同一批用例跑得更久、撞上更多交错；规模 1 就是 ctest 里那一遍。
#      通过 = 退出码 0，并且 Catch2 的汇总行是「All tests passed (N assertions in M test cases)」、用例数不少于 $MinCases。
#   3. -Mutations：用 RINNE_CORE_ONLY 配置（不需要 JUCE）各建一个变异构建，跑同一个压测：
#        no_intent       去掉写方的 write_intent：任何 CPU 上都必须变红。
#        no_read_fence   去掉读侧的 acquire fence：只在弱内存序的 CPU（arm64）上抓得到，必须变红；强序的 x86 上去掉它没有
#                        可见效果，压测自己以「证据不成立」（退出码 70）结束，这里认这个结果，并写明待 arm64 确认。
#      变异的判据与 scripts/ci/mac/expect-red.sh 的 --catch2 模式一致：退出码非 0、有失败断言行（`: FAILED:`）、有跑完才打印的汇总行
#      （`assertions: … failed`）、不是崩溃或未捕获异常；退出码 0 是变异没被抓住，崩溃、被杀、什么都没跑一律判红，不当成「抓住了」。
#      变异构建放在 <构建目录>/stress-mutations/<名字>，复用主构建里已经取下的 Catch2 源码（不联网）。
# 压测的二进制必须是当前 HEAD 的构建（<构建目录>/rinne-build.json 里的 sha 与 HEAD 一致），否则退出码 2；手动跑别处建的目录
# 加 -NoStampCheck。mac 的 arm64 压测与变异构建继续走 scripts/ci/mac/stress.sh，两处共用同一套 expect-red 判据。
# 输出：每一步的日志在 -LogDir（缺省 <构建目录>/logs/stress），屏幕上只打印步骤与失败步骤日志的末尾；最后一行是「完成：…」或「失败：…」。
# 起子进程时本进程的标准输出与标准错误不可继承（scripts/lib/StdHandles.psm1），编译器留下的常驻进程拖不住调用方的管道。
# 开跑前从本进程删掉凭据与个人针变量（判据是 build.ps1 的 $SecretEnv，整个仓库只有那一份）。
# 退出码：0 通过；1 失败；2 没有可跑的压测（构建目录里没有当前 HEAD 的 rinne_stress）。tsan.supp 检查已经失败时，
# 没有可跑的压测也报 1：2 在 gate 里记 SKIP，不能让它把前面已经记下的失败吞掉。
[CmdletBinding()]
param(
    [string]$BuildDir = 'build', [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [ValidateRange(1, 600)][int]$Scale = 1, [switch]$Mutations, [ValidateRange(1, 64)][int]$Jobs = 2, [string]$LogDir = '', [string]$Ninja = '',
    [int]$RunTimeoutSeconds = 0, [switch]$NoStampCheck, [switch]$Lint, [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) } catch { $null = $_ }
$Root = Split-Path -Parent $PSScriptRoot
$TsanSupp = Join-Path $Root 'tests/tsan.supp'
# 压测至少要跑这么多个用例才算数：有人把用例删光、或过滤成空，汇总行仍是「All tests passed」，数目不够就判红。
$MinCases = 6
$MutationNames = @('no_intent', 'no_read_fence')
# 强内存序的 CPU：去掉读侧 fence 在上面没有可见效果
$StrongOrderArch = @('X64', 'X86')
$StressTimeoutBase = 300
$StressTimeoutPerScale = 30

# ---------- tsan.supp ----------

# TSan 抑制文件认得的行类型（clang 文档：race、race_top、thread、mutex、signal、deadlock、called_from_lib）
$SuppTypes = 'race', 'race_top', 'thread', 'mutex', 'signal', 'deadlock', 'called_from_lib'
# 第一方代码的痕迹：命名空间或仓名、源码目录。抑制只许点到第三方库（Catch2、标准库）内部。
$FirstPartyHint = '(?i)rinne|synchain|[/\\]src[/\\]|[/\\]tests[/\\]|[/\\]tools[/\\]|^(?:src|tests|tools)[/\\]'

# 检查 tsan.supp 的全部行，返回问题列表（空 = 合格）。每个条目（非空、非 # 开头的行）必须：
#   - 形如 <类型>:<模式>，类型在 $SuppTypes 里，模式不是光秃秃的通配（*）；
#   - 模式没有第一方的痕迹；
#   - 紧挨着它上面的一段连续的 # 注释里，有一行以「# 理由：」开头且后面有内容（中间不隔空行，也不隔别的条目：
#     每个条目有自己的理由，两个条目不共用一段）。
function Test-TsanSuppLines([string[]]$Lines) {
    $problems = [System.Collections.Generic.List[string]]::new()
    $reason = $false   # 紧挨着当前行上面的注释段里有没有合格的理由
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i].Trim()
        $n = $i + 1
        if ($line -eq '') { $reason = $false; continue }
        if ($line.StartsWith('#')) {
            # 注释段从空行或条目之后重新开始，所以只要这一行是理由就记下，空行与条目会把标记清掉
            if ($line -match '^#\s*理由[：:]\s*\S') { $reason = $true }
            continue
        }
        $m = [regex]::Match($line, '^(?<type>[a-z_]+):(?<pattern>.+)$')
        if (-not $m.Success) { $problems.Add("第 ${n} 行不是 <类型>:<模式>：$line"); $reason = $false; continue }
        if ($m.Groups['type'].Value -cnotin $SuppTypes) { $problems.Add("第 ${n} 行的类型 '$($m.Groups['type'].Value)' 不是 TSan 认得的：$line") }
        $pattern = $m.Groups['pattern'].Value
        if ($pattern.Trim('*') -eq '') { $problems.Add("第 ${n} 行的模式只有通配符，等于关掉整一类报告：$line") }
        if ($pattern -match $FirstPartyHint) { $problems.Add("第 ${n} 行点到了第一方代码（绝不为第一方代码里的竞争加抑制，要修）：$line") }
        if (-not $reason) { $problems.Add("第 ${n} 行没有理由：紧挨着它上面要有一行「# 理由：…」，写明库与版本、为什么是误报、对应哪段代码；写不出理由就删掉这一条：$line") }
        $reason = $false
    }
    , @($problems)
}

function Invoke-SuppLint {
    if (-not (Test-Path -LiteralPath $TsanSupp -PathType Leaf)) { Write-Host "tsan.supp：找不到 $TsanSupp"; return $false }
    $lines = @(Get-Content -LiteralPath $TsanSupp)
    $problems = Test-TsanSuppLines $lines
    $entries = @($lines | Where-Object { $_.Trim() -ne '' -and -not $_.Trim().StartsWith('#') }).Count
    if ($problems.Count) {
        Write-Host "tsan.supp：$($problems.Count) 处不合格"
        foreach ($p in $problems) { Write-Host "    $p" }
        return $false
    }
    Write-Host "tsan.supp：合格（$entries 条抑制，每条都有理由）"
    $true
}

# ---------- 判据 ----------

# Catch2 的汇总：通过时一行「All tests passed (N assertions in M test cases)」，失败时有「test cases:」「assertions:」两行。
# 返回 @{ Passed; Assertions; Cases }；找不到汇总行时 Passed 为 $false、计数为 0。
function Get-CatchSummary([string[]]$Lines) {
    foreach ($l in $Lines) {
        $m = [regex]::Match("$l", '^All tests passed \((\d+) assertions? in (\d+) test cases?\)\s*$')
        if ($m.Success) { return @{ Passed = $true; Assertions = [int]$m.Groups[1].Value; Cases = [int]$m.Groups[2].Value } }
    }
    @{ Passed = $false; Assertions = 0; Cases = 0 }
}

# 压测本身的结论。退出码 -1 = 超时被结束。返回 @{ Ok; Reason; Cases; Assertions }。
function Get-StressVerdict([int]$ExitCode, [string[]]$Lines, [int]$MinimumCases) {
    $s = Get-CatchSummary $Lines
    $v = { param($ok, $why) @{ Ok = $ok; Reason = $why; Cases = $s.Cases; Assertions = $s.Assertions } }
    if ($ExitCode -eq -1) { return & $v $false '超过时限被结束' }
    if ($ExitCode -ne 0) {
        $tail = @($Lines | Where-Object { $_ -match '^(?:test cases|assertions):' }) -join '；'
        return & $v $false "退出码 $ExitCode$($tail ? "（$tail）" : '，输出里没有 Catch2 的汇总行')"
    }
    if (-not $s.Passed) { return & $v $false '退出码 0 但输出里没有「All tests passed (…)」汇总行（没跑完？）' }
    if ($s.Cases -lt $MinimumCases) { return & $v $false "只跑了 $($s.Cases) 个用例，少于 $MinimumCases 个（用例被删或被过滤掉了？）" }
    if ($s.Assertions -lt 1) { return & $v $false '一条断言都没有' }
    & $v $true "$($s.Cases) 个用例，$($s.Assertions) 条断言"
}

# 一个变异构建跑出来的结论，判据与 expect-red.sh 的 --catch2 模式一致（见头部注释）。返回 @{ Ok; Verdict; Reason }：
#   Verdict  CAUGHT（红，抓住了）、NO_EVIDENCE（压测自己说证据不成立，退出码 70）、SURVIVED（退出码 0，没抓住）、BROKEN（崩溃、被杀、
#            超时、什么都没跑：不能当成抓住了）
#   Ok       no_intent 只认 CAUGHT；no_read_fence 在弱序 CPU 上只认 CAUGHT，在强序 CPU 上 CAUGHT 与 NO_EVIDENCE 都认
#            （后者要写明待 arm64 确认，见 Reason）。
function Get-MutationVerdict([string]$Name, [int]$ExitCode, [string[]]$Lines, [string]$Arch) {
    $text = $Lines -join "`n"
    $verdict = if ($ExitCode -eq 0) { 'SURVIVED' }
    elseif ($ExitCode -eq -1) { 'BROKEN' }
    elseif ($text -match ': FAILED:' -and $text -match '(?m)^assertions:.* failed' -and $text -notmatch 'fatal error condition|unexpected exception') { 'CAUGHT' }
    elseif ($ExitCode -eq 70 -and $text -match '证据不成立') { 'NO_EVIDENCE' }
    else { 'BROKEN' }
    $weakOrder = $Arch -cnotin $StrongOrderArch
    $ok = switch ($verdict) {
        'CAUGHT' { $true }
        'NO_EVIDENCE' { $Name -ceq 'no_read_fence' -and -not $weakOrder }
        default { $false }
    }
    $reason = switch ($verdict) {
        'CAUGHT' { '红，抓住了' }
        'NO_EVIDENCE' {
            if ($ok) { "证据不成立（$Arch 是强内存序，去掉读侧 fence 没有可见效果）：待 arm64 的 dispatch 确认这个变异变红" }
            else { "证据不成立，但 $Name 在 $Arch 上必须变红" }
        }
        'SURVIVED' { '退出码 0：压测没有抓住这个变异' }
        default { ($ExitCode -eq -1) ? '超过时限被结束' : "退出码 $ExitCode：崩溃、被杀或什么都没跑，不能算抓住了" }
    }
    @{ Ok = $ok; Verdict = $verdict; Reason = $reason }
}

# ---------- 子进程 ----------

# Start-Process 只把参数用空格拼起来：按 Windows 命令行规则给含空白或引号的参数加引号，引号前与结尾的反斜杠加倍。
# （与 build.ps1 同一写法）
function ConvertTo-CommandLine([string[]]$Arguments) {
    @($Arguments | ForEach-Object {
            if ($_ -and $_ -notmatch '[\s"]') { $_ } else { '"' + (($_ -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"' }
        }) -join ' '
}

# 结束整棵进程树。进程已经退出（InvalidOperationException）、没有权限（Win32Exception）一类任何失败都吞掉：
# 调用方照样按超时返回 -1，不能把超时变成没人接的异常
function Stop-ProcessTree($Process) {
    try { $Process.Kill($true) } catch { $null = $_ }
}

# 跑一个外部命令，标准输出与标准错误进 $Log 与 $Log.err（文件，不是管道）；只等它自己退出，不等它留下的子进程；
# 到点结束整棵进程树。返回退出码，超时返回 -1。
function Invoke-Logged([string]$Exe, [string[]]$Arguments, [string]$Log, [int]$TimeoutSeconds) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Log)
    $start = @{ FilePath = $Exe; WorkingDirectory = $Root; NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $Log; RedirectStandardError = "$Log.err" }
    if ($Arguments.Count) { $start.ArgumentList = ConvertTo-CommandLine $Arguments }
    $p = Start-Process @start
    $null = $p.Handle   # 先拿住句柄，退出后才读得到退出码
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-ProcessTree $p
        return -1
    }
    $p.ExitCode
}

function Get-LogLines([string]$Log) {
    @(foreach ($f in $Log, "$Log.err") { if (Test-Path -LiteralPath $f) { Get-Content -LiteralPath $f } })
}

# 跑一步构建命令，失败时打印日志末尾并抛错
function Invoke-BuildStep([string]$Name, [string]$Exe, [string[]]$Arguments, [int]$TimeoutSeconds) {
    Write-Host "== $Name"
    $log = Join-Path $script:StressLogDir "$Name.log"
    $code = Invoke-Logged $Exe $Arguments $log $TimeoutSeconds
    if ($code -eq 0) { return }
    Get-LogLines $log | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
    throw ($code -eq -1 ? "$Name 超过 $TimeoutSeconds 秒，已结束" : "$Name 退出码 $code（日志 $log）")
}

# 凭据与个人针：整个仓库只有 build.ps1 里那一份判据。在函数作用域里点开它（它遇到点调用就只定义函数与变量后返回），
# 它的形参不会盖掉本脚本的变量。
function Clear-SecretEnv {
    . (Join-Path $PSScriptRoot 'build.ps1')
    Remove-EnvMatching $SecretEnv $SecretEnvValue
}

# MSVC 环境：cl 不在 PATH 上时按 vswhere 找到 VS，载入它的 64 位开发者环境（只改本进程）。
function Enter-MsvcShell {
    if (-not $IsWindows -or (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "找不到 vswhere：$vswhere" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'vswhere 没找到带 VC 工具集的 VS 安装' }
    $env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"   # vcvars 自己也调 vswhere
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    foreach ($l in (& cmd.exe /d /c "`"$bat`" >nul && set")) {
        if ($l -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
    $cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
    if (-not $cl -or $cl -notmatch '\\bin\\Hostx64\\x64\\') { throw "cl.exe 不是 64 位宿主的 64 位编译器：$cl" }
}

function Get-Setting([string]$Value, [string]$EnvName, [string]$GitKey, [string]$Fallback = '') {
    if ($Value) { return $Value }
    $v = [Environment]::GetEnvironmentVariable($EnvName)
    if (-not $v) { $v = "$(git -C $Root config --get $GitKey 2>$null)".Trim() }
    $v ? $v : $Fallback
}

# ---------- 主流程 ----------

function Find-StressExe([string]$Dir) {
    $name = $IsWindows ? 'rinne_stress.exe' : 'rinne_stress'
    foreach ($c in (Join-Path $Dir "tests/$Config/$name"), (Join-Path $Dir "tests/$name")) { if (Test-Path -LiteralPath $c -PathType Leaf) { return $c } }
    # 主构建目录下的变异构建与第三方目录里的同名文件不算（按相对 $Dir 的路径判，$Dir 自己在变异目录里时不受影响）
    $hit = Get-ChildItem -LiteralPath $Dir -Recurse -File -Filter $name -ErrorAction SilentlyContinue |
        Where-Object { [IO.Path]::GetRelativePath($Dir, $_.FullName) -notmatch '^(?:stress-mutations|_deps)[\\/]|[\\/]_deps[\\/]' } | Select-Object -First 1
    $hit ? $hit.FullName : $null
}

# 构建目录里的构建记录是当前 HEAD 的吗？返回错误文字，对上了返回空。
function Test-BuildStamp([string]$Dir) {
    $stamp = Join-Path $Dir 'rinne-build.json'
    if (-not (Test-Path -LiteralPath $stamp -PathType Leaf)) { return "$Dir 里没有构建记录（rinne-build.json），先用 scripts/build.ps1 构建" }
    $head = "$(git -C $Root rev-parse HEAD 2>$null)".Trim()
    $built = (Get-Content -LiteralPath $stamp -Raw | ConvertFrom-Json).sha
    if ($built -cne $head) { return "构建记录是 $built 的，不是当前 HEAD $head 的" }
    ''
}

# 设置本进程的环境变量；$null 是删掉它。PowerShell 把 $null 传给 .NET 的字符串参数时会变成空串，变量就成了「存在但为空」，
# 要传真正的 null。
function Set-ProcessEnv([string]$Name, $Value) {
    [Environment]::SetEnvironmentVariable($Name, ($null -eq $Value ? [NullString]::Value : [string]$Value))
}

# 带环境变量跑压测（只给这一个子进程，跑完恢复）
function Invoke-StressExe([string]$Exe, [string]$Log, [int]$TimeoutSeconds, [hashtable]$Vars, [string[]]$Arguments = @()) {
    $saved = @{}
    foreach ($k in $Vars.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        Set-ProcessEnv $k $Vars[$k]
    }
    try { Invoke-Logged $Exe $Arguments $Log $TimeoutSeconds }
    finally { foreach ($k in $saved.Keys) { Set-ProcessEnv $k $saved[$k] } }
}

function Invoke-MutationBuild([string]$Name, [string]$MainBuild, [string]$NinjaExe) {
    $dir = Join-Path $MainBuild "stress-mutations/$Name"
    $cfg = @('-S', $Root, '-B', $dir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
        '-DRINNE_CORE_ONLY=ON', '-DRINNE_BUILD_TOOLS=OFF', "-DRINNE_STRESS_MUTATE=$Name")
    if ($IsWindows) { $cfg += '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl' }
    # 本机构建一律用带 D 的段名前缀（与维护者 DAW 里装的正式版互不相见）；压测不碰共享内存，只是口径一致
    if ($env:GITHUB_ACTIONS -ne 'true') { $cfg += '-DRINNE_SHM_PREFIX=SynchainRinneD' }
    # 主构建已经取下了 Catch2 的源码：复用它，不联网
    $catch2 = Join-Path $MainBuild '_deps/catch2-src'
    if (Test-Path -LiteralPath (Join-Path $catch2 'CMakeLists.txt')) { $cfg += "-DFETCHCONTENT_SOURCE_DIR_CATCH2=$catch2" }
    Invoke-BuildStep "mutation-$Name-configure" 'cmake' $cfg 900
    Invoke-BuildStep "mutation-$Name-build" 'cmake' @('--build', $dir, '--target', 'rinne_stress', '--parallel', "$Jobs") 3600
    $exe = Find-StressExe $dir
    if (-not $exe) { throw "变异构建 $Name 里找不到 rinne_stress" }
    $exe
}

function Invoke-Stress {
    $failures = [System.Collections.Generic.List[string]]::new()
    $notes = [System.Collections.Generic.List[string]]::new()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    # 没有可跑的压测：退出码 2（gate 记 SKIP）。但前面已经记下的失败（tsan.supp 不合格）不能被这个 2 吞掉，有失败就报 1
    $noStress = {
        param([string]$Why)
        Write-Host $Why
        if (-not $failures.Count) { return 2 }
        Write-Host "失败：$($failures -join '；')"
        1
    }

    Write-Host '== tsan.supp'
    if (-not (Invoke-SuppLint)) { $failures.Add('tsan.supp 有抑制没写理由') }

    $main = [IO.Path]::GetFullPath($BuildDir, $Root)
    if (-not (Test-Path -LiteralPath $main -PathType Container)) { return (& $noStress "没有构建目录 $main") }
    if (-not $NoStampCheck) {
        $stale = Test-BuildStamp $main
        if ($stale) { return (& $noStress $stale) }
    }
    $exe = Find-StressExe $main
    if (-not $exe) { return (& $noStress "$main 里没有 rinne_stress（这个构建没建压测目标？）") }
    $script:StressLogDir = $LogDir ? [IO.Path]::GetFullPath($LogDir, $Root) : (Join-Path $main 'logs/stress')
    $null = New-Item -ItemType Directory -Force -Path $script:StressLogDir

    $timeout = $RunTimeoutSeconds -gt 0 ? $RunTimeoutSeconds : ($StressTimeoutBase + $StressTimeoutPerScale * $Scale)
    Write-Host "== 压测（规模 $Scale，时限 $timeout 秒）：$exe"
    $log = Join-Path $script:StressLogDir 'rinne_stress.log'
    $t0 = $sw.Elapsed.TotalSeconds
    $code = Invoke-StressExe $exe $log $timeout @{ RINNE_STRESS_SCALE = "$Scale" }
    $v = Get-StressVerdict $code (Get-LogLines $log) $MinCases
    $stressSeconds = [Math]::Round($sw.Elapsed.TotalSeconds - $t0, 1)
    if ($v.Ok) { Write-Host "压测：通过（$($v.Reason)，$stressSeconds 秒）"; $notes.Add("压测规模 $Scale 通过（$($v.Reason)，$stressSeconds 秒）") }
    else {
        Get-LogLines $log | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
        Write-Host "压测：失败（$($v.Reason)；日志 $log）"
        $failures.Add("压测规模 $Scale：$($v.Reason)")
    }

    if ($Mutations) {
        $ninjaExe = (Get-Command (Get-Setting $Ninja 'RINNE_NINJA' 'rinne.ninja' 'ninja') -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
        $arch = [string][Runtime.InteropServices.RuntimeInformation]::OSArchitecture
        if (-not $ninjaExe) { $failures.Add('变异构建：找不到 ninja（传 -Ninja，或设 RINNE_NINJA / git config rinne.ninja）') }
        else {
            $env:VSLANG = '1033'
            # 进不了 MSVC 环境（找不到 vswhere、没有 VC 工具集）：把原因记进失败，不让异常带走前面已经算出的 tsan.supp 与压测
            # 结论和最后的「失败：/完成：」行；也不带着坏环境去跑变异循环
            $msvcError = $null
            try { Enter-MsvcShell } catch { $msvcError = $_.Exception.Message }
            if ($msvcError) {
                Write-Host "变异构建：进不了 MSVC 环境（$msvcError）"
                $failures.Add("变异构建：进不了 MSVC 环境（$msvcError）")
            }
            foreach ($name in ($msvcError ? @() : $MutationNames)) {
                try {
                    $m0 = $sw.Elapsed.TotalSeconds
                    $mexe = Invoke-MutationBuild $name $main $ninjaExe
                    Write-Host "== 变异 $name：$mexe"
                    $mlog = Join-Path $script:StressLogDir "mutation-$name-run.log"
                    # 变异构建恒按规模 1 跑（StressScale.h），这里把环境变量也去掉，免得读者以为放大了
                    $mcode = Invoke-StressExe $mexe $mlog 900 @{ RINNE_STRESS_SCALE = $null }
                    $mv = Get-MutationVerdict $name $mcode (Get-LogLines $mlog) $arch
                    $mSeconds = [Math]::Round($sw.Elapsed.TotalSeconds - $m0, 1)
                    if ($mv.Ok) { Write-Host "变异 ${name}：$($mv.Reason)（$mSeconds 秒）"; $notes.Add("变异 ${name}：$($mv.Reason)") }
                    else {
                        Get-LogLines $mlog | Select-Object -Last 30 | ForEach-Object { Write-Host "    $_" }
                        Write-Host "变异 ${name}：失败（$($mv.Reason)；日志 $mlog）"
                        $failures.Add("变异 ${name}：$($mv.Reason)")
                    }
                } catch {
                    Write-Host "变异 ${name}：构建失败（$($_.Exception.Message)）"
                    $failures.Add("变异 ${name}：构建失败（$($_.Exception.Message)）")
                }
            }
        }
    }

    $total = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
    if ($failures.Count) { Write-Host "失败：$($failures -join '；')（共 $total 秒）"; return 1 }
    Write-Host "完成：$($notes -join '；')（共 $total 秒）"
    0
}

# ---------- 自测 ----------

function Invoke-SelfTest {
    $cases = [System.Collections.Generic.List[object]]::new()
    $check = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $lint = { param($text) (Test-TsanSuppLines ($text -split "`n")).Count }

    # tsan.supp
    & $check 'tsan.supp：仓里的清单合格' $false ((Test-TsanSuppLines @(Get-Content -LiteralPath $TsanSupp)).Count -eq 0)
    & $check 'tsan.supp：只有注释与空行，合格（当前清单就是这样）' $false ((& $lint "# 说明`n`n# 理由：不是条目，只是注释`n") -eq 0)
    & $check 'tsan.supp：条目前面有「# 理由：」，合格' $false ((& $lint "# 理由：Catch2 3.5.4 的计时器线程读 started_ 没加锁，上游已知，见 catch_timer.cpp`nrace:Catch::Timer::getElapsedNanoseconds`n") -eq 0)
    & $check 'tsan.supp：理由段里有多行注释，其中一行是理由，合格' $false ((& $lint "# 来源：Catch2 3.5.4`n# 理由：上游已知的误报`n# 见 issue`nrace:Catch::Timer::start`n") -eq 0)
    & $check 'tsan.supp：条目没有任何注释，判红' $true ((& $lint "race:Catch::Timer::start`n") -eq 1)
    & $check 'tsan.supp：注释不是「理由：」开头，判红' $true ((& $lint "# 这里有个误报`nrace:Catch::Timer::start`n") -eq 1)
    & $check 'tsan.supp：「理由：」后面是空的，判红' $true ((& $lint "# 理由：`nrace:Catch::Timer::start`n") -eq 1)
    & $check 'tsan.supp：理由与条目之间隔了空行，判红' $true ((& $lint "# 理由：上游已知`n`nrace:Catch::Timer::start`n") -eq 1)
    & $check 'tsan.supp：两个条目共用一段理由，第二个判红' $true ((& $lint "# 理由：上游已知`nrace:Catch::Timer::start`nrace:Catch::Timer::stop`n") -eq 1)
    & $check 'tsan.supp：类型不认识，判红' $true ((& $lint "# 理由：上游已知`nsuppress:Catch::Timer::start`n") -ge 1)
    & $check 'tsan.supp：不是 <类型>:<模式>，判红' $true ((& $lint "# 理由：上游已知`nCatch::Timer::start`n") -ge 1)
    & $check 'tsan.supp：模式只有通配符，判红' $true ((& $lint "# 理由：全关掉`nrace:*`n") -ge 1)
    & $check 'tsan.supp：点到第一方符号（rinne 命名空间），判红' $true ((& $lint "# 理由：我觉得是误报`nrace:rinne::ipc::LoopRing::write`n") -ge 1)
    & $check 'tsan.supp：点到第一方路径（src/），判红' $true ((& $lint "# 理由：我觉得是误报`nrace:*/src/core/loop/LocalLine.cpp`n") -ge 1)
    & $check 'tsan.supp：called_from_lib 点到第三方库，合格' $false ((& $lint "# 理由：libc++ 内部的锁，TSan 看不到它的同步`ncalled_from_lib:libc++.so`n") -eq 0)

    # 压测结论
    $okLines = @('[stress][ring] edge-two-writers chunks=1', 'All tests passed (155 assertions in 6 test cases)')
    & $check '压测结论：退出码 0、汇总行、用例数够，通过' $false ((Get-StressVerdict 0 $okLines 6).Ok)
    & $check '压测结论：单数写法「1 assertion in 1 test case」也认得（用例数不够时判红，原因写用例数）' $true ((-not (Get-StressVerdict 0 @('All tests passed (1 assertion in 1 test case)') 6).Ok) -and ((Get-StressVerdict 0 @('All tests passed (1 assertion in 1 test case)') 6).Reason -match '只跑了 1 个用例'))
    & $check '压测结论：退出码非 0 判红，并带上汇总行' $true ((Get-StressVerdict 3 @('test cases:  6 |  5 passed | 1 failed', 'assertions: 147 | 145 passed | 2 failed') 6).Reason -match 'assertions: 147')
    & $check '压测结论：退出码 0 但没有汇总行（没跑完），判红，原因写没有汇总行' $true ((-not (Get-StressVerdict 0 @('[stress] 开头') 6).Ok) -and ((Get-StressVerdict 0 @('[stress] 开头') 6).Reason -match '汇总行'))
    & $check '压测结论：用例数少于下限（被删或被过滤），判红' $true (-not (Get-StressVerdict 0 @('All tests passed (40 assertions in 3 test cases)') 6).Ok)
    & $check '压测结论：零断言，判红，原因写没有断言' $true ((-not (Get-StressVerdict 0 @('All tests passed (0 assertions in 6 test cases)') 6).Ok) -and ((Get-StressVerdict 0 @('All tests passed (0 assertions in 6 test cases)') 6).Reason -match '一条断言都没有'))
    & $check '压测结论：超时（-1），判红，原因写超过时限' $true ((-not (Get-StressVerdict -1 $okLines 6).Ok) -and ((Get-StressVerdict -1 $okLines 6).Reason -match '时限'))

    # 变异结论
    $failed = @('a.cpp(1): FAILED:', 'test cases:  6 |  4 passed |  1 failed | 1 skipped', 'assertions: 91 | 78 passed | 13 failed')
    $noEvidence = @('[stress] 变异 no_read_fence 的证据不成立：命中 0 次')
    & $check '变异结论：no_intent 有失败断言与汇总行，抓住了' $false ((Get-MutationVerdict 'no_intent' 12 $failed 'X64').Ok -and (Get-MutationVerdict 'no_intent' 12 $failed 'X64').Verdict -ceq 'CAUGHT')
    & $check '变异结论：退出码 0，没抓住' $true ((Get-MutationVerdict 'no_intent' 0 $okLines 'Arm64').Verdict -ceq 'SURVIVED')
    & $check '变异结论：崩溃（有 FAILED 行但是 fatal error condition），不算抓住' $true ((Get-MutationVerdict 'no_intent' 134 ($failed + 'due to a fatal error condition:') 'X64').Verdict -ceq 'BROKEN')
    & $check '变异结论：未捕获异常，不算抓住' $true ((Get-MutationVerdict 'no_intent' 3 ($failed + 'unexpected exception with message:') 'X64').Verdict -ceq 'BROKEN')
    & $check '变异结论：先出 FAILED 再被杀（没有汇总行），不算抓住' $true ((Get-MutationVerdict 'no_intent' 137 @('a.cpp(1): FAILED:') 'X64').Verdict -ceq 'BROKEN')
    & $check '变异结论：什么都没跑（退出码 2，无失败断言），不算抓住' $true ((Get-MutationVerdict 'no_intent' 2 @('No test cases matched') 'X64').Verdict -ceq 'BROKEN')
    & $check '变异结论：超时（-1），不算抓住' $true ((Get-MutationVerdict 'no_intent' -1 $failed 'X64').Verdict -ceq 'BROKEN')
    & $check '变异结论：no_read_fence 在 x64 上证据不成立（退出码 70），接受并注明待 arm64 确认' $false (
        (Get-MutationVerdict 'no_read_fence' 70 $noEvidence 'X64').Ok -and (Get-MutationVerdict 'no_read_fence' 70 $noEvidence 'X64').Reason -match 'arm64')
    & $check '变异结论：no_read_fence 在 arm64 上证据不成立，判红' $true (-not (Get-MutationVerdict 'no_read_fence' 70 $noEvidence 'Arm64').Ok)
    & $check '变异结论：no_intent 证据不成立，任何 CPU 上都判红' $true (-not (Get-MutationVerdict 'no_intent' 70 $noEvidence 'X64').Ok)
    & $check '变异结论：no_read_fence 在 arm64 上抓住了，通过' $false ((Get-MutationVerdict 'no_read_fence' 3 $failed 'Arm64').Ok)
    & $check '变异结论：退出码 70 却没有「证据不成立」字样，不接受' $true (-not (Get-MutationVerdict 'no_read_fence' 70 @('随便一行') 'X64').Ok)

    # 命令行引号（与 build.ps1 同一写法）
    & $check '命令行引号：空白、引号、结尾反斜杠' $false ((ConvertTo-CommandLine @('a', 'b c', 'C:\x y\', 'q"r', '')) -ceq 'a "b c" "C:\x y\\" "q\"r" ""')

    # 真起进程：退出码、超时、环境变量只给子进程
    $dir = Join-Path ([IO.Path]::GetTempPath()) "rinne-stress-selftest-$PID-$(Get-Random)"
    $null = New-Item -ItemType Directory -Path $dir
    try {
        $pwshExe = (Get-Process -Id $PID).Path
        $log = Join-Path $dir 'a.log'
        $c = Invoke-Logged $pwshExe @('-NoProfile', '-Command', 'exit 7') $log 60
        & $check '起进程：退出码原样带回' $false ($c -eq 7)
        $c = Invoke-Logged $pwshExe @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30') (Join-Path $dir 'b.log') 1
        & $check '起进程：超时返回 -1 并结束进程' $true ($c -eq -1)
        $probe = Join-Path $dir 'probe.ps1'
        Set-Content -LiteralPath $probe -Value '[Console]::Out.Write("scale=" + $env:RINNE_STRESS_SCALE)'
        $probeArgs = @('-NoProfile', '-File', $probe)
        $before = [Environment]::GetEnvironmentVariable('RINNE_STRESS_SCALE')
        $null = Invoke-StressExe $pwshExe (Join-Path $dir 'c.log') 60 @{ RINNE_STRESS_SCALE = '7' } $probeArgs
        & $check '起进程：子进程看得到给它设的环境变量' $false ((Get-LogLines (Join-Path $dir 'c.log')) -join '' -ceq 'scale=7')
        & $check '起进程：环境变量跑完恢复调用方原来的值' $false ([Environment]::GetEnvironmentVariable('RINNE_STRESS_SCALE') -ceq $before)
        Set-ProcessEnv 'RINNE_STRESS_SCALE' '9'
        try { $null = Invoke-StressExe $pwshExe (Join-Path $dir 'd.log') 60 @{ RINNE_STRESS_SCALE = $null } $probeArgs } finally { Set-ProcessEnv 'RINNE_STRESS_SCALE' $before }
        & $check '起进程：给 $null 就是把这个变量从子进程里去掉（变异构建不带放大系数）' $false ((Get-LogLines (Join-Path $dir 'd.log')) -join '' -ceq 'scale=')

        # 没有可跑的压测（退出码 2，gate 记 SKIP）的三个出口：tsan.supp 不合格时必须仍报 1，不能被 SKIP 吞掉；合格时仍是 2
        $badSupp = Join-Path $dir 'bad.supp'
        $goodSupp = Join-Path $dir 'good.supp'
        Set-Content -LiteralPath $badSupp -Value 'race:Catch::Timer::start'
        Set-Content -LiteralPath $goodSupp -Value '# 只有注释，没有条目'
        $head = "$(git -C $Root rev-parse HEAD 2>$null)".Trim()
        $missingBuild = Join-Path $dir 'build-missing'
        $staleBuild = Join-Path $dir 'build-stale'
        $noExeBuild = Join-Path $dir 'build-no-exe'
        foreach ($b in @(@($staleBuild, '0000000'), @($noExeBuild, $head))) {
            $null = New-Item -ItemType Directory -Path $b[0]
            Set-Content -LiteralPath (Join-Path $b[0] 'rinne-build.json') -Value (@{ sha = $b[1] } | ConvertTo-Json)
        }
        $savedSupp = $script:TsanSupp
        $savedBuildDir = $script:BuildDir
        # 跑一遍 Invoke-Stress：退出码是输出流的最后一个值，Write-Host 的信息流用 6>&1 收下来，断言走的是哪个出口
        $runStress = {
            $all = @(Invoke-Stress 6>&1)
            @{ Code = $all[-1]; Text = (@($all | Where-Object { $_ -is [System.Management.Automation.InformationRecord] } | ForEach-Object { "$($_.MessageData)" }) -join "`n") }
        }
        try {
            # 每个出口：名字、夹具目录、这个出口打印的原因文本（正则）。夹具搭错了（比如构建记录对上了 HEAD）原因文本就对不上
            $exits = @(
                @('没有构建目录', $missingBuild, '没有构建目录'),
                @('构建记录不是当前 HEAD', $staleBuild, '构建记录是 0000000 的'),
                @('构建目录里没有 rinne_stress', $noExeBuild, '里没有 rinne_stress'))
            foreach ($exit in $exits) {
                $script:BuildDir = $exit[1]
                $script:TsanSupp = $badSupp
                $bad = & $runStress
                $script:TsanSupp = $goodSupp
                $good = & $runStress
                & $check "没有可跑的压测（$($exit[0])）而 tsan.supp 不合格：退出码 1，原因文本是这个出口的，并列出 tsan.supp 的失败" $true (
                    $bad.Code -eq 1 -and $bad.Text -match $exit[2] -and $bad.Text -match '失败：.*tsan\.supp')
                & $check "没有可跑的压测（$($exit[0])）而 tsan.supp 合格：仍是 2（gate 记 SKIP），原因文本是这个出口的，没有「失败：」" $false (
                    $good.Code -eq 2 -and $good.Text -match $exit[2] -and $good.Text -notmatch '失败：')
            }
        } finally {
            $script:TsanSupp = $savedSupp
            $script:BuildDir = $savedBuildDir
        }

        # 进不了 MSVC 环境：记进失败，已经算出的 tsan.supp 与压测结论照常报告，变异循环一个都不跑；环境正常时两个变异都会去构建。
        # 在子作用域里用同名函数顶掉 Enter-MsvcShell、Invoke-StressExe、Invoke-MutationBuild，不真的起进程、不真的构建
        $fakeBuild = Join-Path $dir 'build-fake'
        $null = New-Item -ItemType Directory -Path (Join-Path $fakeBuild 'tests')
        Set-Content -LiteralPath (Join-Path $fakeBuild ('tests/' + ($IsWindows ? 'rinne_stress.exe' : 'rinne_stress'))) -Value 'x'
        $pwshForNinja = (Get-Process -Id $PID).Path
        $savedMutations = $script:Mutations
        $savedNinja = $script:Ninja
        $savedLogDir = $script:LogDir
        $savedNoStamp = $script:NoStampCheck
        $savedVslang = $env:VSLANG
        $savedSupp = $script:TsanSupp
        $savedBuildDir = $script:BuildDir
        try {
            $script:Mutations = $true
            $script:Ninja = $pwshForNinja
            $script:LogDir = Join-Path $dir 'stress-logs'
            $script:NoStampCheck = $true
            $script:TsanSupp = $goodSupp
            $script:BuildDir = $fakeBuild
            $run = & {
                $mutationCalls = [System.Collections.Generic.List[string]]::new()
                function Invoke-StressExe([string]$Exe, [string]$Log, [int]$TimeoutSeconds, [hashtable]$Vars, [string[]]$Arguments = @()) {
                    Set-Content -LiteralPath $Log -Value 'All tests passed (155 assertions in 6 test cases)'
                    0
                }
                function Invoke-MutationBuild([string]$Name, [string]$MainBuild, [string]$NinjaExe) {
                    $mutationCalls.Add($Name)
                    throw '桩：不真的构建'
                }
                function Enter-MsvcShell { throw '桩：找不到 vswhere' }
                $broken = & $runStress
                $brokenCalls = $mutationCalls.Count
                function Enter-MsvcShell { }
                $healthy = & $runStress
                @{ Broken = $broken; BrokenCalls = $brokenCalls; Healthy = $healthy; HealthyCalls = @($mutationCalls) }
            }
            & $check '进不了 MSVC 环境：退出码 1，失败行里有这条原因' $true (
                $run.Broken.Code -eq 1 -and $run.Broken.Text -match '变异构建：进不了 MSVC 环境（桩：找不到 vswhere）' -and $run.Broken.Text -match '失败：.*进不了 MSVC 环境')
            & $check '进不了 MSVC 环境：前面已经算出的压测与 tsan.supp 结论照常报告，不被异常带走' $true (
                $run.Broken.Text -match '压测：通过' -and $run.Broken.Text -match 'tsan\.supp：合格')
            & $check '进不了 MSVC 环境：变异循环一个都不跑' $true ($run.BrokenCalls -eq 0)
            & $check 'MSVC 环境正常：两个变异都去构建（上一条不是恒不跑）' $false (($run.HealthyCalls -join ',') -ceq 'no_intent,no_read_fence')
            & $check 'MSVC 环境正常：构建失败照常记进失败，退出码 1' $false (
                $run.Healthy.Code -eq 1 -and $run.Healthy.Text -notmatch '进不了 MSVC 环境' -and $run.Healthy.Text -match '变异 no_intent：构建失败')
        } finally {
            $script:Mutations = $savedMutations
            $script:Ninja = $savedNinja
            $script:LogDir = $savedLogDir
            $script:NoStampCheck = $savedNoStamp
            $env:VSLANG = $savedVslang
            $script:TsanSupp = $savedSupp
            $script:BuildDir = $savedBuildDir
        }

        # 结束进程树失败：什么异常都不能冒出来（调用方照样按超时返回 -1）
        foreach ($thrown in @(
                @('进程已退出（InvalidOperationException）', { [InvalidOperationException]::new('已退出') }),
                @('没有权限（Win32Exception）', { [System.ComponentModel.Win32Exception]::new('拒绝访问') }),
                @('别的异常', { [ArgumentException]::new('随便什么') }))) {
            $fake = [pscustomobject]@{ Killed = 0 }
            $makeError = $thrown[1]
            Add-Member -InputObject $fake -MemberType ScriptMethod -Name Kill -Value ({ param($tree) throw (& $makeError) }.GetNewClosure())
            $escaped = $null
            try { Stop-ProcessTree $fake } catch { $escaped = $_ }
            & $check "结束进程树失败（$($thrown[0])）：异常不冒出来" $true ($null -eq $escaped)
        }
    } finally {
        # 只删本次新建的临时目录里的文件与子目录（最深两层，逐层列出，不用递归删除），再删这个空目录
        foreach ($d in Get-ChildItem -LiteralPath $dir -Directory) {
            foreach ($d2 in Get-ChildItem -LiteralPath $d.FullName -Directory) {
                foreach ($f in Get-ChildItem -LiteralPath $d2.FullName -File) { Remove-Item -LiteralPath $f.FullName }
                Remove-Item -LiteralPath $d2.FullName
            }
            foreach ($f in Get-ChildItem -LiteralPath $d.FullName -File) { Remove-Item -LiteralPath $f.FullName }
            Remove-Item -LiteralPath $d.FullName
        }
        foreach ($f in Get-ChildItem -LiteralPath $dir -File) { Remove-Item -LiteralPath $f.FullName }
        Remove-Item -LiteralPath $dir
    }

    $failedCases = @($cases | Where-Object { -not $_.Passed })
    foreach ($c in $cases) { Write-Host ('{0}  {1}{2}' -f ($c.Passed ? 'PASS' : 'FAIL'), $c.Label, ($c.Bad ? '（坏样例）' : '')) }
    $nBad = @($cases | Where-Object Bad).Count
    Write-Host ('stress --self-test：{0}（{1} 项，坏样例 {2}、正例 {3}）' -f ($failedCases.Count ? '失败' : '通过'), $cases.Count, $nBad, ($cases.Count - $nBad))
    [int]($failedCases.Count -gt 0)
}

if ($MyInvocation.InvocationName -eq '.') { return }   # 只取函数
if ($SelfTest) { exit (Invoke-SelfTest) }
if ($Lint) { exit ((Invoke-SuppLint) ? 0 : 1) }

Import-Module (Join-Path $PSScriptRoot 'lib/StdHandles.psm1') -Force
Clear-SecretEnv
$code = Invoke-WithoutStdInherit { Invoke-Stress }
exit ([int]@($code)[-1])
