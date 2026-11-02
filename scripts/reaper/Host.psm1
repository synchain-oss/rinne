# SPDX-License-Identifier: GPL-3.0-or-later
#
# 启动 REAPER 并盯住它：全仓唯一拼 -cfgfile 的地方。
#   Invoke-ReaperHost      起一个 REAPER，等它自己退出；超时、到点、模态框卡死时按它自己的 PID 结束
#   Get-ReaperModalVerdict 一个窗口怎么处理（纯函数，自测直接用）
#   Resolve-ReaperExitedUtc 进程退出的时刻：进程自己报的不可信时（真机上偶尔读出 1601 年）换成检测到退出的当下（纯函数，自测直接用）
#   Get-ReaperModuleVerdict 被测的 .vst3 模块是不是都从私有目录里这次复制进去的那份加载的（纯函数，自测直接用）：模块路径与 sha256 都要对上
# 规则：只看、只点属于被启动的那个 PID 的窗口（用户自己的 REAPER 若开着，它的对话框不会被碰）；唯一自动点的是
# 「REAPER Query」（存盘提示，点「否」）；渲染进度窗放行；其余可见对话框持续 0.5 s 以上就当卡死，关窗、结束进程，
# 状态为 Modal，由调用方判 INCOMPLETE。进程只按自己启动时拿到的 PID 结束。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Guard.psm1') -ErrorAction Stop

