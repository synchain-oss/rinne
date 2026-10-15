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
# -Stage 把两个插件与 tools-bin 下的可执行文件拷进一个新目录（CI 上传的就是它）。-CheckCache 断言 cache 里的段名前缀是默认值。
# 构建目录缺省 build，探针缺省 build-probe：两种身份的产物不同名，混在一个目录里会被产物断言拦下。
# JUCE、ninja、pluginval：参数，其次环境变量 RINNE_JUCE_PATH / RINNE_NINJA / RINNE_PLUGINVAL，再次 git config rinne.jucePath /
# rinne.ninja / rinne.pluginval，ninja 与 pluginval 最后找 PATH。
# 段名前缀：本机构建一律用带 D 的 gates 前缀，与维护者 DAW 里装的正式版互不相见；-Ci 用默认值（正式版，探针时是探针前缀），
# 本机的 -Probe 用探针前缀。真源与校验在根 CMakeLists.txt（这里只从那里读正式前缀），配置后再核一次 cache 里的值。
# 构建槽：配置与构建持一把（同时最多两份编译），先等可用内存，再按内存折算并发。调用方已经持有一把时（gates 的构建、合并后
# 构建）设 RINNE_BUILD_SLOT_HELD=1，这里就不再取：两个持槽的调用方各自的子进程再去取，会互相等到超时。设了它、两把槽却都
# 空着，说明它是残留的，照常取槽并提示。-Ci 不取槽。
# 输出：cmake、ninja、ctest、pluginval 的输出写进 <构建目录>/logs/ 下的文件，不经管道；本进程的标准句柄开跑前设为不可继承。
# MSVC 的 mspdbsrv、编译缓存的服务进程会比构建活得久，它们继承了调用方的管道，调用方读 EOF 就要等它们退出，超时也就失效了。
# 屏幕上只打印步骤、告警与失败步骤日志的末尾。
# 环境：开跑前从本进程删掉凭据与个人针变量，CMake 配置期与编译器都看不到；VSLANG=1033 让 MSVC 输出英文，零 warning 才判得准；
# pluginval 会把同名环境变量（SKIP_GUI_TESTS 之类）当参数用，验之前一并删掉。
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
# 与 maintainer/lib/RinneGh.psm1 拿掉的是同一组，另加个人访问令牌的 *_PAT
$SecretEnv = '^(?:GH_TOKEN|GITHUB_TOKEN|GH_ENTERPRISE_TOKEN|PRIVACY_NEEDLES|RINNE_PRIVACY_LOCAL|.+_TOKEN|.+_KEY|.+_SECRET|.+_PASSWORD|.+_PAT)$'
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

