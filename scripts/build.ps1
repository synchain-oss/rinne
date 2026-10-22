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
# project() 之后覆盖 CMAKE_CL_SHOWINCLUDES_PREFIX，再重配；两步之后仍对不上判红。
# 判红而不是悄悄放过的理由：前缀对不上时 ninja 不报错，只是不记依赖，只改 .h 时 gates 会跑在旧对象上。
# clean：修过的构建目录 clean 一次（旧目标文件没有依赖记录）；核对通过的前缀记在 <构建目录>/rinne-deps-prefix-<配置>.txt
# （clean 只清当前配置，所以按配置分开记），没有记录（含 0 字节的残留文件）或记录不同的目录也 clean 一次。
# 构建后再查 ninja -t deps：至少记下一条依赖，且本次新编的第一方编译单元（源文件里有 #include "…" 的）各自都要记下，否则判红。
# 前缀的空白口径：ninja 逐字节比前缀，前缀之后只跳过空格（0x20），Tab、换行、0x85、0xA0 都会让路径记错。核对只容忍空格。
# 探针贴近第一方目标的真实编译命令：第一方目标在 cmake/RinneWarnings.cmake 里加了 /utf-8（源文件注释是中文），CI 上还经
# -Launcher（sccache）跑，所以探针带 /utf-8 跑一次、不带 /utf-8 跑一次、给了 -Launcher 时再经它跑一次，几次的前缀必须一致。
# 探针自己出错（没跑起来、输出里找不到 include 行、几种跑法的前缀不一致）与「前缀对不上」是两回事，分开报错，不清探测也不重配。
# 代码页：cl 的输出编码跟随控制台代码页（实测 936 是乱码、65001 才是「注意: 包含文件:」）。脚本开头把 [Console]::OutputEncoding
# 钉在 UTF-8 是前缀一致的前提之一（探针与第一方 TU 都靠它），别去掉。
# 覆盖文件（rinne-showincludes-prefix.cmake）是持久的：-DCMAKE_PROJECT_INCLUDE 写进 cache 后一直有效，所以每次配置都显式表态
# （文件在就传 -D、不在就传 -U），重配那一步固定传 -U（要真的重新探测），覆盖那一步才传 -D。cache 里记着覆盖文件而文件没了时，
# 不带 -U 的 cmake 会在 project() 处报「could not find requested file」；带 -U 就把那条记录清掉，这里打印一行说明并照常往下走。
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

# rules.ninja 的前缀（$Rules）与 cl 实际输出的前缀（$Cl）对不上就返回错误文字，对上了返回空。
# ninja 先要求行以 $Rules 开头，再跳过后面的空白，所以 cl 的前缀可以比 $Rules 多出空白，但不能多出别的。
# 跳过的空白只认空格（0x20）：ninja 1.13.1 实测只跳过空格，Tab、换行、0x85、0xA0 都会被它算进路径里（路径记错），
# 所以这里也只容忍空格，别用 .Trim()（它还吞 \t \r \n \v \f U+0085 U+00A0）。正则用 \z 收尾：$ 会放过末尾的一个 \n。
function Test-ShowIncludesPrefix($Rules, $Cl) {
    if ($null -eq $Rules -or -not "$Rules".Trim()) { return 'rules.ninja 里没有 msvc_deps_prefix，或者它是空的' }
    if ($null -eq $Cl) { return 'cl 探针的输出里找不到 /showIncludes 行' }
    if ($Cl.StartsWith($Rules, [StringComparison]::Ordinal) -and $Cl.Substring($Rules.Length) -match '^ *\z') { return '' }
    "rules.ninja 的 msvc_deps_prefix 是「$(ConvertTo-Display $Rules)」，cl 实际输出的是「$(ConvertTo-Display $Cl)」，字节对不上，ninja 一条头文件依赖都不会记"
}

# ninja -t deps 的输出里至少有一个目标记下了依赖（形如 `x.obj: #deps 12, deps mtime …`）；返回错误文字，有就返回空。
function Test-DepsRecorded([string[]]$Lines) {
    $n = @($Lines | Where-Object { $_ -match ': #deps ([1-9]\d*),' }).Count
    $n ? '' : 'ninja -t deps 里没有任何一个目标记下头文件依赖：只改 .h 时含它的编译单元不会重编'
}

# ninja -t deps 的输出解析成 { 对象路径 => 依赖个数 }；对象路径以构建目录为基准（ninja 就是这么记的）。
function Get-DepsCounts([string[]]$Lines) {
    $out = @{}
    foreach ($l in $Lines) {
        if ($l -match '^(?<obj>.+?): #deps (?<n>\d+),') { $out[$Matches.obj] = [int]$Matches.n }
    }
    $out
}

# 构建日志里这次新编的对象（形如 `[3/264] Building CXX object src\core\…\Foo.cpp.obj`）；返回构建目录为基准的相对路径。
function Get-BuiltObjects([string[]]$Lines) {
    @($Lines | ForEach-Object {
            if ($_ -match '^\[\d+/\d+\] Building C(?:XX)? object (?<obj>.+?\.obj)\s*$') {
                $Matches.obj -replace '\\', '/'
            }
        })
}

# 对象路径 → 源文件（仓内相对路径）：CMake 的对象路径是 <目标目录>/CMakeFiles/<目标>.dir/<配置>/<源路径>.obj，
# 源路径里的 `__` 是 CMake 对仓外路径的转义（../..）。返回 $null 表示不是仓内的源文件（JUCE、第三方）。
function Get-ObjectSource([string]$Obj) {
    if ($Obj -notmatch '^(?<dir>.+)/CMakeFiles/(?<tgt>[^/]+)\.dir/[^/]+/(?<src>.+)\.obj$') { return $null }
    $rel = $Matches.src -replace '(^|/)__($|/)', '$1../$2'
    $segs = [System.Collections.Generic.List[string]]::new()
    foreach ($s in (($Matches.dir + '/' + $rel) -split '/')) {
        if ($s -eq '..') { if ($segs.Count) { $segs.RemoveAt($segs.Count - 1) } }
        elseif ($s -and $s -ne '.') { $segs.Add($s) }
    }
    $segs -join '/'
}

