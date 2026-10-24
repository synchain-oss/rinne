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
# 标记文件出现时进程已经退出（替身启动后先等它退出、再写标记文件，确定地造出「来不及取样」）：记作 SampleLost，原因照实写
$quickMark = Join-Path $Work 'mark-after-exit.txt'
$quickLauncher = { param($psi) $psi.FileName = $pwshExe; $psi.Arguments = '-NoProfile -Command exit 0'; $p = [Diagnostics.Process]::Start($psi); $p.WaitForExit(); Set-Content -LiteralPath $quickMark -Value x; $p }.GetNewClosure()
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -WatchFiles @{ loop = $quickMark } -Launcher $quickLauncher
Write-Case '采样：标记文件出现时进程已退出，记作 SampleLost，原因是「进程在采样前退出」而不是「没取到」' $true ($run.Status -eq 'Exited' -and @($run.SampleLost) -contains 'loop' -and -not $run.Marks.Contains('loop') -and
    (Get-ReaperMarkProblem -Samples $run.Marks -LostAt @($run.SampleLost) -Need 'loop') -like '进程在采样前退出*')
# 轮询里取样时进程已经退出（取样替身直接给出 Exited）：记作 SampleLost，不把空的计数当成取到了
$preMark = Join-Path $Work 'mark-present.txt'
Set-Content -LiteralPath $preMark -Value x
$exitedSampler = { param($p) [pscustomobject]@{ Exited = $true; Handles = $null; PrivateMB = $null; WorkingMB = $null } }
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1.5 -WatchFiles @{ warm = $preMark } -Sampler $exitedSampler -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '采样：轮询里取样时进程已退出，记作 SampleLost，不当成取到了计数' $true ($run.Status -eq 'Killed' -and @($run.SampleLost) -contains 'warm' -and -not $run.Marks.Contains('warm') -and (& $gone $run.Pid))
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
# 起 REAPER 持的锁与看门狗反例事后核对的锁都从上下文取（默认 gui），按本进程的锁名前缀解析：自测查的是自测专用名下的那把，不依赖正式的锁空闲
Write-Case '锁名：按本进程当前的锁名前缀解析（自测专用），子进程核对的就是这个确切的名字' $false ((Get-ReaperLockMutexName gui) -ceq ('Local\Rinne-selftest-reaper-{0}-gui' -f $PID))
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
    # 同一个情形走真正的反例这一行（真实的 scenarios.json）：结局同样是 INCOMPLETE，但看门狗没有跑过，这一行不能判 PASS
    $realConfig = Get-ReaperSmokeConfig (Join-Path $RepoRoot 'tests/reaper/scenarios.json')
    $negTimeout = @($realConfig.negative | Where-Object { $_.id -eq 'neg-timeout' })[0]
    $negKilled = @($realConfig.negative | Where-Object { $_.id -eq 'neg-killed' })[0]
    $guardParams = @{ Unused = 1 } # 只有守卫类反例才用得上参数，这里是 hang 类，凑齐即可
    $during.Called = $false
    $rowTimeout = Invoke-ReaperNegativeRow -Context $hangCtx -Scenario $negTimeout -GuardParams $guardParams -TempRoot $Work
    $rowKilled = Invoke-ReaperNegativeRow -Context $hangCtx -Scenario $negKilled -GuardParams $guardParams -TempRoot $Work
    Write-Case '看门狗反例 neg-timeout 在 gui 锁等不到时：整行 INCOMPLETE（不是 PASS），写明没能验证，REAPER 没有启动' $true ($rowTimeout.observed -eq 'INCOMPLETE' -and $rowTimeout.status -eq 'INCOMPLETE' -and $rowTimeout.detail -like '*没能验证*' -and -not $during.Called -and $hangCtx.Launches -eq 0)
    Write-Case '看门狗反例 neg-killed 在 gui 锁等不到时：整行 INCOMPLETE（不是 PASS）' $true ($rowKilled.observed -eq 'INCOMPLETE' -and $rowKilled.status -eq 'INCOMPLETE' -and $rowKilled.detail -like '*没能验证*')
    Write-Case '锁核对：自测专用名下的 gui 锁被别人占着时，另起进程查得出取不到' $true (-not (Test-RinneLockFree gui))
    # 上下文给了别的锁名：起 REAPER 与事后核对都用它，所以 gui 被别人占着也不影响看门狗反例照常 PASS（改回写死的 gui 就会等不到）
    $injCtx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $hangConfig -LockWaitSec 5 -GuiLock 'ipc-tests'
    $injCtx.Signals['48000-2-0'] = Join-Path $Work 'sig.wav'
    $injCtx.Launcher = New-StandIn 'Start-Sleep -Seconds 60' @{}
    $injTimeout = $negTimeout | ConvertTo-Json | ConvertFrom-Json; $injTimeout.timeoutSec = 2
    $rowInj = Invoke-ReaperNegativeRow -Context $injCtx -Scenario $injTimeout -GuardParams $guardParams -TempRoot $Work
    Write-Case '看门狗反例：上下文给了锁名时起 REAPER 与事后核对都用它，gui 被别人占着也照常 PASS' $false ($rowInj.status -eq 'PASS' -and $rowInj.detail -like '超时（*' -and $injCtx.Launches -eq 1)
} finally { if (-not $holder.HasExited) { $holder.Kill(); $holder.WaitForExit() } }

