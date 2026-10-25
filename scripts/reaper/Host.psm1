# SPDX-License-Identifier: GPL-3.0-or-later
#
# 启动 REAPER 并盯住它：全仓唯一拼 -cfgfile 的地方。
#   Invoke-ReaperHost      起一个 REAPER，等它自己退出；超时、到点、模态框卡死时按它自己的 PID 结束
#   Get-ReaperModalVerdict 一个窗口怎么处理（纯函数，自测直接用）
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

# 标题里可能带工程路径：盘符路径一律换成 <path>，再截 40 个字符。
function ConvertTo-ReaperSafeTitle([string]$Title) {
    $t = $Title -replace '[A-Za-z]:[\\/][^\s"]*', '<path>' -replace '\\\\[^\s"]*', '<path>'
    $t.Length -gt 40 ? $t.Substring(0, 40) : $t
}

function Get-ReaperProcessSample([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    [pscustomobject]@{
        Handles    = $Process.HandleCount
        PrivateMB  = [math]::Round($Process.PrivateMemorySize64 / 1MB, 1)
        WorkingMB  = [math]::Round($Process.WorkingSet64 / 1MB, 1)
    }
}

# 起 REAPER 并等它退出。Status：Exited / Timeout（到 TimeoutSec）/ Killed（到 KillAfterSec，反例用）/ Modal（卡死的对话框）。
# WatchFiles：标签 → 路径，文件一出现就记一次进程的句柄与内存（Lua 用它标出循环的开头与结尾）。
# Launcher 只给自测注入：收到准备好的 ProcessStartInfo，返回已启动的 Process。
function Invoke-ReaperHost {
    param(
        [Parameter(Mandatory)][string]$ReaperExe,
        [string[]]$ReaperArgs = @(),
        [hashtable]$Environment = @{},
        [string]$WorkDir = '',
        [double]$TimeoutSec = 90,
        [double]$KillAfterSec = 0,
        [hashtable]$WatchFiles = @{},
        [scriptblock]$Launcher = $null
    )
    $cfgArgs = @('-nosplash', '-newinst', '-cfgfile', (Get-ReaperCfgArgument))
    $psi = [Diagnostics.ProcessStartInfo]::new($ReaperExe)
    $psi.Arguments = (($cfgArgs + $ReaperArgs) -join ' ')
    $psi.UseShellExecute = $false
    if ($WorkDir) { $psi.WorkingDirectory = $WorkDir }
    foreach ($k in $Environment.Keys) { $psi.Environment[[string]$k] = [string]$Environment[$k] }
    $proc = $Launcher ? (& $Launcher $psi) : [Diagnostics.Process]::Start($psi)
    $ownPid = $proc.Id
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $status = $null
    $modal = $null
    $dismissed = [Collections.Generic.List[string]]::new()
    $titles = [Collections.Generic.SortedSet[string]]::new()
    $marks = [ordered]@{}
    $firstSeen = @{}
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 250
        $now = $sw.Elapsed.TotalSeconds
        if ($KillAfterSec -gt 0 -and $now -ge $KillAfterSec) { $status = 'Killed'; break }
        if ($now -ge $TimeoutSec) { $status = 'Timeout'; break }
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
        foreach ($label in @($WatchFiles.Keys)) {
            if (-not $marks.Contains($label) -and (Test-Path -LiteralPath $WatchFiles[$label])) { $marks[$label] = Get-ReaperProcessSample $proc }
        }
    }
    if ($status) {
        # 只结束自己启动的那个 PID
        try { Stop-Process -Id $ownPid -Force -ErrorAction Stop } catch { }
        $null = $proc.WaitForExit(5000)
    }
    $exit = ($proc.HasExited -and -not $status) ? $proc.ExitCode : $null
    [pscustomobject]@{
        Status = $status ?? 'Exited'; ExitCode = $exit; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Pid = $ownPid
        Modal = $modal; Dismissed = $dismissed.ToArray(); Windows = @($titles); Marks = $marks
    }
}

# 本机现有的 reaper.exe 进程数（启动前后各取一次：有别人的 REAPER 在跑就不跑；跑完不该留下自己的）。
function Get-ReaperProcessIds { Get-Process -Name reaper -ErrorAction SilentlyContinue | ForEach-Object { $_.Id } }

Export-ModuleMember -Function Get-ReaperModalVerdict, ConvertTo-ReaperSafeTitle, Invoke-ReaperHost, Get-ReaperProcessIds