# 仓内源文件里有 #include "…" 的（尖括号的是系统或第三方头文件，不算第一方）。
function Test-SourceHasQuotedInclude([string]$Abs) {
    if (-not (Test-Path -LiteralPath $Abs -PathType Leaf)) { return $false }
    foreach ($l in @(Get-Content -LiteralPath $Abs)) {
        if ($l -match '^\s*#\s*include\s+"') { return $true }
    }
    $false
}

# 本次新编的第一方编译单元各自都要记下依赖：源文件里有 #include "…" 的，ninja 必须给它记下至少一条。
# 只看「目录里有任意一个目标记了依赖」抓不到「第一方 TU 这次没记依赖」（增量构建里旧记录一直在）。
# 本次没有新编第一方 TU（增量构建无事可做）时退回查 ninja 的记录：记录跨构建保留，至少要有一个第一方对象记下依赖。
# $Objects 是本次新编的对象（构建目录为基准），$Counts 是 ninja -t deps 的 { 对象 => 依赖个数 }，$RepoRoot 是仓根。
# 返回错误文字，都记下了返回空。
function Test-FirstPartyDeps([string[]]$Objects, $Counts, [string]$RepoRoot) {
    $bad = [System.Collections.Generic.List[string]]::new()
    foreach ($obj in $Objects) {
        $src = Get-ObjectSource $obj
        if (-not $src) { continue }
        $abs = Join-Path $RepoRoot $src
        # 仓外的源文件（JUCE、第三方）在仓里找不到，没有带引号的头文件可查，一并跳过；没有第一方头文件要记的 TU 也跳过
        if (-not (Test-SourceHasQuotedInclude $abs)) { continue }
        $n = 0
        if ($Counts.ContainsKey($obj)) { $n = $Counts[$obj] }
        if ($n -lt 1) { $bad.Add("$src（$obj）") }
    }
    if ($bad.Count) {
        return "本次新编的第一方编译单元没有记下头文件依赖：$($bad -join '、')"
    }
    # 都记下了，或者本次没有新编第一方 TU：查 ninja 的记录里有没有第一方对象记下依赖（记录跨构建保留；
    # 本次新编的第一方 TU 都记下了的话，它们自己就在记录里，这一步必然通过）
    foreach ($obj in $Counts.Keys) {
        $src = Get-ObjectSource $obj
        if (-not $src) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $src) -PathType Leaf)) { continue }
        if ($Counts[$obj] -ge 1) { return '' }
    }
    'ninja -t deps 里没有任何一个第一方对象记下头文件依赖：只改 .h 时含它的编译单元不会重编'
}

# 核对通过的前缀（Latin-1 字符串）的十六进制，记在构建目录的 rinne-deps-prefix-<配置>.txt 里。
function Get-DepsPrefixHex([string]$Prefix) { [BitConverter]::ToString([Text.Encoding]::Latin1.GetBytes("$Prefix")) }

# 记录缺失或与这次核对通过的前缀不同，就要 clean：旧目标文件可能没有依赖记录。
function Test-DepsNeedClean($Recorded, [string]$Verified) { $null -eq $Recorded -or $Recorded -cne $Verified }