if (-not ('RinneWin' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class RinneWin
{
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32")] static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
    [DllImport("user32")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32")] static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);

    // 属于 pid 的顶层窗口，每行：句柄 TAB 可见(0/1) TAB 类名 TAB 标题
    public static string[] Windows(uint pid)
    {
        var rows = new List<string>();
        EnumWindows((h, l) =>
        {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner != pid) return true;
            var c = new StringBuilder(256);
            var t = new StringBuilder(512);
            GetClassName(h, c, 256);
            GetWindowText(h, t, 512);
            rows.Add(h.ToInt64() + "\t" + (IsWindowVisible(h) ? 1 : 0) + "\t" + c + "\t" + t);
            return true;
        }, IntPtr.Zero);
        return rows.ToArray();
    }

    public static bool Press(long dialog, int ctrlId)
    {
        IntPtr hit = IntPtr.Zero;
        EnumChildWindows(new IntPtr(dialog), (h, l) => { if (GetDlgCtrlID(h) == ctrlId) hit = h; return true; }, IntPtr.Zero);
        if (hit == IntPtr.Zero) return false;
        PostMessage(hit, 0xF5, IntPtr.Zero, IntPtr.Zero); // BM_CLICK
        return true;
    }

    public static void Close(long window) { PostMessage(new IntPtr(window), 0x10, IntPtr.Zero, IntPtr.Zero); } // WM_CLOSE
}
'@
}

$script:IdNo = 7

# 已知的会卡死的对话框：标题 → 名字。名字进结果 json；不认识的写 unknown-dialog，标题打码后另记。
$script:KnownModals = [ordered]@{
    'Project Load Warning'            = 'project-load-warning'
    'REAPER Error'                    = 'reaper-error'
    'Select files to save or delete'  = 'record-save-prompt'
    'REAPER'                          = 'no-audio-device'
}

# 窗口怎么处理：Ignore（不是对话框 / 不可见）、PressNo（存盘提示）、Benign（渲染、插件扫描、峰值文件进度窗，会自己消失）、Fatal（卡死，Name 是名字）。
function Get-ReaperModalVerdict {
    param([string]$Class, [bool]$Visible, [string]$Title)
    if (-not $Visible -or $Class -cne '#32770') { return [pscustomobject]@{ Action = 'Ignore'; Name = '' } }
    if ($Title -ceq 'REAPER Query') { return [pscustomobject]@{ Action = 'PressNo'; Name = 'save-prompt' } }
    if ($Title -imatch '^Render') { return [pscustomobject]@{ Action = 'Benign'; Name = 'render-progress' } }
    if ($Title -imatch '^Scanning') { return [pscustomobject]@{ Action = 'Benign'; Name = 'plugin-scan-progress' } }
    if ($Title -imatch '^Building peaks') { return [pscustomobject]@{ Action = 'Benign'; Name = 'peaks-progress' } }
    # 评估许可的提示窗（标题 About REAPER …）：不自动点掉，判卡死并点名，由使用者去买许可证
    if ($Title -imatch '^About REAPER') { return [pscustomobject]@{ Action = 'Fatal'; Name = 'license-nag' } }
    $name = $script:KnownModals.Contains($Title) ? $script:KnownModals[$Title] : 'unknown-dialog'
    [pscustomobject]@{ Action = 'Fatal'; Name = $name }
}

# 标题里可能带工程路径：从盘符 / UNC 起一直换成 <path>，到下一个引号或标题末尾为止，再截 40 个字符。
# 路径里常有空格（C:\Work\John Smith\a.rpp），不能在第一个空格处停：留下的半截目录名与文件名会进结果文件。
# 路径后面若正好是 REAPER 自己的标题后缀（" - REAPER"，可带版本号），并且在标题最末，后缀留着；认不出的结尾（后缀不在末尾、后面不是版本号）
# 一起打码，宁可多打。
function ConvertTo-ReaperSafeTitle([string]$Title) {
    $tail = [regex]::Match($Title, ' - REAPER(?: v[0-9][0-9A-Za-z.+_-]*(?:/[A-Za-z0-9_]+)?)?$')
    $body = $tail.Success ? $Title.Substring(0, $tail.Index) : $Title
    $t = ($body -replace '(?:[A-Za-z]:[\\/]|\\\\)[^"]*', '<path>') + ($tail.Success ? $tail.Value : '')
    $t.Length -gt 40 ? $t.Substring(0, 40) : $t
}

# 进程的句柄数与内存。进程刚退出时只认这一种情况，返回 Exited = $true（调用方照实写「进程在采样前退出」）：
# 计数用 get_ 方法读：进程已退出时 .NET 抛 InvalidOperationException（PowerShell 包成 MethodInvocationException），在这里捕获。
# 不用属性读：属性的 getter 抛错会被 PowerShell 吞成空值（句柄数 $null、内存 0），差值就成了「没增长」。
# 只有异常（或它的内层）是 InvalidOperationException、并且进程确实已退出，才算这一种；别的异常照常抛出。
function Get-ReaperProcessSample([System.Diagnostics.Process]$Process) {
    try {
        $Process.Refresh()
        $h = $Process.get_HandleCount(); $pm = $Process.get_PrivateMemorySize64(); $ws = $Process.get_WorkingSet64()
    } catch {
        $ex = $_.Exception
        $exitedNow = ($ex -is [InvalidOperationException] -or $ex.InnerException -is [InvalidOperationException]) -and ($Process.HasExited -eq $true)
        if (-not $exitedNow) { throw }
        return [pscustomobject]@{ Exited = $true; Handles = $null; PrivateMB = $null; WorkingMB = $null }
    }
    [pscustomobject]@{ Exited = $false; Handles = $h; PrivateMB = [math]::Round($pm / 1MB, 1); WorkingMB = [math]::Round($ws / 1MB, 1) }
}

# 纯函数：几处标记的进程计数齐不齐。齐了返回 $null；有标记因为进程在采样前退出而没取到，原因照实写；别的缺失另写。
function Get-ReaperMarkProblem {
    param($Samples, [string[]]$LostAt = @(), [Parameter(Mandatory)][string[]]$Need)
    $missing = @($Need | Where-Object { -not ($Samples -and $Samples.Contains($_)) })
    if (-not $missing.Count) { return $null }
    $lostHere = @($missing | Where-Object { $_ -in $LostAt })
    if ($lostHere.Count) { return "进程在采样前退出（$($lostHere -join '、') 处的句柄与内存没取到）" }
    "没取到 $($missing -join '、') 处的进程计数"
}

# 进程退出的时刻。Process.ExitTime 在真机上偶尔读出 FILETIME 0（换算成 UTC 就是 1601-01-01），读到的值早于这次启动，不可能是真的；
# 这样的值直接当上界会把这次启动自己写的日志全判成「退出之后才新建」。所以进程自己报的（Reported）只有落在 [NotBeforeUtc（启动前一刻）,
# NoticedUtc（检测到它退出的当下）+ 1 s] 里才采信（上界留的 1 s 是两个时钟源的粗细差），否则取 NoticedUtc：检测在退出之后的几百毫秒内，
# 只会把上界放宽一点点，不会误拒真日志。参数都按 UTC。
function Resolve-ReaperExitedUtc {
    param([Nullable[datetime]]$Reported, [Parameter(Mandatory)][datetime]$NotBeforeUtc, [Parameter(Mandatory)][datetime]$NoticedUtc)
    if ($null -ne $Reported -and $Reported -ge $NotBeforeUtc -and $Reported -le $NoticedUtc.AddSeconds(1)) { return [datetime]$Reported }
    $NoticedUtc
}

# 进程里此刻加载着的模块的完整路径；此刻读不到（进程在启动或退出中、已退出、拒绝访问）返回 $null，不抛错。
function Get-ReaperProcessModulePaths($Process) {
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return $null }
        $names = @($Process.Modules | ForEach-Object { $_.FileName } | Where-Object { $_ })
    } catch { return $null }
    , $names
}

