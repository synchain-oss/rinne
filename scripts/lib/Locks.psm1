# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/gates.json
#
# 本机资源锁。全部是 Local\ 命名互斥体，由内核持有，持有进程被杀或崩溃时必然释放，下一个等待者以 AbandonedMutex
# 的形式接手。没有 owner 文件，也没有「等超时就抢别人的锁」这条路：等不到就返回 $null（Invoke-With* 抛错），
# 由调用方决定记 FAIL 还是资源超时，绝不无锁硬跑。
# Local\ 只在同一个登录会话里互斥：本机所有 agent 都在维护者的桌面会话里跑，所以够用；从 SSH、计划任务或另一个
# 用户会话里起的 gates 拿到的是另一组锁，不与这里互斥，不要那样跑。不设 Global\ 降级：已有的同名 Global\ 对象
# 拒绝当前 token 时退回 Local\，两个进程会各持一把不同的锁，以为有锁其实没有。
#   build-slot-1、build-slot-2  编译（同时 ≤ 2），Enter-RinneBuildSlot 取其中空着的一把
#   gui                         GUI pluginval 与无头浏览器冒烟（同时 ≤ 1）
#   merge                       合并（同时 ≤ 1）
#   git                         fetch、merge、worktree 这类改动共享 .git 的操作，一律经 Invoke-WithGitLock
#   ipc-tests                   用真实段名的跨进程测试
# 取锁顺序固定为 merge → 构建槽 → gui / ipc-tests → git，持着后面的锁时不再取前面的；git 锁只包住单条 git 操作、
# 里面不再取别的锁。按这个顺序不会互相等成环。
# 互斥体有线程归属：取与放要在同一线程上，在同一个 pwsh 进程里顺序调用即可。同一线程重复取同一把会重入成功，
# 所以互斥只在进程之间成立。内存只经 Get-Counter 读。
# 锁名前缀只能从模块内部改（自测用自己的一组名字，与真在跑的 gates 互不等待），没有环境变量或参数可以换掉它。
#
# 用法（导入一定带 -ErrorAction Stop：模块缺失时 Import-Module 默认只写一条非终止错误）：
#   Import-Module (Join-Path $PSScriptRoot '../lib/Locks.psm1') -ErrorAction Stop
#   Invoke-WithGitLock { git fetch --quiet origin feature/v1 }            # 默认最多等 600 秒
#   Invoke-WithRinneLock -Name merge -TimeoutSeconds 1800 { … }
#   $lock = Enter-RinneLock gui -TimeoutSeconds 60; if (-not $lock) { … }; try { … } finally { Exit-RinneLock $lock }
# 脚本块的输出原样返回；块里原生命令的非 0 退出码不会变成异常，由调用方看 $LASTEXITCODE。
# 等不到锁时 Invoke-With* 抛 TimeoutException、块不执行；锁名不认识、互斥体建不出来也都抛错。
# 上一个持有者被杀（AbandonedMutex）时照常接手并打印一行提示，这是有意的：内核已经把所有权交给了等待者，
# 持锁方被杀后要能马上接着用；git 自己的 *.lock 文件会让残留状态在下一条 git 命令上明确报错。

$script:Names = 'build-slot-1', 'build-slot-2', 'gui', 'merge', 'git', 'ipc-tests'
$script:Scope = 'Rinne'

function Get-RinneLockName {
    param([Parameter(Mandatory)][string]$Name)
    if ($Name -notin $script:Names) { throw "未知的锁 $Name（只有 $($script:Names -join ' / ')）" }
    "Local\$script:Scope-$Name"
}

function New-RinneLockHandle([string]$Full, $Mutex, [double]$Waited) {
    [pscustomobject]@{ Name = $Full; Mutex = $Mutex; Waited = $Waited }
}

function Write-Abandoned([string]$Full) {
    Write-Host "  [lock] 上一个持有者没放锁就退出了，已接手 $Full；它留下的进程与临时文件可能还在"
}

# 取一把锁；等满 TimeoutSeconds 仍拿不到返回 $null。返回的对象交给 Exit-RinneLock 释放。
function Enter-RinneLock {
    param([Parameter(Mandatory, Position = 0)][string]$Name, [double]$TimeoutSeconds = 1800, [switch]$Quiet)
    $full = Get-RinneLockName $Name
    $m = [System.Threading.Mutex]::new($false, $full)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try { $got = $m.WaitOne([TimeSpan]::FromSeconds($TimeoutSeconds)) }
    catch [System.Threading.AbandonedMutexException] { $got = $true; Write-Abandoned $full }
    if (-not $got) { $m.Dispose(); return $null }
    if (-not $Quiet -and $sw.Elapsed.TotalSeconds -ge 1) { Write-Host ('  [lock] {0} 等了 {1:N1} 秒' -f $full, $sw.Elapsed.TotalSeconds) }
    New-RinneLockHandle $full $m $sw.Elapsed.TotalSeconds
}