# 读记录文件（核对通过的前缀的十六进制）。0 字节的文件（写的时候进程被杀）读出来是 $null：直接对它 .Trim() 会抛
# RuntimeException（catch [BuildStop] 接不住，之后每次构建都失败且不自愈）。空值与全是空白都按「没有记录」处理（触发一次 clean）。
function Read-DepsMarker([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $raw = Get-Content -LiteralPath $Path -Raw
    if ($null -eq $raw) { return $null }
    $text = $raw.Trim()
    $text ? $text : $null
}

# 决定要不要 clean（前缀修过一次，或记录缺失、为空、不同），再把这次核对通过的前缀记下来。clean 失败就抛，记录不更新，下次还会 clean。
# $Resolved 是 Resolve-DepsPrefix 的返回值，$Verified 是这次核对通过的前缀的十六进制，$Clean 是执行 clean 的脚本块。
function Sync-DepsMarker($Resolved, [string]$MarkerFile, [string]$Verified, [scriptblock]$Clean) {
    $recorded = Read-DepsMarker $MarkerFile
    if ($Resolved.Clean -or (Test-DepsNeedClean $recorded $Verified)) { & $Clean }
    Set-Content -LiteralPath $MarkerFile -Value $Verified
}

# 构建后的依赖断言：ninja -t deps 里至少一个目标记下依赖，且本次新编的第一方编译单元各自都记下，否则抛 BuildStop。
function Assert-DepsAfterBuild([string[]]$DepsLines, [string[]]$BuildLines, [string]$RepoRoot) {
    $noDeps = Test-DepsRecorded $DepsLines
    if ($noDeps) { throw [BuildStop]::new($noDeps, 1) }
    $bad = Test-FirstPartyDeps (Get-BuiltObjects $BuildLines) (Get-DepsCounts $DepsLines) $RepoRoot
    if ($bad) { throw [BuildStop]::new($bad, 1) }
}

# 每次配置显式表态要不要带覆盖文件（见头部注释）：文件在就 -D；不在就 -U，cache 里还记着它时附一行说明。返回 @{ Arg; Note }。
function Get-ProjectIncludeArg([string]$OverrideFile, [string]$CacheText) {
    if (Test-Path -LiteralPath $OverrideFile -PathType Leaf) { return @{ Arg = "-DCMAKE_PROJECT_INCLUDE=$OverrideFile"; Note = '' } }
    $cached = Get-CacheValue $CacheText 'CMAKE_PROJECT_INCLUDE'
    $note = ''
    if ($cached) {
        $note = "CMakeCache.txt 里还记着覆盖文件 $cached，但它不在了：这次配置带 -UCMAKE_PROJECT_INCLUDE 清掉那条记录，" +
        '前缀按无覆盖重新探测，对不上再重写覆盖文件（不清掉的话 cmake 会在 project() 处失败）'
    }
    @{ Arg = '-UCMAKE_PROJECT_INCLUDE'; Note = $note }
}

# 用一个两行的探针编译单元（写在构建目录的 logs 下）按本进程的环境跑 cl，取它的 /showIncludes 前缀（Latin-1 字符串）；
# 失败时 $script:ProbeError 记原因并返回 $null。
# 探针跑的命令要与第一方目标一致：第一方目标带 /utf-8（cmake/RinneWarnings.cmake），CI 上还经 launcher（sccache）跑。
# $Extra 是额外加的编译开关（/utf-8），$Launcher 是编译缓存（经它跑时命令变成 launcher cl …）。
function Invoke-ClProbe([string[]]$Extra, [string]$Launcher) {
    $dir = Join-Path $script:LogDir 'prefix-probe'
    $null = New-Item -ItemType Directory -Force -Path $dir
    Set-Content -LiteralPath (Join-Path $dir 'probe.h') -Value '#define RINNE_PROBE_H 1'
    Set-Content -LiteralPath (Join-Path $dir 'probe.cpp') -Value '#include "probe.h"'
    $log = Join-Path $dir 'cl.log'
    $exe = 'cl.exe'
    $clArgs = @('/nologo', '/showIncludes', '/c') + $Extra + @((Join-Path $dir 'probe.cpp'), "/Fo$(Join-Path $dir 'probe.obj')")
    if ($Launcher) { $exe = $Launcher; $clArgs = @('cl.exe') + $clArgs }
    $code = Invoke-Logged $exe $clArgs $log 120
    if ($code -ne 0) { return @{ Error = "cl 探针编译退出码 $code（日志 $log）" } }
    @{ Prefix = Get-ClIncludePrefix (ConvertFrom-Latin1 ([IO.File]::ReadAllBytes($log))) 'probe.h' }
}

# 探针几种跑法得到的前缀必须一致：与第一方目标跑的命令一致才算数。返回错误文字，一致返回空。
function Test-ProbeRunsAgree([string[]]$Labels, [string[]]$Prefixes) {
    if (@($Prefixes | Select-Object -Unique).Count -le 1) { return '' }
    'cl 探针的几种跑法得到的前缀不一致：' + (@(0..($Prefixes.Count - 1) | ForEach-Object { "$($Labels[$_])=「$(ConvertTo-Display $Prefixes[$_])」" }) -join '；')
}

# 探针的跑法：带 /utf-8、不带 /utf-8，给了 -Launcher 时再经它跑一次。返回 cl 的前缀（Latin-1 字符串）；
# 失败时 $script:ProbeError 记原因并返回 $null（探针自己没跑起来、输出里没有那条 include 行、几种跑法的前缀不一致都算探针出错）。
function Get-ProbedClPrefix {
    $script:ProbeError = ''
    $runs = @(@{ Label = '带 /utf-8'; Extra = @('/utf-8'); Launcher = '' }, @{ Label = '不带 /utf-8'; Extra = @(); Launcher = '' })
    if ($Launcher) { $runs += @{ Label = '经编译缓存'; Extra = @('/utf-8'); Launcher = $Launcher } }
    $labels, $prefixes = @(), @()
    foreach ($run in $runs) {
        $r = Invoke-ClProbe $run.Extra $run.Launcher
        if ($r.Error) { $script:ProbeError = "cl 探针（$($run.Label)）没跑起来：$($r.Error)"; return $null }
        if ($null -eq $r.Prefix) { $script:ProbeError = "cl 探针（$($run.Label)）的输出里找不到 /showIncludes 行"; return $null }
        $labels += $run.Label
        $prefixes += $r.Prefix
    }
    $disagree = Test-ProbeRunsAgree $labels $prefixes
    if ($disagree) { $script:ProbeError = $disagree; return $null }
    $prefixes[0]
}

# 构建目录的 rules.ninja 与 cl 探针核对。返回 @{ Problem; ProbeError; ClPrefix }：
# ProbeError 非空是探针自己出错（与前缀无关，调用方要分开报错，不能走「删编译器信息重配」那条路）；
# Problem 非空是前缀对不上；ClPrefix 是这次探到的 cl 前缀（写覆盖文件时要用）。
function Get-DepsPrefixStatus {
    $cl = Get-ProbedClPrefix
    if ($script:ProbeError) { return @{ Problem = ''; ProbeError = $script:ProbeError; ClPrefix = $null } }
    $problem = Test-ShowIncludesPrefix (Get-NinjaDepsPrefix (Join-Path $BuildDir 'CMakeFiles/rules.ninja')) $cl
    @{ Problem = $problem; ProbeError = ''; ClPrefix = $cl }
}

# 前缀对不上时的修复流程（纯函数：只按探针与配置的结果决定下一步，跑 cmake 与写文件交给传入的脚本块）。
# $Probe 返回 @{ Problem; ProbeError; ClPrefix }；
# $Configure 参数是覆盖文件路径（$null = 不带覆盖重配，要真的重新探测），返回同样的形状；
# $Clear 清掉构建目录里缓存的编译器探测结果；$WriteOverride 参数是 cl 实测的前缀，返回写出的覆盖文件路径
# （前缀不能写成 cmake 字符串时返回 $null）。
# 返回 @{ Clean; Override }：Clean 是要不要 clean 一次（旧目标文件没有依赖记录）。
# 探针自己失败与「前缀对不上」分开报错：探针失败不重配、不清探测。覆盖之后仍对不上就抛 BuildStop。
function Resolve-DepsPrefix {
    param([scriptblock]$Probe, [scriptblock]$Configure, [scriptblock]$Clear, [scriptblock]$WriteOverride)
    $s = & $Probe
    if ($s.ProbeError) { throw [BuildStop]::new("cl 探针出错，与前缀对不对上无关，所以不清探测也不重配：$($s.ProbeError)", 1) }
    if (-not $s.Problem) { return @{ Clean = $false; Override = $null } }
    & $Clear
    $s = & $Configure $null
    if ($s.ProbeError) { throw [BuildStop]::new("重配后 cl 探针出错：$($s.ProbeError)", 1) }
    if (-not $s.Problem) { return @{ Clean = $true; Override = $null } }
    $file = & $WriteOverride $s.ClPrefix
    if (-not $file) { throw [BuildStop]::new("重配后头文件依赖前缀仍对不上，cl 的前缀也不能写成 cmake 字符串：$($s.Problem)", 1) }
    $s = & $Configure $file
    if ($s.ProbeError) { throw [BuildStop]::new("覆盖后 cl 探针出错：$($s.ProbeError)", 1) }
    if ($s.Problem) { throw [BuildStop]::new("覆盖之后头文件依赖前缀仍对不上：$($s.Problem)", 1) }
    @{ Clean = $true; Override = $s.ClPrefix }
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
    # 覆盖文件是持久的（-DCMAKE_PROJECT_INCLUDE 写进 cache 后一直有效）：每次配置都显式表态，文件在就传 -D、不在就传 -U。
    # cache 里还记着覆盖文件而文件没了：-U 把那条记录清掉（不清的话 cmake 在 project() 处报一个看不懂的错），这里说一声。
    $overrideFile = Join-Path $BuildDir 'rinne-showincludes-prefix.cmake'
    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    $includeArg = Get-ProjectIncludeArg $overrideFile ((Test-Path -LiteralPath $cacheFile -PathType Leaf) ? (Get-Content -LiteralPath $cacheFile -Raw) : '')
    if ($includeArg.Note) { Write-Host $includeArg.Note }
    $null = Invoke-Step 'configure' 'cmake' ($cfg + $includeArg.Arg) 1800
    # 头文件依赖的前缀对不上：先清掉编译器探测结果重配（这一步不带覆盖，要真的重新探测），仍不行就用 cl 实测的前缀覆盖；
    # 旧目标文件没有依赖记录，clean 一次让它们全部重编。探针自己失败与前缀对不上分开报错。
    $probeStatus = {
        $s = Get-DepsPrefixStatus
        if ($s.Problem) { Write-Host "头文件依赖前缀对不上：$($s.Problem)" }
        $s
    }
    $configure = {
        param($file)
        $cmakeArgs = $cfg + ($file ? "-DCMAKE_PROJECT_INCLUDE=$file" : '-UCMAKE_PROJECT_INCLUDE')
        $null = Invoke-Step ($file ? 'configure-override' : 'configure-retry') 'cmake' $cmakeArgs 1800
        Get-DepsPrefixStatus
    }
    $clear = {
        Write-Host '清掉编译器探测结果后重配'
        Clear-CompilerProbe
    }
    $writeOverride = {
        param($clPrefix)
        $override = Get-PrefixOverride $clPrefix
        if ($null -eq $override) { return $null }
        Write-Host '改用 cl 实测的前缀覆盖'
        $file = Join-Path $BuildDir 'rinne-showincludes-prefix.cmake'
        [IO.File]::WriteAllText($file, $override, [Text.UTF8Encoding]::new($false))
        $file
    }
    $resolved = Resolve-DepsPrefix $probeStatus $configure $clear $writeOverride
    # 核对通过的前缀记在构建目录里；没有记录（含 0 字节的残留文件）或记录的前缀不同，说明这个目录里的目标文件可能是前缀对不上时
    # 编的（#deps 0，改头文件不会重编，哪怕别的目标有依赖，断言也看不出来），clean 一次
    $verified = Get-DepsPrefixHex (Get-NinjaDepsPrefix (Join-Path $BuildDir 'CMakeFiles/rules.ninja'))
    $clean = { $null = Invoke-Step 'clean' 'cmake' @('--build', $BuildDir, '--config', $Config, '--target', 'clean') 1800 }
    Sync-DepsMarker $resolved (Join-Path $BuildDir "rinne-deps-prefix-$Config.txt") $verified $clean
    $cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw
    $got = Get-CacheValue $cache 'RINNE_SHM_PREFIX'
    if ($got -cne $prefix) { throw [BuildStop]::new("配置后的 RINNE_SHM_PREFIX 是 $got，本次应当是 $prefix", 1) }
    Write-Host "段名前缀：$prefix"
    $b = @('--build', $BuildDir, '--config', $Config)
    if ($script:Jobs -gt 0) { $b += '--parallel', "$($script:Jobs)" }
    $log = Invoke-Step 'build' 'cmake' $b 5400
    $depsLines = Get-LogLines (Invoke-Step 'ninja-deps' $ninjaExe @('-C', $BuildDir, '-t', 'deps') 600)
    # 本次新编的第一方编译单元各自都要记下依赖：只看「有任意一个目标记了依赖」抓不到「第一方 TU 这次没记」。
    Assert-DepsAfterBuild $depsLines (Get-LogLines $log) $Root
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

# 钉版文件里的版本号（去掉开头的 v）。文件是空的时 Get-Content -Raw 返回 $null，直接 .Trim() 会抛 RuntimeException，
# 这里先转成字符串，空版本号抛 BuildStop。
function Read-PinnedVersion([string]$Path) {
    $v = "$(Get-Content -LiteralPath $Path -Raw)".Trim() -replace '^v', ''
    if (-not $v) { throw [BuildStop]::new("$(Split-Path -Leaf $Path) 是空的：钉版文件必须写版本号", 1) }
    $v
}

function Invoke-Pluginval([string[]]$Bundles, [bool]$Gui) {
    $pv = Get-Setting $Pluginval 'RINNE_PLUGINVAL' 'rinne.pluginval' 'pluginval'
    $want = Read-PinnedVersion (Join-Path $Root '.pluginval-version')
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
    & $check '依赖前缀：rules.ninja 没有前缀（null）：判红，理由是没有 msvc_deps_prefix，哪怕 cl 的输出是空的' $true ((Test-ShowIncludesPrefix $null $okLine) -match '没有 msvc_deps_prefix' -and (Test-ShowIncludesPrefix $null '') -match '没有 msvc_deps_prefix')
    & $check '依赖前缀：rules.ninja 的前缀全是空白：判红，哪怕 cl 的输出开头恰好也是这些空白' $true ((Test-ShowIncludesPrefix '  ' $okLine) -match '没有 msvc_deps_prefix' -and (Test-ShowIncludesPrefix '  ' '  ') -match '没有 msvc_deps_prefix')
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
    # 前缀之后只容忍空格：ninja 只跳过 0x20，Tab、换行、0x85、0xA0 都会让路径记错
    & $check '依赖前缀：cl 多出的空白只认空格' $true (Test-ShowIncludesPrefix $rules ($okLine + "`t"))
    & $check '依赖前缀：cl 多出的 Latin-1 0xA0 不算空白' $true (Test-ShowIncludesPrefix $rules ($okLine + [char]0xA0))
    & $check '依赖前缀：cl 多出的 0x85 不算空白' $true (Test-ShowIncludesPrefix $rules ($okLine + [char]0x85))
    & $check '依赖前缀：cl 多出的换行不算空白' $true (Test-ShowIncludesPrefix $rules ($okLine + "`r`n"))
    & $check '依赖前缀：cl 多出的末尾单个 LF 不算空白（正则的 $ 会放过它，要用 \z）' $true (Test-ShowIncludesPrefix $rules ($okLine + "`n"))
    & $check '依赖前缀：cl 多出的 VT 不算空白' $true (Test-ShowIncludesPrefix $rules ($okLine + [char]0x0B))
    & $check '依赖前缀：cl 多出的只是空格时对得上' $false (-not (Test-ShowIncludesPrefix $rules ($okLine + '  ')))
    # 探针几种跑法的前缀必须一致
    & $check '探针一致性：几种跑法的前缀相同' $false (-not (Test-ProbeRunsAgree @('带 /utf-8', '不带 /utf-8') @($rules, $rules)))
    $disagree = Test-ProbeRunsAgree @('带 /utf-8', '不带 /utf-8', '经编译缓存') @($rules, $rules, $moji)
    & $check '探针一致性：其中一种跑法的前缀不同时判红，文字里列出每种跑法的前缀' $true ($disagree -and $disagree.Contains('经编译缓存=') -and $disagree.Contains((ConvertTo-Display $moji)))
    # 记录文件是 0 字节时按「没有记录」处理：Get-Content -Raw 对空文件返回 $null，直接 .Trim() 会抛 RuntimeException
    $emptyMarker = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-marker-$PID-$(Get-Random).txt"
    Set-Content -LiteralPath $emptyMarker -Value '' -NoNewline
    $emptyRaw = Get-Content -LiteralPath $emptyMarker -Raw
    & $check '依赖记录：0 字节的记录文件读出来是 $null，不是空串' $false ($null -eq $emptyRaw)
    & $check '依赖记录：0 字节的记录文件按「没有记录」处理（要 clean）' $true (Test-DepsNeedClean (Read-DepsMarker $emptyMarker) 'AA-BB')
    & $check '依赖记录：0 字节的记录文件不会在 .Trim() 上抛异常' $false ($null -eq (Read-DepsMarker $emptyMarker))
    $okMarker = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-marker-ok-$PID-$(Get-Random).txt"
    Set-Content -LiteralPath $okMarker -Value 'AA-BB'
    & $check '依赖记录：正常的记录文件读出前缀十六进制' $false ((Read-DepsMarker $okMarker) -ceq 'AA-BB')
    & $check '依赖记录：没有记录文件时按「没有记录」处理' $true (Test-DepsNeedClean (Read-DepsMarker ($okMarker + '.missing')) 'AA-BB')
    Set-Content -LiteralPath $emptyMarker -Value "  `r`n" -NoNewline
    & $check '依赖记录：全是空白的记录文件按「没有记录」处理' $false ($null -eq (Read-DepsMarker $emptyMarker))
    Remove-Item -LiteralPath $emptyMarker, $okMarker -ErrorAction SilentlyContinue
    # 钉版文件：空文件不能在 .Trim() 上抛 RuntimeException
    $pinFile = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-pin-$PID-$(Get-Random).txt"
    Set-Content -LiteralPath $pinFile -Value "v3.4.5`n" -NoNewline
    & $check '钉版文件：去掉开头的 v 与末尾换行' $false ((Read-PinnedVersion $pinFile) -ceq '3.4.5')
    Set-Content -LiteralPath $pinFile -Value '' -NoNewline
    $pinMsg = ''
    try { $null = Read-PinnedVersion $pinFile } catch [BuildStop] { $pinMsg = $_.Exception.Message }
    & $check '钉版文件：空文件抛 BuildStop（不是 RuntimeException）' $true ($pinMsg -match '是空的')
    Remove-Item -LiteralPath $pinFile -ErrorAction SilentlyContinue
    # 记录文件与 clean 的接线：每删掉一处，下面对应的用例都要变红
    $syncDir = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-sync-$PID-$(Get-Random)"
    $null = New-Item -ItemType Directory -Path $syncDir
    $syncMarker = Join-Path $syncDir 'marker.txt'
    # $Recorded：$null 没有记录文件，字符串是文件内容；返回 clean 了几次、之后记录文件里的内容、有没有抛异常
    $sync = {
        param([bool]$ResolvedClean, $Recorded, [scriptblock]$Clean = { $script:Cleaned++ })
        Remove-Item -LiteralPath $syncMarker -ErrorAction SilentlyContinue
        if ($null -ne $Recorded) { Set-Content -LiteralPath $syncMarker -Value $Recorded -NoNewline }
        $script:Cleaned = 0
        $err = ''
        try { Sync-DepsMarker @{ Clean = $ResolvedClean } $syncMarker 'AA-BB' $Clean } catch { $err = "$($_.Exception.Message)" }
        $after = (Test-Path -LiteralPath $syncMarker) ? "$(Get-Content -LiteralPath $syncMarker -Raw)".Trim() : $null
        @{ Cleaned = $script:Cleaned; After = $after; Error = $err }
    }
    $s = & $sync $false 'AA-BB'
    & $check '记录接线：记录与这次相同、前缀没修过：不 clean' $false ($s.Cleaned -eq 0 -and $s.After -ceq 'AA-BB' -and -not $s.Error)
    $s = & $sync $false $null
    & $check '记录接线：没有记录文件：clean 一次并写下记录' $true ($s.Cleaned -eq 1 -and $s.After -ceq 'AA-BB')
    $s = & $sync $false ''
    & $check '记录接线：记录文件是 0 字节：clean 一次、写下记录、不抛异常' $true ($s.Cleaned -eq 1 -and $s.After -ceq 'AA-BB' -and -not $s.Error)
    $s = & $sync $false 'AA-CC'
    & $check '记录接线：记录的前缀与这次不同：clean 一次并改写记录' $true ($s.Cleaned -eq 1 -and $s.After -ceq 'AA-BB')
    $s = & $sync $true 'AA-BB'
    & $check '记录接线：记录相同但前缀这次修过：照样 clean' $true ($s.Cleaned -eq 1 -and $s.After -ceq 'AA-BB')
    $s = & $sync $false 'AA-CC' { throw [BuildStop]::new('clean 失败', 1) }
    & $check '记录接线：clean 失败时记录不更新（下次还会 clean）' $true ($s.Error -eq 'clean 失败' -and $s.After -ceq 'AA-CC')
    # 覆盖文件：每次配置显式表态，文件在就 -D，不在就 -U；cache 里还记着一个不在了的文件时说明一声
    $ovFile = Join-Path $syncDir 'rinne-showincludes-prefix.cmake'
    $stale = "// x`r`nCMAKE_PROJECT_INCLUDE:UNINITIALIZED=$($ovFile -replace '\\', '/')`r`n"
    Set-Content -LiteralPath $ovFile -Value '# x'
    $a = Get-ProjectIncludeArg $ovFile $stale
    & $check '覆盖文件在：每次配置都传 -DCMAKE_PROJECT_INCLUDE，没有说明' $false ($a.Arg -ceq "-DCMAKE_PROJECT_INCLUDE=$ovFile" -and -not $a.Note)
    Remove-Item -LiteralPath $ovFile
    $a = Get-ProjectIncludeArg $ovFile ''
    & $check '覆盖文件不在、cache 里也没记：传 -UCMAKE_PROJECT_INCLUDE，没有说明' $false ($a.Arg -ceq '-UCMAKE_PROJECT_INCLUDE' -and -not $a.Note)
    $a = Get-ProjectIncludeArg $ovFile $stale
    & $check '覆盖文件不在、cache 里还记着它：传 -UCMAKE_PROJECT_INCLUDE，并说明记录的路径与做法' $true (
        $a.Arg -ceq '-UCMAKE_PROJECT_INCLUDE' -and $a.Note.Contains('rinne-showincludes-prefix.cmake') -and $a.Note.Contains('-UCMAKE_PROJECT_INCLUDE'))
    Remove-Item -LiteralPath $syncMarker -ErrorAction SilentlyContinue   # 不递归删：夹具里只有这一个文件，之后目录是空的
    Remove-Item -LiteralPath $syncDir -ErrorAction SilentlyContinue
    # 前缀对不上时的修复流程（接线）：每删掉一处判红，下面对应的用例都要变红
    $prefixFlow = {
        param($Script)
        $script:Flow = @()
        $probeStep = {
            $script:Flow += 'probe'
            & $Script.Probe
        }
        $configure = {
            param($file)
            $script:Flow += $(if ($file) { 'configure-override' } else { 'configure-retry' })
            & $Script.Configure $file
        }
        $clear = { $script:Flow += 'clear' }
        $writeOverride = {
            param($cl)
            $script:Flow += 'write-override'
            & $Script.WriteOverride $cl
        }
        $r = Resolve-DepsPrefix $probeStep $configure $clear $writeOverride
        @{ Result = $r; Flow = $script:Flow }
    }
    $okProbe = { @{ Problem = ''; ProbeError = ''; ClPrefix = 'PFX' } }
    $badProbe = { @{ Problem = '对不上'; ProbeError = ''; ClPrefix = 'PFX' } }
    $probeFail = { @{ Problem = ''; ProbeError = 'cl 探针编译退出码 1'; ClPrefix = $null } }
    $r = & $prefixFlow @{ Probe = $okProbe; Configure = $okProbe; WriteOverride = { 'x' } }
    & $check '前缀接线：对上了就什么都不做' $false ((-not $r.Result.Clean) -and $r.Flow.Count -eq 1)
    $r = & $prefixFlow @{ Probe = $badProbe; Configure = $okProbe; WriteOverride = { 'x' } }
    & $check '前缀接线：对不上就清探测重配一次，并 clean' $false ($r.Result.Clean -and ($r.Flow -join ',') -eq 'probe,clear,configure-retry')
    # 重配后才好：重配（不带覆盖）仍对不上，写了覆盖再配才对上
    $script:RetryProbe = 0
    $afterRetry = { param($f) $script:RetryProbe++; if ($script:RetryProbe -ge 2) { & $okProbe } else { & $badProbe } }
    $r = & $prefixFlow @{ Probe = $badProbe; Configure = $afterRetry; WriteOverride = { 'x' } }
    & $check '前缀接线：重配后仍对不上就写覆盖文件再配一次，并 clean，返回写进覆盖的前缀' $false (
        $r.Result.Clean -and $r.Result.Override -ceq 'PFX' -and ($r.Flow -join ',') -eq 'probe,clear,configure-retry,write-override,configure-override')
    $r = & $prefixFlow @{ Probe = $badProbe; Configure = $okProbe; WriteOverride = { 'x' } }
    & $check '前缀接线：重配一步就好时不写覆盖' $false ($null -eq $r.Result.Override -and $r.Flow -cnotcontains 'write-override')
    $threw = $false
    $msg = ''
    try { $null = & $prefixFlow @{ Probe = $badProbe; Configure = $probeFail; WriteOverride = { 'x' } } } catch [BuildStop] { $threw = $true; $msg = $_.Exception.Message }
    & $check '前缀接线：重配之后探针自己失败：判红并说是探针出错，不写覆盖' $true ($threw -and $msg -match '重配后 cl 探针出错' -and $script:Flow -cnotcontains 'write-override')
    $script:RetryProbe = 0
    $failOnOverride = { param($f) $script:RetryProbe++; if ($script:RetryProbe -ge 2) { & $probeFail } else { & $badProbe } }
    $threw = $false
    $msg = ''
    try { $null = & $prefixFlow @{ Probe = $badProbe; Configure = $failOnOverride; WriteOverride = { 'x' } } } catch [BuildStop] { $threw = $true; $msg = $_.Exception.Message }
    & $check '前缀接线：写了覆盖之后探针自己失败：判红并说是探针出错' $true ($threw -and $msg -match '覆盖后 cl 探针出错')
    $threw = $false
    $msg = ''
    try { $null = & $prefixFlow @{ Probe = $probeFail; Configure = $okProbe; WriteOverride = { 'x' } } } catch [BuildStop] { $threw = $true; $msg = $_.Exception.Message }
    & $check '前缀接线：探针自己失败时判红，不清探测也不重配' $true ($threw -and ($script:Flow -join ',') -eq 'probe' -and $msg -match 'cl 探针出错')
    $threw = $false
    $msg = ''
    try { $null = & $prefixFlow @{ Probe = $badProbe; Configure = { param($f) & $badProbe }; WriteOverride = { $null } } } catch [BuildStop] { $threw = $true; $msg = $_.Exception.Message }
    & $check '前缀接线：前缀写不成 cmake 字符串时判红' $true ($threw -and $msg -match '不能写成 cmake 字符串')
    $threw = $false
    $msg = ''
    try { $null = & $prefixFlow @{ Probe = $badProbe; Configure = { param($f) & $badProbe }; WriteOverride = { 'x' } } } catch [BuildStop] { $threw = $true; $msg = $_.Exception.Message }
    & $check '前缀接线：覆盖之后仍对不上时判红' $true ($threw -and $msg -match '覆盖之后头文件依赖前缀仍对不上')
    # 构建后：本次新编的第一方编译单元各自都要记下依赖（夹具只有两个文件，不递归删）
    $fpRoot = Join-Path ([IO.Path]::GetTempPath()) "rinne-build-fp-$PID-$(Get-Random)"
    $fpCore = Join-Path $fpRoot 'src/core'
    $null = New-Item -ItemType Directory -Force -Path $fpCore
    Set-Content -LiteralPath (Join-Path $fpCore 'A.cpp') -Value '#include "A.h"'
    Set-Content -LiteralPath (Join-Path $fpCore 'B.cpp') -Value '#include <vector>'
    $fpObj = 'src/core/CMakeFiles/rinne_core.dir/Release/A.cpp.obj'
    $fpLog = @("[1/2] Building CXX object src\core\CMakeFiles\rinne_core.dir\Release\A.cpp.obj",
        "[2/2] Building CXX object src\core\CMakeFiles\rinne_core.dir\Release\B.cpp.obj")
    $fpDeps = @("$fpObj`: #deps 3, deps mtime 1 (VALID)")
    $fpZero = @("$fpObj`: #deps 0, deps mtime 1 (VALID)")
    $fpOther = "_deps/juce/CMakeFiles/juce.dir/Release/a.cpp.obj`: #deps 9, deps mtime 1 (VALID)"
    & $check '第一方依赖：本次新编的带引号头文件的 TU 记下了依赖（只含系统头的 TU 没记也不要紧）' $false (-not (Test-FirstPartyDeps (Get-BuiltObjects $fpLog) (Get-DepsCounts $fpDeps) $fpRoot))
    & $check '第一方依赖：本次新编的 TU 没记下依赖时判红' $true (Test-FirstPartyDeps (Get-BuiltObjects $fpLog) (Get-DepsCounts $fpZero) $fpRoot)
    & $check '第一方依赖：别处有依赖记录也不能替这次新编的第一方 TU 作数' $true (Test-FirstPartyDeps (Get-BuiltObjects $fpLog) (Get-DepsCounts ($fpZero + $fpOther)) $fpRoot)
    & $check '第一方依赖：本次没新编第一方 TU 时查 ninja 的记录，都没有依赖就判红' $true (Test-FirstPartyDeps @() @{} $fpRoot)
    & $check '第一方依赖：本次没新编第一方 TU 时 ninja 的记录里有第一方对象记下依赖就不判红' $false (-not (Test-FirstPartyDeps @() (Get-DepsCounts $fpDeps) $fpRoot))
    & $check '第一方依赖：本次没新编第一方 TU 时只有第三方对象记了依赖，仍判红' $true (Test-FirstPartyDeps @() (Get-DepsCounts @($fpOther)) $fpRoot)
    & $check '第一方依赖：本次没新编第一方 TU 时第一方对象的依赖个数是 0，仍判红' $true (Test-FirstPartyDeps @() (Get-DepsCounts $fpZero) $fpRoot)
    & $check '第一方依赖：记录里的对象路径不是 CMake 对象的形状，仍判红' $true (Test-FirstPartyDeps @() (Get-DepsCounts @('weird.obj: #deps 5, deps mtime 1 (VALID)')) $fpRoot)
    & $check '第一方依赖：本次新编的对象路径不是 CMake 对象的形状时跳过，不抛错也不判红' $false (-not (Test-FirstPartyDeps @('weird.obj', $fpObj) (Get-DepsCounts $fpDeps) $fpRoot))
    $cLog = @('[1/2] Building C object tools\CMakeFiles\x.dir\Release\a.c.obj', '[2/2] Linking CXX executable tools\Release\x.exe')
    $builtCxx, $builtC = @(Get-BuiltObjects $fpLog), @(Get-BuiltObjects $cLog)   # 单个结果会被函数返回值展开成字符串，用 @() 兜住
    & $check '构建日志：取出本次新编的对象（CXX 与 C，不含链接行）' $false ($builtCxx.Count -eq 2 -and $builtCxx[0] -eq $fpObj -and
        $builtC.Count -eq 1 -and $builtC[0] -eq 'tools/CMakeFiles/x.dir/Release/a.c.obj')
    # 构建后断言的接线：每删掉一处判红，对应的用例都要变红（消息文字也要对：判红的理由不能张冠李戴）
    $assert = { param($Deps, $Log) try { Assert-DepsAfterBuild $Deps $Log $fpRoot; '' } catch [BuildStop] { $_.Exception.Message } }
    & $check '构建后断言：第一方 TU 记下了依赖，不抛' $false ((& $assert $fpDeps $fpLog) -eq '')
    & $check '构建后断言：ninja 里一条依赖都没有：判红，理由是没有任何目标记下依赖' $true ((& $assert @() $fpLog) -match '没有任何一个目标记下头文件依赖')
    & $check '构建后断言：别处有依赖、本次新编的第一方 TU 没记：判红，理由是新编的第一方 TU' $true ((& $assert ($fpZero + $fpOther) $fpLog) -match '本次新编的第一方编译单元没有记下')
    # Invoke-Build 里这些函数的调用（删掉任何一处，或者换了先后顺序，这里变红）：先给配置表态，再核对前缀，再记录与 clean，最后断言依赖
    $called = @((Get-Command Invoke-Build).ScriptBlock.Ast.FindAll({ param($n) $n -is [Management.Automation.Language.CommandAst] }, $true) |
            ForEach-Object { $_.GetCommandName() } | Where-Object { $_ })
    $wired = 'Get-ProjectIncludeArg', 'Resolve-DepsPrefix', 'Sync-DepsMarker', 'Assert-DepsAfterBuild'
    $pos = @($wired | ForEach-Object { [array]::IndexOf($called, $_) })
    & $check 'Invoke-Build 的接线：表态覆盖文件、核对前缀、记录与 clean、断言依赖，四步都在且先后不乱' $false (
        @($pos | Where-Object { $_ -lt 0 }).Count -eq 0 -and ($pos -join ',') -eq (($pos | Sort-Object) -join ','))
    # 函数里给与脚本参数同名的变量赋值：PowerShell 的变量名不分大小写，`$probe = {…}` 会把 -Probe 开关盖成脚本块，
    # 之后 Get-ExpectedBundles $Probe 才在真跑里抛错，上面那些纯函数的用例碰不到它。
    $astTokens, $astErrors = $null, $null
    $selfAst = [Management.Automation.Language.Parser]::ParseFile($PSCommandPath, [ref]$astTokens, [ref]$astErrors)
    $paramNames = @($selfAst.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
    $assigned = {
        param($a)
        if ($a -isnot [Management.Automation.Language.AssignmentStatementAst]) { return $false }
        $targets = $a.Left -is [Management.Automation.Language.ArrayLiteralAst] ? $a.Left.Elements : @($a.Left)
        @($targets | Where-Object { $_ -is [Management.Automation.Language.VariableExpressionAst] -and $paramNames -contains $_.VariablePath.UserPath }).Count -gt 0
    }
    $shadow = @($selfAst.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true) |
            ForEach-Object { $_.Body.FindAll($assigned, $true) })
    & $check "函数里不给与脚本参数同名的变量赋值（变量名不分大小写）$(if ($shadow.Count) { '：第 ' + (($shadow | ForEach-Object { $_.Extent.StartLineNumber }) -join '、') + ' 行' })" $false ($shadow.Count -eq 0)
    Remove-Item -LiteralPath (Join-Path $fpCore 'A.cpp'), (Join-Path $fpCore 'B.cpp') -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fpCore, (Join-Path $fpRoot 'src'), $fpRoot -ErrorAction SilentlyContinue
    $formal, $probeBundles = (Get-ExpectedBundles $false), (Get-ExpectedBundles $true)
    & $check '产物断言：正式版恰好两个' $false (-not (Test-BundleSet $formal $formal))
    & $check '产物断言：缺一个' $true (Test-BundleSet @($formal[0]) $formal)
    & $check '产物断言：正式目录里混进探针产物' $true (Test-BundleSet ($formal + $probeBundles[0]) $formal)
    & $check '产物断言：探针恰好两个' $false (-not (Test-BundleSet $probeBundles $probeBundles))
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
        $ciRun = { param($v) $env:GITHUB_ACTIONS = $v; $o = & $pwsh -NoProfile -NonInteractive -File $PSCommandPath -Ci -NoBuild -BuildDir $bd 2>&1 | Out-String; @($LASTEXITCODE, $o) }
        $off, $on = (& $ciRun $null), (& $ciRun 'true')
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
