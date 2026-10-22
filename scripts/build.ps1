#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/build.json
#
# 在 Windows 上构建 Rinne（Ninja Multi-Config + MSVC）。本机 gates、合并后构建与 CI 共用这一份配方：
#   pwsh scripts/build.ps1 [-BuildDir <目录>] [-JucePath <目录>] [-Config Release] [-Ci] [-Probe] [-Jobs <n>] [-Launcher <编译缓存>]
#        [-BuildLabel <标签>] [-Ninja <exe>] [-NoBuild] [-Test] [-Validate | -ValidateGui] [-Pluginval <exe>] [-Stage <目录>]
#   pwsh scripts/build.ps1 -CheckCache [-BuildDir <目录>] [-Probe]
#   pwsh scripts/build.ps1 -SelfTest
# 不带开关时只配置、构建、查第一方零 warning 与产物（合并后构建就这样调用）。-Test 跑 ctest；-Validate 用 pluginval 无 GUI 地验
# 两个插件（strictness 5），-ValidateGui 带 GUI 验；-NoBuild 只跑这些后续步骤，构建目录里的记录必须是当前 HEAD 构建的。
# -Stage 把两个插件与 tools-bin 下的可执行文件拷进一个新目录（CI 上传的就是它）。-CheckCache 断言 cache 里的段名前缀是默认值，
# 并读生成的 src/core/generated/util/RinneVersion.h，断言实际编进去的段名前缀与探针开关：cache 值会被同名的普通变量遮住。
# 构建目录缺省 build，探针缺省 build-probe：两种身份的产物不同名，混在一个目录里会被产物断言拦下。
# JUCE、ninja、pluginval：参数，其次环境变量 RINNE_JUCE_PATH / RINNE_NINJA / RINNE_PLUGINVAL，再次 git config rinne.jucePath /
# rinne.ninja / rinne.pluginval，ninja 与 pluginval 最后找 PATH。
# 段名前缀：本机构建一律用带 D 的 gates 前缀，与维护者 DAW 里装的正式版互不相见；-Ci 用默认值（正式版，探针时是探针前缀），
# 只在 GITHUB_ACTIONS=true 时允许；本机的 -Probe 用探针前缀。真源与校验在根 CMakeLists.txt（这里只从那里读正式前缀），
# 配置后再核一次 cache 里的值。
# 构建槽：配置与构建持一把（同时最多两份编译），先等可用内存，再按内存折算并发。调用方已经持有一把时（gates 的构建、合并后
# 构建）把 RINNE_BUILD_SLOT_HELD 设成持槽进程自己的 pid，这里就不再取：两个持槽的调用方各自的子进程再去取，会互相等到超时。
# 这个 pid 不是本进程的祖先、或者两把槽都空着，说明标记是残留的，照常取槽并提示。-Ci 不取槽。
# 输出：cmake、ninja、ctest、pluginval 的输出写进 <构建目录>/logs/ 下的文件，不经管道；本进程的标准句柄开跑前设为不可继承。
# MSVC 的 mspdbsrv、编译缓存的服务进程会比构建活得久，它们继承了调用方的管道，调用方读 EOF 就要等它们退出，超时也就失效了。
# 屏幕上只打印步骤、告警与失败步骤日志的末尾。
# 环境：开跑前从本进程删掉凭据与个人针变量（判据见 $SecretEnv），CMake 配置期与编译器都看不到；VSLANG=1033 在配置与构建两边都设
# （进 MSVC 开发者 shell 之后再设一次），让 MSVC 的输出语言前后一致，零 warning 才判得准；
# pluginval 会把同名环境变量（SKIP_GUI_TESTS 之类）当参数用，验之前一并删掉。
# 头文件依赖：ninja 靠 cl /showIncludes 的输出行开头与 rules.ninja 的 msvc_deps_prefix 逐字节相同才记依赖，对不上就一条都不记，
# 只改 .h 时含它的编译单元不会重编。配置后拿一个探针编译单元实测 cl 的输出，与 rules.ninja 的前缀逐字节核对；对不上说明构建目录里
# 的编译器探测结果是在别的语言或编码下留下的，或者 CMake 解码 cl 输出用的代码页与 cl 编码用的不同（无控制台的后台进程里，cl 输出
# UTF-8、CMake 按 ANSI 解，探测结果就成了二次编码的乱码）。先删掉 CMakeFiles/<版本>/ 下 C 与 CXX 的编译器信息文件（只删这两个文件）
# 重配；仍对不上，且探针里 cl 的前缀是干净的 UTF-8 文本，就把它写进构建目录里的一个 cmake 文件、经 CMAKE_PROJECT_INCLUDE 在
# project() 之后覆盖 CMAKE_CL_SHOWINCLUDES_PREFIX，再重配；两步之后仍对不上判红。修过的构建目录 clean 一次（旧目标文件没有依赖
# 记录）；核对通过的前缀记在 <构建目录>/rinne-deps-prefix-<配置>.txt（clean 只清当前配置，所以按配置分开记），没有记录或记录不同的目录也 clean 一次。判红而不是悄悄放过的理由：前缀对不上时 ninja 不报错，只是不记依赖，只改 .h 时 gates 会跑在旧对象上。构建后再查
# ninja -t deps 至少记下一条依赖，否则判红。
# 退出码：0 通过；1 失败；2 -NoBuild 时构建目录里没有当前 HEAD 的构建；3 等构建槽超时。
[CmdletBinding()]
param(
    [string]$BuildDir = '', [string]$JucePath = '', [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Ci, [switch]$Probe, [int]$Jobs = 0, [string]$Launcher = '', [string]$BuildLabel = '', [string]$Ninja = '',
    [switch]$NoBuild, [switch]$Test, [switch]$Validate, [switch]$ValidateGui, [string]$Pluginval = '', [string]$Stage = '',
    [switch]$CheckCache, [switch]$SelfTest, [double]$SlotWaitMinutes = 45, [double]$MemWaitMinutes = 15
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) } catch { $null = $_ }
$Root = Split-Path -Parent $PSScriptRoot
# 凭据与个人针：名字带这些词或以 _KEY、_PAT、_AUTH、_PWD、_PASS、_JWT 结尾的，值像带口令的 URL 或常见 token 前缀
# （GitHub、OpenAI 类的 sk-、JWT、Slack、GitLab、npm、AWS 访问键）的，一律删掉。黑名单总有漏网的，所以合并后构建只在
# 其余判据都通过、没碰基础设施时才跑（见 pr-merge.ps1）。
# 全仓只有这一份：合并后构建（maintainer/lib/RinneGh.psm1）从主 clone 的这个文件里读这两条，起子进程之前就拿掉。
$SecretEnv = '^(?:PRIVACY_NEEDLES|RINNE_PRIVACY_LOCAL|DOCKER_AUTH_CONFIG|.*(?:TOKEN|SECRET|PASSWORD|PASSWD|PASSPHRASE|CREDENTIAL|APIKEY|API_KEY).*|.+_(?:KEY|PAT|AUTH|PWD|PASS|JWT))$'
$SecretEnvValue = '^(?:[A-Za-z][A-Za-z0-9+.-]*://[^/\s:@]+:[^/\s@]+@|gh[pousr]_[A-Za-z0-9]{20}|github_pat_\w{20}|sk-[\w-]{20}|eyJ[\w-]{10,}\.[\w-]{10,}\.|xox[abprs]-[\w-]{10}|glpat-[\w-]{20}|npm_[A-Za-z0-9]{36}|AKIA[0-9A-Z]{16})'
$PluginvalEnv = '^(?:VALIDATE|VALIDATE_IN_PROCESS|STRICTNESS_LEVEL|TIMEOUT_MS|SKIP_GUI_TESTS|DISABLED_TESTS|REPEAT|RANDOMISE|RANDOM_SEED|SAMPLE_RATES|BLOCK_SIZES|DATA_FILE|VST3VALIDATOR|OUTPUT_DIR|OUTPUT_FILENAME|VERBOSE)$'

