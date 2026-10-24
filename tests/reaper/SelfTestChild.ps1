#Requires -Version 7
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 自测的子进程部分（SelfTest.psm1 调用）：Initialize-ReaperCfg 把配置目录钉进进程后不可逆，所以这些用例单开一个进程。
# 这里用「替身」代替 REAPER：睡觉的 pwsh、弹出指定标题对话框的 pwsh，用来验证看门狗、模态框处理、锁和入口护栏。
# 输出：每个用例一行 RESULT<TAB>是否反例<TAB>是否按预期<TAB>标签。APPDATA 由父进程换成了假的，继承过来。
param(
    [Parameter(Mandatory)][string]$RepoRoot,
    [Parameter(Mandatory)][string]$CfgA,
    [Parameter(Mandatory)][string]$CfgB,
    [Parameter(Mandatory)][string]$Work
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) # 父进程按 UTF-8 解码

$scripts = Join-Path $RepoRoot 'scripts/reaper'
Import-Module (Join-Path $scripts 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $scripts 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $scripts 'Scenarios.psm1') -ErrorAction Stop
Import-Module (Join-Path $scripts 'Suite.psm1') -ErrorAction Stop
Import-Module (Join-Path $RepoRoot 'scripts/lib/Locks.psm1') -ErrorAction Stop

function Write-Case([string]$Label, [bool]$Bad, [bool]$Passed) { Write-Output ("RESULT`t{0}`t{1}`t{2}" -f [int]$Bad, [int]$Passed, $Label) }
function Test-Throws([scriptblock]$Action, [string]$Like = '*') { try { $null = & $Action; $false } catch { $_.Exception.Message -like $Like } }

$pwshExe = (Get-Process -Id $PID).Path
$flag = '-cfg' + 'file'
$gone = { param($ProcessId) $null -eq (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) }