# 看门狗反例正常触发时仍是 PASS：替身睡觉，真的靠超时与到点被杀结束（真实的 scenarios.json 里的两条，只把时限缩短）。
# 起 REAPER 与事后核对用的都是上下文里的锁名（这里是默认的 gui），按自测专用的锁名前缀解析，与正式的 gui 锁互不等待。
$sleeper = New-StandIn 'Start-Sleep -Seconds 60' @{}
$genConfig = [pscustomobject]@{ limits = [pscustomobject]@{ luaTimeoutSec = 30 }; signal = [pscustomobject]@{ seconds = 4; kind = 'x' } }
$genCtx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $genConfig -LockWaitSec 10
$genCtx.Signals['48000-2-0'] = Join-Path $Work 'sig.wav'
$genCtx.Launcher = $sleeper
$quickTimeout = $negTimeout | ConvertTo-Json | ConvertFrom-Json; $quickTimeout.timeoutSec = 2
$quickKilled = $negKilled | ConvertTo-Json | ConvertFrom-Json; $quickKilled.killAfterSec = 1.5
$genTimeout = Invoke-ReaperNegativeRow -Context $genCtx -Scenario $quickTimeout -GuardParams $guardParams -TempRoot $Work
$launchedOnce = $genCtx.Launches
$genKilled = Invoke-ReaperNegativeRow -Context $genCtx -Scenario $quickKilled -GuardParams $guardParams -TempRoot $Work
Write-Case '看门狗反例 neg-timeout 正常触发（超时被结束、进程没了、锁放开）仍是 PASS' $false ($genTimeout.status -eq 'PASS' -and $genTimeout.observed -eq 'INCOMPLETE' -and $genTimeout.detail -like '超时（*' -and $launchedOnce -eq 1)
Write-Case '看门狗反例 neg-killed 正常触发（到点被结束）仍是 PASS' $false ($genKilled.status -eq 'PASS' -and $genKilled.observed -eq 'INCOMPLETE' -and $genKilled.detail -like '到点被结束（*' -and $genCtx.Launches -eq 2)


# ---- 入口护栏 ----
$scenarioFile = Join-Path $RepoRoot 'tests/reaper/scenarios.json'
$smoke = @{ CfgDir = $CfgA; ReaperExe = (Join-Path $Work 'no-reaper.exe'); BuildDir = (Join-Path $Work 'no-build'); LogDir = $Work; ScenarioFile = $scenarioFile }
$marker = Join-Path $Work 'USER_TESTING'
Set-Content -LiteralPath $marker -Value x
try {
    $r = Invoke-ReaperSmoke @smoke -UserTestingMarker $marker
    Write-Case '入口：USER_TESTING 标记在时整套 SKIPPED，一次 REAPER 都没启动' $true ($r.verdict -eq 'SKIPPED' -and $r.launches -eq 0 -and $r.skipReasons.Count -eq 1)
    $r = Invoke-ReaperSmoke @smoke -UserTestingMarker $marker -Probe
    Write-Case '入口（第二段）：USER_TESTING 标记在时整套 SKIPPED，一次 REAPER 都没启动，结果标为第二段' $true ($r.verdict -eq 'SKIPPED' -and $r.launches -eq 0 -and $r.phase -eq 2 -and $r.skipReasons.Count -eq 1)
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

# 守卫反例 neg-user-testing 的参数：调用方给的 $Params 里本来就带着 UserTestingMarker，带不带都要用守卫自己新建的标记
$withKey = $smoke.Clone(); $withKey.UserTestingMarker = Join-Path $Work 'caller-marker-that-is-not-there'
$guardRun = { param($callerParams) try { $g = Invoke-GuardNegative -Params $callerParams -Guard 'user-testing' -TempRoot $Work; $g.Observed -ceq 'SKIPPED' -and $g.Detail -like 'launches=0；USER_TESTING 标记存在*' } catch { $false } }
Write-Case '守卫反例 neg-user-testing：调用方的参数里没带标记时，照样用自己新建的标记，SKIPPED 且一次 REAPER 都没启动' $true (& $guardRun $smoke)
Write-Case '守卫反例 neg-user-testing：调用方的参数里已带着别的（不存在的）标记时，仍用自己新建的标记，不重复绑定也不被覆盖' $true (& $guardRun $withKey)
$negUser = @($realConfig.negative | Where-Object { $_.id -eq 'neg-user-testing' })[0]
$rowUser = Invoke-ReaperNegativeRow -Context $genCtx -Scenario $negUser -GuardParams $withKey -TempRoot $Work
Write-Case '守卫反例 neg-user-testing 走完整的反例这一行（真实的 scenarios.json）：PASS' $true ($rowUser.status -eq 'PASS' -and $rowUser.observed -eq 'SKIPPED')

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
