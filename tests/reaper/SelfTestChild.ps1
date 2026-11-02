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
Import-Module (Join-Path $scripts 'ProbeLogs.psm1') -ErrorAction Stop
Import-Module (Join-Path $RepoRoot 'tests/reaper/Rpp.psm1') -ErrorAction Stop
Import-Module (Join-Path $RepoRoot 'tests/reaper/SelfTestProbe.psm1') -ErrorAction Stop # 替身（分析器、nulltest、探针构建目录）与假日志文本

function Write-Case([string]$Label, [bool]$Bad, [bool]$Passed) { Write-Output ("RESULT`t{0}`t{1}`t{2}" -f [int]$Bad, [int]$Passed, $Label) }
function Test-Throws([scriptblock]$Action, [string]$Like = '*') { try { $null = & $Action; $false } catch { $_.Exception.Message -like $Like } }

$pwshExe = (Get-Process -Id $PID).Path
$flag = '-cfg' + 'file'
$gone = { param($ProcessId) $null -eq (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) }

# 替身：把 Host 准备好的启动信息换成别的命令，记下原来的命令行
function New-StandIn([string]$Command, [hashtable]$Seen) {
    { param($psi) $Seen.Args = $psi.Arguments; $Seen.Called = $true; $psi.FileName = $pwshExe; $psi.Arguments = "-NoProfile -Command `"$Command`""; [Diagnostics.Process]::Start($psi) }.GetNewClosure()
}
# 真机上 Process.ExitTime 偶尔读出 FILETIME 0（换算成 UTC 是 1601-01-01）：把真进程包一层，ExitTime 照这个读，其余照真进程
function New-BadExitTimeProcess($Wrapped) {
    $w = [pscustomobject]@{ Real = $Wrapped }
    foreach ($member in 'Id', 'HasExited', 'ExitCode') { Add-Member -InputObject $w -MemberType ScriptProperty -Name $member -Value ([scriptblock]::Create("`$this.Real.$member")) }
    Add-Member -InputObject $w -MemberType ScriptProperty -Name ExitTime -Value { [DateTime]::FromFileTime(0) }
    Add-Member -InputObject $w -MemberType ScriptMethod -Name WaitForExit -Value { param($ms) $this.Real.WaitForExit($ms) }
    $w
}
function New-BadExitTimeStandIn([string]$Command) {
    $launch = New-StandIn $Command @{}
    { param($psi) New-BadExitTimeProcess (& $launch $psi) }.GetNewClosure()
}
# 结束不了的进程：把真进程包一层，WaitForExit 说「还没退出」（不真等 5 秒）。Seen.Pids 记下每个替身的进程号，用例结束后自己清掉
function New-StuckProcess($Wrapped) {
    $w = [pscustomobject]@{ Real = $Wrapped }
    foreach ($member in 'Id', 'HasExited', 'ExitCode') { Add-Member -InputObject $w -MemberType ScriptProperty -Name $member -Value ([scriptblock]::Create("`$this.Real.$member")) }
    Add-Member -InputObject $w -MemberType ScriptMethod -Name WaitForExit -Value { param($ms) $false }
    $w
}
function New-StuckStandIn([string]$Command, [hashtable]$Seen) {
    $launch = New-StandIn $Command $Seen
    { param($psi) $p = & $launch $psi; $Seen.Pids.Add($p.Id); New-StuckProcess $p }.GetNewClosure()
}
function Stop-StuckStandIns([hashtable]$Seen) { foreach ($stuckPid in @($Seen.Pids)) { Stop-Process -Id $stuckPid -Force -ErrorAction SilentlyContinue } }

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
Write-Case '启动记录：ExitedUtc 是进程自己报的退出时刻，晚于启动前一刻、不晚于现在；StartedUtc 在它之前' $false (
    $run.PSObject.Properties.Name -contains 'ExitedUtc' -and $run.ExitedUtc -ge $run.StartedUtc -and $run.ExitedUtc -le [DateTime]::UtcNow.AddSeconds(1) -and ($run.ExitedUtc - $run.StartedUtc).TotalSeconds -lt 20 -and $run.ExitedUtc.Kind -eq [DateTimeKind]::Utc)
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1.5 -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '启动记录：被结束的进程也记 ExitedUtc（结束之后、不晚于现在），收日志靠它认「退出之后新建的文件」' $false (
    $run.Status -eq 'Killed' -and $run.PSObject.Properties.Name -contains 'ExitedUtc' -and $run.ExitedUtc -gt $run.StartedUtc.AddSeconds(1) -and $run.ExitedUtc -le [DateTime]::UtcNow.AddSeconds(1))
# 进程自报的退出时刻不可信（真机上偶尔是 1601 年）：取检测到退出的当下，不拿它当上界
$xs = [DateTime]::new(2026, 10, 26, 6, 0, 0, [DateTimeKind]::Utc); $xd = $xs.AddSeconds(20)
$xr = { param($rep) Resolve-ReaperExitedUtc -Reported $rep -NotBeforeUtc $xs -NoticedUtc $xd }
Write-Case '退出时刻：进程自报的落在 [启动前一刻, 检测当下 + 1 s] 里就采信（含两个端点）' $false (
    (& $xr $xs.AddSeconds(19.9)) -eq $xs.AddSeconds(19.9) -and (& $xr $xs) -eq $xs -and (& $xr $xd.AddSeconds(1)) -eq $xd.AddSeconds(1) -and (& $xr $xs.AddSeconds(19.9)).Kind -eq [DateTimeKind]::Utc)
Write-Case '退出时刻：进程自报 FILETIME 0（1601-01-01 UTC，真机上见过）：不采信，取检测到退出的当下' $true ((& $xr ([DateTime]::FromFileTime(0).ToUniversalTime())) -eq $xd)
Write-Case '退出时刻：进程自报早于启动前一刻哪怕 1 ms：不采信' $true ((& $xr $xs.AddMilliseconds(-1)) -eq $xd)
Write-Case '退出时刻：进程自报晚于检测当下 1 s 哪怕 1 ms：不采信' $true ((& $xr $xd.AddSeconds(1).AddMilliseconds(1)) -eq $xd)
Write-Case '退出时刻：进程没报（取不到）：取检测到退出的当下' $true ((& $xr $null) -eq $xd -and (& $xr $null).Kind -eq [DateTimeKind]::Utc)
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-BadExitTimeStandIn 'exit 0')
Write-Case '启动记录：进程自报的退出时刻是 1601 年（自己退出的进程）：ExitedUtc 取检测到退出的当下，晚于启动、不晚于现在' $true (
    $run.Status -eq 'Exited' -and $run.ExitedUtc -ge $run.StartedUtc -and $run.ExitedUtc -le [DateTime]::UtcNow.AddSeconds(1) -and ($run.ExitedUtc - $run.StartedUtc).TotalSeconds -lt 20)
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1.5 -Launcher (New-BadExitTimeStandIn 'Start-Sleep -Seconds 60')
Write-Case '启动记录：进程自报的退出时刻是 1601 年（被结束的进程）：ExitedUtc 同样取检测到退出的当下' $true (
    $run.Status -eq 'Killed' -and $run.ExitedUtc -gt $run.StartedUtc.AddSeconds(1) -and $run.ExitedUtc -le [DateTime]::UtcNow.AddSeconds(1))