# 两个路径是不是同一个文件：完整路径相同（不分大小写），或文件身份（卷序列号 + 文件 ID）相同（硬链接、subst 盘符、junction 绕过的写法）。
# 身份读不到的不算「同一个」。
function Test-ReaperSameFile([string]$A, [string]$B) {
    if (-not $A -or -not $B) { return $false }
    if ([string]::Equals([IO.Path]::GetFullPath($A), [IO.Path]::GetFullPath($B), [StringComparison]::OrdinalIgnoreCase)) { return $true }
    $idA = Get-ReaperFileIdentity $A; $idB = Get-ReaperFileIdentity $B
    [bool]($idA -and $idB -and -not $idA.StartsWith('ERROR:') -and $idA -ceq $idB)
}

function Get-ReaperFileSha256([string]$Path) {
    try { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() } catch { $null }
}

# 纯函数：被测的 .vst3 模块是不是都从私有目录里这次复制进去的那份加载的，别处没有加载过同名的。
# REAPER 对同名的 VST3 包只留扫描顺序里最后的那个；系统目录里装着同名同类 ID 的发布版时，结果里的 sha256 只证明复制了哪份文件，
# 不证明 REAPER 加载了哪份，所以要读进程实际加载的模块来核对。
# Expected：模块文件名 → @{ Path（复制进私有目录的那一份）; Sha256（复制时算的，完整小写十六进制） }。Seen：模块文件名 → 进程里见到加载过的完整路径
# （各种写法都行）。Scans：成功读到模块列表的次数。SameFile（比较两个路径是不是同一个文件）与 HashOf（算一个文件的 sha256，读不出返回 $null）只给自测注入。
# 每个模块一个状态：Private（见到过，见到的都是那一份，并且那份文件现在的 sha256 与复制时相同）、Foreign（见到过别处的同名模块，不论那一份有没有见到）、
# Changed（路径对，但文件现在的 sha256 与复制时不同，或读不出来）、Unseen（没见到）。整体：没有被测副本的清单是 NoExpected；Scans 为 0 是 Unreadable
# （读不到模块列表，什么都证明不了）；其余按 Foreign、Changed、Unseen 的顺序取最先成立的，全是 Private 才是 Proven。
# 只有 Proven 才能往下按场景自己的判据判；别的整体结论一律没能证明是被测副本，调用方判 INCOMPLETE。返回 @{ Verdict; Binaries（模块文件名 → @{ State; Seen }）; Reasons }。
function Get-ReaperModuleVerdict {
    param([Parameter(Mandatory)][hashtable]$Expected, [hashtable]$Seen = @{}, [int]$Scans = 0, [scriptblock]$SameFile = $null, [scriptblock]$HashOf = $null)
    $same = $SameFile ?? ${function:Test-ReaperSameFile}
    $hashFile = $HashOf ?? ${function:Get-ReaperFileSha256}
    $rows = [ordered]@{}
    $reasons = [Collections.Generic.List[string]]::new()
    foreach ($leaf in @($Expected.get_Keys() | Sort-Object)) {
        $want = $Expected[$leaf]
        $paths = @($Seen.ContainsKey($leaf) ? $Seen[$leaf] : @())
        $foreign = @($paths | Where-Object { -not (& $same $_ $want.Path) })
        $state = 'Private'
        if ($foreign.Count) { $state = 'Foreign'; $reasons.Add("$leaf 从被测副本以外的位置加载过") }
        elseif ($paths.Count -eq 0) { $state = 'Unseen'; $reasons.Add("$leaf 没有在进程的模块列表里见到") }
        else {
            $now = & $hashFile $want.Path
            if (-not $now -or [string]$now -cne [string]$want.Sha256) { $state = 'Changed'; $reasons.Add("$leaf 路径对，但文件现在的 sha256 与这次复制进去时的不同（或读不出来）") }
        }
        $rows[$leaf] = [pscustomobject]@{ State = $state; Seen = $paths }
    }
    $states = @($rows.get_Values() | ForEach-Object { $_.State })
    $verdict = 'Proven'
    foreach ($candidate in 'Foreign', 'Changed', 'Unseen') { if ($candidate -in $states) { $verdict = $candidate; break } }
    if ($Expected.get_Count() -eq 0) { $verdict = 'NoExpected'; $reasons.Add('没有被测副本的清单，没法核对加载来源') }
    elseif ($Scans -le 0) { $verdict = 'Unreadable'; $reasons.Insert(0, '一次也没读到进程的模块列表（进程在启动或退出中、被拒绝访问……），没法证明加载的是被测副本') }
    [pscustomobject]@{ Verdict = $verdict; Binaries = $rows; Reasons = $reasons.ToArray() }
}