# 替身：把 Host 准备好的启动信息换成别的命令，记下原来的命令行
function New-StandIn([string]$Command, [hashtable]$Seen) {
    { param($psi) $Seen.Args = $psi.Arguments; $Seen.Called = $true; $psi.FileName = $pwshExe; $psi.Arguments = "-NoProfile -Command `"$Command`""; [Diagnostics.Process]::Start($psi) }.GetNewClosure()
}
function New-BoxCommand([string]$Title, [string]$Buttons, [string]$OutFile) {
    "Add-Type -AssemblyName System.Windows.Forms; `$r = [System.Windows.Forms.MessageBox]::Show('x', '$Title', '$Buttons'); Set-Content -LiteralPath '$OutFile' -Value `$r"
}

# ---- 钉死 ----
$pinned = Initialize-ReaperCfg $CfgA
Write-Case 'Initialize 钉死配置目录，配置文件实参只来自常量' $false ((Get-ReaperCfgArgument) -ceq ('"{0}"' -f $pinned.Ini))
Write-Case '钉死后换另一个配置目录被拒绝' $true (Test-Throws { $null = Initialize-ReaperCfg $CfgB } 'REFUSED:*')
Write-Case 'ini 路径常量不能被赋值' $true (Test-Throws { & (Get-Module Guard) { $script:RinneReaperCfgIni = 'x' } })
Write-Case '只差大小写的同名变量赋值同样是硬错误' $true (Test-Throws { & (Get-Module Guard) { Set-Variable -Name ('rinnereaper' + 'CFGini') -Scope Script -Value 'x' } })
$lead = [byte[]](0x5B, 0x52, 0x45, 0x41, 0x50, 0x45, 0x52, 0x5D, 0x0A, 0x6E, 0x61, 0x6D, 0x65, 0x3D, 0xE4, 0xB8, 0xAD, 0xFF, 0x0A)
[IO.File]::WriteAllBytes($pinned.Ini, $lead)
$changed = Set-ReaperIniValue -Section audioconfig -Key mode -Value 4
$now = [IO.File]::ReadAllBytes($pinned.Ini)
Write-Case 'ini 改键后原有字节（含非 ASCII）一个不丢，只在末尾补了一节' $false ($changed -and $now.Length -gt $lead.Length -and (-join $now[0..($lead.Length - 1)]) -ceq (-join $lead) -and [Text.Encoding]::Latin1.GetString($now[$lead.Length..($now.Length - 1)]) -ceq "[audioconfig]`nmode=4`n")
Write-Case 'ini 值没变时不写文件' $false (-not (Set-ReaperIniValue -Section audioconfig -Key mode -Value 4))

# ---- 看门狗（替身进程）----
$seenA = @{ Called = $false }
$bystander = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new($pwshExe, '-NoProfile -Command Start-Sleep -Seconds 120'))
try {
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 2 -Launcher (New-StandIn 'Start-Sleep -Seconds 60' $seenA)
    Write-Case '看门狗：超时被结束，状态 Timeout' $true ($run.Status -eq 'Timeout' -and (& $gone $run.Pid))
    Write-Case '看门狗：只按自己启动的 PID 结束，旁人的进程还活着' $false (-not $bystander.HasExited)
    Write-Case '启动命令行是 -nosplash -newinst 加私有 ini 的配置文件参数' $false ($seenA.Args -ceq ('-nosplash -newinst {0} "{1}"' -f $flag, $pinned.Ini))
} finally { if (-not $bystander.HasExited) { Stop-Process -Id $bystander.Id -Force } }
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1.5 -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '看门狗：到点被结束，状态 Killed，没有等到超时' $true ($run.Status -eq 'Killed' -and $run.Seconds -lt 8 -and (& $gone $run.Pid))
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-StandIn 'exit 3' @{})
Write-Case '自己退出的进程：状态 Exited，退出码照实记' $false ($run.Status -eq 'Exited' -and $run.ExitCode -eq 3)
$seenB = @{ Called = $false }
$sentinelFile = Join-Path $pinned.Dir (Get-ReaperSentinel).Name
$sentinelText = Get-Content -LiteralPath $sentinelFile -Raw
Remove-Item -LiteralPath $sentinelFile
try { Write-Case '启动前重新校验：哨兵被删后拒绝启动，替身根本没被调用' $true ((Test-Throws { $null = Invoke-ReaperHost -ReaperExe 'stand-in' -Launcher (New-StandIn 'exit 0' $seenB) } 'REFUSED:*') -and -not $seenB.Called) }
finally { [IO.File]::WriteAllText($sentinelFile, $sentinelText) }

# ---- 模态框（真的弹窗，占 gui 锁）----
Invoke-WithRinneLock -Name gui -TimeoutSeconds 600 {
    $outNo = Join-Path $Work 'box-no.txt'
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-StandIn (New-BoxCommand 'REAPER Query' 'YesNo' $outNo) @{})
    Write-Case '模态框：REAPER Query 被点「否」，进程正常退出' $false ($run.Status -eq 'Exited' -and 'save-prompt' -in $run.Dismissed -and (Test-Path -LiteralPath $outNo) -and (Get-Content -LiteralPath $outNo -Raw).Trim() -eq 'No')
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-StandIn (New-BoxCommand 'Project Load Warning' 'OK' (Join-Path $Work 'box-warn.txt')) @{})
    Write-Case '模态框：Project Load Warning 判卡死，进程被结束，状态 Modal' $true ($run.Status -eq 'Modal' -and $run.Modal -eq 'project-load-warning' -and (& $gone $run.Pid))
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-StandIn (New-BoxCommand 'Something New' 'OK' (Join-Path $Work 'box-new.txt')) @{})
    Write-Case '模态框：没见过的对话框也判卡死（unknown-dialog）' $true ($run.Status -eq 'Modal' -and $run.Modal -eq 'unknown-dialog')
    $outBy = Join-Path $Work 'box-bystander.txt'
    $boxCmd = New-BoxCommand 'REAPER Query' 'YesNo' $outBy
    $bystander = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new($pwshExe, "-NoProfile -Command `"$boxCmd`""))
    try {
        Start-Sleep -Seconds 3 # 让旁人的对话框弹出来
        $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 3 -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
        Write-Case '模态框：别的进程的 REAPER Query 不会被点（只处理自己 PID 的窗口）' $false ($run.Status -eq 'Killed' -and -not $bystander.HasExited -and -not (Test-Path -LiteralPath $outBy))
    } finally { if (-not $bystander.HasExited) { Stop-Process -Id $bystander.Id -Force } }
}

# ---- gui 锁（换成自测专用的锁名前缀，与真在跑的 gates 互不等待）----
& (Get-Module Locks) { $script:Scope = "Rinne-selftest-reaper-$PID" }
$lockChild = { param($Command) & (Get-Module Locks) { param($c) Start-LockChild $c } $Command }
$probe = 'if (Enter-RinneLock gui -TimeoutSeconds 0 -Quiet) { exit 0 } else { exit 1 }'
$probeExit = { $p = & $lockChild $probe; $p.WaitForExit(); $p.ExitCode }.GetNewClosure()
$ctxConfig = [pscustomobject]@{ limits = [pscustomobject]@{}; signal = [pscustomobject]@{} }
$ctx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $ctxConfig
$during = @{}
$sleepIn = New-StandIn 'Start-Sleep -Seconds 60' @{}
$ctx.Launcher = { param($psi) $during.Exit = (& $probeExit); & $sleepIn $psi }.GetNewClosure()
$run = Invoke-ReaperLaunch -Ctx $ctx -HostArgs @{ TimeoutSec = 2 }
Write-Case 'gui 锁：REAPER 运行期间别的进程取不到' $true ($during.Exit -eq 1)
Write-Case 'gui 锁：超时被结束后锁已释放，进程已没了' $true ($run.Status -eq 'Timeout' -and (& $probeExit) -eq 0 -and (& $gone $run.Pid))
$ctx.Launcher = { param($psi) throw 'boom' }
Write-Case 'gui 锁：启动过程抛错时锁照样释放' $true ((Test-Throws { $null = Invoke-ReaperLaunch -Ctx $ctx -HostArgs @{ TimeoutSec = 2 } } 'boom') -and (& $probeExit) -eq 0)
$holder = & $lockChild 'if (-not (Enter-RinneLock gui -TimeoutSeconds 5 -Quiet)) { exit 2 }; Start-Sleep -Seconds 60'
try {
    $until = (Get-Date).AddSeconds(10)
    while ((& $probeExit) -ne 1 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 200 }
    $ctx.Launcher = New-StandIn 'exit 0' $during
    $during.Called = $false; $ctx.LockWaitSec = 1; $before = $ctx.Launches
    $run = Invoke-ReaperLaunch -Ctx $ctx -HostArgs @{ TimeoutSec = 5 }
    Write-Case 'gui 锁：别人占着时等不到，状态 LockBusy，不无锁硬跑' $true ($run.Status -eq 'LockBusy' -and -not $during.Called -and $ctx.Launches -eq $before -and (Get-HostFailure $run) -eq 'gui 锁等不到')
    # 反例场景（hang）在锁等不到时：REAPER 没启动，Run 里没有进程号。原因要写准，不能落成「场景抛错」
    $hangConfig = [pscustomobject]@{ limits = [pscustomobject]@{ luaTimeoutSec = 30 }; signal = [pscustomobject]@{ seconds = 4; kind = 'x' } }
    $hangCtx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $hangConfig -LockWaitSec 1
    $hangCtx.Signals['48000-2-0'] = Join-Path $Work 'sig.wav' # 信号已“造好”，用不着 nulltest
    $hangCtx.Launcher = New-StandIn 'exit 0' $during
    $during.Called = $false
    $hang = Invoke-ReaperScenario -Ctx $hangCtx -Scenario ([pscustomobject]@{ id = 'hang-lockbusy'; kind = 'hang'; timeoutSec = 2; killAfterSec = 0 })
    Write-Case '反例场景 hang 在 gui 锁等不到时：INCOMPLETE，原因写锁而不是「场景抛错」，REAPER 没有启动' $true ($hang.Status -eq 'INCOMPLETE' -and $hang.Detail -like '*gui 锁等不到*' -and $hang.Detail -notlike '*场景抛错*' -and -not $during.Called -and $hangCtx.Launches -eq 0)
} finally { if (-not $holder.HasExited) { $holder.Kill() } }

# ---- 入口护栏 ----
$scenarioFile = Join-Path $RepoRoot 'tests/reaper/scenarios.json'
$smoke = @{ CfgDir = $CfgA; ReaperExe = (Join-Path $Work 'no-reaper.exe'); BuildDir = (Join-Path $Work 'no-build'); LogDir = $Work; ScenarioFile = $scenarioFile }
$marker = Join-Path $Work 'USER_TESTING'
Set-Content -LiteralPath $marker -Value x
try {
    $r = Invoke-ReaperSmoke @smoke -UserTestingMarker $marker
    Write-Case '入口：USER_TESTING 标记在时整套 SKIPPED，一次 REAPER 都没启动' $true ($r.verdict -eq 'SKIPPED' -and $r.launches -eq 0 -and $r.skipReasons.Count -eq 1)
} finally { Remove-Item -LiteralPath $marker }
$r = Invoke-ReaperSmoke @smoke -UserTestingMarker $marker
Write-Case '入口：没有 REAPER 和构建产物时 SKIPPED 并写明原因，不当 PASS' $true ($r.verdict -eq 'SKIPPED' -and $r.launches -eq 0 -and $r.skipReasons.Count -ge 2)
$noSentinel = Join-Path $Work 'no-sentinel'
$null = New-Item -ItemType Directory -Path $noSentinel
Set-Content -LiteralPath (Join-Path $noSentinel 'reaper.ini') -Value '[REAPER]'
$other = $smoke.Clone(); $other.CfgDir = $noSentinel
Write-Case '入口：哨兵缺失的配置目录被拒绝' $true (Test-Throws { $null = Invoke-ReaperSmoke @other -UserTestingMarker $marker } 'REFUSED:*哨兵*')
$other = $smoke.Clone(); $other.CfgDir = Get-ReaperDefaultCfgDir
Write-Case '入口：默认资源目录当配置目录被拒绝' $true (Test-Throws { $null = Invoke-ReaperSmoke @other -UserTestingMarker $marker } 'REFUSED:*永不碰*')
Remove-Item -LiteralPath (Join-Path $noSentinel 'reaper.ini'); Remove-Item -LiteralPath $noSentinel
# 用法错误：选不出任何场景时不能落成零个场景的 PASS，也不能默默 SKIPPED
Write-Case '入口：-Only 一个场景都没选中是用法错误' $true (Test-Throws { $null = Invoke-ReaperSmoke @smoke -UserTestingMarker $marker -Only '^no-such-scenario$' } 'USAGE:*没有选中任何场景*')
Write-Case '入口：-Repeat 0 是用法错误' $true (Test-Throws { $null = Invoke-ReaperSmoke @smoke -UserTestingMarker $marker -Repeat 0 } 'USAGE:*-Repeat*')
$usageLogs = Join-Path $Work 'usage-logs'
$u = Invoke-Native -Exe $pwshExe -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $scripts 'run.ps1'), '-CfgDir', $CfgA, '-BuildDir', (Join-Path $Work 'no-build'),
    '-LogDir', $usageLogs, '-UserTestingMarker', $marker, '-Only', '^no-such-scenario$')