class BuildStop : Exception {
    [int]$Code
    BuildStop([string]$Message, [int]$Code) : base($Message) { $this.Code = $Code }
}

# 标准输出与标准错误的 HANDLE_FLAG_INHERIT；控制台句柄设不了也无妨，它们不是管道。标准输入不动：留着它不会拖住任何人。
function Set-StdHandleInherit([bool]$Inherit) {
    if (-not $IsWindows) { return }
    if (-not ('Rinne.StdHandles' -as [type])) {
        Add-Type -Namespace Rinne -Name StdHandles -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int nStdHandle);
[DllImport("kernel32.dll")] public static extern bool SetHandleInformation(System.IntPtr hObject, int dwMask, int dwFlags);
'@
    }
    foreach ($n in -11, -12) { $null = [Rinne.StdHandles]::SetHandleInformation([Rinne.StdHandles]::GetStdHandle($n), 1, [int]$Inherit) }
}

function Protect-StdHandles { Set-StdHandleInherit $false }

function Remove-EnvMatching([string]$Pattern, [string]$ValuePattern = '(?!)') {
    foreach ($e in @([Environment]::GetEnvironmentVariables().GetEnumerator())) {
        if ($e.Key -match $Pattern -or "$($e.Value)" -match $ValuePattern) { [Environment]::SetEnvironmentVariable($e.Key, $null) }
    }
}

function Get-Setting([string]$Value, [string]$EnvName, [string]$GitKey, [string]$Fallback = '') {
    if ($Value) { return $Value }
    $v = [Environment]::GetEnvironmentVariable($EnvName)
    if (-not $v) { $v = "$(git -C $Root config --get $GitKey 2>$null)".Trim() }
    $v ? $v : $Fallback
}