# 两把构建槽里哪把先空出来就取哪把。
function Enter-RinneBuildSlot {
    param([double]$TimeoutSeconds = 2700, [switch]$Quiet)
    $full = 'build-slot-1', 'build-slot-2' | ForEach-Object { Get-RinneLockName $_ }
    $ms = [System.Threading.Mutex[]]@($full | ForEach-Object { [System.Threading.Mutex]::new($false, $_) })
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try { $i = [System.Threading.WaitHandle]::WaitAny([System.Threading.WaitHandle[]]$ms, [TimeSpan]::FromSeconds($TimeoutSeconds)) }
    catch [System.Threading.AbandonedMutexException] { $i = $_.Exception.MutexIndex; Write-Abandoned $full[$i] }
    for ($k = 0; $k -lt $ms.Count; $k++) { if ($k -ne $i) { $ms[$k].Dispose() } }
    if ($i -eq [System.Threading.WaitHandle]::WaitTimeout) { return $null }
    if (-not $Quiet -and $sw.Elapsed.TotalSeconds -ge 1) { Write-Host ('  [lock] {0} 等了 {1:N1} 秒' -f $full[$i], $sw.Elapsed.TotalSeconds) }
    New-RinneLockHandle $full[$i] $ms[$i] $sw.Elapsed.TotalSeconds
}

function Exit-RinneLock {
    param([Parameter(Position = 0)]$Lock)
    if ($null -eq $Lock) { return }
    try { $Lock.Mutex.ReleaseMutex() } catch { Write-Host "  [lock] 释放 $($Lock.Name) 时出错：$($_.Exception.Message)" }
    $Lock.Mutex.Dispose()
}

# 持锁执行脚本块，结束（含抛错、Ctrl-C）时一定释放；等不到锁时抛 TimeoutException，块不执行。
function Invoke-WithRinneLock {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][scriptblock]$ScriptBlock, [double]$TimeoutSeconds = 1800)
    $lock = Enter-RinneLock $Name -TimeoutSeconds $TimeoutSeconds
    if (-not $lock) { throw [System.TimeoutException]::new("等 $(Get-RinneLockName $Name) 超过 $TimeoutSeconds 秒") }
    try { & $ScriptBlock } finally { Exit-RinneLock $lock }
}

function Invoke-WithGitLock {
    param([Parameter(Mandatory, Position = 0)][scriptblock]$ScriptBlock, [double]$TimeoutSeconds = 600)
    Invoke-WithRinneLock -Name git -ScriptBlock $ScriptBlock -TimeoutSeconds $TimeoutSeconds
}

function Get-RinneAvailableMB {
    [int](Get-Counter -Counter '\Memory\Available MBytes' -ErrorAction Stop).CounterSamples[0].CookedValue
}

# 编译并发：clamp(floor((可用 MB − 4096) / 1500), 2, 16)。
function Get-RinneJobs {
    param([Parameter(Mandatory)][double]$AvailMB)
    [int][Math]::Clamp([Math]::Floor(($AvailMB - 4096) / 1500), 2, 16)
}

# 等可用内存 ≥ MinMB；等满 TimeoutSeconds 仍不够时 Ok = $false、Jobs = 2（降为 -j2 继续编，不判负）。
function Wait-RinneMemory {
    param([int]$MinMB = 6000, [double]$TimeoutSeconds = 900, [double]$PollSeconds = 15)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ($true) {
        $avail = Get-RinneAvailableMB
        if ($avail -ge $MinMB) { return [pscustomobject]@{ Ok = $true; AvailMB = $avail; Jobs = (Get-RinneJobs $avail) } }
        if ((Get-Date) -ge $deadline) { return [pscustomobject]@{ Ok = $false; AvailMB = $avail; Jobs = 2 } }
        Start-Sleep -Seconds ([Math]::Min($PollSeconds, [Math]::Max(1, ($deadline - (Get-Date)).TotalSeconds)))
    }
}

# 起一个 pwsh 子进程，导入本模块、换成与本进程相同的锁名前缀后执行 Command；返回 Process。子进程最多打印一两行，
# 输出接走不读。
function Start-LockChild([string]$Command) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    foreach ($a in '-NoProfile', '-NonInteractive', '-Command', "Import-Module '$PSCommandPath'; & (Get-Module Locks) { `$script:Scope = '$script:Scope' }; $Command") { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    [System.Diagnostics.Process]::Start($psi)
}