# ---- 结束不了的进程（StillRunning）：杀不掉，或杀了之后 5 秒还没退出，不能静默吞掉 ----
$denyKill = { param($ProcessId) throw 'Access is denied' }
$noopKill = { param($ProcessId) }
$killThenThrow = { param($ProcessId) Stop-Process -Id $ProcessId -Force; throw 'late error' }
$stuckSeen = @{ Pids = [Collections.Generic.List[int]]::new() }
try {
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 1 -Terminator $denyKill -Launcher (New-StuckStandIn 'Start-Sleep -Seconds 60' $stuckSeen)
    Write-Case '结束不了的进程：结束动作报错、进程没退出，StillRunning，带着结束动作的报错，进程确实还活着' $true (
        $run.Status -eq 'Timeout' -and $run.StillRunning -eq $true -and $run.KillError -like '*Access is denied*' -and -not (& $gone $run.Pid))
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1 -Terminator $denyKill -Launcher (New-StuckStandIn 'Start-Sleep -Seconds 60' $stuckSeen)
    Write-Case '结束不了的进程：到点被结束（Killed）时同样记 StillRunning' $true ($run.Status -eq 'Killed' -and $run.StillRunning -eq $true -and $run.KillError -like '*Access is denied*')
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 1 -Launcher (New-StuckStandIn 'Start-Sleep -Seconds 60' $stuckSeen)
    Write-Case '结束不了的进程：杀成功了但 WaitForExit(5000) 返回 false（退出卡住），StillRunning，没有结束动作的报错' $true (
        $run.Status -eq 'Timeout' -and $run.StillRunning -eq $true -and -not $run.KillError)
    $run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 1 -Terminator $noopKill -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
    $noopPid = $run.Pid
    Write-Case '结束不了的进程：结束动作什么都没做（没报错），真等 5 秒后进程还活着：StillRunning' $true (
        $run.Status -eq 'Timeout' -and $run.StillRunning -eq $true -and -not $run.KillError -and -not (& $gone $noopPid))
    Stop-Process -Id $noopPid -Force -ErrorAction SilentlyContinue
} finally { Stop-StuckStandIns $stuckSeen }
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 1 -Terminator $killThenThrow -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '结束动作报了错但进程已经没了：不算 StillRunning（看进程退没退出，不看有没有报错），报错照实记' $false (
    $run.Status -eq 'Timeout' -and $run.StillRunning -eq $false -and $run.KillError -like '*late error*' -and (& $gone $run.Pid))
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 1 -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '正常结束的进程：超时被结束后 StillRunning 为 false，没有 KillError' $false ($run.Status -eq 'Timeout' -and $run.StillRunning -eq $false -and $null -eq $run.KillError -and (& $gone $run.Pid))
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1 -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '正常结束的进程：到点被结束后 StillRunning 为 false' $false ($run.Status -eq 'Killed' -and $run.StillRunning -eq $false)
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-StandIn 'exit 0' @{})
Write-Case '正常结束的进程：自己退出的 StillRunning 为 false' $false ($run.Status -eq 'Exited' -and $run.StillRunning -eq $false)
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
# ---- REAPER 结束不了：Invoke-ReaperLaunch 记下 Halted、锁照样放开；之后一律不再启动；场景据此 INCOMPLETE ----
$stuckConfig = [pscustomobject]@{ limits = [pscustomobject]@{ luaTimeoutSec = 30; renderTimeoutSec = 1 }; signal = [pscustomobject]@{ seconds = 4; kind = 'x' } }
$stuckSeenL = @{ Pids = [Collections.Generic.List[int]]::new() }
try {
    $stuckCtx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $stuckConfig
    $stuckCtx.Signals['48000-2-0'] = Join-Path $Work 'sig.wav'
    $stuckCtx.Launcher = New-StuckStandIn 'Start-Sleep -Seconds 60' $stuckSeenL
    $stuckCtx.Terminator = $denyKill
    Write-Case '上下文：新建时 Halted 为空，没有 Terminator' $false ($null -eq (New-ReaperContext -ReaperExe 'x' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $stuckConfig).Halted)
    $r1 = Invoke-ReaperLaunch -Ctx $stuckCtx -HostArgs @{ TimeoutSec = 1 }
    Write-Case 'REAPER 结束不了：Invoke-ReaperLaunch 返回 StillRunning，上下文记下 Halted（点名进程号和原因），gui 锁照样放开' $true (
        $r1.StillRunning -eq $true -and "$($stuckCtx.Halted)" -like "*进程 $($r1.Pid) 没能结束*Access is denied*" -and (& $probeExit) -eq 0 -and (Test-HostStuck $r1))
    Write-Case 'REAPER 结束不了：Get-HostFailure 写明进程没能结束、可能还在运行、本轮不再启动' $true ((Get-HostFailure $r1) -like "*进程 $($r1.Pid) 没能结束*可能还在运行*不再启动*")
    $startedBefore = $stuckSeenL.Pids.Count; $launchesBefore = $stuckCtx.Launches; $logBeforeH = $stuckCtx.LaunchLog.Count
    $r2 = Invoke-ReaperLaunch -Ctx $stuckCtx -HostArgs @{ TimeoutSec = 1 }
    Write-Case '已停止的一轮：再要启动 REAPER，状态 Halted，没有启动进程、没有拿锁、没有记启动次数与启动日志' $true (
        $r2.Status -eq 'Halted' -and $stuckSeenL.Pids.Count -eq $startedBefore -and $stuckCtx.Launches -eq $launchesBefore -and $stuckCtx.LaunchLog.Count -eq $logBeforeH -and (& $probeExit) -eq 0 -and (Get-HostFailure $r2) -like '*已停止*没有启动*')
    $moved = try { & (Get-Module Scenarios) { param($c, $run) Save-ProbeLogs $c $run (Join-Path $Work 'stuck-cell') } ([pscustomobject]@{ Probe = [pscustomobject]@{} }) $r1 } catch { "THREW: $($_.Exception.Message)" }
    Write-Case '没能结束的进程不收它的日志（它可能还活着、还在写）：Save-ProbeLogs 直接返回空，不碰日志目录' $true ($null -eq $moved)
    # 场景层：第一个场景撞上结束不了的进程，INCOMPLETE 点名；第二个场景根本不跑
    $stuckCtx2 = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $stuckConfig
    $stuckCtx2.Signals['48000-2-0'] = Join-Path $Work 'sig.wav'
    $stuckCtx2.Launcher = New-StuckStandIn 'Start-Sleep -Seconds 60' $stuckSeenL
    $stuckCtx2.Terminator = $denyKill
    $renderStuck = [pscustomobject]@{ id = 'render-stuck'; kind = 'render'; sr = 48000; block = 512 }
    $o1 = Invoke-ReaperScenario -Ctx $stuckCtx2 -Scenario $renderStuck
    Write-Case '场景撞上结束不了的进程：INCOMPLETE，原因写进程没能结束，上下文记下 Halted' $true (
        $o1.Status -eq 'INCOMPLETE' -and $o1.Detail -like '*没能结束*' -and "$($stuckCtx2.Halted)" -like '*没能结束*')
    $startedBefore2 = $stuckSeenL.Pids.Count
    $o2 = Invoke-ReaperScenario -Ctx $stuckCtx2 -Scenario ([pscustomobject]@{ id = 'render-after'; kind = 'render'; sr = 48000; block = 512 })
    Write-Case '已停止的一轮里的下一个场景：INCOMPLETE，写明本轮已停止、没有启动 REAPER，真的一个进程都没起' $true (
        $o2.Status -eq 'INCOMPLETE' -and $o2.Detail -like '*本轮已停止*没有启动 REAPER*没能结束*' -and $stuckSeenL.Pids.Count -eq $startedBefore2 -and $o2.PSObject.Properties.Name -contains 'Seconds')
    # 看门狗反例（hang）撞上结束不了的进程：这正是它要查的东西，FAIL 而不是 INCOMPLETE
    $stuckCtx3 = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $stuckConfig
    $stuckCtx3.Signals['48000-2-0'] = Join-Path $Work 'sig.wav'
    $stuckCtx3.Launcher = New-StuckStandIn 'Start-Sleep -Seconds 60' $stuckSeenL
    $stuckCtx3.Terminator = $denyKill
    # 直接调场景处理器：经 Invoke-ReaperScenario 时，加载来源核对会把没证明的 FAIL 一律改判 INCOMPLETE（这个上下文没有被测插件），这里要看的是处理器自己的结局
    $oh = & (Get-Module Scenarios) { param($c, $sc) Invoke-HangScenario $c $sc } $stuckCtx3 ([pscustomobject]@{ id = 'hang-stuck'; kind = 'hang'; timeoutSec = 1; killAfterSec = 0 })
    Write-Case '看门狗反例撞上结束不了的进程：FAIL，写明看门狗没能结束进程（不是 INCOMPLETE，更不是 PASS）' $true ($oh.Status -eq 'FAIL' -and $oh.Detail -like '*看门狗没能结束进程*')
    $oh2 = Invoke-ReaperScenario -Ctx $stuckCtx3 -Scenario ([pscustomobject]@{ id = 'hang-after'; kind = 'hang'; timeoutSec = 1; killAfterSec = 0 })
    Write-Case '看门狗反例在已停止的一轮里：INCOMPLETE，不启动 REAPER' $true ($oh2.Status -eq 'INCOMPLETE' -and $oh2.Detail -like '*本轮已停止*')
    # 处理器本身撞上已停止的一轮（不经 Invoke-ReaperScenario 的早退）：Run 里没有进程号，要写成 REAPER 没有启动，不能落成「场景抛错」
    $startedBeforeH = $stuckSeenL.Pids.Count
    $oh3 = try { & (Get-Module Scenarios) { param($c, $sc) Invoke-HangScenario $c $sc } $stuckCtx3 ([pscustomobject]@{ id = 'hang-direct'; kind = 'hang'; timeoutSec = 1; killAfterSec = 0 }) } catch { [pscustomobject]@{ Status = 'THREW'; Detail = $_.Exception.Message } }
    Write-Case '看门狗反例的处理器遇到已停止的一轮：INCOMPLETE，写明 REAPER 没有启动、看门狗没能验证，不是「场景抛错」，没起进程' $true (
        $oh3.Status -eq 'INCOMPLETE' -and $oh3.Detail -like '*REAPER 没有启动*' -and $oh3.Detail -notlike '*场景抛错*' -and $stuckSeenL.Pids.Count -eq $startedBeforeH)
} finally { Stop-StuckStandIns $stuckSeenL }
$holder = & $lockChild 'if (-not (Enter-RinneLock gui -TimeoutSeconds 5 -Quiet)) { exit 2 }; Start-Sleep -Seconds 60'
try {
    $until = (Get-Date).AddSeconds(10)
    while ((& $probeExit) -ne 1 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 200 }
    $ctx.Launcher = New-StandIn 'exit 0' $during
    $during.Called = $false; $ctx.LockWaitSec = 1; $before = $ctx.Launches; $logBefore = $ctx.LaunchLog.Count
    $run = Invoke-ReaperLaunch -Ctx $ctx -HostArgs @{ TimeoutSec = 5 }
    Write-Case 'gui 锁：别人占着时等不到，状态 LockBusy，不无锁硬跑' $true ($run.Status -eq 'LockBusy' -and -not $during.Called -and $ctx.Launches -eq $before -and (Get-HostFailure $run) -eq 'gui 锁等不到')
    Write-Case 'gui 锁：等不到的那次不记进启动日志（REAPER 没有启动，加载来源没有可核对的东西）' $true ($ctx.LaunchLog.Count -eq $logBefore)
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

# ---- 进程里加载的模块（WatchModules / ModuleSampler）----
$watchLeaves = 'Rinne Input.vst3', 'Rinne Output.vst3'
$modCalls = @{ N = 0 }
$modSampler = { param($proc) $modCalls.N++; , @('C:\Windows\System32\kernel32.dll', 'C:\x\Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3', 'D:\y\RINNE OUTPUT.VST3', 'C:\x\Other.vst3') }.GetNewClosure()
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 2 -WatchModules $watchLeaves -ModuleSampler $modSampler -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '模块取样：每次轮询取一次、结束前再取一次，名字对上的（不分大小写）记下完整路径，不在关注名单里的不记' $false (
    $run.Status -eq 'Timeout' -and $run.ModuleScans -eq $modCalls.N -and $run.ModuleScans -ge 3 -and $run.Modules.get_Count() -eq 2 -and
    @($run.Modules['Rinne Input.vst3']).Count -eq 1 -and @($run.Modules['Rinne Input.vst3'])[0] -ceq 'C:\x\Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3' -and
    @($run.Modules['Rinne Output.vst3']).Count -eq 1 -and @($run.Modules['Rinne Output.vst3'])[0] -ceq 'D:\y\RINNE OUTPUT.VST3')
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -KillAfterSec 1.5 -WatchModules $watchLeaves -ModuleSampler $modSampler -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '模块取样：到点被杀的启动也留着被杀之前见到的模块' $false ($run.Status -eq 'Killed' -and $run.ModuleScans -ge 2 -and @($run.Modules['Rinne Input.vst3']).Count -eq 1)
$modCalls.N = 0
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -WatchModules $watchLeaves -ModuleSampler $modSampler -Launcher (New-StandIn 'exit 0' @{})
Write-Case '模块取样：进程退出得比第一次轮询还快，结束后那一次取样照样记下（取样次数至少 1）' $false ($run.Status -eq 'Exited' -and $run.ModuleScans -ge 1 -and @($run.Modules['Rinne Input.vst3']).Count -eq 1)
$modNull = { param($proc) $null }
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 2 -WatchModules $watchLeaves -ModuleSampler $modNull -Launcher (New-StandIn 'Start-Sleep -Seconds 60' @{})
Write-Case '模块取样：一次也读不到模块列表时取样次数是 0，两个模块都是空的，不抛错' $true ($run.ModuleScans -eq 0 -and @($run.Modules['Rinne Input.vst3']).Count -eq 0 -and @($run.Modules['Rinne Output.vst3']).Count -eq 0)
$modCalls.N = 0
$run = Invoke-ReaperHost -ReaperExe 'stand-in' -TimeoutSec 30 -Launcher (New-StandIn 'exit 0' @{}) -ModuleSampler $modSampler
Write-Case '模块取样：没给关注名单就一次也不取样，Modules 是空的' $false ($modCalls.N -eq 0 -and $run.ModuleScans -eq 0 -and $run.Modules.get_Count() -eq 0)
$modProc = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new($pwshExe, '-NoProfile -Command Start-Sleep -Seconds 30'))
try {
    $listed = $null; $untilMod = (Get-Date).AddSeconds(15)
    while (-not $listed -and (Get-Date) -lt $untilMod) { $listed = Get-ReaperProcessModulePaths $modProc; if (-not $listed) { Start-Sleep -Milliseconds 200 } }
    Write-Case '读进程模块（真进程）：列得出完整路径，里面有进程自己的可执行文件' $false ([bool]$listed -and @($listed | Where-Object { [IO.Path]::GetFileName($_) -ieq (Split-Path -Leaf $pwshExe) }).Count -ge 1)
} finally { if (-not $modProc.HasExited) { $modProc.Kill(); $modProc.WaitForExit() } }
Write-Case '读进程模块（真进程）：进程已退出时返回 $null，不抛错，不当作空列表' $true ($null -eq (Get-ReaperProcessModulePaths $modProc))