# Start-Process 只把参数用空格拼起来：按 Windows 命令行规则给含空白或引号的参数加引号，引号前与结尾的反斜杠加倍。
function ConvertTo-CommandLine([string[]]$Arguments) {
    @($Arguments | ForEach-Object {
            if ($_ -and $_ -notmatch '[\s"]') { $_ } else { '"' + (($_ -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"' }
        }) -join ' '
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
        try { $p.Kill($true) } catch [InvalidOperationException] { $null = $_ }
        return -1
    }
    $p.ExitCode
}

function Get-LogLines([string]$Log) {
    @(foreach ($f in $Log, "$Log.err") { if (Test-Path -LiteralPath $f) { Get-Content -LiteralPath $f } })
}

function Invoke-Step([string]$Name, [string]$Exe, [string[]]$Arguments, [int]$TimeoutSeconds = 3600) {
    Write-Host "== $Name"
    $log = Join-Path $script:LogDir "$Name.log"
    $code = Invoke-Logged $Exe $Arguments $log $TimeoutSeconds
    if ($code -eq 0) { return $log }
    Get-LogLines $log | Select-Object -Last 60 | ForEach-Object { Write-Host "    $_" }
    throw [BuildStop]::new(($code -eq -1 ? "$Name 超过 $TimeoutSeconds 秒，已结束" : "$Name 退出码 $code（日志 $log）"), 1)
}

# MSVC 的告警：<文件>(<行>[,<列>]): warning C<号>；链接器与命令行的告警（LNK、D 开头）没有源文件。第一方是仓内、又不在
# 排除目录（JUCE、构建目录）与 third_party/ 下的文件；没有源文件的告警一律算第一方，除非行里点名了排除目录。
function Get-FirstPartyWarnings([string[]]$Lines, [string]$RepoRoot, [string[]]$Exclude) {
    $dir = { param($p) ([IO.Path]::GetFullPath($p) -replace '\\', '/').TrimEnd('/') + '/' }
    $root = & $dir $RepoRoot
    $ex = @(@($Exclude | Where-Object { $_ }) + (Join-Path $RepoRoot 'third_party') | ForEach-Object { & $dir $_ })
    $under = { param($f, $d) $f.StartsWith($d, [StringComparison]::OrdinalIgnoreCase) }
    foreach ($l in $Lines) {
        if ($l -match '^\s*(?<file>[A-Za-z]:[\\/].*?)\(\d+(?:,\d+)?\)\s*:\s*warning\s+C\d+') {
            $f = [IO.Path]::GetFullPath($Matches.file) -replace '\\', '/'
            if ((& $under $f $root) -and -not @($ex | Where-Object { & $under $f $_ }).Count) { $l.Trim() }
        } elseif ($l -match '\bwarning\s+(?:LNK|D)\d+') {
            $norm = $l -replace '\\', '/'
            if (-not @($ex | Where-Object { $norm.IndexOf($_.TrimEnd('/'), [StringComparison]::OrdinalIgnoreCase) -ge 0 }).Count) { $l.Trim() }
        }
    }
}

function Get-ExpectedBundles([bool]$IsProbe) {
    $IsProbe ? @('Rinne Probe Input.vst3', 'Rinne Probe Output.vst3') : @('Rinne Input.vst3', 'Rinne Output.vst3')
}

# 构建出的 bundle 名必须恰好是本次身份的那两个：缺一个、多一个（例如另一种身份留下的）都算错。返回错误文字，对上了返回空。
function Test-BundleSet([string[]]$Found, [string[]]$Expected) {
    $missing = @($Expected | Where-Object { $_ -cnotin $Found })
    $extra = @($Found | Where-Object { $_ -cnotin $Expected })
    $dup = @($Found | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
    @(if ($missing.Count) { "缺 $($missing -join '、')" }; if ($extra.Count) { "多出 $($extra -join '、')" }; if ($dup.Count) { "重复 $($dup -join '、')" }) -join '；'
}

function Find-Bundles([string]$Dir, [string]$Cfg) {
    $src = Join-Path $Dir 'src'
    if (-not (Test-Path -LiteralPath $src)) { return @() }
    @(Get-ChildItem -LiteralPath $src -Directory -Recurse -Filter '*.vst3' | Where-Object { $_.Parent.Name -eq 'VST3' -and $_.Parent.Parent.Name -eq $Cfg })
}

function Get-CacheValue([string]$Text, [string]$Name) {
    $m = [regex]::Match($Text, "(?m)^$([regex]::Escape($Name)):[A-Z]+=(.*?)\r?$")
    $m.Success ? $m.Groups[1].Value : $null
}

# ninja 记头文件依赖的前提：cl /showIncludes 每行输出的开头与 rules.ninja 里 msvc_deps_prefix 的字节逐字节相同（ninja 按字节比，
# 编码不同的同一句话也对不上）。字节一律按 Latin-1 转成字符串比较，字节与字符一一对应；报错时再按 UTF-8 显示。
function ConvertFrom-Latin1([byte[]]$Bytes) { [Text.Encoding]::Latin1.GetString($Bytes) }
function ConvertTo-Display([string]$Latin1) { [Text.Encoding]::UTF8.GetString([Text.Encoding]::Latin1.GetBytes($Latin1)) }

# rules.ninja 里第一条 msvc_deps_prefix 的值（Latin-1 字符串）；没有这一条返回 $null。
function Get-NinjaDepsPrefix([string]$RulesFile) {
    if (-not (Test-Path -LiteralPath $RulesFile)) { return $null }
    $m = [regex]::Match((ConvertFrom-Latin1 ([IO.File]::ReadAllBytes($RulesFile))), '(?m)^[ \t]*msvc_deps_prefix[ \t]*=[ \t]?(?<v>[^\r\n]*)')
    $m.Success ? $m.Groups['v'].Value : $null
}

# 探针编译的 cl 输出（Latin-1 字符串）里那条 /showIncludes 行开头、路径之前的部分；找不到返回 $null。
function Get-ClIncludePrefix([string]$ClOutput, [string]$Header) {
    $m = [regex]::Match($ClOutput, '(?m)^(?<p>.*?)(?:[A-Za-z]:\\|\\\\)[^\r\n]*' + [regex]::Escape($Header) + '[ \t]*\r?$')
    $m.Success ? $m.Groups['p'].Value : $null
}

# rules.ninja 的前缀（$Rules）与 cl 实际输出的前缀（$Cl）对不上就返回错误文字，对上了返回空。ninja 先要求行以 $Rules 开头，
# 再跳过后面的空白，所以 cl 的前缀可以比 $Rules 多出空白，但不能多出别的。
function Test-ShowIncludesPrefix($Rules, $Cl) {
    if ($null -eq $Rules -or -not "$Rules".Trim()) { return 'rules.ninja 里没有 msvc_deps_prefix，或者它是空的' }
    if ($null -eq $Cl) { return 'cl 探针的输出里找不到 /showIncludes 行' }
    if ($Cl.StartsWith($Rules, [StringComparison]::Ordinal) -and -not $Cl.Substring($Rules.Length).Trim()) { return '' }
    "rules.ninja 的 msvc_deps_prefix 是「$(ConvertTo-Display $Rules)」，cl 实际输出的是「$(ConvertTo-Display $Cl)」，字节对不上，ninja 一条头文件依赖都不会记"
}

# ninja -t deps 的输出里至少有一个目标记下了依赖（形如 `x.obj: #deps 12, deps mtime …`）；返回错误文字，有就返回空。
function Test-DepsRecorded([string[]]$Lines) {
    $n = @($Lines | Where-Object { $_ -match ': #deps ([1-9]\d*),' }).Count
    $n ? '' : 'ninja -t deps 里没有任何一个目标记下头文件依赖：只改 .h 时含它的编译单元不会重编'
}

# 核对通过的前缀（Latin-1 字符串）的十六进制，记在构建目录的 rinne-deps-prefix-<配置>.txt 里。
function Get-DepsPrefixHex([string]$Prefix) { [BitConverter]::ToString([Text.Encoding]::Latin1.GetBytes("$Prefix")) }

# 记录缺失或与这次核对通过的前缀不同，就要 clean：旧目标文件可能没有依赖记录。
function Test-DepsNeedClean($Recorded, [string]$Verified) { $null -eq $Recorded -or $Recorded -cne $Verified }

# 用一个两行的探针编译单元（写在构建目录的 logs 下）按本进程的环境跑 cl，取它的 /showIncludes 前缀（Latin-1 字符串）；
# 失败时 $script:ProbeError 记原因并返回 $null。
function Get-ProbedClPrefix {
    $script:ProbeError = ''
    $dir = Join-Path $script:LogDir 'prefix-probe'
    $null = New-Item -ItemType Directory -Force -Path $dir
    Set-Content -LiteralPath (Join-Path $dir 'probe.h') -Value '#define RINNE_PROBE_H 1'
    Set-Content -LiteralPath (Join-Path $dir 'probe.cpp') -Value '#include "probe.h"'
    $log = Join-Path $dir 'cl.log'
    $code = Invoke-Logged 'cl.exe' @('/nologo', '/showIncludes', '/c', (Join-Path $dir 'probe.cpp'), "/Fo$(Join-Path $dir 'probe.obj')") $log 120
    if ($code -ne 0) { $script:ProbeError = "cl 探针编译退出码 $code（日志 $log）"; return $null }
    Get-ClIncludePrefix (ConvertFrom-Latin1 ([IO.File]::ReadAllBytes($log))) 'probe.h'
}

# 构建目录的 rules.ninja 与 cl 探针核对，返回错误文字，对上了返回空。
function Get-DepsPrefixProblem {
    $cl = Get-ProbedClPrefix
    if ($script:ProbeError) { return $script:ProbeError }
    Test-ShowIncludesPrefix (Get-NinjaDepsPrefix (Join-Path $BuildDir 'CMakeFiles/rules.ninja')) $cl
}

# 覆盖用的 cmake 文件的内容：$ClPrefix（Latin-1 字符串）必须是干净的 UTF-8 文本，且不含 cmake 字符串里要转义的字符；
# 否则返回 $null。文件按 UTF-8 写，cmake 原样写进 rules.ninja，字节就与 cl 输出的一致。
function Get-PrefixOverride([string]$ClPrefix) {
    if (-not $ClPrefix.Trim() -or $ClPrefix -match '["\\$;\[\]\x00-\x1f]') { return $null }
    try { $text = [Text.UTF8Encoding]::new($false, $true).GetString([Text.Encoding]::Latin1.GetBytes($ClPrefix)) } catch { return $null }
    "# 由 build.ps1 按本次 cl 实际输出生成：CMake 解码 cl 输出的代码页与 cl 编码用的不同时，探测到的 /showIncludes 前缀是乱码，这里改正。`n" +
    "foreach(lang C CXX)`n    set(CMAKE_`${lang}_CL_SHOWINCLUDES_PREFIX `"$text`")`nendforeach()`nset(CMAKE_CL_SHOWINCLUDES_PREFIX `"$text`")`n"
}

# 清掉构建目录里缓存的编译器探测结果（CMakeFiles/<版本>/ 下 C 与 CXX 的编译器信息文件，只删这两个文件、不递归），下次配置会重新探测。
function Clear-CompilerProbe {
    $cmakeFiles = Join-Path $BuildDir 'CMakeFiles'
    foreach ($d in @(Get-ChildItem -LiteralPath $cmakeFiles -Directory -ErrorAction SilentlyContinue)) {
        foreach ($n in 'CMakeCCompiler.cmake', 'CMakeCXXCompiler.cmake') {
            $f = Join-Path $d.FullName $n
            if (Test-Path -LiteralPath $f -PathType Leaf) { Remove-Item -LiteralPath $f }
        }
    }
}

# CI 与发布构建的段名前缀必须是默认值：返回错误文字，对上了返回空。
function Test-CachePrefix([string]$CacheText) {
    $have, $want = (Get-CacheValue $CacheText 'RINNE_SHM_PREFIX'), (Get-CacheValue $CacheText 'RINNE_SHM_PREFIX_DEFAULT')
    if (-not $have -or -not $want) { return 'CMakeCache.txt 里没有 RINNE_SHM_PREFIX 或 RINNE_SHM_PREFIX_DEFAULT' }
    ($have -ceq $want) ? '' : "RINNE_SHM_PREFIX 被覆盖成 $have，默认值是 $want"
}

# 生成的 RinneVersion.h 里实际编进去的段名前缀与探针开关：cache 里的值可能被同名的普通变量遮住（CMP0126），
# 编进去的才算数。返回错误文字，对上了返回空。
function Test-BuildConstants([string]$Header, [string]$WantPrefix, [bool]$WantProbe) {
    $p = [regex]::Match($Header, '(?m)^\s*inline constexpr const char\* kShmPrefix = "([^"]*)";')
    $b = [regex]::Match($Header, '(?m)^\s*inline constexpr bool kProbeBuild = (true|false);')
    if (-not $p.Success -or -not $b.Success) { return '生成的 RinneVersion.h 里找不到 kShmPrefix 或 kProbeBuild' }
    @(if ($p.Groups[1].Value -cne $WantPrefix) { "编进去的段名前缀是 $($p.Groups[1].Value)，应为 $WantPrefix" }
        if (($b.Groups[1].Value -ceq 'true') -ne $WantProbe) { "编进去的 kProbeBuild 是 $($b.Groups[1].Value)，本次应为 $($WantProbe ? 'true' : 'false')" }) -join '；'
}

# 正式前缀只在根 CMakeLists.txt 里写一次；本机前缀与探针前缀是它加 D、加 P。
function Get-ShmPrefix([string]$Flavor) {
    $m = [regex]::Match((Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw), '(?m)^set\(RINNE_SHM_PREFIX_FORMAL "([A-Za-z]+)"\)')
    if (-not $m.Success) { throw [BuildStop]::new('根 CMakeLists.txt 里找不到 set(RINNE_SHM_PREFIX_FORMAL "…")', 1) }
    $m.Groups[1].Value + @{ ci = ''; dev = 'D'; probe = 'P' }[$Flavor]
}

# 持槽标记的值是持槽进程的 pid，它必须是本进程的祖先（调用方或调用方的调用方）。
function Test-HolderIsAncestor([string]$Marker) {
    if ($Marker -notmatch '^\d+$') { return $false }
    $p = (Get-Process -Id $PID).Parent
    for ($hops = 0; $p -and $hops -lt 8; $hops++) {
        if ($p.Id -eq [int]$Marker) { return $true }
        $p = $p.Parent
    }
    $false
}

function Enter-BuildSlot {
    if ($Ci) { return $null }
    Import-Module (Join-Path $PSScriptRoot 'lib/Locks.psm1') -ErrorAction Stop
    if ($env:RINNE_BUILD_SLOT_HELD) {
        $free = @('build-slot-1', 'build-slot-2' | ForEach-Object { Enter-RinneLock $_ -TimeoutSeconds 0 -Quiet })
        $free | ForEach-Object { Exit-RinneLock $_ }
        $ancestor = Test-HolderIsAncestor $env:RINNE_BUILD_SLOT_HELD
        if ($ancestor -and @($free | Where-Object { $_ }).Count -lt 2) {
            Write-Host "构建槽由调用方（pid $($env:RINNE_BUILD_SLOT_HELD)）持有，这里不再取"
            if (-not $script:Jobs) { $script:Jobs = Get-RinneJobs (Get-RinneAvailableMB) }
            return $null
        }
        Write-Host ($ancestor ? 'RINNE_BUILD_SLOT_HELD 设了，两把构建槽却都空着：当成残留的设置，照常取槽' :
            "RINNE_BUILD_SLOT_HELD=$($env:RINNE_BUILD_SLOT_HELD) 不是本进程某个祖先的 pid：当成残留的设置，照常取槽")
    }
    $mem = Wait-RinneMemory -MinMB 6000 -TimeoutSeconds ($MemWaitMinutes * 60)
    if (-not $mem.Ok) { Write-Host "等了 $MemWaitMinutes 分钟，可用内存仍只有 $($mem.AvailMB) MB，降为 -j2" }
    $lock = Enter-RinneBuildSlot -TimeoutSeconds ($SlotWaitMinutes * 60)
    if (-not $lock) { throw [BuildStop]::new("等构建槽超过 $SlotWaitMinutes 分钟", 3) }
    if (-not $script:Jobs) { $script:Jobs = $mem.Ok ? (Get-RinneJobs (Get-RinneAvailableMB)) : 2 }
    $lock
}

# MSVC 环境：cl 不在 PATH 上时按 vswhere 找到 VS，进它的开发者 shell（只改本进程的环境）。必须是 64 位宿主的 64 位编译器。
# 开发者 shell 起的 cmd 要继承标准输出与标准错误才取得到环境，这一步临时放开继承；那几个进程跑完就退出。
function Enter-MsvcShell {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path -LiteralPath $vswhere)) { throw [BuildStop]::new("找不到 vswhere：$vswhere", 1) }
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vs) { throw [BuildStop]::new('vswhere 没找到带 VC 工具集的 VS 安装', 1) }
        $env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"   # VsDevCmd 自己也调 vswhere
        Set-StdHandleInherit $true
        try { & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null }
        finally { Protect-StdHandles }
    }
    $cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
    if ($cl -notmatch '\\bin\\Hostx64\\x64\\') { throw [BuildStop]::new("cl.exe 不是 64 位宿主的 64 位编译器：$cl", 1) }
    Write-Host "cl.exe：$cl"
}

function Invoke-Build([string]$Flavor) {
    $juce = Get-Setting $JucePath 'RINNE_JUCE_PATH' 'rinne.jucePath'
    if (-not $juce) { throw [BuildStop]::new('没有 JUCE 路径：传 -JucePath，或设 RINNE_JUCE_PATH / git config rinne.jucePath', 1) }
    $ninjaExe = (Get-Command (Get-Setting $Ninja 'RINNE_NINJA' 'rinne.ninja' 'ninja') -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if (-not $ninjaExe) { throw [BuildStop]::new('找不到 ninja：传 -Ninja，或设 RINNE_NINJA / git config rinne.ninja', 1) }
    Remove-Item -LiteralPath $script:StampFile -ErrorAction SilentlyContinue
    $env:VSLANG = '1033'
    Enter-MsvcShell
    $env:VSLANG = '1033'
    $prefix = Get-ShmPrefix $Flavor
    $cfg = @('-S', $Root, '-B', $BuildDir, '-G', 'Ninja Multi-Config', "-DCMAKE_CONFIGURATION_TYPES=$Config", "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', "-DJUCE_PATH=$juce", "-DRINNE_PROBE_BUILD=$($Probe ? 'ON' : 'OFF')", "-DRINNE_BUILD_LABEL=$BuildLabel")
    # CI 不传前缀、先删掉 cache 里的旧值，让根 CMakeLists.txt 的默认值生效；本机显式给 D 或 P
    $cfg += ($Flavor -eq 'ci') ? '-URINNE_SHM_PREFIX' : "-DRINNE_SHM_PREFIX=$prefix"
    if ($Launcher) { $cfg += "-DCMAKE_C_COMPILER_LAUNCHER=$Launcher", "-DCMAKE_CXX_COMPILER_LAUNCHER=$Launcher" }
    $null = Invoke-Step 'configure' 'cmake' $cfg 1800
    # 头文件依赖的前缀对不上：先清掉编译器探测结果重配，仍不行就用 cl 实测的前缀覆盖；旧目标文件没有依赖记录，clean 一次让它们全部重编
    $needClean = $false
    $depsProblem = Get-DepsPrefixProblem
    if ($depsProblem) {
        $needClean = $true
        Write-Host "头文件依赖前缀对不上，清掉编译器探测结果重配：$depsProblem"
        Clear-CompilerProbe
        $null = Invoke-Step 'configure-retry' 'cmake' $cfg 1800
        $depsProblem = Get-DepsPrefixProblem
        if ($depsProblem) {
            $override = Get-PrefixOverride (Get-ProbedClPrefix)
            if ($null -eq $override) { throw [BuildStop]::new("重配后头文件依赖前缀仍对不上，cl 的前缀也不能写成 cmake 字符串：$depsProblem", 1) }
            Write-Host "重配后仍对不上，改用 cl 实测的前缀覆盖：$depsProblem"
            $file = Join-Path $BuildDir 'rinne-showincludes-prefix.cmake'
            [IO.File]::WriteAllText($file, $override, [Text.UTF8Encoding]::new($false))
            $null = Invoke-Step 'configure-override' 'cmake' ($cfg + "-DCMAKE_PROJECT_INCLUDE=$file") 1800
            $depsProblem = Get-DepsPrefixProblem
            if ($depsProblem) { throw [BuildStop]::new("覆盖之后头文件依赖前缀仍对不上：$depsProblem", 1) }
        }
    }
    # 核对通过的前缀记在构建目录里；没有记录或记录的前缀不同，说明这个目录里的目标文件可能是前缀对不上时编的（#deps 0，
    # 改头文件不会重编，哪怕别的目标有依赖，断言也看不出来），clean 一次
    $markerFile = Join-Path $BuildDir "rinne-deps-prefix-$Config.txt"
    $verified = Get-DepsPrefixHex (Get-NinjaDepsPrefix (Join-Path $BuildDir 'CMakeFiles/rules.ninja'))
    $recorded = (Test-Path -LiteralPath $markerFile) ? (Get-Content -LiteralPath $markerFile -Raw).Trim() : $null
    if ($needClean -or (Test-DepsNeedClean $recorded $verified)) {
        $null = Invoke-Step 'clean' 'cmake' @('--build', $BuildDir, '--config', $Config, '--target', 'clean') 1800
    }
    Set-Content -LiteralPath $markerFile -Value $verified
    $cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw
    $got = Get-CacheValue $cache 'RINNE_SHM_PREFIX'
    if ($got -cne $prefix) { throw [BuildStop]::new("配置后的 RINNE_SHM_PREFIX 是 $got，本次应当是 $prefix", 1) }
    Write-Host "段名前缀：$prefix"
    $b = @('--build', $BuildDir, '--config', $Config)
    if ($script:Jobs -gt 0) { $b += '--parallel', "$($script:Jobs)" }
    $log = Invoke-Step 'build' 'cmake' $b 5400
    $noDeps = Test-DepsRecorded (Get-LogLines (Invoke-Step 'ninja-deps' $ninjaExe @('-C', $BuildDir, '-t', 'deps') 600))
    if ($noDeps) { throw [BuildStop]::new($noDeps, 1) }
    $warnings = @(Get-FirstPartyWarnings (Get-LogLines $log) $Root @($juce, $BuildDir))
    if ($warnings.Count) {
        $warnings | ForEach-Object { Write-Host "    $_" }
        throw [BuildStop]::new("第一方代码有 $($warnings.Count) 条告警，要求零 warning", 1)
    }
    $bundles = Find-Bundles $BuildDir $Config
    $bad = Test-BundleSet @($bundles.Name) (Get-ExpectedBundles $Probe)
    if ($bad) { throw [BuildStop]::new("插件产物不对：$bad（两种身份请用不同的构建目录）", 1) }
    $bundles | ForEach-Object { Write-Host "产物：$($_.FullName)" }
    [ordered]@{ sha = $script:Head; flavor = $Flavor; config = $Config; bundles = @($bundles.FullName) } | ConvertTo-Json | Set-Content -LiteralPath $script:StampFile
}

# -NoBuild 的后续步骤只认当前 HEAD、同一身份与配置的构建记录。
function Get-BuildRecord([string]$Flavor) {
    if (-not (Test-Path -LiteralPath $script:StampFile)) { throw [BuildStop]::new("$BuildDir 里没有构建记录，先构建", 2) }
    $r = Get-Content -LiteralPath $script:StampFile -Raw | ConvertFrom-Json
    if ($r.sha -cne $script:Head -or $r.flavor -cne $Flavor -or $r.config -cne $Config) {
        throw [BuildStop]::new("$BuildDir 里的构建记录是 $($r.flavor)/$($r.config) @$("$($r.sha)".Substring(0, [Math]::Min(7, "$($r.sha)".Length)))，不是当前 HEAD 的本次构建", 2)
    }
    $r
}

function Invoke-Pluginval([string[]]$Bundles, [bool]$Gui) {
    $pv = Get-Setting $Pluginval 'RINNE_PLUGINVAL' 'rinne.pluginval' 'pluginval'
    $want = (Get-Content -LiteralPath (Join-Path $Root '.pluginval-version') -Raw).Trim() -replace '^v', ''
    $vlog = Invoke-Step 'pluginval-version' $pv @('--version') 120
    if (-not ((Get-LogLines $vlog) -join "`n" -match "(?<![\d.])$([regex]::Escape($want))(?![\d.])")) {
        throw [BuildStop]::new("pluginval 不是钉版的 $want（.pluginval-version）", 1)
    }
    Remove-EnvMatching $PluginvalEnv
    $mode = $Gui ? 'gui' : 'headless'
    foreach ($b in $Bundles) {
        $a = @('--strictness-level', '5', '--timeout-ms', '60000', '--output-dir', (Join-Path $script:LogDir "pluginval-$mode"))
        if (-not $Gui) { $a += '--skip-gui-tests' }
        $null = Invoke-Step "pluginval-$mode-$(([IO.Path]::GetFileNameWithoutExtension($b)) -replace '\s+', '-')" $pv ($a + '--validate', $b) 1800
    }
}

function Invoke-Stage([string[]]$Bundles) {
    if ((Test-Path -LiteralPath $Stage) -and @(Get-ChildItem -LiteralPath $Stage -Force).Count) { throw [BuildStop]::new("-Stage 的目录必须不存在或为空：$Stage", 1) }
    $null = New-Item -ItemType Directory -Force -Path $Stage
    foreach ($b in $Bundles) { Copy-Item -LiteralPath $b -Destination $Stage -Recurse }
    $tools = @(Get-ChildItem -LiteralPath (Join-Path $BuildDir "tools-bin/$Config") -Filter '*.exe' -File -ErrorAction SilentlyContinue)
    if ($tools.Count) {
        $null = New-Item -ItemType Directory -Path (Join-Path $Stage 'tools')
        $tools | Copy-Item -Destination (Join-Path $Stage 'tools')
    }
    Write-Host "已拷到 $Stage：$($Bundles.Count) 个插件、$($tools.Count) 个工具"
}

# 自测：纯判据逐条给正反例；另起子进程核对两件只有真跑才看得出的事：构建留下的长命子进程不拖住调用方的管道，
# 调用方持槽的标记生效、残留的标记不生效。
function Invoke-SelfTest {
    $cases = [System.Collections.Generic.List[object]]::new()
    $check = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    & $check '命令行引号：空白、引号、结尾反斜杠' $false ((ConvertTo-CommandLine @('a', 'b c', 'C:\x y\', 'q"r', '')) -ceq 'a "b c" "C:\x y\\" "q\"r" ""')
    $r, $j, $bd = 'C:\repo', 'C:\repo\JUCE', 'C:\repo\build'
    $lines = @("$r\src\input\InputProcessor.cpp(12): warning C4100: 'x': unreferenced formal parameter", "$j\modules\juce_core\a.cpp(3,1): warning C4244: x",
        "$bd\juce\gen.cpp(1): warning C4996: x", "$r\third_party\x\y.c(9): warning C4018: x", 'C:\elsewhere\z.cpp(1): warning C4100: x',
        "$r\src\core\a.cpp(4): error C2065: x", 'LINK : warning LNK4098: defaultlib conflicts', "$($j -replace '\\', '/')/x.lib(y.obj) : warning LNK4221: x",
        'C:/repo/src/output/OutputProcessor.cpp(7): warning C4189: x')
    $w = @(Get-FirstPartyWarnings $lines $r @($j, $bd))
    & $check '告警归属：仓内第一方与无源文件的链接器告警算，JUCE、构建目录、third_party/、仓外与 error 不算' $true ($w.Count -eq 3 -and
        $w[0] -match 'InputProcessor' -and $w[1] -match 'LNK4098' -and $w[2] -match 'OutputProcessor')
    & $check '告警归属：没有告警时为空' $false (-not @(Get-FirstPartyWarnings @($lines[1], $lines[2]) $r @($j, $bd)).Count)
    # 头文件依赖的前缀：ninja 按字节比，同一句话换了编码也对不上
    $zh = [Text.Encoding]::UTF8.GetBytes('注意: 包含文件:')
    $rules, $okLine = (ConvertFrom-Latin1 $zh), (ConvertFrom-Latin1 ($zh + [byte[]]@(32, 32)))
    $gbk = [byte[]]@(0xD7, 0xA2, 0xD2, 0xE2, 0x3A, 0x20, 0xB0, 0xFC, 0xBA, 0xAC, 0xCE, 0xC4, 0xBC, 0xFE, 0x3A, 0x20, 0x20)   # 「注意: 包含文件:  」的 GBK 字节，不是合法 UTF-8
    $moji = ConvertFrom-Latin1 ([Text.Encoding]::UTF8.GetBytes((ConvertFrom-Latin1 $zh)))
    & $check '依赖前缀：rules.ninja 与 cl 的输出一致（cl 多出的空白不碍事）' $false (-not (Test-ShowIncludesPrefix $rules $okLine))
    & $check '依赖前缀：二次编码的乱码前缀（别的语言或编码下配置时留下的）对不上 cl 的输出' $true (Test-ShowIncludesPrefix $moji $okLine)
    & $check '依赖前缀：英文前缀对不上中文的 cl 输出' $true (Test-ShowIncludesPrefix 'Note: including file:' $okLine)
    & $check '依赖前缀：cl 的前缀比 rules.ninja 的多出别的字符' $true (Test-ShowIncludesPrefix $rules ($okLine + 'x'))
    & $check '依赖前缀：rules.ninja 没有前缀或为空' $true ((Test-ShowIncludesPrefix $null $okLine) -and (Test-ShowIncludesPrefix '  ' $okLine))
    & $check '依赖前缀：探针输出里找不到 /showIncludes 行' $true (Test-ShowIncludesPrefix $rules $null)
    $rf = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-rules-$PID-$(Get-Random).ninja"
    [IO.File]::WriteAllBytes($rf, [byte[]]([Text.Encoding]::ASCII.GetBytes("rule CXX`r`n  command = cl`r`n  msvc_deps_prefix = ") + $zh + [Text.Encoding]::ASCII.GetBytes("  `r`n  deps = msvc`r`n")))
    try { $got = Get-NinjaDepsPrefix $rf } finally { Remove-Item -LiteralPath $rf }
    & $check '依赖前缀：从 rules.ninja 按字节读出 msvc_deps_prefix（含行尾空白，不含换行）' $false ($got -ceq ($rules + '  '))
    $out = "probe.cpp`r`n$(ConvertFrom-Latin1 ($zh + [byte[]]@(32, 32)))C:\x\logs\probe.h`r`n"
    & $check '依赖前缀：从 cl 输出里取 /showIncludes 行的路径前缀' $false ((Get-ClIncludePrefix $out 'probe.h') -ceq ($rules + '  '))
    $unc = { param($path) Get-ClIncludePrefix "x`r`n$(ConvertFrom-Latin1 $zh)  $path`r`n" 'probe.h' }
    & $check '依赖前缀：UNC 路径与 \\?\ 长路径的 include 行也取得到' $false (((& $unc '\\srv\share\logs\probe.h') -ceq ($rules + '  ')) -and ((& $unc '\\?\C:\x\probe.h') -ceq ($rules + '  ')))
    & $check '依赖前缀：cl 输出里没有那条 include 行时取不到' $false ($null -eq (Get-ClIncludePrefix "probe.cpp`r`n" 'probe.h'))
    $ov = Get-PrefixOverride $rules
    & $check '依赖前缀覆盖：干净的 UTF-8 前缀生成 cmake 文件，C、CXX 与通用变量都设' $false ($ov -match 'foreach\(lang C CXX\)' -and $ov -match '\$\{lang\}_CL_SHOWINCLUDES_PREFIX' -and $ov.Contains((ConvertTo-Display $rules)))
    & $check '依赖前缀覆盖：不是合法 UTF-8 的前缀（GBK 字节）不覆盖' $true ($null -eq (Get-PrefixOverride (ConvertFrom-Latin1 $gbk)))
    & $check '依赖前缀覆盖：含引号、反斜杠、美元符或空的前缀不覆盖' $true (-not ((Get-PrefixOverride 'a"b: ') -or (Get-PrefixOverride 'a\b: ') -or (Get-PrefixOverride 'a$b: ') -or (Get-PrefixOverride '  ')))
    & $check '依赖记录：没有记录要 clean' $true (Test-DepsNeedClean $null 'AA-BB')
    & $check '依赖记录：记录的前缀与这次的不同要 clean' $true (Test-DepsNeedClean 'AA-BB' 'AA-CC')
    & $check '依赖记录：记录与这次相同不 clean' $false (-not (Test-DepsNeedClean 'AA-BB' 'AA-BB'))
    & $check '依赖记录：前缀转十六进制按字节' $false ((Get-DepsPrefixHex 'a:') -ceq '61-3A')
    & $check 'ninja -t deps：有目标记下依赖' $false (-not (Test-DepsRecorded @('a.obj: #deps 3, deps mtime 1 (VALID)', '    ../a.h')))
    & $check 'ninja -t deps：全是 #deps 0（前缀对不上时的样子）' $true (Test-DepsRecorded @('a.obj: #deps 0, deps mtime 1 (VALID)', 'b.obj: #deps 0, deps mtime 2 (VALID)'))
    & $check 'ninja -t deps：什么都没输出' $true (Test-DepsRecorded @())
    $formal, $probe = (Get-ExpectedBundles $false), (Get-ExpectedBundles $true)
    & $check '产物断言：正式版恰好两个' $false (-not (Test-BundleSet $formal $formal))
    & $check '产物断言：缺一个' $true (Test-BundleSet @($formal[0]) $formal)
    & $check '产物断言：正式目录里混进探针产物' $true (Test-BundleSet ($formal + $probe[0]) $formal)
    & $check '产物断言：探针恰好两个' $false (-not (Test-BundleSet $probe $probe))
    $cache = { param($v, $d) "// x`r`nRINNE_SHM_PREFIX:STRING=$v`r`nRINNE_SHM_PREFIX_DEFAULT:INTERNAL=$d`r`n" }
    & $check 'cache 断言：前缀是默认值' $false (-not (Test-CachePrefix (& $cache 'SynchainRinne' 'SynchainRinne')))
    & $check 'cache 断言：CI 覆盖成本机前缀' $true (Test-CachePrefix (& $cache 'SynchainRinneD' 'SynchainRinne'))
    & $check 'cache 断言：缺条目' $true (Test-CachePrefix 'RINNE_SHM_PREFIX:STRING=SynchainRinne')
    $f = Get-ShmPrefix 'ci'
    & $check '段名前缀从根 CMakeLists.txt 读：本机加 D、探针加 P' $false ($f -and (Get-ShmPrefix 'dev') -ceq "${f}D" -and (Get-ShmPrefix 'probe') -ceq "${f}P")
    # 凭据：按名字（*_PAT、*_CREDENTIALS、不带下划线的 *APIKEY*、*_PASSPHRASE）与按值（带口令的 URL、GitHub token）都删，别的留着。
    # 夹具的值在运行时拼出来，源码里没有完整的形态
    $fake = @{ RINNE_SELFTEST_PAT = 'x'; RINNE_SELFTEST_CREDENTIALS = 'x'; RINNE_SELFTESTAPIKEY = 'x'; RINNE_SELFTEST_PASSPHRASE = 'x'
        RINNE_SELFTEST_PWD = 'x'; RINNE_SELFTEST_PASS = 'x'; RINNE_SELFTEST_JWT = 'x'; DOCKER_AUTH_CONFIG = 'x'
        RINNE_SELFTEST_URL = 'https://u' + ':p@example.invalid/x'; RINNE_SELFTEST_GH = 'gh' + 'p_' + ('a' * 36)
        RINNE_SELFTEST_V1 = 'ey' + 'J' + ('a' * 12) + '.' + ('b' * 12) + '.c'; RINNE_SELFTEST_V2 = 'xo' + 'xb-' + ('1' * 12)
        RINNE_SELFTEST_V3 = 'gl' + 'pat-' + ('a' * 20); RINNE_SELFTEST_V4 = 'np' + 'm_' + ('a' * 36); RINNE_SELFTEST_V5 = 'AK' + 'IA' + ('A' * 16) }
    $keep = @{ RINNE_SELFTEST_KEEP = 'y'; RINNE_SELFTEST_PLAINURL = 'https://example.invalid/x' }
    foreach ($e in @($fake.GetEnumerator()) + @($keep.GetEnumerator())) { [Environment]::SetEnvironmentVariable($e.Key, $e.Value) }
    Remove-EnvMatching $SecretEnv $SecretEnvValue
    $left = @($fake.Keys | Where-Object { [Environment]::GetEnvironmentVariable($_) })
    $lost = @($keep.Keys | Where-Object { [Environment]::GetEnvironmentVariable($_) -cne $keep[$_] })
    & $check "凭据变量按名字与按值从环境里删掉，别的留着$(if ($left.Count + $lost.Count) { "（漏删 $($left -join '、')；误删 $($lost -join '、')）" })" $true (-not ($left.Count + $lost.Count))
    foreach ($k in $keep.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
    $pwsh = (Get-Process -Id $PID).Path
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-selftest-$PID-$(Get-Random)"
    $null = New-Item -ItemType Directory -Path $tmp
    $spawn = { param([string]$Command) $psi = [Diagnostics.ProcessStartInfo]::new($pwsh)
        foreach ($a in '-NoProfile', '-NonInteractive', '-Command', $Command) { $psi.ArgumentList.Add($a) }
        $psi.UseShellExecute, $psi.RedirectStandardOutput, $psi.RedirectStandardError = $false, $true, $true
        [Diagnostics.Process]::Start($psi) }
    $child = $null
    try {
        # 子进程用 Invoke-Logged 跑一条命令，它留下一个活 8 秒的孙进程；父进程像调用方那样读子进程的管道直到 EOF
        $eof = {
            param([bool]$Protect)
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $p = & $spawn (". '$PSCommandPath'; $($Protect ? 'Protect-StdHandles;' : '') " +
                "exit (Invoke-Logged 'cmd.exe' @('/c', 'start', '/b', '', '$pwsh', '-NoProfile', '-Command', 'Start-Sleep', '8') '$tmp\l$([int]$Protect).log' 60)")
            $out, $err = $p.StandardOutput.ReadToEndAsync(), $p.StandardError.ReadToEndAsync()
            $null = $out.Wait(30000) -and $err.Wait(30000)
            $sw.Elapsed.TotalSeconds
        }
        $fast, $slow = (& $eof $true), (& $eof $false)
        & $check ('构建留下的长命子进程不拖住调用方的管道（{0:N1} 秒读到 EOF）' -f $fast) $false ($fast -lt 5)
        & $check ('反例：不改句柄继承时调用方要等孙进程退出（{0:N1} 秒）' -f $slow) $true ($slow -ge 6)
        # 持槽标记（换一组自测专用的锁名，标记取本进程父进程的 pid）：两把槽都空时标记当残留、照常取槽；
        # 子进程占住一把时标记生效、不再取；标记的 pid 不是祖先时（例如那个子进程自己），哪怕有一把槽被占也照常取槽
        Import-Module (Join-Path $PSScriptRoot 'lib/Locks.psm1') -ErrorAction Stop
        $scope = "Rinne-selftest-$PID"
        & (Get-Module Locks) { param($s) $script:Scope = $s } $scope
        $env:RINNE_BUILD_SLOT_HELD, $script:Jobs, $script:MemWaitMinutes = "$((Get-Process -Id $PID).Parent.Id)", 2, 0
        $lock = Enter-BuildSlot
        & $check '持槽标记是残留的（两把槽都空）：照常取槽' $true ($null -ne $lock)
        Exit-RinneLock $lock
        $ready = Join-Path $tmp 'ready'
        $child = & $spawn ("Import-Module '$(Join-Path $PSScriptRoot 'lib/Locks.psm1')'; & (Get-Module Locks) { `$script:Scope = '$scope' }; " +
            "`$l = Enter-RinneLock build-slot-1 -TimeoutSeconds 5 -Quiet; if (`$l) { Set-Content -LiteralPath '$ready' x }; Start-Sleep 60")
        $deadline = (Get-Date).AddSeconds(30)
        while (-not (Test-Path -LiteralPath $ready) -and -not $child.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
        $lock = Enter-BuildSlot
        & $check '调用方持着一把构建槽：不再取槽' $false ((Test-Path -LiteralPath $ready) -and $null -eq $lock)
        Exit-RinneLock $lock
        $env:RINNE_BUILD_SLOT_HELD = "$($child.Id)"
        $lock = Enter-BuildSlot
        & $check '标记的 pid 不是本进程的祖先，另一把槽被别人占着：照常取槽' $true ((Test-Path -LiteralPath $ready) -and $null -ne $lock)
        Exit-RinneLock $lock
        # -CheckCache 真跑（子进程，临时构建目录）：cache 是默认值还不够，编进去的段名前缀与探针开关也要对
        $bd = Join-Path $tmp 'cc'
        $null = New-Item -ItemType Directory -Path (Join-Path $bd 'src/core/generated/util')
        Set-Content -LiteralPath (Join-Path $bd 'CMakeCache.txt') -Value (& $cache $f $f)
        $cc = { param($p, $b) Set-Content -LiteralPath (Join-Path $bd 'src/core/generated/util/RinneVersion.h') -Value "inline constexpr const char* kShmPrefix = `"$p`";`ninline constexpr bool kProbeBuild = $b;"
            $null = & $pwsh -NoProfile -NonInteractive -File $PSCommandPath -CheckCache -BuildDir $bd 2>&1; $LASTEXITCODE }
        & $check '-CheckCache：cache 与编进去的段名前缀、探针开关都对' $false ((& $cc $f 'false') -eq 0)
        & $check '-CheckCache：cache 是默认值，编进去的却被同名普通变量遮成本机前缀' $true ((& $cc "${f}D" 'false') -eq 1)
        & $check '-CheckCache：正式构建编进去的 kProbeBuild 是 true' $true ((& $cc $f 'true') -eq 1)
        # 编进去的值都对、cache 却被覆盖成本机前缀：cache 那一道照样判红
        Set-Content -LiteralPath (Join-Path $bd 'CMakeCache.txt') -Value (& $cache "${f}D" $f)
        & $check '-CheckCache：编进去的对、cache 被覆盖成本机前缀' $true ((& $cc $f 'false') -eq 1)
        Set-Content -LiteralPath (Join-Path $bd 'CMakeCache.txt') -Value (& $cache $f $f)
        # -Ci 只在 GitHub Actions 上：没有 GITHUB_ACTIONS=true 退出 1；有的话照常往下走（这里没有构建记录，退出 2）
        $savedGa = $env:GITHUB_ACTIONS
        $ci = { param($v) $env:GITHUB_ACTIONS = $v; $o = & $pwsh -NoProfile -NonInteractive -File $PSCommandPath -Ci -NoBuild -BuildDir $bd 2>&1 | Out-String; @($LASTEXITCODE, $o) }
        $off, $on = (& $ci $null), (& $ci 'true')
        $env:GITHUB_ACTIONS = $savedGa
        & $check '-Ci 在 GitHub Actions 之外被拒' $true ($off[0] -eq 1 -and $off[1] -match 'GITHUB_ACTIONS=true')
        & $check '-Ci 在 GITHUB_ACTIONS=true 时照常往下走' $false ($on[0] -eq 2)
        # JUCE 守卫（真跑 CMake，配置一个不启用语言的小工程）：干净的检出通过，只多了未跟踪的文件也通过，跟踪的文件改过判 FATAL
        $cmakeExe = (Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
        $ninjaExe = (Get-Command (Get-Setting $Ninja 'RINNE_NINJA' 'rinne.ninja' 'ninja') -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
        if (-not $cmakeExe -or -not $ninjaExe) { & $check 'JUCE 守卫自测要 PATH 上的 cmake 与 ninja（或 RINNE_NINJA / git config rinne.ninja）' $true $false }
        else {
            $jx, $gs = (Join-Path $tmp 'juce'), (Join-Path $tmp 'guard')
            $null = New-Item -ItemType Directory -Path $jx, $gs
            Set-Content -LiteralPath (Join-Path $jx 'CMakeLists.txt') -Value 'project(JUCE VERSION 8.0.8)'
            $g = { git -C $jx -c user.name=selftest -c user.email=selftest@example.invalid -c core.autocrlf=false -c commit.gpgsign=false @args 2>&1 }
            $null = (& $g init -q), (& $g add -A), (& $g commit -q -m x)
            Set-Content -LiteralPath (Join-Path $gs '.juce-commit') -Value "$(& $g rev-parse HEAD)".Trim()
            Set-Content -LiteralPath (Join-Path $gs '.juce-version') -Value '8.0.8'
            Set-Content -LiteralPath (Join-Path $gs 'CMakeLists.txt') -Value ("cmake_minimum_required(VERSION 3.22)`nproject(guard NONE)`n" +
                "include(`"$((Join-Path $Root 'cmake/RinneJuceGuard.cmake') -replace '\\', '/')`")")
            $guard = { param($n) $o = & $cmakeExe -S $gs -B (Join-Path $tmp "gb-$n") -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DJUCE_PATH=$jx" 2>&1 | Out-String; @($LASTEXITCODE, $o) }
            $clean = & $guard 'clean'
            Set-Content -LiteralPath (Join-Path $jx 'stray.txt') -Value 'x'
            $stray = & $guard 'stray'
            Add-Content -LiteralPath (Join-Path $jx 'CMakeLists.txt') -Value '# 改过'
            $dirty = & $guard 'dirty'
            & $check 'JUCE 守卫：干净的检出通过' $false ($clean[0] -eq 0)
            & $check 'JUCE 守卫：只多了未跟踪的文件，不算改动' $false ($stray[0] -eq 0)
            & $check 'JUCE 守卫：跟踪的文件被改过，配置期 FATAL' $true ($dirty[0] -ne 0 -and $dirty[1] -match '检出有改动')
        }
    } catch {
        & $check "自测抛错：$($_.Exception.Message)" $true $false
    } finally {
        if ($child -and -not $child.HasExited) { $child.Kill() }
        $env:RINNE_BUILD_SLOT_HELD = $null
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
    $failed = @($cases | Where-Object { -not $_.Passed })
    $failed | ForEach-Object { Write-Host "自测未通过：$($_.Label)" }
    $nBad = @($cases | Where-Object Bad).Count
    Write-Host ('build --self-test：{0}（{1} 项，坏样例 {2}、正例 {3}）' -f ($failed ? '失败' : '通过'), $cases.Count, $nBad, ($cases.Count - $nBad))
    [int]($failed.Count -gt 0)
}

if ($MyInvocation.InvocationName -eq '.') { return }   # 只取函数（自测的子进程这样用）
Protect-StdHandles
Remove-EnvMatching $SecretEnv $SecretEnvValue
if ($SelfTest) { exit (Invoke-SelfTest) }

$code, $slot = 0, $null
try {
    if (-not $IsWindows) { throw [BuildStop]::new('build.ps1 只在 Windows 上构建', 1) }
    if (-not $BuildDir) { $BuildDir = $Probe ? 'build-probe' : 'build' }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir, $Root)
    $script:LogDir = Join-Path $BuildDir 'logs'
    $script:StampFile = Join-Path $BuildDir 'rinne-build.json'
    $script:Jobs = $Jobs
    $script:Head = "$(git -C $Root rev-parse HEAD 2>$null)".Trim()
    $flavor = $Probe ? 'probe' : ($Ci ? 'ci' : 'dev')
    if ($Ci -and $env:GITHUB_ACTIONS -cne 'true') { throw [BuildStop]::new('-Ci 只在 GitHub Actions 上用（GITHUB_ACTIONS=true）：本机构建要取构建槽、用带 D 的段名前缀', 1) }
    if ($CheckCache) {
        $f = Join-Path $BuildDir 'CMakeCache.txt'
        $h = Join-Path $BuildDir 'src/core/generated/util/RinneVersion.h'
        $read = { param($p) (Test-Path -LiteralPath $p) ? (Get-Content -LiteralPath $p -Raw) : $null }
        $bad = @($(if ($null -eq ($c = & $read $f)) { "没有 $f" } else { Test-CachePrefix $c })
            $(if ($null -eq ($t = & $read $h)) { "没有 $h" } else { Test-BuildConstants $t (Get-ShmPrefix ($Probe ? 'probe' : 'ci')) $Probe })) | Where-Object { $_ }
        if ($bad) { throw [BuildStop]::new($bad -join '；', 1) }
        Write-Host "段名前缀是默认值，编进去的也是它：$(Get-CacheValue $c 'RINNE_SHM_PREFIX')（探针 $($Probe ? '是' : '否')）"
    } else {
        if (-not $NoBuild) {
            $slot = Enter-BuildSlot
            Invoke-Build $flavor
            if ($slot) { Exit-RinneLock $slot; $slot = $null }
        }
        $record = Get-BuildRecord $flavor
        if ($Test) {
            if (Test-Path -LiteralPath (Join-Path $Root 'tests/CMakeLists.txt')) {
                $null = Invoke-Step 'ctest' 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure', '--no-tests=error', '--timeout', '300') 1800
            } else { Write-Host '== ctest：还没有 tests/CMakeLists.txt，跳过' }
        }
        if ($Validate) { Invoke-Pluginval @($record.bundles) $false }
        if ($ValidateGui) { Invoke-Pluginval @($record.bundles) $true }
        if ($Stage) { Invoke-Stage @($record.bundles) }
    }
    Write-Host '完成'
} catch [BuildStop] {
    Write-Host "失败：$($_.Exception.Message)"
    $code = $_.Exception.Code
} finally {
    if ($slot) { Exit-RinneLock $slot }
}
exit $code