# 自测：返回 { Label; Bad; Passed } 列表（Bad 表示这是一个必须拦住的反例）。用自测专用的锁名前缀。
function Test-RinneLocks {
    $saved = $script:Scope
    $script:Scope = "Rinne-selftest-$PID"
    $out = [System.Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $out.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $exitOf = { param($Command) $p = Start-LockChild $Command; $p.WaitForExit(); $p.ExitCode }
    $probe = { param($n) & $exitOf "if (Enter-RinneLock $n -TimeoutSeconds 0 -Quiet) { exit 0 } else { exit 1 }" }
    $ready = Join-Path ([IO.Path]::GetTempPath()) "rinne-lock-$PID-$(Get-Random).ready"
    $child = $null
    try {
        & $add '未知的锁名被拒绝' $true $(try { $null = Get-RinneLockName 'nope'; $false } catch { $true })
        & $add '编译并发按可用内存折算，夹在 2–16' $false ((@(0, 7000, 8596, 40000) | ForEach-Object { Get-RinneJobs $_ }) -join ',' -eq '2,2,3,16')
        $mem = Wait-RinneMemory -MinMB ([int]::MaxValue) -TimeoutSeconds 0
        & $add '可用内存等不到时降为 -j2' $true ((-not $mem.Ok) -and $mem.Jobs -eq 2 -and $mem.AvailMB -gt 0)
        $child = Start-LockChild ("`$h = 'build-slot-1', 'build-slot-2', 'gui' | ForEach-Object { Enter-RinneLock `$_ -TimeoutSeconds 5 -Quiet }; " +
            "if (@(`$h | Where-Object { `$_ }).Count -ne 3) { exit 2 }; Set-Content -LiteralPath '$ready' x; Start-Sleep 120")
        $deadline = (Get-Date).AddSeconds(30)
        while (-not (Test-Path -LiteralPath $ready) -and -not $child.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
        & $add '子进程取得两把构建槽与 gui' $false (Test-Path -LiteralPath $ready)
        $s = Enter-RinneBuildSlot -TimeoutSeconds 0 -Quiet
        & $add '两把构建槽都被占时取不到第三把' $true ($null -eq $s)
        Exit-RinneLock $s
        $g = Enter-RinneLock gui -TimeoutSeconds 0 -Quiet
        & $add 'gui 被别的进程占着时取不到' $true ($null -eq $g)
        Exit-RinneLock $g
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $child.Kill()
        $g = Enter-RinneLock gui -TimeoutSeconds 1 -Quiet 6>$null
        $ms = $sw.ElapsedMilliseconds
        & $add "持锁的子进程被杀后 1 s 内接手 gui（$ms ms）" $false (($null -ne $g) -and $ms -le 1000)
        Exit-RinneLock $g
        $s = Enter-RinneBuildSlot -TimeoutSeconds 1 -Quiet 6>$null
        & $add '持锁的子进程被杀后构建槽空出来' $false ($s.Name -like '*-build-slot-[12]')
        $other = ($s.Name -like '*-1') ? '2' : '1'
        & $add '一把构建槽被占时，另一个进程取到的是另一把' $false ((& $exitOf "`$h = Enter-RinneBuildSlot -TimeoutSeconds 0 -Quiet; exit [int](-not (`$h.Name -like '*-build-slot-$other'))") -eq 0)
        Exit-RinneLock $s
        & $add 'Invoke-WithGitLock 块内别的进程取不到 git 锁' $true ((Invoke-WithGitLock { & $probe git }) -eq 1)
        & $add 'Invoke-WithGitLock 结束后 git 锁已释放' $false ((& $probe git) -eq 0)
        $thrown = try { Invoke-WithRinneLock -Name merge -ScriptBlock { throw 'boom' }; $false } catch { $_.Exception.Message -eq 'boom' }
        & $add '块内抛错时照样释放 merge 锁' $false ($thrown -and (& $probe merge) -eq 0)
    } finally {
        if ($child -and -not $child.HasExited) { $child.Kill() }
        Remove-Item -LiteralPath $ready -ErrorAction SilentlyContinue
        $script:Scope = $saved
    }
    $out
}

Export-ModuleMember -Function Get-RinneLockName, Enter-RinneLock, Enter-RinneBuildSlot, Exit-RinneLock, Invoke-WithRinneLock,
    Invoke-WithGitLock, Get-RinneAvailableMB, Get-RinneJobs, Wait-RinneMemory, Test-RinneLocks