# Invoke-ReaperLaunch：会加载插件的启动取样并记进启动日志，不加载插件的（参照渲染）不取样，也不会被当成被测插件的启动
$launchInstalled = @{ 'Rinne Input.vst3' = @{ Path = 'C:\x\Rinne Input.vst3'; Sha256 = ('a' * 64) }; 'Rinne Output.vst3' = @{ Path = 'D:\y\RINNE OUTPUT.VST3'; Sha256 = ('b' * 64) } }
$launchCtx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $ctxConfig -Installed $launchInstalled -PluginsDir (Join-Path $Work 'plugins')
$launchCtx.Launcher = New-StandIn 'exit 0' @{}
$launchCtx.ModuleSampler = $modSampler
$modCalls.N = 0
$null = Invoke-ReaperLaunch -Ctx $launchCtx -HostArgs @{ TimeoutSec = 30 } -Label 'with-plugins'
$sampledWith = $modCalls.N
$modCalls.N = 0
$null = Invoke-ReaperLaunch -Ctx $launchCtx -HostArgs @{ TimeoutSec = 30 } -Label 'no-plugins' -LoadsPlugins $false
Write-Case '启动日志：会加载插件的启动取样了模块，并记下标签、状态、见到的路径与取样次数' $false (
    $sampledWith -ge 1 -and $launchCtx.LaunchLog.Count -eq 2 -and $launchCtx.LaunchLog[0].Label -ceq 'with-plugins' -and $launchCtx.LaunchLog[0].LoadsPlugins -and $launchCtx.LaunchLog[0].Status -eq 'Exited' -and
    $launchCtx.LaunchLog[0].ModuleScans -ge 1 -and @($launchCtx.LaunchLog[0].Modules['Rinne Input.vst3']).Count -eq 1)
Write-Case '启动日志：不加载插件的启动（参照渲染）一次也不取样，记为不加载插件，没有模块' $true (
    $modCalls.N -eq 0 -and -not $launchCtx.LaunchLog[1].LoadsPlugins -and $launchCtx.LaunchLog[1].ModuleScans -eq 0 -and $launchCtx.LaunchLog[1].Modules.get_Count() -eq 0)