# 起 REAPER 并等它退出。Status：Exited / Timeout（到 TimeoutSec）/ Killed（到 KillAfterSec，反例用）/ Modal（卡死的对话框）。
# Pid、StartedUtc（启动前一刻）与 ExitedUtc（进程退出的时刻）给第二段收探针日志用；SampleLost 是想取进程计数时进程已经退出的那些标记。
# WatchFiles：标签 → 路径，文件一出现就记一次进程的句柄与内存（Lua 用它标出循环的开头与结尾）。
# WatchModules：模块文件名（如 Rinne Input.vst3）。每次轮询读一次进程加载的模块，名字在这里面的记下完整路径（Modules：文件名 → 见到过的路径；
# ModuleScans：读到模块列表的次数；结束或超时前再读一次）。读不到的那次不计，不抛错。
# StillRunning：到点要结束进程（Timeout / Killed / Modal）时，结束之后 5 秒进程还没退出，它可能还活着，调用方不得再启动新的 REAPER；KillError：结束动作
# 自己报的错（没报错是 $null）。结束动作报错但进程已经没了不算 StillRunning。
# Launcher 只给自测注入：收到准备好的 ProcessStartInfo，返回已启动的 Process。Sampler、ModuleSampler 也只给自测注入：代替 Get-ReaperProcessSample、
# Get-ReaperProcessModulePaths。Terminator 同样只给自测注入：收进程号，代替 Stop-Process 去结束进程（用来造「杀不掉」）。
function Invoke-ReaperHost {
    param(
        [Parameter(Mandatory)][string]$ReaperExe,
        [string[]]$ReaperArgs = @(),
        [hashtable]$Environment = @{},
        [string]$WorkDir = '',
        [double]$TimeoutSec = 90,
        [double]$KillAfterSec = 0,
        [hashtable]$WatchFiles = @{},
        [string[]]$WatchModules = @(),
        [scriptblock]$Launcher = $null,
        [scriptblock]$Sampler = $null,
        [scriptblock]$ModuleSampler = $null,
        [scriptblock]$Terminator = $null
    )
    $cfgArgs = @('-nosplash', '-newinst', '-cfgfile', (Get-ReaperCfgArgument))
    $psi = [Diagnostics.ProcessStartInfo]::new($ReaperExe)
    $psi.Arguments = (($cfgArgs + $ReaperArgs) -join ' ')
    $psi.UseShellExecute = $false
    if ($WorkDir) { $psi.WorkingDirectory = $WorkDir }
    foreach ($k in $Environment.get_Keys()) { $psi.Environment[[string]$k] = [string]$Environment[$k] }
    $startedUtc = [DateTime]::UtcNow # 启动之前取：这次启动的进程写下的文件，修改时间都不早于它
    $proc = $Launcher ? (& $Launcher $psi) : [Diagnostics.Process]::Start($psi)
    $ownPid = $proc.Id
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $status = $null
    $modal = $null
    $dismissed = [Collections.Generic.List[string]]::new()
    $titles = [Collections.Generic.SortedSet[string]]::new()
    $marks = [ordered]@{}
    $lost = [Collections.Generic.List[string]]::new()
    $firstSeen = @{}
    $modSeen = @{}
    foreach ($leaf in $WatchModules) { $modSeen[$leaf] = [Collections.Generic.SortedSet[string]]::new([StringComparer]::OrdinalIgnoreCase) }
    $modScans = 0
    # 读一次进程加载的模块：名字在 WatchModules 里的记下完整路径。返回 1 表示读到了模块列表，0 表示此刻读不到
    $sampleModules = {
        $paths = $ModuleSampler ? (& $ModuleSampler $proc) : (Get-ReaperProcessModulePaths $proc)
        if ($null -eq $paths) { return 0 }
        foreach ($modPath in $paths) {
            $leaf = [IO.Path]::GetFileName($modPath)
            foreach ($w in $WatchModules) { if ([string]::Equals($leaf, $w, [StringComparison]::OrdinalIgnoreCase)) { $null = $modSeen[$w].Add($modPath) } }
        }
        1
    }
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 250
        $now = $sw.Elapsed.TotalSeconds
        if ($KillAfterSec -gt 0 -and $now -ge $KillAfterSec) { $status = 'Killed'; break }
        if ($now -ge $TimeoutSec) { $status = 'Timeout'; break }
        if ($WatchModules.Count) { $modScans += & $sampleModules }
        foreach ($row in [RinneWin]::Windows([uint32]$ownPid)) {
            $f = $row -split "`t", 4
            $verdict = Get-ReaperModalVerdict -Class $f[2] -Visible ($f[1] -eq '1') -Title $f[3]
            if ($verdict.Action -eq 'Ignore') { continue }
            $null = $titles.Add($verdict.Name + ($verdict.Name -eq 'unknown-dialog' ? ':' + (ConvertTo-ReaperSafeTitle $f[3]) : ''))
            $h = [long]$f[0]
            if ($verdict.Action -eq 'PressNo') {
                if ([RinneWin]::Press($h, $script:IdNo)) { $dismissed.Add($verdict.Name) }
            } elseif ($verdict.Action -eq 'Fatal') {
                if (-not $firstSeen.ContainsKey($h)) { $firstSeen[$h] = $now }
                elseif ($now - $firstSeen[$h] -ge 0.5) { $modal = $verdict.Name; [RinneWin]::Close($h); $status = 'Modal'; break }
            }
        }
        if ($status) { break }
        foreach ($label in @($WatchFiles.get_Keys())) {
            if ($marks.Contains($label) -or $label -in $lost -or -not (Test-Path -LiteralPath $WatchFiles[$label])) { continue }
            $sample = $Sampler ? (& $Sampler $proc) : (Get-ReaperProcessSample $proc)
            if ($sample.Exited) { $lost.Add($label) } else { $marks[$label] = $sample }
        }
    }
    if ($WatchModules.Count) { $modScans += & $sampleModules } # 结束或杀掉之前再读一次（进程已退出时读不到，不计）
    if (-not $status) {
        # 进程退出得比下一次轮询还快：标记文件已经出现、却没来得及取样，同样是「进程在采样前退出」，不混进「没取到」
        foreach ($label in @($WatchFiles.get_Keys())) {
            if (-not $marks.Contains($label) -and $label -notin $lost -and (Test-Path -LiteralPath $WatchFiles[$label])) { $lost.Add($label) }
        }
    }
    $stillRunning = $false
    $killError = $null
    if ($status) {
        # 只结束自己启动的那个 PID。结束动作报错、或结束之后 5 秒还没退出，都不能当没发生：进程还在，调用方据此不再启动新的 REAPER
        try {
            if ($Terminator) { & $Terminator $ownPid } else { Stop-Process -Id $ownPid -Force -ErrorAction Stop }
        } catch { $killError = $_.Exception.Message -replace '\s+', ' ' }
        $stillRunning = -not $proc.WaitForExit(5000)
    }
    $exit = ($proc.HasExited -and -not $status) ? $proc.ExitCode : $null
    # 进程退出的时刻：收日志用它认文件是不是这次启动的进程在世时新建的。进程自己报的（ExitTime）取不到（进程没退出、替身没有这个属性）
    # 或不可信（真机上偶尔是 1601 年）时，Resolve-ReaperExitedUtc 换成检测到退出的当下，只会放宽上界，不会误拒真日志
    $seenGoneUtc = [DateTime]::UtcNow
    $reportedUtc = try { $proc.ExitTime.ToUniversalTime() } catch { $null }
    $exitedUtc = Resolve-ReaperExitedUtc -Reported $reportedUtc -NotBeforeUtc $startedUtc -NoticedUtc $seenGoneUtc
    $modules = @{}
    foreach ($leaf in $modSeen.get_Keys()) { $modules[$leaf] = @($modSeen[$leaf]) }
    [pscustomobject]@{
        Status = $status ?? 'Exited'; ExitCode = $exit; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Pid = $ownPid; StartedUtc = $startedUtc; ExitedUtc = $exitedUtc
        Modal = $modal; Dismissed = $dismissed.ToArray(); Windows = @($titles); Marks = $marks; SampleLost = $lost.ToArray(); Modules = $modules; ModuleScans = $modScans
        StillRunning = $stillRunning; KillError = $killError
    }
}

# 本机现有的 reaper.exe 进程数（启动前后各取一次：有别人的 REAPER 在跑就不跑；跑完不该留下自己的）。
function Get-ReaperProcessIds { Get-Process -Name reaper -ErrorAction SilentlyContinue | ForEach-Object { $_.Id } }

Export-ModuleMember -Function Get-ReaperModalVerdict, ConvertTo-ReaperSafeTitle, Resolve-ReaperExitedUtc, Invoke-ReaperHost, Get-ReaperProcessIds, Get-ReaperProcessSample, Get-ReaperMarkProblem,
    Get-ReaperProcessModulePaths, Test-ReaperSameFile, Get-ReaperFileSha256, Get-ReaperModuleVerdict