function Remove-EnvMatching([string]$Pattern) {
    foreach ($k in @([Environment]::GetEnvironmentVariables().Keys)) { if ($k -match $Pattern) { [Environment]::SetEnvironmentVariable($k, $null) } }
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

# CI 与发布构建的段名前缀必须是默认值：返回错误文字，对上了返回空。
function Test-CachePrefix([string]$CacheText) {
    $have, $want = (Get-CacheValue $CacheText 'RINNE_SHM_PREFIX'), (Get-CacheValue $CacheText 'RINNE_SHM_PREFIX_DEFAULT')
    if (-not $have -or -not $want) { return 'CMakeCache.txt 里没有 RINNE_SHM_PREFIX 或 RINNE_SHM_PREFIX_DEFAULT' }
    ($have -ceq $want) ? '' : "RINNE_SHM_PREFIX 被覆盖成 $have，默认值是 $want"
}

# 正式前缀只在根 CMakeLists.txt 里写一次；本机前缀与探针前缀是它加 D、加 P。
function Get-ShmPrefix([string]$Flavor) {
    $m = [regex]::Match((Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw), '(?m)^set\(RINNE_SHM_PREFIX_FORMAL "([A-Za-z]+)"\)')
    if (-not $m.Success) { throw [BuildStop]::new('根 CMakeLists.txt 里找不到 set(RINNE_SHM_PREFIX_FORMAL "…")', 1) }
    $m.Groups[1].Value + @{ ci = ''; dev = 'D'; probe = 'P' }[$Flavor]
}

function Enter-BuildSlot {
    if ($Ci) { return $null }
    Import-Module (Join-Path $PSScriptRoot 'lib/Locks.psm1') -ErrorAction Stop
    if ($env:RINNE_BUILD_SLOT_HELD) {
        $free = @('build-slot-1', 'build-slot-2' | ForEach-Object { Enter-RinneLock $_ -TimeoutSeconds 0 -Quiet })
        $free | ForEach-Object { Exit-RinneLock $_ }
        if (@($free | Where-Object { $_ }).Count -lt 2) {
            Write-Host '构建槽由调用方持有（RINNE_BUILD_SLOT_HELD），这里不再取'
            if (-not $script:Jobs) { $script:Jobs = Get-RinneJobs (Get-RinneAvailableMB) }
            return $null
        }
        Write-Host 'RINNE_BUILD_SLOT_HELD 设了，两把构建槽却都空着：当成残留的设置，照常取槽'
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
    $prefix = Get-ShmPrefix $Flavor
    $cfg = @('-S', $Root, '-B', $BuildDir, '-G', 'Ninja Multi-Config', "-DCMAKE_CONFIGURATION_TYPES=$Config", "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', "-DJUCE_PATH=$juce", "-DRINNE_PROBE_BUILD=$($Probe ? 'ON' : 'OFF')", "-DRINNE_BUILD_LABEL=$BuildLabel")
    # CI 不传前缀、先删掉 cache 里的旧值，让根 CMakeLists.txt 的默认值生效；本机显式给 D 或 P
    $cfg += ($Flavor -eq 'ci') ? '-URINNE_SHM_PREFIX' : "-DRINNE_SHM_PREFIX=$prefix"
    if ($Launcher) { $cfg += "-DCMAKE_C_COMPILER_LAUNCHER=$Launcher", "-DCMAKE_CXX_COMPILER_LAUNCHER=$Launcher" }
    $null = Invoke-Step 'configure' 'cmake' $cfg 1800
    $cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw
    $got = Get-CacheValue $cache 'RINNE_SHM_PREFIX'
    if ($got -cne $prefix) { throw [BuildStop]::new("配置后的 RINNE_SHM_PREFIX 是 $got，本次应当是 $prefix", 1) }
    Write-Host "段名前缀：$prefix"
    $b = @('--build', $BuildDir, '--config', $Config)
    if ($script:Jobs -gt 0) { $b += '--parallel', "$($script:Jobs)" }
    $log = Invoke-Step 'build' 'cmake' $b 5400
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
    $env:RINNE_SELFTEST_PAT, $env:RINNE_SELFTEST_KEEP = 'x', 'y'
    Remove-EnvMatching $SecretEnv
    & $check '凭据变量从环境里删掉，别的留着' $true (-not $env:RINNE_SELFTEST_PAT -and $env:RINNE_SELFTEST_KEEP -eq 'y')
    $env:RINNE_SELFTEST_KEEP = $null
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
        # 持槽标记（换一组自测专用的锁名）：两把槽都空时标记当残留、照常取槽；子进程占住一把时标记生效、不再取
        Import-Module (Join-Path $PSScriptRoot 'lib/Locks.psm1') -ErrorAction Stop
        $scope = "Rinne-selftest-$PID"
        & (Get-Module Locks) { param($s) $script:Scope = $s } $scope
        $env:RINNE_BUILD_SLOT_HELD, $script:Jobs, $script:MemWaitMinutes = '1', 2, 0
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
Remove-EnvMatching $SecretEnv
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
    if ($CheckCache) {
        $f = Join-Path $BuildDir 'CMakeCache.txt'
        $bad = (Test-Path -LiteralPath $f) ? (Test-CachePrefix (Get-Content -LiteralPath $f -Raw)) : "没有 $f"
        if ($bad) { throw [BuildStop]::new($bad, 1) }
        Write-Host "段名前缀是默认值：$(Get-CacheValue (Get-Content -LiteralPath $f -Raw) 'RINNE_SHM_PREFIX')"
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