$emptyCtx = New-ReaperContext -ReaperExe 'stand-in' -NullTest 'x' -WorkDir $Work -Driver 'x' -Config $ctxConfig
$emptyCtx.Launcher = New-StandIn 'exit 0' @{}
$emptyCtx.ModuleSampler = $modSampler
$modCalls.N = 0
$null = Invoke-ReaperLaunch -Ctx $emptyCtx -HostArgs @{ TimeoutSec = 30 }
Write-Case '启动日志：上下文里没有被测副本的清单（没给 Installed）时不取样' $true ($modCalls.N -eq 0 -and $emptyCtx.LaunchLog.Count -eq 1)

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
# vstpath64 已经是「系统 VST3 目录在前、私有目录排在最后」时不再改写 ini：LF 行尾、CRLF 行尾、前面还有别的目录（分号）都要认。
# 旧写法（私有目录排在最前、只有私有目录、没有这一行）必须被改写成新写法，否则 REAPER 会让系统目录里同名同类 ID 的发布版盖掉私有副本
Write-Case '插件复制：重复安装不改写 ini（vstpath64 已是新写法，LF 行尾）' $false ($iniNow -ceq $iniFirst)
$pluginsDir = Join-Path $pinned.Dir 'plugins'
$systemDirs = '%COMMONPROGRAMFILES%\VST3;%LOCALAPP' + 'DATA%\Programs\Common\VST3'
$elsewhereDir = Join-Path $Work 'elsewhere'
$iniRaw = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($pinned.Ini)) # 前面的用例往 ini 里写过非 UTF-8 的字节，按 Latin-1 读才能原样往返
$vstLine = { [regex]::Match([Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($pinned.Ini)), '(?im)^vstpath64=(.*)$').Groups[1].Value.TrimEnd("`r") }
Write-Case '插件复制：vstpath64 是两个系统 VST3 目录在前、私有目录排在最后' $false ((& $vstLine) -ceq "$systemDirs;$pluginsDir")
foreach ($variant in @(@('CRLF 行尾', "`r`n", ''), @('前面还有别的目录', "`n", "$elsewhereDir;"), @('CRLF 行尾且前面还有别的目录', "`r`n", "$elsewhereDir;"))) {
    $text = (($iniRaw -replace "`r?`n", "`n") -replace '(?m)^vstpath64=.*$', ('vstpath64=' + $variant[2] + $systemDirs + ';' + $pluginsDir)) -replace "`n", $variant[1]
    [IO.File]::WriteAllBytes($pinned.Ini, [Text.Encoding]::Latin1.GetBytes($text))
    $null = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
    Write-Case "插件复制：重复安装不改写 ini（$($variant[0])）" $false ([Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($pinned.Ini)) -ceq $text)
}
$legacyOrdered = @(
    @('私有目录排在最前（旧写法）', "$pluginsDir;$systemDirs", "$systemDirs;$pluginsDir"),
    @('只有私有目录（旧写法）', $pluginsDir, "$systemDirs;$pluginsDir"),
    @('私有目录在前、后面还有别的目录', "$pluginsDir;$elsewhereDir", "$elsewhereDir;$systemDirs;$pluginsDir"),
    @('私有目录用大小写不同的写法并带结尾反斜杠', ($pluginsDir.ToUpperInvariant() + '\'), "$systemDirs;$pluginsDir"),
    @('私有目录夹在两个系统目录中间', ('%COMMONPROGRAMFILES%\VST3;' + $pluginsDir + ';%LOCALAPP' + 'DATA%\Programs\Common\VST3'), "$systemDirs;$pluginsDir")
)
foreach ($legacy in $legacyOrdered) {
    [IO.File]::WriteAllBytes($pinned.Ini, [Text.Encoding]::Latin1.GetBytes((($iniRaw -replace "`r?`n", "`n") -replace '(?m)^vstpath64=.*$', ('vstpath64=' + $legacy[1]))))
    $null = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
    Write-Case "插件复制：vstpath64 是$($legacy[0])，安装后改写成新写法" $true ((& $vstLine) -ceq $legacy[2])
}
$strippedIni = ($iniRaw -replace "`r?`n", "`n") -replace '(?m)^vstpath64=.*\n', ''
[IO.File]::WriteAllBytes($pinned.Ini, [Text.Encoding]::Latin1.GetBytes($strippedIni))
$null = Install-ReaperPlugins -CfgDir $pinned.Dir -Bundles $bundles
Write-Case '插件复制：ini 里没有 vstpath64 这一行时，安装后写出新写法' $true ($strippedIni -notmatch 'vstpath64' -and (& $vstLine) -ceq "$systemDirs;$pluginsDir")
Write-Case '插件复制：报告里带复制进去的那一份的路径与完整 sha256（加载来源核对用），都与磁盘上的文件对得上' $false (
    @($third | Where-Object { $_.path -and (Test-Path -LiteralPath $_.path -PathType Leaf) -and $_.sha256Full -ceq (Get-FileHash -LiteralPath $_.path -Algorithm SHA256).Hash.ToLowerInvariant() -and $_.sha256Full.StartsWith($_.sha256) }).Count -eq 2)
Write-Case '插件复制：第一次全复制，内容相同第二次不动，改了的再复制' $false (@($first | Where-Object { $_.copied }).Count -eq 2 -and @($second | Where-Object { $_.copied }).Count -eq 0 -and @($third | Where-Object { $_.copied }).Count -eq 1)
Write-Case '插件复制：私有 VST3 目录排在 vstpath64 最后，目标里只有这次的构建' $false ($iniNow -match ('(?m)^vstpath64=' + [regex]::Escape($systemDirs) + ';' + [regex]::Escape($pluginsDir) + '\r?$') -and (Get-Content -LiteralPath (Join-Path $pinned.Dir 'plugins/Rinne Input.vst3/Contents/x86_64-win/Rinne Input.vst3') -Raw).Trim() -eq 'bits-new')

# ---- 扫描缓存：装插件时去掉被测插件的条目（真文件，Latin-1 字节原样往返）----
$cacheFile = Join-Path $pinned.Dir 'reaper-vstplugins64.ini'
$cacheNames = 'Rinne Input.vst3', 'Rinne Output.vst3'
if (Test-Path -LiteralPath $cacheFile) { Remove-Item -LiteralPath $cacheFile }
$noCache = Clear-ReaperScanCache -CfgDir $pinned.Dir -FileNames $cacheNames
Write-Case '扫描缓存：缓存文件不存在时什么都不做，也不建这个文件' $false ($noCache.Removed -eq 0 -and -not (Test-Path -LiteralPath $cacheFile))
$cacheKept = "[vstcache]`r`nOther.vst3=0102030405060708,11{FF,Caf" + [char]0xE9 + " " + [char]0xFF + "`r`n"
$cacheStaleText = "[vstcache]`r`nRinne_Output.vst3=525EE8DD736ADD01,1{AB,Rinne Output (Synchain)`r`nOther.vst3=0102030405060708,11{FF,Caf" + [char]0xE9 + " " + [char]0xFF + "`r`nRinne_Input.vst3=BCE5AEDD736ADD01,1{AB,Rinne Input (Synchain)`r`n"
[IO.File]::WriteAllBytes($cacheFile, [Text.Encoding]::Latin1.GetBytes($cacheStaleText))
$cleared = Clear-ReaperScanCache -CfgDir $pinned.Dir -FileNames $cacheNames
Write-Case '扫描缓存：留着旧时间戳的两个被测条目从文件里去掉了，别的行（含非 ASCII 字节）与 CRLF 原样，返回去掉 2 行' $true (
    $cleared.Removed -eq 2 -and [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($cacheFile)) -ceq $cacheKept)
(Get-Item -LiteralPath $cacheFile).LastWriteTimeUtc = [DateTime]::new(2020, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
$clearedAgain = Clear-ReaperScanCache -CfgDir $pinned.Dir -FileNames $cacheNames
Write-Case '扫描缓存：没有要去掉的条目时不写文件（内容与修改时间都不变）' $false (
    $clearedAgain.Removed -eq 0 -and [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($cacheFile)) -ceq $cacheKept -and (Get-Item -LiteralPath $cacheFile).LastWriteTimeUtc -eq [DateTime]::new(2020, 1, 1, 0, 0, 0, [DateTimeKind]::Utc))
Remove-Item -LiteralPath $cacheFile

# ---- 第二段接线：替身 REAPER 走真正的场景处理器、收日志与入口 ----
# 替身先起一个马上退出的进程，拿它（已退出）的 pid 写两份探针日志、结果文件和导出的 wav，像一次正常结束的 REAPER；
# 任务文件的路径从启动信息的环境变量取。分析器与 rinne_nulltest 用 .cmd 替身（吐准备好的输出）。
$wire = Join-Path $Work 'wiring'
$null = New-Item -ItemType Directory -Force -Path $wire
$probeSection = (Get-ReaperSmokeConfig $scenarioFile).probe
$fakePy = New-FakePython -Dir (Join-Path $wire 'fake')
$fakeNull = New-FakeNullTest -Dir (Join-Path $wire 'fake')
$newLog = Get-Command New-FakeProbeLogText
$diffReport = Join-Path $wire 'compare-diff.txt'
[IO.File]::WriteAllText($diffReport, (Get-FakeCompareText -Bit '否' -Lag '0' -Whole '-96.5' -Head '-101.25'), [Text.UTF8Encoding]::new($false))

# Spec 的键：LogDir（假的文档目录里的日志目录）、Seen（记每次启动）、NoPing / NoWav（哪一趟不写：'offline' / 'online' / 'play'）、PingFor（趟 → Ping 值）、
# NonRt（趟 → 播放中的块的 nonrt）、Plant（启动中往日志目录放别人的文件的块）、DuringRun（启动中动别人文件的块）、AfterExit（进程退出之后才往日志目录放文件的块，收到 pid、目录、退出时刻）
function New-FakeReaperLauncher([hashtable]$Spec) {
    {
        param($psi)
        # 替身进程在世时写日志（真 REAPER 的探针在进程活着时新建日志，收日志的创建时间判据看的就是这个）：它等一个放行文件，写完才放它退出
        $release = [IO.Path]::GetFullPath((Join-Path $Spec.LogDir "../release-$([guid]::NewGuid().ToString('N')).flag"))
        $started = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new($Spec.Pwsh, "-NoProfile -NonInteractive -Command `"while (-not (Test-Path -LiteralPath '$release')) { Start-Sleep -Milliseconds 20 }`""))
        $ownPid = $started.Id
        $job = @{}
        foreach ($m in [regex]::Matches([IO.File]::ReadAllText($psi.Environment['RINNE_REAPER_JOB']), '\["(\w+)"\]=("(?:[^"\\]|\\.)*"|[^,}]+)')) {
            $v = $m.Groups[2].Value
            $job[$m.Groups[1].Value] = $v.StartsWith('"') ? ($v.Substring(1, $v.Length - 2) -replace '\\\\', '\') : $v
        }
        $isExport = $job['step'] -eq 'probeexport'
        $resultName = [IO.Path]::GetFileNameWithoutExtension($job['result'])
        $leg = $isExport ? ($resultName.EndsWith('-offline') ? 'offline' : 'online') : 'play'
        $variant = $isExport ? ($leg -eq 'offline' ? 'export-offline' : 'export-rt') : 'steady'
        $case = $isExport ? 'CB-04' : 'CB-01'
        $nonRt = $Spec.NonRt.Contains($leg) ? $Spec.NonRt[$leg] : ($leg -eq 'offline' ? 1 : 0)
        $peer = ($resultName -match 'neg-probe-unpaired') ? 0 : -1
        $pings = $Spec.NoPing -contains $leg ? @() : @($Spec.PingFor.Contains($leg) ? $Spec.PingFor[$leg] : 4800)
        $renderSpeed = [regex]::Match([IO.File]::ReadAllText($job['project']), '(?m)^\s*RENDER_1X (\d+)').Groups[1].Value
        $Spec.Seen.Add([pscustomobject]@{ Pid = $ownPid; Step = $job['step']; Leg = $leg; RenderSpeed = $renderSpeed; AfterRender = $job['afterRenderSeconds'] })
        $null = New-Item -ItemType Directory -Force -Path $Spec.LogDir
        [IO.File]::WriteAllText((Join-Path $Spec.LogDir "probe-I-$ownPid-a1b1.csv"), (& $Spec.NewLog -Role I -LogPid $ownPid -Case $case -Variant $variant -NonRt $nonRt -PeerPid $peer -Pings $pings -Tok4 a1b1))
        [IO.File]::WriteAllText((Join-Path $Spec.LogDir "probe-O-$ownPid-c2d2.csv"), (& $Spec.NewLog -Role O -LogPid $ownPid -Case $case -Variant $variant -NonRt $nonRt -PeerPid $peer -Pings @() -Tok4 c2d2))
        if ($Spec.Plant) { & $Spec.Plant $ownPid $Spec.LogDir }
        if ($Spec.DuringRun) { & $Spec.DuringRun $ownPid $Spec.LogDir }
        if ($isExport -and $Spec.NoWav -notcontains $leg) { [IO.File]::WriteAllBytes($job['wav'], [byte[]]@(0)) }
        $driver = $isExport ? "render_seconds=10.000`nexit_bound=window`nrender_end_to_exit_s=8.000`n" : "stopped_after=9.100`nstop_to_exit_s=6.500`n"
        [IO.File]::WriteAllText($job['result'], "${driver}done=1`n")
        [IO.File]::WriteAllText($release, 'go')
        $started.WaitForExit()
        Remove-Item -LiteralPath $release -ErrorAction SilentlyContinue
        if ($Spec.AfterExit) { & $Spec.AfterExit $ownPid $Spec.LogDir $started.ExitTime.ToUniversalTime() }
        $Spec.BadExitTime ? (New-BadExitTimeProcess $started) : $started
    }.GetNewClosure()
}

# 替身 REAPER 里「进程加载的模块」：Spec.Modules 选一种。good：被测的两个插件都从私有目录加载；foreign：Input 来自别处的同名模块；none：一个被测模块都没有；
# unreadable：读不到模块列表。每次取样时才看 Spec.Modules，所以用例可以在中途改
$wireLeaves = @(Get-ReaperPluginIdentities -Probe | ForEach-Object { $_.Bundle })
function New-WireModuleSampler([hashtable]$Spec, [string]$ModulesRoot) {
    $leaves = $wireLeaves
    $foreignDir = Join-Path $wire 'foreign-vst3'
    {
        param($proc)
        $own = @($leaves | ForEach-Object { Join-Path $ModulesRoot (Join-Path $_ "Contents\x86_64-win\$_") })
        switch ($Spec.Modules) {
            'unreadable' { $null }
            'none' { , @('C:\Windows\System32\kernel32.dll') }
            'foreign' { , @((Join-Path $foreignDir (Join-Path $leaves[0] "Contents\x86_64-win\$($leaves[0])")), $own[1]) }
            default { , (@('C:\Windows\System32\kernel32.dll') + $own) }
        }
    }.GetNewClosure()
}

function New-WireSpec([string]$Id) {
    $logDir = Join-Path $wire "$Id-docs/Rinne Probe Logs"
    $null = New-Item -ItemType Directory -Force -Path $logDir
    @{ Modules = 'good'; Pwsh = $pwshExe; NewLog = $newLog; LogDir = $logDir; Seen = [Collections.Generic.List[object]]::new(); NoPing = @(); NoWav = @(); PingFor = @{}; NonRt = @{}; Plant = $null; DuringRun = $null; AfterExit = $null; BadExitTime = $false }
}

# 一个场景的上下文：假的文档目录、本轮结果目录、替身启动器
function New-WireContext([string]$Id, [hashtable]$Spec) {
    $roundDir = Join-Path $wire "$Id-round"; $workDir = Join-Path $wire "$Id-work"
    $null = New-Item -ItemType Directory -Force -Path $roundDir, $workDir
    $installedHere = @{}
    $pluginsHere = Join-Path $workDir 'plugins'
    foreach ($leaf in $wireLeaves) {
        $binFile = Join-Path $pluginsHere (Join-Path $leaf "Contents\x86_64-win\$leaf")
        $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $binFile)
        [IO.File]::WriteAllText($binFile, "bits-$leaf")
        $installedHere[$leaf] = @{ Path = $binFile; Sha256 = (Get-FileHash -LiteralPath $binFile -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $c = New-ReaperContext -ReaperExe 'stand-in' -NullTest $fakeNull -WorkDir $workDir -Driver 'x' -Config $probeSection -LockWaitSec 20 -Installed $installedHere -PluginsDir $pluginsHere
    $c.Launcher = New-FakeReaperLauncher $Spec
    $c.ModuleSampler = New-WireModuleSampler $Spec $pluginsHere
    $c.Probe = & (Get-Module Suite) { param($pre, $sec, $dir) New-ReaperProbeRound $pre $sec $dir } ([pscustomobject]@{ ProbeLogDir = $Spec.LogDir; Python = $fakePy.Python; Analyzer = $fakePy.Analyzer }) $probeSection $roundDir
    $c
}

$playScenario = @($probeSection.scenarios | Where-Object { $_.id -eq 'probe-steady-48000-512' })[0]
$exportScenario = @($probeSection.scenarios | Where-Object { $_.id -eq 'probe-export-48000-512' })[0]
function Invoke-WireScenario([string]$Id, $Scenario, [hashtable]$Spec, [string]$CompareFile = '') {
    $env:FAKE_COMPARE_FILE = $CompareFile # 空串等于没设
    $c = New-WireContext $Id $Spec
    $o = Invoke-ReaperScenario -Ctx $c -Scenario $Scenario
    $env:FAKE_COMPARE_FILE = ''
    [pscustomobject]@{ Out = $o; Ctx = $c; Spec = $Spec; Cell = (Join-Path $c.Probe.RoundDir "probe-logs/$($Scenario.id)") }
}
# 这一格的结果目录里搬进来的日志名（收日志出错、目录没建起来时是空的，不让用例在这里抛错：要的是一条干净的红）
function Get-WireCellLogs([string]$Cell) { if (Test-Path -LiteralPath $Cell -PathType Container) { @(Get-ChildItem -LiteralPath $Cell -File -Filter 'probe-*.csv' | ForEach-Object { $_.Name } | Sort-Object) } else { @() } }
function New-WireSpecWith([string]$Id, [hashtable]$Set) { $s = New-WireSpec $Id; foreach ($k in $Set.get_Keys()) { $s[$k] = $Set[$k] }; $s }

# 稳态格：别人的同名文件不被碰（Save-ProbeLogs 把启动时刻、pid 交给收日志的那一步）
$plantSpec = New-WireSpec 'play-plant'
$plantSpec.Plant = {
    param($ownPid, $dir)
    $stale = Join-Path $dir "probe-O-$ownPid-5a1e.csv"; [IO.File]::WriteAllText($stale, "stale`n"); (Get-Item -LiteralPath $stale).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-1)
    $other = Join-Path $dir "probe-I-$($ownPid + 1)-0bad.csv"; [IO.File]::WriteAllText($other, "other-pid`n")
    $padded = Join-Path $dir "probe-I-0$ownPid-0bad.csv"; [IO.File]::WriteAllText($padded, "padded`n")
    $future = Join-Path $dir "probe-O-$ownPid-fa57.csv"; [IO.File]::WriteAllText($future, "future`n"); (Get-Item -LiteralPath $future).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(1)
}
$w = Invoke-WireScenario 'play-plant' $playScenario $plantSpec
$ownPid = $plantSpec.Seen[0].Pid
$left = @(Get-ChildItem -LiteralPath $plantSpec.LogDir -File | ForEach-Object { $_.Name } | Sort-Object)
$expectLeft = @("probe-O-$ownPid-5a1e.csv", "probe-I-$($ownPid + 1)-0bad.csv", "probe-I-0$ownPid-0bad.csv", "probe-O-$ownPid-fa57.csv") | Sort-Object
$moved = Get-WireCellLogs $w.Cell
Write-Case "稳态格走收日志：只搬本次启动的两份（$($moved -join ' ')），启动前就有的同号旧文件、别的 pid、补零 pid、将来时间的文件都留在原处" $true (
    $w.Out.Status -eq 'PASS' -and ($left -join '|') -ceq ($expectLeft -join '|') -and ($moved -join '|') -ceq (@("probe-I-$ownPid-a1b1.csv", "probe-O-$ownPid-c2d2.csv") -join '|') -and
    $w.Ctx.Probe.Moved -eq 2 -and $w.Ctx.Probe.Own.Count -eq 2 -and $w.Ctx.Probe.Problems.Count -eq 0)
Write-Case '稳态格：Ctx.Probe.Own 只有本次启动的两份（轮末前后清单比较时才会只除外这两份）' $true (($w.Ctx.Probe.Own -join '|') -ceq "probe-I-$ownPid-a1b1.csv|probe-O-$ownPid-c2d2.csv")
Write-Case "稳态格：任务里的 afterRenderSeconds 就是场景表的值（$($plantSpec.Seen[0].AfterRender)）" $false ([double]$plantSpec.Seen[0].AfterRender -eq [double]$probeSection.limits.afterRenderSeconds)
Write-Case '稳态格：好路径的结局 PASS，写出 Ping 4800 / D 4800 与驱动记下的停走带后留给 Ping 的时间' $false ($w.Out.Status -eq 'PASS' -and $w.Out.Detail -match 'Ping 4800 样本 / D 4800 样本' -and $w.Out.Detail -match 'stop_to_exit_s=6\.500')

# pid 复用的接线：同 pid 的 Cubase 日志（H 行 host_kind=cubase）与退出之后才新建的同 pid 日志都留在原处，真日志两份照常搬走；这一格判 FAIL 并写明原因
$cbSpec = New-WireSpec 'play-cubase'
$cbSpec.Plant = { param($ownPid, $dir) [IO.File]::WriteAllText((Join-Path $dir "probe-I-$ownPid-cb01.csv"), (New-LogForName "probe-I-$ownPid-cb01.csv" 'decoy' -HostKind cubase)) }
$w = Invoke-WireScenario 'play-cubase' $playScenario $cbSpec
$cbPid = $cbSpec.Seen[0].Pid
$cbFile = Join-Path $cbSpec.LogDir "probe-I-$cbPid-cb01.csv"
Write-Case '稳态格：同 pid 的 Cubase 日志（H 行 host_kind=cubase）留在原处没变，真日志两份照常搬走，这一格判 FAIL 并写明 host_kind，问题记进这一轮' $true (
    $w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*收日志出错*host_kind 是 cubase*' -and $w.Ctx.Probe.Moved -eq 2 -and $w.Ctx.Probe.Problems.Count -eq 1 -and
    (Test-Path -LiteralPath $cbFile) -and [IO.File]::ReadAllText($cbFile) -ceq (New-LogForName "probe-I-$cbPid-cb01.csv" 'decoy' -HostKind cubase) -and (Get-WireCellLogs $w.Cell).Count -eq 2)
$lateSpec = New-WireSpec 'play-late'
$lateSpec.AfterExit = {
    param($ownPid, $dir, $exitUtc)
    # 退出 3 s 之后才新建（pid 复用者的样子，比 2 s 余量晚）；H 行、名字、pid、修改时间都对得上。真等 3 s：收日志拿不到 Run 里的退出时刻、退回「取现在」时，
    # 这份文件的创建时间还在「现在 + 2 s」以内，会被误收，这条用例才分得出两者
    Start-Sleep -Seconds 3
    $f = Join-Path $dir "probe-O-$ownPid-1a7e.csv"; [IO.File]::WriteAllText($f, (New-LogForName "probe-O-$ownPid-1a7e.csv" 'late'))
}
$w = Invoke-WireScenario 'play-late' $playScenario $lateSpec
$latePid = $lateSpec.Seen[0].Pid
Write-Case '稳态格：这次启动的进程退出之后才新建的同 pid 日志（创建时间晚于退出）留在原处，真日志两份照常搬走，这一格判 FAIL 并写明创建时间' $true (
    $w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*收日志出错*创建时间晚于这次启动的 REAPER 退出*' -and $w.Ctx.Probe.Moved -eq 2 -and $w.Ctx.Probe.Problems.Count -eq 1 -and
    (Test-Path -LiteralPath (Join-Path $lateSpec.LogDir "probe-O-$latePid-1a7e.csv")) -and (Get-WireCellLogs $w.Cell).Count -eq 2)

# 进程自报的退出时刻是 1601 年（真机上偶尔如此）：真日志照常搬走，不能因为上界退到 1601 年把它们判成「退出之后才新建」
$w = Invoke-WireScenario 'play-badexit' $playScenario (New-WireSpecWith 'play-badexit' @{ BadExitTime = $true })
Write-Case '稳态格：进程自报的退出时刻是 1601 年（真机上偶尔如此）：真日志两份照常搬走，这一格 PASS，没有收日志的问题' $true (
    $w.Out.Status -eq 'PASS' -and $w.Ctx.Probe.Moved -eq 2 -and $w.Ctx.Probe.Problems.Count -eq 0 -and (Get-WireCellLogs $w.Cell).Count -eq 2)
$w = Invoke-WireScenario 'play-noping' $playScenario (New-WireSpecWith 'play-noping' @{ NoPing = @('play') })
Write-Case '稳态格：Ping 没出来判 FAIL（没有 Ping）' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*没有 Ping*')
$w = Invoke-WireScenario 'play-offline' $playScenario (New-WireSpecWith 'play-offline' @{ NonRt = @{ play = 1 } })
Write-Case '稳态格：实时播放时播放中的块 nonrt 为 1（宿主按离线调用）判 FAIL' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*播放中的块的 nonrt 是 [[]1[]]，应恰为 0*')

# 收日志出了问题（日志名其实是别处一个文件的另一个名字）：格判 FAIL，问题记进这一轮的 Problems，别处的那个文件不受影响
$precious = Join-Path $wire 'precious.txt'
[IO.File]::WriteAllText($precious, "precious`n")
$hardSpec = New-WireSpec 'play-hardlink'
$hardSpec.Plant = { param($ownPid, $dir) [IO.File]::WriteAllText($precious, "precious`n"); $null = New-Item -ItemType HardLink -Path (Join-Path $dir "probe-O-$ownPid-6767.csv") -Value $precious }.GetNewClosure() # 别处的文件此刻重写一遍：它的修改时间落在这次启动的窗口里
$hardOk = try { $null = New-Item -ItemType HardLink -Path (Join-Path $wire 'hardlink-probe.txt') -Value $precious; $true } catch { $false }
if ($hardOk) {
    $w = Invoke-WireScenario 'play-hardlink' $playScenario $hardSpec
    $hardPid = $hardSpec.Seen[0].Pid
    Write-Case '稳态格：日志名是别处文件的硬链接（有 2 个名字）：这一格判 FAIL，问题记进这一轮，硬链接和别处的文件都还在' $true (
        $w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*收日志出错*硬链接*' -and $w.Ctx.Probe.Problems.Count -eq 1 -and $w.Ctx.Probe.Moved -eq 2 -and
        (Test-Path -LiteralPath (Join-Path $hardSpec.LogDir "probe-O-$hardPid-6767.csv")) -and [IO.File]::ReadAllText($precious) -ceq "precious`n")
} else {
    Write-Case '稳态格：硬链接这一组（本机建不了硬链接）没能真跑' $true $true
}

# 导出格：两趟（离线 0、在线 2）各自要有 Ping、要有对的 nonrt；渲染速度写在工程里，经任务文件里的工程路径读回来核对
$w = Invoke-WireScenario 'export-good' $exportScenario (New-WireSpec 'export-good')
$speeds = @($w.Spec.Seen | ForEach-Object { "$($_.Leg)=$($_.RenderSpeed)" }) -join ' '
Write-Case "导出格：好路径 PASS，两趟都起了，离线那趟工程的渲染速度是 0、在线那趟是 2（$speeds）" $false (
    $w.Out.Status -eq 'PASS' -and $speeds -ceq 'offline=0 online=2' -and $w.Out.Detail -match 'export-offline' -and $w.Out.Detail -match 'export-rt' -and $w.Out.Detail -match 'exit_bound=window' -and
    (Test-Path -LiteralPath (Join-Path $w.Cell 'compare-reaper.csv')) -and (Get-WireCellLogs $w.Cell).Count -eq 4 -and $w.Ctx.Probe.Moved -eq 4)
$afterSeen = @($w.Spec.Seen | ForEach-Object { $_.AfterRender }) -join ' '
Write-Case "导出格：两趟任务里的 afterRenderSeconds 都是场景表的值（$afterSeen）" $false (
    @($w.Spec.Seen).Count -eq 2 -and @($w.Spec.Seen | Where-Object { [double]$_.AfterRender -ne [double]$probeSection.limits.afterRenderSeconds }).Count -eq 0)
$w = Invoke-WireScenario 'export-noping-off' $exportScenario (New-WireSpecWith 'export-noping-off' @{ NoPing = @('offline') })
Write-Case '导出格：离线那趟渲染后的 Ping 没出来判 FAIL（没有 Ping），不再放过去' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*export-offline：没有 Ping*')
$w = Invoke-WireScenario 'export-noping-on' $exportScenario (New-WireSpecWith 'export-noping-on' @{ NoPing = @('online') })
Write-Case '导出格：在线那趟渲染后的 Ping 没出来判 FAIL（没有 Ping），不再放过去' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*export-rt：没有 Ping*')
$w = Invoke-WireScenario 'export-badping' $exportScenario (New-WireSpecWith 'export-badping' @{ PingFor = @{ online = 4799 } })
Write-Case '导出格：在线那趟 Ping 求出的延迟不等于 D 判 FAIL' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*export-rt：*Ping 求出的延迟是 4799*')
$w = Invoke-WireScenario 'export-online-offlinemode' $exportScenario (New-WireSpecWith 'export-online-offlinemode' @{ NonRt = @{ online = 1 } })
Write-Case '导出格：在线那趟播放中的块 nonrt 是 1（宿主仍按离线调用，不是实时渲染）判 FAIL' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*export-rt：*播放中的块的 nonrt 是 [[]1[]]，应恰为 0*')
$w = Invoke-WireScenario 'export-offline-rtmode' $exportScenario (New-WireSpecWith 'export-offline-rtmode' @{ NonRt = @{ offline = 0 } })
Write-Case '导出格：离线那趟播放中的块 nonrt 是 0（宿主按实时调用）判 FAIL' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*export-offline：*播放中的块的 nonrt 是 [[]0[]]，应恰为 1*')
$w = Invoke-WireScenario 'export-nowav-off' $exportScenario (New-WireSpecWith 'export-nowav-off' @{ NoWav = @('offline') })
Write-Case '导出格：离线那趟没产出 wav 判 INCOMPLETE，不再起在线那趟' $true ($w.Out.Status -eq 'INCOMPLETE' -and $w.Out.Detail -like '*offline：offline 渲染没有产出 wav*' -and $w.Spec.Seen.Count -eq 1)
$w = Invoke-WireScenario 'export-nowav-on' $exportScenario (New-WireSpecWith 'export-nowav-on' @{ NoWav = @('online') })
Write-Case '导出格：在线那趟没产出 wav 判 INCOMPLETE' $true ($w.Out.Status -eq 'INCOMPLETE' -and $w.Out.Detail -like '*online：online 渲染没有产出 wav*' -and $w.Spec.Seen.Count -eq 2)
$w = Invoke-WireScenario 'export-diff' $exportScenario (New-WireSpec 'export-diff') $diffReport
Write-Case '导出格：离线与在线两份渲染有差异（nulltest 报逐位不同）判 FAIL，比对行照写' $true ($w.Out.Status -eq 'FAIL' -and $w.Out.Detail -like '*离线对在线对拍不通过*' -and (Test-Path -LiteralPath (Join-Path $w.Cell 'compare-reaper.csv')))

# 加载来源（接线）：替身 REAPER 报的模块列表走真正的场景处理器与核对。其余判据全绿时，来源对得上才 PASS；
# 别处的同名模块、没见到、读不到模块列表都得是 INCOMPLETE，并且说明里写着场景自己本来是 PASS
$srcGood = Invoke-WireScenario 'src-good' $playScenario (New-WireSpec 'src-good')
Write-Case '加载来源（稳态格）：模块都从私有目录加载时 PASS，证据写进 Facts.modules（Proven，每次启动一条，位置是 <cfg>\plugins\…）' $false (
    $srcGood.Out.Status -eq 'PASS' -and $srcGood.Out.Facts.modules.Count -eq $srcGood.Spec.Seen.Count -and $srcGood.Out.Facts.modules[0].verdict -ceq 'Proven' -and
    $srcGood.Out.Facts.modules[0].binaries[$wireLeaves[0]].loaded[0] -like '<cfg>\plugins\*')
$srcReason = [ordered]@{ foreign = '被测副本以外'; none = '没有在进程的模块列表里见到'; unreadable = '一次也没读到' }
foreach ($mode in $srcReason.get_Keys()) {
    $srcBad = Invoke-WireScenario "src-$mode" $playScenario (New-WireSpecWith "src-$mode" @{ Modules = $mode })
    Write-Case "加载来源（稳态格，模块列表：$mode）：场景自己本来是 PASS，来源核不上改判 INCOMPLETE，原因写明" $true (
        $srcBad.Out.Status -eq 'INCOMPLETE' -and $srcBad.Out.Detail -like '*插件加载来源没核实*场景自己的结局是 PASS*' -and $srcBad.Out.Detail -like "*$($srcReason[$mode])*")
}
$srcExport = Invoke-WireScenario 'src-export-foreign' $exportScenario (New-WireSpecWith 'src-export-foreign' @{ Modules = 'foreign' })
Write-Case '加载来源（导出格，离线与在线各启动一次）：每次启动都核对，别处的同名模块让整格 INCOMPLETE' $true (
    $srcExport.Out.Status -eq 'INCOMPLETE' -and $srcExport.Out.Facts.modules.Count -eq 2 -and $srcExport.Out.Facts.modules[0].verdict -ceq 'Foreign' -and $srcExport.Out.Facts.modules[1].verdict -ceq 'Foreign')
$srcExportGood = Invoke-WireScenario 'src-export-good' $exportScenario (New-WireSpec 'src-export-good')
Write-Case '加载来源（导出格）：两次启动的来源都对得上时 PASS，证据两条' $false ($srcExportGood.Out.Status -eq 'PASS' -and $srcExportGood.Out.Facts.modules.Count -eq 2)

# gui 锁被别人占着、等不到：REAPER 没启动，Run 里没有进程号。两种场景都判 INCOMPLETE，原因写锁，不落成「场景抛错」，也没有收日志
$holder2 = & $lockChild 'if (-not (Enter-RinneLock gui -TimeoutSeconds 5 -Quiet)) { exit 2 }; Start-Sleep -Seconds 60'
try {
    $until2 = (Get-Date).AddSeconds(10)
    while ((& $probeExit) -ne 1 -and (Get-Date) -lt $until2) { Start-Sleep -Milliseconds 200 }
    foreach ($sc in $playScenario, $exportScenario) {
        $lbSpec = New-WireSpec "lockbusy-$($sc.id)"
        $lbCtx = New-WireContext "lockbusy-$($sc.id)" $lbSpec
        $lbCtx.LockWaitSec = 1
        $lbOut = Invoke-ReaperScenario -Ctx $lbCtx -Scenario $sc
        Write-Case "$($sc.kind) 场景在 gui 锁等不到时：INCOMPLETE，原因写锁而不是「场景抛错」，REAPER 没有启动、没有收日志" $true (
            $lbOut.Status -eq 'INCOMPLETE' -and $lbOut.Detail -like '*gui 锁等不到*' -and $lbOut.Detail -notlike '*场景抛错*' -and $lbSpec.Seen.Count -eq 0 -and $lbCtx.Launches -eq 0 -and $lbCtx.Probe.Moved -eq 0)
    }
} finally { if (-not $holder2.HasExited) { $holder2.Kill(); $holder2.WaitForExit() } }

# ---- 整轮入口（Invoke-ReaperSmoke -Probe）：文档目录前后清单、停手、反例格不并进整轮分析 ----
$realReaperRunning = @(Get-ReaperProcessIds).Count -gt 0
if ($realReaperRunning) {
    Write-Case '整轮入口（第二段）：本机有 reaper.exe 在跑，预检会 SKIPPED，这一组接线用例没能真跑' $true $true
} else {
    $build = New-FakeProbeBuild -Root (Join-Path $wire 'probe-build')
    $results = Join-Path $wire 'results'
    function Invoke-WireSmoke([string]$Id, [hashtable]$Spec, [string]$Only, [int]$Repeat = 1, [switch]$NoNegative, [hashtable]$ExtraHooks = @{}, [string]$ScenarioOverride = '') {
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $Spec.LogDir 'sub')
        [IO.File]::WriteAllText((Join-Path $Spec.LogDir 'foreign-a.csv'), "foreign-a`n") # 别人的文件（用户在宿主里写的）
        [IO.File]::WriteAllText((Join-Path $Spec.LogDir 'notes.txt'), "notes`n")
        $hooks = @{ ProbeLogDir = $Spec.LogDir; Analyzer = $fakePy.Analyzer; Launcher = (New-FakeReaperLauncher $Spec); ModuleSampler = (New-WireModuleSampler $Spec (Join-Path $pinned.Dir 'plugins')) }
        foreach ($hookName in $ExtraHooks.get_Keys()) { $hooks[$hookName] = $ExtraHooks[$hookName] }
        $p = @{ CfgDir = $CfgA; ReaperExe = $pwshExe; BuildDir = $build.Root; NullTest = $fakeNull; LogDir = (Join-Path $results $Id); UserTestingMarker = $marker; ScenarioFile = ($ScenarioOverride ? $ScenarioOverride : $scenarioFile)
                Only = $Only; Repeat = $Repeat; Probe = $true; Python = $fakePy.Python; SelfTestHooks = $hooks }
        if ($NoNegative) { $p.NoNegative = $true }
        $env:FAKE_COMPARE_FILE = ''
        $all = @(Invoke-ReaperSmoke @p 6>&1) # 控制台上的 Write-Host 是信息流：留下来，核对里面没有别人的文件名
        $r = @($all | Where-Object { $_ -isnot [Management.Automation.InformationRecord] })[-1]
        $console = (@($all | Where-Object { $_ -is [Management.Automation.InformationRecord] } | ForEach-Object { "$($_.MessageData)" }) -join "`n")
        [pscustomobject]@{ R = $r; Results = (Join-Path $results $Id); Console = $console }
    }
    # 整轮用例跑到一半，本机冒出别人的 reaper.exe（预检整套 SKIPPED、没有 runs）：降级成一条占位用例，不在 runs[0] 上抛错。
    # 只认「SKIPPED 且原因是本机已有 reaper.exe」，别的原因照旧让用例红
    function Test-WireSkippedByRealReaper($WireResult) { $WireResult.R.verdict -eq 'SKIPPED' -and "$($WireResult.R.skipReasons)" -like '*本机已有 reaper.exe*' }
    # 整轮入口装插件时去掉被测插件在扫描缓存里的条目（探针的两个包名），别的条目原样
    $wireCache = Join-Path $CfgA 'reaper-vstplugins64.ini'
    $wireCacheKept = "[vstcache]`nOther.vst3=1112131415161718,2{CD,Other`n"
    [IO.File]::WriteAllBytes($wireCache, [Text.Encoding]::Latin1.GetBytes("[vstcache]`nRinne_Probe_Input.vst3=0102030405060708,1{AB,Rinne Probe Input`n" + "Other.vst3=1112131415161718,2{CD,Other`n" + "Rinne_Probe_Output.vst3=2122232425262728,3{EF,Rinne Probe Output`n"))
    $smoke1 = Invoke-WireSmoke 'clean' (New-WireSpec 'clean') '^probe-steady-48000-512$' -NoNegative
    $run1 = $smoke1.R.runs[0]
    Write-Case '整轮入口：被测插件在扫描缓存里的条目被去掉，别的条目原样，结果里记下去掉了 2 条' $true (
        $smoke1.R.scanCache.removedEntries -eq 2 -and $smoke1.R.scanCache.unkeyed -eq 0 -and [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($wireCache)) -ceq $wireCacheKept)
    Write-Case "整轮入口（第二段）：干净的一轮 PASS，文档目录除本轮自己的文件外没变，搬走 2 份，没留 reaper 进程（$($smoke1.R.verdict)）" $false (
        $smoke1.R.verdict -eq 'PASS' -and $run1.probe.docsListing.unchanged -and $run1.probe.moved -eq 2 -and @($run1.probe.notes).Count -eq 0 -and $run1.reaperProcessesLeft -eq 0 -and -not ($run1.PSObject.Properties.Name -contains 'stoppedBecause'))

    Write-Case '整轮入口：来源对得上时，场景行带 Facts.modules（位置是 <cfg>\plugins\…，没有盘符路径），结果里的插件清单仍是 file、sha256、copied' $false (
        $run1.scenarios[0].facts.modules[0].verdict -ceq 'Proven' -and $run1.scenarios[0].facts.modules[0].binaries.get_Count() -eq 2 -and (ConvertTo-Json $run1.scenarios[0].facts -Depth 8) -notmatch '[A-Za-z]:\\\\' -and
        (@($smoke1.R.plugins[0].get_Keys()) -join ',') -ceq 'file,sha256,copied')
    $smokeSrc = Invoke-WireSmoke 'src-foreign' (New-WireSpecWith 'src-foreign' @{ Modules = 'foreign' }) '^probe-steady-48000-512$' -NoNegative
    Write-Case "整轮入口：加载的是别处的同名模块时，场景与整轮都是 INCOMPLETE，不是 PASS（$($smokeSrc.R.verdict)）" $true (
        $smokeSrc.R.verdict -eq 'INCOMPLETE' -and $smokeSrc.R.runs[0].scenarios[0].status -eq 'INCOMPLETE' -and $smokeSrc.R.runs[0].scenarios[0].detail -like '*被测副本以外*' -and $smokeSrc.R.runs[0].scenarios[0].facts.modules[0].verdict -ceq 'Foreign')

    # 文档目录的前后清单落盘时，别人的文件名（文件、子目录）只留散列；比较用的是全部条目，所以清单里条目一个不少
    $roundDir1 = Join-Path $smoke1.Results $run1.probe.roundDir
    $listBefore1 = Get-Content -LiteralPath (Join-Path $roundDir1 'docs-listing-before.json') -Raw
    $listAfter1 = Get-Content -LiteralPath (Join-Path $roundDir1 'docs-listing-after.json') -Raw
    $hashed1 = @((ConvertFrom-Json $listBefore1).PSObject.Properties.Name | Where-Object { $_ -cmatch '^other-[0-9a-f]{8}$' })
    Write-Case "整轮入口（第二段）：落盘的 docs-listing-before / after.json 里没有别人的文件名（foreign-a.csv、notes.txt、子目录 sub），三个条目都只留散列（$($hashed1.Count) 个）" $true (
        $listBefore1 -cnotmatch 'foreign|notes|sub' -and $listAfter1 -cnotmatch 'foreign|notes|sub' -and $hashed1.Count -eq 3 -and
        @((ConvertFrom-Json $listAfter1).PSObject.Properties.Name | Where-Object { $_ -cmatch '^other-[0-9a-f]{8}$' }).Count -eq 3)
    Write-Case '整轮入口（第二段）：控制台输出与结果 json 里也没有别人的文件名' $true (
        $smoke1.Console -cnotmatch 'foreign|notes' -and (($smoke1.R | ConvertTo-Json -Depth 12) -cnotmatch 'foreign|notes\.txt'))

    # 别人的文件在这一轮里被动了（改、删、增）：场景全绿也判 FAIL，记下停手的原因，-Repeat 2 也只跑一轮
    $touch = [ordered]@{
        '改了' = { param($ownPid, $dir) [IO.File]::AppendAllText((Join-Path $dir 'foreign-a.csv'), "more`n") }
        '删了' = { param($ownPid, $dir) [IO.File]::Delete((Join-Path $dir 'foreign-a.csv')) }
        '多了一个' = { param($ownPid, $dir) [IO.File]::WriteAllText((Join-Path $dir 'foreign-new.csv'), "new`n") }
        '子目录没了' = { param($ownPid, $dir) [IO.Directory]::Delete((Join-Path $dir 'sub')) }
    }
    $k = 0
    foreach ($what in $touch.get_Keys()) {
        $k++
        $sp = New-WireSpec "touch$k"; $sp.DuringRun = $touch[$what]
        $sm = Invoke-WireSmoke "touch$k" $sp '^probe-steady-48000-512$' -Repeat 2 -NoNegative
        $row = $sm.R.runs[0]
        Write-Case "整轮入口（第二段）：别人的文件在这一轮里$what，场景全绿也判 FAIL，文档目录记为有变，停手，-Repeat 2 只跑了 $(@($sm.R.runs).Count) 轮" $true (
            $row.scenarios[0].status -eq 'PASS' -and $row.verdict -eq 'FAIL' -and $sm.R.verdict -eq 'FAIL' -and -not $row.probe.docsListing.unchanged -and $row.stoppedBecause -like '*探针日志目录*' -and @($sm.R.runs).Count -eq 1 -and $sm.R.launches -eq 1)
        # FAIL 的原因文字、控制台、结果 json 里只有变化类型加散列，没有别人的文件名；散列在这一轮落盘的清单里找得到
        $changedOne = @($row.probe.docsListing.changed)
        $roundDirT = Join-Path $sm.Results $row.probe.roundDir
        $keysT = @((ConvertFrom-Json (Get-Content -LiteralPath (Join-Path $roundDirT 'docs-listing-before.json') -Raw)).PSObject.Properties.Name)
        $keysAfterT = @((ConvertFrom-Json (Get-Content -LiteralPath (Join-Path $roundDirT 'docs-listing-after.json') -Raw)).PSObject.Properties.Name)
        $hashOfChange = ($changedOne.Count -eq 1 -and $changedOne[0] -cmatch '^(removed|changed|added):(other-[0-9a-f]{8})$') ? $Matches[2] : ''
        Write-Case "整轮入口（第二段）：别人的文件$what 判 FAIL 时，变化报告只写类型加散列（$($changedOne -join ' ')），控制台与结果 json 里没有文件名，散列在这一轮落盘的清单里找得到" $true (
            $hashOfChange -and ($hashOfChange -in $keysT -or $hashOfChange -in $keysAfterT) -and $sm.Console -cnotmatch 'foreign|notes' -and (($sm.R | ConvertTo-Json -Depth 12) -cnotmatch 'foreign|notes\.txt') -and $sm.Console -cmatch $hashOfChange)
        if ($k -eq 1) { $keysTouch1 = $keysT }
    }
    Write-Case '整轮入口（第二段）：盐每轮不同：两轮里同一个别人的文件（foreign-a.csv）的散列对不上' $true (
        @($keysTouch1 | Where-Object { $_ -cmatch '^other-' -and $_ -in (ConvertFrom-Json $listBefore1).PSObject.Properties.Name }).Count -eq 0)

    # 默认资源目录（这里是假的 AppData 下的那个）里有列不出来的子目录（拒绝访问）：前后都读不了，证明不了没被碰过，按可能被碰过处理：
    # 场景全绿也判 FAIL，停手，-Repeat 2 只跑一轮。以前列目录的错误被吞掉，这个子目录像不存在一样，这一轮会判 PASS
    $lockedDefault = Join-Path (Get-ReaperDefaultCfgDir) 'rn-locked'
    $null = New-Item -ItemType Directory -Force -Path $lockedDefault
    Set-Content -LiteralPath (Join-Path $lockedDefault 'x.ini') -Value x
    $sidU = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $lockedDefault /deny "*${sidU}:(RD)" 2>&1 | Out-Null
    try {
        $canList = try { $null = Get-ChildItem -LiteralPath $lockedDefault -Force -ErrorAction Stop; $true } catch { $false }
        if ($canList) { Write-Case '（本机没能造出读不了的目录：默认资源目录有读不了的子目录的整轮用例没能真跑）' $true $true }
        else {
            $smU = Invoke-WireSmoke 'unreadable-default' (New-WireSpec 'unreadable-default') '^probe-steady-48000-512$' -Repeat 2 -NoNegative
            if (Test-WireSkippedByRealReaper $smU) { Write-Case '（整轮预检时本机冒出了别人的 reaper.exe，整套 SKIPPED：默认资源目录有读不了的子目录的整轮用例没能真跑）' $true $true }
            else {
                $rowU = $smU.R.runs[0]
                Write-Case "整轮入口：默认资源目录里有读不了的子目录，场景全绿也判 FAIL，记为证明不了没变，停手，-Repeat 2 只跑了 $(@($smU.R.runs).Count) 轮" $true (
                    $rowU.scenarios[0].status -eq 'PASS' -and $rowU.verdict -eq 'FAIL' -and $smU.R.verdict -eq 'FAIL' -and -not $rowU.defaultCfgDir.unchanged -and $rowU.defaultCfgDir.Contains('unreadable') -and @($rowU.defaultCfgDir['unreadable']) -ceq 'rn-locked' -and
                    $rowU.stoppedBecause -like '*读不了*' -and $rowU.stoppedBecause -like '*可能被碰过*' -and @($smU.R.runs).Count -eq 1 -and $smU.R.launches -eq 1 -and $smU.Console -like '*读不了的子目录*')
            }
        }
    } finally { & icacls.exe $lockedDefault /remove:d "*$sidU" 2>&1 | Out-Null; Remove-Item -LiteralPath $lockedDefault -Recurse -Force -ErrorAction SilentlyContinue }

    # ---- 结束不了的进程（整轮）：第一个场景撞上 → 它 INCOMPLETE、后面的场景不再启动 REAPER、整轮 FAIL 并停手，-Repeat 2 只跑一轮，gui 锁放开 ----
    $shortCfg = Get-Content -LiteralPath $scenarioFile -Raw | ConvertFrom-Json
    $shortCfg.probe.limits.luaTimeoutSec = 2
    $shortScenarioFile = Join-Path $wire 'scenarios-short-timeout.json'
    [IO.File]::WriteAllText($shortScenarioFile, ($shortCfg | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    $stuckSeenW = @{ Pids = [Collections.Generic.List[int]]::new() }
    try {
        $smW = Invoke-WireSmoke 'stuck-round' (New-WireSpec 'stuck-round') '^probe-steady-48000-(512|1024)$' -Repeat 2 -NoNegative -ScenarioOverride $shortScenarioFile `
            -ExtraHooks @{ Launcher = (New-StuckStandIn 'Start-Sleep -Seconds 120' $stuckSeenW); Terminator = $denyKill }
        if (Test-WireSkippedByRealReaper $smW) { Write-Case '（整轮预检时本机冒出了别人的 reaper.exe，整套 SKIPPED：REAPER 结束不了的整轮用例没能真跑）' $true $true }
        else {
            $rowW = $smW.R.runs[0]
            Write-Case "整轮入口：REAPER 结束不了，第一个场景 INCOMPLETE 点名，后面的场景 INCOMPLETE 写本轮已停止，只启动过 $($stuckSeenW.Pids.Count) 个进程" $true (
                @($rowW.scenarios).Count -eq 2 -and $rowW.scenarios[0].status -eq 'INCOMPLETE' -and $rowW.scenarios[0].detail -like '*没能结束*' -and
                $rowW.scenarios[1].status -eq 'INCOMPLETE' -and $rowW.scenarios[1].detail -like '*本轮已停止*' -and $stuckSeenW.Pids.Count -eq 1 -and $smW.R.launches -eq 1)
            Write-Case '整轮入口：REAPER 结束不了，整轮判 FAIL（不论场景），记下原因，停手，-Repeat 2 只跑了一轮，没有别的 reaper.exe 残留，进程确实还活着' $true (
                $rowW.verdict -eq 'FAIL' -and $smW.R.verdict -eq 'FAIL' -and $rowW.Contains('haltedBecause') -and "$($rowW['haltedBecause'])" -like '*没能结束*' -and $rowW.stoppedBecause -like '*没能结束*' -and
                @($smW.R.runs).Count -eq 1 -and $rowW.reaperProcessesLeft -eq 0 -and -not (& $gone $stuckSeenW.Pids[0]) -and $smW.Console -like '*停止后续轮次*没能结束*')
            Write-Case '整轮入口：REAPER 结束不了，gui 锁照样放开；结果 json 里没有盘符路径' $true ((Test-RinneLockFree gui) -and (($smW.R | ConvertTo-Json -Depth 12) -cnotmatch '[A-Za-z]:\\\\'))
        }
    } finally { Stop-StuckStandIns $stuckSeenW }

    # 硬链接数一时取不到（OneDrive 正在同步、杀毒软件短暂独占……）：前一份清单是 1、后一份记 ?。列清单在 Suite 模块里换成替身，
    # 这一轮里「放行文件」出现之后，别人的 foreign-a.csv 的硬链接数就记成 ?
    $linkFlag = Join-Path $wire 'link-unknown.flag'
    $env:RINNE_SELFTEST_LINKFLAG = $linkFlag
    & (Get-Module Suite) {
        function script:Get-ReaperProbeLogListing {
            param([Parameter(Mandatory)][Alias('Dir')][string]$ListingDir)
            $l = ProbeLogs\Get-ReaperProbeLogListing -Dir $ListingDir
            $flagPath = $env:RINNE_SELFTEST_LINKFLAG
            if ($flagPath -and (Test-Path -LiteralPath $flagPath) -and $l.Contains('foreign-a.csv')) { $l['foreign-a.csv'] = $l['foreign-a.csv'] -replace '\|[0-9]+$', '|?' }
            $l
        }
    }
    try {
        Remove-Item -LiteralPath $linkFlag -ErrorAction SilentlyContinue
        $sp = New-WireSpec 'link-unknown'; $sp.DuringRun = { param($ownPid, $dir) [IO.File]::WriteAllText($linkFlag, '1') }.GetNewClosure()
        $smokeQ = Invoke-WireSmoke 'link-unknown' $sp '^probe-steady-48000-512$' -Repeat 2 -NoNegative
        $runQ = $smokeQ.R.runs[0]
        Write-Case "整轮入口（第二段）：别人的文件的硬链接数前一份是 1、后一份取不到（?）、其余相同：不判变，这一轮 PASS，-Repeat 2 照常跑完两轮（$(@($smokeQ.R.runs).Count) 轮），结果里记一笔「硬链接数没取到」" $false (
            $smokeQ.R.verdict -eq 'PASS' -and @($smokeQ.R.runs).Count -eq 2 -and $runQ.probe.docsListing.unchanged -and $runQ.probe.docsListing.Contains('linkCountUnknown') -and $runQ.probe.docsListing['linkCountUnknown'] -eq 1 -and
            -not ($runQ.PSObject.Properties.Name -contains 'stoppedBecause') -and @($runQ.probe.notes | Where-Object { $_ -like '*硬链接数没取到：1 项*' }).Count -eq 1)
        Write-Case '整轮入口（第二段）：硬链接数没取到的提示里和控制台上都只有散列，没有别人的文件名' $true (
            $smokeQ.Console -cmatch '有 1 项硬链接数没取到' -and $smokeQ.Console -cnotmatch 'foreign|notes' -and (($smokeQ.R | ConvertTo-Json -Depth 12) -cnotmatch 'foreign|notes\.txt') -and $runQ.probe.notes[0] -cmatch 'other-[0-9a-f]{8}')
        # 同样取不到，但文件被改了（大小变了）：照样判 FAIL 并停手
        Remove-Item -LiteralPath $linkFlag -ErrorAction SilentlyContinue
        $sp2 = New-WireSpec 'link-unknown-edit'; $sp2.DuringRun = { param($ownPid, $dir) [IO.File]::WriteAllText($linkFlag, '1'); [IO.File]::AppendAllText((Join-Path $dir 'foreign-a.csv'), "more`n") }.GetNewClosure()
        $smokeQ2 = Invoke-WireSmoke 'link-unknown-edit' $sp2 '^probe-steady-48000-512$' -Repeat 2 -NoNegative
        $runQ2 = $smokeQ2.R.runs[0]
        Write-Case '整轮入口（第二段）：硬链接数取不到，但别人的文件同时被改了：照样判 FAIL 并停手' $true (
            $smokeQ2.R.verdict -eq 'FAIL' -and -not $runQ2.probe.docsListing.unchanged -and $runQ2.stoppedBecause -like '*探针日志目录*' -and @($smokeQ2.R.runs).Count -eq 1)
    } finally {
        $env:RINNE_SELFTEST_LINKFLAG = ''
        & (Get-Module Suite) { Remove-Item -LiteralPath Function:\script:Get-ReaperProbeLogListing -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $linkFlag -ErrorAction SilentlyContinue
    }

    # 自己的文件因为收日志出了问题留在了日志目录里：场景判 FAIL，但这不算「别人的文件变了」（本轮自己的新文件才除外），文档目录记为没变
    if ($hardOk) {
        $sp = New-WireSpec 'kept-own'
        $sp.Plant = { param($ownPid, $dir) [IO.File]::WriteAllText($precious, "precious`n"); $null = New-Item -ItemType HardLink -Path (Join-Path $dir "probe-O-$ownPid-6767.csv") -Value $precious }.GetNewClosure() # 别处的文件此刻重写一遍：它的修改时间落在这次启动的窗口里
        $smokeK = Invoke-WireSmoke 'kept-own' $sp '^probe-steady-48000-512$' -NoNegative
        $runK = $smokeK.R.runs[0]
        Write-Case '整轮入口（第二段）：自己的日志因收日志出问题留在目录里：场景判 FAIL，文档目录除本轮自己的文件外没变（不把它当成别人的文件）' $true (
            $runK.scenarios[0].status -eq 'FAIL' -and $runK.verdict -eq 'FAIL' -and $runK.probe.docsListing.unchanged -and $runK.probe.problems.Count -eq 1 -and -not ($runK.PSObject.Properties.Name -contains 'stoppedBecause'))
    }

    # 反例格（故意不设组）的日志不并进整轮分析：all-logs 里只有正例那一格的两份日志
    $smoke5 = Invoke-WireSmoke 'excluded' (New-WireSpec 'excluded') '^(probe-steady-48000-512|neg-probe-unpaired)$'
    $run5 = $smoke5.R.runs[0]
    $roundDir5 = Join-Path $smoke5.Results $run5.probe.roundDir
    $allLogs = @(Get-ChildItem -LiteralPath (Join-Path $roundDir5 'all-logs') -File -Filter 'probe-*.csv' | ForEach-Object { $_.Name })
    $negCellLogs = @(Get-ChildItem -LiteralPath (Join-Path $roundDir5 'probe-logs/neg-probe-unpaired') -File -Filter 'probe-*.csv' | ForEach-Object { $_.Name })
    Write-Case "整轮入口（第二段）：反例格 neg-probe-unpaired 自己的两份日志留在它的格子里，不并进 all-logs（all-logs 里 $($allLogs.Count) 份），整轮分析的格与排除的格都照实记" $true (
        $run5.scenarios.Count -eq 2 -and $smoke5.R.verdict -eq 'PASS' -and $negCellLogs.Count -eq 2 -and $allLogs.Count -eq 2 -and @($allLogs | Where-Object { $_ -in $negCellLogs }).Count -eq 0 -and
        (@($run5.probe.analyzer.whole.cells) -join ',') -ceq 'probe-steady-48000-512' -and (@($run5.probe.analyzer.whole.excluded) -join ',') -ceq 'neg-probe-unpaired')
}