Write-Case '入口脚本：-Only 一个场景都没选中退出码 64，不写结果文件' $true ($u.Exit -eq 64 -and $u.Out -match 'USAGE:' -and -not (Test-Path -LiteralPath $usageLogs))

# ---- 插件复制 ----
$bundles = foreach ($name in 'Rinne Input', 'Rinne Output') {
    $dir = Join-Path $Work "$name.vst3"
    $null = New-Item -ItemType Directory -Path (Join-Path $dir 'Contents/x86_64-win') -Force
    Set-Content -LiteralPath (Join-Path $dir "Contents/x86_64-win/$name.vst3") -Value "bits-$name"
    $dir
}
$first = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
$iniFirst = [IO.File]::ReadAllText($pinned.Ini)
$second = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
Set-Content -LiteralPath (Join-Path $bundles[0] 'Contents/x86_64-win/Rinne Input.vst3') -Value 'bits-new'
$third = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
$iniNow = [IO.File]::ReadAllText($pinned.Ini)
# vstpath64 已经含私有目录时不再改写 ini：LF 行尾、CRLF 行尾、后面还跟别的目录（分号）三种写法都要认
Write-Case '插件复制：重复安装不改写 ini（vstpath64 已含私有目录，LF 行尾）' $false ($iniNow -ceq $iniFirst)
$pluginsDir = Join-Path $pinned.Dir 'plugins'
$iniRaw = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($pinned.Ini)) # 前面的用例往 ini 里写过非 UTF-8 的字节，按 Latin-1 读才能原样往返
foreach ($variant in @(@('CRLF 行尾', "`r`n", ''), @('后面跟着别的目录', "`n", ';' + (Join-Path $Work 'elsewhere')), @('CRLF 行尾且后面跟着别的目录', "`r`n", ';' + (Join-Path $Work 'elsewhere')))) {
    $text = (($iniRaw -replace "`r?`n", "`n") -replace '(?m)^vstpath64=.*$', ('vstpath64=' + $pluginsDir + $variant[2])) -replace "`n", $variant[1]
    [IO.File]::WriteAllBytes($pinned.Ini, [Text.Encoding]::Latin1.GetBytes($text))
    $null = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
    Write-Case "插件复制：重复安装不改写 ini（$($variant[0])）" $false ([Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($pinned.Ini)) -ceq $text)
}
Write-Case '插件复制：第一次全复制，内容相同第二次不动，改了的再复制' $false (@($first | Where-Object { $_.copied }).Count -eq 2 -and @($second | Where-Object { $_.copied }).Count -eq 0 -and @($third | Where-Object { $_.copied }).Count -eq 1)
Write-Case '插件复制：私有 VST3 目录排在 vstpath64 最前，目标里只有这次的构建' $false ($iniNow.Contains('vstpath64=' + (Join-Path $pinned.Dir 'plugins')) -and (Get-Content -LiteralPath (Join-Path $pinned.Dir 'plugins/Rinne Input.vst3/Contents/x86_64-win/Rinne Input.vst3') -Raw).Trim() -eq 'bits-new')
