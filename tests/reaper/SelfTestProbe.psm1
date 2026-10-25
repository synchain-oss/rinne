# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 烟测第二段（探针）的自测用例：不启动 REAPER。SelfTest.psm1 调 Get-ProbeSelfTestCases，结果并进同一张表。
# 每个用例是 (标签, 是否反例, 是否按预期)：反例（Bad）是「必须被拦住 / 必须判红」的情形，Passed 表示它确实被拦住了。
#   收日志：别的 pid、名字不对（含 pid 补零）、时间不对（早于启动或晚于现在）、哈希不对、目录与联接、硬链接、子目录里的、进程还在、
#           目标已有同名、列目录之后原文件变了、被别的程序占着，一律不删；钉住期间别人追加、覆盖、改名、删除都被拒；删的就是复制出去的那个对象
#   清单：本轮新出现的自己的文件之外，增、删、改任何一项都判变；开头就在清单里的名字，哪怕是「自己的」也照常比
#   一格的判据（Test-ReaperProbeCell，含播放中的块的 nonrt）、分析器结果（Get-ReaperAnalyzerSummary）、比对行（ConvertTo-ReaperCompareRow）
#   整轮合并分析：反例格不并进去
#   替身（分析器、nulltest、探针构建目录）也从这里导出，SelfTestChild.ps1 的接线用例用它们
#   契约对拍：日志列序与 analyze-probe.py 的规格、比对表头与它的 COMPARE_HEAD、case 规则表与 ProbeLog.cpp 的 kCaseKeys
#   工程模板：探针插件的身份与状态（组字节）、正式与探针不混用；隐藏参数的编号；scenarios.json 的 probe 一节；预检按风味

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Scripts = Join-Path $PSScriptRoot '../../scripts/reaper'
Import-Module (Join-Path $script:Scripts 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts 'Scenarios.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts 'Suite.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts 'ProbeLogs.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Rpp.psm1') -ErrorAction Stop

# 合成一份探针日志的文本：列序按 Get-ReaperProbeLogColumns。PeerPid：-1 = 与本进程同 pid 配对（REAPER 里两端同进程），0 = 没有 paired 事件。
function New-FakeProbeLogText {
    param([ValidateSet('I', 'O')][string]$Role = 'I', [long]$LogPid = 4242, [int]$Blocks = 40, [int]$N = 512, [int]$NDecl = 512, [long]$D = 4800,
          [long[]]$Pings = @(4800), [long]$PeerPid = -1, [long]$Dropped = 0, [switch]$NoClose, [int]$Sr = 48000, [string]$Case = 'CB-01',
          [string]$Variant = 'steady', [int]$GapAt = -1, [int]$NonRt = 0, [int]$PlayingFrom = 0, [int]$NOverride = -1, [int]$NOverrideAt = -1,
          [int]$NonRtFrom = -1, [int]$NonRtTo = -1) # NonRtFrom..NonRtTo-1 号块的 nonrt 是 1，别的块取 NonRt（造「播放中的块 nonrt 不一致」）
    $peer = ($PeerPid -lt 0) ? $LogPid : $PeerPid
    $sb = [Text.StringBuilder]::new()
    $null = $sb.Append("rinne-probe-log,1`nH,$Role,vst3,other,win,x64,$LogPid,1,abcd,00000000`nE,10,prepare,$Sr,$NDecl`nC,100,$Case,$Variant,-,na,$Sr`n")
    if ($PeerPid -ne 0) { $null = $sb.Append("E,101,paired,$peer,A`n") }
    $seq = 0
    for ($k = 0; $k -lt $Blocks; $k++) {
        if ($k -eq $GapAt) { $seq++ }
        $n = ($k -eq $NOverrideAt) ? $NOverride : $N
        $nonRtHere = ($k -ge $NonRtFrom -and $k -lt $NonRtTo) ? 1 : $NonRt
        $null = $sb.Append("B,$seq,$(2 * $k + 1),$Role,$n,$NDecl,0,0,$([int]($k -ge $PlayingFrom)),$nonRtHere,0,$(200 + $k),0000abcd,0,$($k * $N),0,0,$D`n")
        $seq++
    }
    $null = $sb.Append("S,5000,$Blocks,$Dropped,$N,0,0`n")
    if ($Role -eq 'I') { foreach ($p in $Pings) { $null = $sb.Append("E,6000,ping,$p,`n") } }
    if (-not $NoClose) { $null = $sb.Append("Z,7000,$Blocks,$Dropped`n") }
    $sb.ToString()
}

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# 起一个马上退出的进程，拿它的 pid：「已经退出的 REAPER」的替身
function Get-ExitedPid {
    $p = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path, '-NoProfile -NonInteractive -Command exit 0'))
    $p.WaitForExit()
    $p.Id
}

# 不要管理员权限也能造出「重解析点文件」：给普通文件打一个没有任何过滤驱动认得的重解析标记（符号链接要管理员权限，本机建不了）。
# 打了之后属性带 ReparsePoint，按正常方式打开会失败（没有谁解得开这个标记），只有带 FILE_FLAG_OPEN_REPARSE_POINT 才打得开：
# 和符号链接一样，是「名字指到别处的文件」。造不出来（比如卷不是 NTFS）就返回 $false，用例标注这条没能真跑。
if (-not ('RinneFakeReparse' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RinneFakeReparse {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuffer, uint inLength, IntPtr outBuffer, uint outLength, out uint returned, IntPtr overlapped);
    public static int Mark(string path) {
        using (var h = CreateFileW(path, 0x40000000u, 0u, IntPtr.Zero, 3u, 0x00200000u, IntPtr.Zero)) {
            if (h.IsInvalid) return Marshal.GetLastWin32Error();
            var data = new byte[28];
            BitConverter.GetBytes(0x80001234u).CopyTo(data, 0);          // 标记值：没有谁认
            BitConverter.GetBytes((ushort)20).CopyTo(data, 4);           // 数据长度 = GUID 16 + 内容 4
            Guid.NewGuid().ToByteArray().CopyTo(data, 8);
            uint returned;
            if (!DeviceIoControl(h, 0x000900A4u, data, (uint)data.Length, IntPtr.Zero, 0, out returned, IntPtr.Zero)) return Marshal.GetLastWin32Error();
            return 0;
        }
    }
}
'@
}
function New-FakeReparseFile([string]$Path, [string]$Text = "target`n") {
    try { [IO.File]::WriteAllText($Path, $Text); [RinneFakeReparse]::Mark($Path) -eq 0 } catch { $false }
}

$script:Attack = [ordered]@{} # AfterCopy 回调写、用例读（回调在本文件的会话状态里跑，结果经 $script: 变量带出来）

function Get-CollectCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $gone = Get-ExitedPid
    $since = [DateTime]::UtcNow.AddSeconds(-30)
    $logDir = Join-Path $Tmp 'docs/Rinne Probe Logs'
    $dest = Join-Path $Tmp 'probe-dest'
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $logDir 'sub')
    $write = { param($Name, $Text) $f = Join-Path $logDir $Name; [IO.File]::WriteAllText($f, $Text); $f }
    # 先放别人的文件、取「开头」的清单，再放本轮自己的两份：自己的文件开头不在清单里（真跑里也是这样：文档目录先列清单，REAPER 才启动）
    $others = @(
        "probe-I-$($gone + 1)-abcd.csv", # 别的 pid（例如用户自己在 Cubase 里写的）
        "probe-I-$gone-FACE.csv", "probe-X-$gone-abcd.csv", "probe-I-$gone-abcd.csv.bak", "xprobe-I-$gone-abcd.csv", "probe-I-$gone-abc.csv", # 名字不对
        'notes.txt',
        "probe-I-0$gone-1234.csv" # pid 写成补零的：数值等于本轮的 pid，但探针不会这样写
    )
    foreach ($n in $others) { $null = & $write $n "keep $n`n" }
    $old = & $write "probe-O-$gone-dead.csv" "old`n"
    (Get-Item -LiteralPath $old).LastWriteTimeUtc = $since.AddHours(-1) # 修改时间早于这次启动：同号的旧进程留下的
    $future = & $write "probe-O-$gone-fa57.csv" "future`n"
    (Get-Item -LiteralPath $future).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(1) # 修改时间在将来：这次启动的进程写不出来
    $dirLike = Join-Path $logDir "probe-I-$gone-beef.csv" # 名字像日志的目录
    $null = New-Item -ItemType Directory -Path $dirLike
    [IO.File]::WriteAllText((Join-Path $dirLike 'inner.txt'), 'inner')
    $juncTarget = Join-Path $Tmp 'junction-target'; $null = New-Item -ItemType Directory -Path $juncTarget
    [IO.File]::WriteAllText((Join-Path $juncTarget 'inner.txt'), "target`n")
    $junc = Join-Path $logDir "probe-I-$gone-1b2c.csv" # 名字像日志的目录联接
    $juncOk = try { $null = New-Item -ItemType Junction -Path $junc -Value $juncTarget; $true } catch { $false }
    $rpFile = Join-Path $logDir "probe-O-$gone-7e7e.csv" # 名字像日志的重解析点文件（等价于指到别处的符号链接）
    $rpOk = New-FakeReparseFile $rpFile
    [IO.File]::WriteAllText((Join-Path $logDir "sub/probe-O-$gone-cafe.csv"), "nested`n") # 子目录里的：探针只往顶层写，不进子目录
    $before = Get-ReaperProbeLogListing -Dir $logDir
    $own = @{ "probe-I-$gone-abcd.csv" = "own-in`n"; "probe-O-$gone-ef01.csv" = "own-out`n" }
    $ownSha = @{}
    foreach ($n in $own.Keys) { $ownSha[$n] = Get-Sha (& $write $n $own[$n]) }
    $mid = Get-ReaperProbeLogListing -Dir $logDir # 自己的两份出现之后、搬走之前
    $m = Move-ReaperProbeLogs -LogDir $logDir -Pids @($gone) -SinceUtc $since -DestDir $dest
    $after = Get-ReaperProbeLogListing -Dir $logDir
    $movedOk = (@($m.Moved | Sort-Object) -join '|') -ceq (@($own.Keys | Sort-Object) -join '|')
    $copiesOk = @($own.Keys | Where-Object { -not (Test-Path -LiteralPath (Join-Path $logDir $_)) -and (Test-Path -LiteralPath (Join-Path $dest $_)) -and (Get-Sha (Join-Path $dest $_)) -ceq $ownSha[$_] }).Count -eq $own.Count
    & $add '收日志：本轮自己的两份（pid 对、名字对、时间在窗口内）被搬走：复制件 sha256 与原文件相同，原文件删了' $false ($movedOk -and $copiesOk -and $m.Problems.Count -eq 0)
    & $add '收日志：别的 pid 的日志不碰' $true ((Test-Path -LiteralPath (Join-Path $logDir $others[0])) -and $after.Contains($others[0]) -and $after[$others[0]] -ceq $before[$others[0]])
    $namesKept = @($others[1..6] | Where-Object { $after.Contains($_) -and $after[$_] -ceq $before[$_] }).Count -eq 6
    & $add '收日志：名字不对的（tok4 大写、role 不是 I/O、多了后缀、前面多了字、tok4 只有 3 位、别的文件）一个不碰' $true $namesKept
    & $add '收日志：pid 写成补零的（数值等于本轮的 pid）不是探针写的名字，不碰' $true ($after.Contains($others[7]) -and $after[$others[7]] -ceq $before[$others[7]] -and $m.Own -cnotcontains $others[7])
    & $add '收日志：修改时间早于这次启动的同 pid 日志不碰，并写明原因' $true ((Test-Path -LiteralPath $old) -and @($m.Skipped | Where-Object { $_ -like "*dead.csv*修改时间早于*" }).Count -eq 1)
    & $add '收日志：修改时间晚于现在的同 pid 日志不碰，并写明原因（时间窗口有上界）' $true ((Test-Path -LiteralPath $future) -and @($m.Skipped | Where-Object { $_ -like "*fa57.csv*修改时间晚于*" }).Count -eq 1)
    & $add '收日志：名字像日志的目录不碰（不删目录、不进去），并写明原因' $true ((Test-Path -LiteralPath (Join-Path $dirLike 'inner.txt')) -and @($m.Skipped | Where-Object { $_ -like '*beef.csv*不是普通文件*' }).Count -eq 1)
    & $add "收日志：名字像日志的目录联接（junction）不碰，联接的目标里的文件还在$($juncOk ? '' : '（本机建不了联接，这条没能真跑）')" $true ((-not $juncOk) -or ((Test-Path -LiteralPath (Join-Path $juncTarget 'inner.txt')) -and (Test-Path -LiteralPath $junc) -and @($m.Skipped | Where-Object { $_ -like '*1b2c.csv*不是普通文件*' }).Count -eq 1))
    & $add "收日志：名字像日志的重解析点文件（指到别处的链接）不碰，并写明原因$($rpOk ? '' : '（本机造不出重解析点文件，这条没能真跑）')" $true (
        (-not $rpOk) -or ($null -ne (Get-Item -LiteralPath $rpFile -Force -ErrorAction SilentlyContinue) -and @($m.Skipped | Where-Object { $_ -like '*7e7e.csv*不是普通文件*' }).Count -eq 1 -and $m.Own -cnotcontains "probe-O-$gone-7e7e.csv"))
    & $add '收日志：子目录里同名同 pid 的文件不碰（不递归）' $true (Test-Path -LiteralPath (Join-Path $logDir "sub/probe-O-$gone-cafe.csv"))
    & $add '收日志：日志目录本身还在' $true (Test-Path -LiteralPath $logDir -PathType Container)
    # 列目录时的 IsLink：联接（重解析点）记作链接，普通文件与普通目录不是（符号链接要管理员权限，本机建不了，联接走的是同一个属性）
    $byName = @{}
    foreach ($x in (& (Get-Module ProbeLogs) { param($d) Get-ReaperDirEntries $d } $logDir)) { $byName[$x.Name] = $x }
    $juncName = "probe-I-$gone-1b2c.csv"
    & $add "列目录：联接记作链接（IsLink）且不是普通文件，普通文件、普通目录都不是链接$($juncOk ? '' : '（本机建不了联接，联接这半条没能真跑）')" $true (
        ((-not $juncOk) -or ($byName[$juncName].IsLink -and -not $byName[$juncName].IsFile)) -and $byName['notes.txt'].IsFile -and -not $byName['notes.txt'].IsLink -and -not $byName["probe-I-$gone-beef.csv"].IsFile -and -not $byName["probe-I-$gone-beef.csv"].IsLink)

    & $add "列目录：重解析点文件（它不是目录，只靠 IsLink 认出来）记作链接$($rpOk ? '' : '（本机造不出重解析点文件，这条没能真跑）')" $true ((-not $rpOk) -or ($byName["probe-O-$gone-7e7e.csv"].IsLink -and $byName["probe-O-$gone-7e7e.csv"].IsFile))

    $cmpOwn = Compare-ReaperProbeLogListing -Before $before -After $after -Own $m.Own
    & $add '清单：除本轮自己的文件外前后相同' $false $cmpOwn.Same
    & $add '清单：本轮新出现又搬走的文件，对前后清单没有影响（不给 Own 也相同）' $false (Compare-ReaperProbeLogListing -Before $before -After $after).Same
    & $add '清单：自己的文件没搬走、留在原处时，不算 Own 判变（多了文件）' $true (-not (Compare-ReaperProbeLogListing -Before $before -After $mid).Same)
    & $add '清单：自己的文件留在原处，算 Own 时不判变（它们开头不在清单里）' $false (Compare-ReaperProbeLogListing -Before $before -After $mid -Own $m.Own).Same
    $cmpStale = Compare-ReaperProbeLogListing -Before $mid -After $after -Own $m.Own
    & $add '清单：Own 里的名字开头就在清单里（自己的文件不可能早于本轮就在：时间与 pid 的筛选选错了）：搬走了判变，不除外' $true (-not $cmpStale.Same -and @($cmpStale.Changed | Where-Object { $_ -like 'removed:*' }).Count -eq 2)
    # 清单比较：别人的文件增、删、改（大小或修改时间）任何一项都判变；本轮新出现的自己的名字才除外
    $b0 = [ordered]@{ 'a.csv' = '1|10'; 'sub' = 'dir' }
    $edit = { param($k, $v) $x = [ordered]@{}; foreach ($key in $b0.Keys) { $x[$key] = $b0[$key] }; if ($null -eq $v) { $x.Remove($k) } else { $x[$k] = $v }; $x }
    & $add '清单：别人的文件改了（大小或修改时间）判变' $true (-not (Compare-ReaperProbeLogListing -Before $b0 -After (& $edit 'a.csv' '1|11') -Own 'mine.csv').Same)
    & $add '清单：别人的文件没了判变' $true (-not (Compare-ReaperProbeLogListing -Before $b0 -After (& $edit 'a.csv' $null) -Own 'mine.csv').Same)
    & $add '清单：多了别人的文件判变' $true (-not (Compare-ReaperProbeLogListing -Before $b0 -After (& $edit 'new.csv' '3|30') -Own 'mine.csv').Same)
    & $add '清单：子目录没了也判变' $true (-not (Compare-ReaperProbeLogListing -Before $b0 -After (& $edit 'sub' $null) -Own 'mine.csv').Same)
    & $add '清单：本轮新出现的自己的文件算 Own 时不判变' $false (Compare-ReaperProbeLogListing -Before $b0 -After (& $edit 'mine.csv' '2|20') -Own 'mine.csv').Same
    & $add '清单：同一个新文件不算 Own 就是多了文件' $true (-not (Compare-ReaperProbeLogListing -Before $b0 -After (& $edit 'mine.csv' '2|20')).Same)
    $b1 = [ordered]@{ 'a.csv' = '1|10'; 'mine.csv' = '2|20'; 'sub' = 'dir' }
    $edit1 = { param($k, $v) $x = [ordered]@{}; foreach ($key in $b1.Keys) { $x[$key] = $b1[$key] }; if ($null -eq $v) { $x.Remove($k) } else { $x[$k] = $v }; $x }
    & $add '清单：开头就在清单里的名字即使算 Own，没了也判变（不除外）' $true (-not (Compare-ReaperProbeLogListing -Before $b1 -After (& $edit1 'mine.csv' $null) -Own 'mine.csv').Same)
    & $add '清单：开头就在清单里的名字即使算 Own，改了也判变（不除外）' $true (-not (Compare-ReaperProbeLogListing -Before $b1 -After (& $edit1 'mine.csv' '2|21') -Own 'mine.csv').Same)
    & $add '清单：目录不存在是空清单，不报错' $false ((Get-ReaperProbeLogListing -Dir (Join-Path $Tmp 'no-such-logs')).Count -eq 0)
    $subFile = "sub\probe-O-$gone-cafe.csv"
    & $add '清单：子目录也记一项（dir），子目录里的文件记大小与修改时间' $false ($before['sub'] -ceq 'dir' -and $before[$subFile] -cmatch '^[0-9]+\|[0-9]+$' -and $before["probe-I-$gone-beef.csv"] -ceq 'dir')
    # 哈希不同：复制件被换了内容，原文件不删、判红
    $dirH = Join-Path $Tmp 'logs-hash'; $null = New-Item -ItemType Directory -Path $dirH
    $fH = Join-Path $dirH "probe-I-$gone-1111.csv"; [IO.File]::WriteAllText($fH, "real`n")
    $mH = Move-ReaperProbeLogs -LogDir $dirH -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-hash') -AfterCopy { param($src, $dst) [IO.File]::WriteAllText($dst, "corrupted`n") }
    & $add '收日志：复制件的 sha256 与原文件不同就不删原文件，并判红' $true ((Test-Path -LiteralPath $fH) -and $mH.Moved.Count -eq 0 -and @($mH.Problems | Where-Object { $_ -like '*哈希不同*' }).Count -eq 1)
    # 列目录之后、钉住之前原文件变了：大小变了、只是修改时间变了，都不动
    $dirC = Join-Path $Tmp 'logs-changed'; $null = New-Item -ItemType Directory -Path $dirC
    $fC = Join-Path $dirC "probe-O-$gone-2222.csv"; [IO.File]::WriteAllText($fC, "real`n")
    $mC = Move-ReaperProbeLogs -LogDir $dirC -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-changed') -BeforePin { param($src) [IO.File]::AppendAllText($src, "more`n") }
    & $add '收日志：列目录之后原文件变长了就不删，并判红' $true ((Test-Path -LiteralPath $fC) -and $mC.Moved.Count -eq 0 -and @($mC.Problems | Where-Object { $_ -like '*列目录之后原文件变了*' }).Count -eq 1)
    $dirM = Join-Path $Tmp 'logs-touched'; $null = New-Item -ItemType Directory -Path $dirM
    $fM = Join-Path $dirM "probe-O-$gone-2323.csv"; [IO.File]::WriteAllText($fM, "real`n")
    $mM = Move-ReaperProbeLogs -LogDir $dirM -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-touched') -BeforePin { param($src) (Get-Item -LiteralPath $src).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(-5) }
    & $add '收日志：列目录之后只是修改时间变了（大小没变）也不删，并判红' $true ((Test-Path -LiteralPath $fM) -and $mM.Moved.Count -eq 0 -and @($mM.Problems | Where-Object { $_ -like '*列目录之后原文件变了*' }).Count -eq 1)
    # 列目录之后、钉住之前，原文件被同长度、同修改时间的另一个文件顶替：复制出去的、核对的、删掉的，是同一个（钉住的那个）对象
    $dirS = Join-Path $Tmp 'logs-swap'; $null = New-Item -ItemType Directory -Path $dirS
    $fS = Join-Path $dirS "probe-I-$gone-3434.csv"; [IO.File]::WriteAllText($fS, "real`n")
    $destS = Join-Path $Tmp 'dest-swap'
    $mS = Move-ReaperProbeLogs -LogDir $dirS -Pids @($gone) -SinceUtc $since -DestDir $destS -BeforePin {
        param($src) $t = (Get-Item -LiteralPath $src).LastWriteTimeUtc; Remove-Item -LiteralPath $src; [IO.File]::WriteAllText($src, "SWAP`n"); (Get-Item -LiteralPath $src).LastWriteTimeUtc = $t
    }
    $copyS = Join-Path $destS "probe-I-$gone-3434.csv"
    & $add '收日志：钉住之前原文件被同长度同时间的另一个文件顶替：复制出去的正是被删掉的那个，原文件没了、没有问题' $true ($mS.Moved.Count -eq 1 -and -not (Test-Path -LiteralPath $fS) -and (Test-Path -LiteralPath $copyS) -and [IO.File]::ReadAllText($copyS) -ceq "SWAP`n" -and $mS.Problems.Count -eq 0)
    # 钉住期间别人动不了原文件：追加、覆盖、改名、删除都被拒（共享方式只放行读），正常搬走不受影响
    $dirA = Join-Path $Tmp 'logs-attack'; $null = New-Item -ItemType Directory -Path $dirA
    $fA = Join-Path $dirA "probe-O-$gone-4545.csv"; [IO.File]::WriteAllText($fA, "real`n")
    $script:Attack = [ordered]@{}
    $mA = Move-ReaperProbeLogs -LogDir $dirA -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-attack') -AfterCopy {
        param($src, $dst)
        $script:Attack.Append = try { $s = [IO.File]::Open($src, 'Append', 'Write', 'ReadWrite'); $s.Dispose(); 'allowed' } catch { 'refused' }
        $script:Attack.Overwrite = try { [IO.File]::WriteAllText($src, 'x'); 'allowed' } catch { 'refused' }
        $script:Attack.Rename = try { [IO.File]::Move($src, "$src.moved"); 'allowed' } catch { 'refused' }
        $script:Attack.Delete = try { [IO.File]::Delete($src); 'allowed' } catch { 'refused' }
    }
    & $add "收日志：原文件钉住期间，别人追加、覆盖、改名、删除都被拒（$(($script:Attack.Keys | ForEach-Object { "$_=$($script:Attack[$_])" }) -join ' ')）" $true (
        $script:Attack.Count -eq 4 -and @($script:Attack.Values | Where-Object { $_ -ceq 'refused' }).Count -eq 4)
    & $add '收日志：被攻击也不影响正常搬走：复制件与原文件内容相同、原文件删了' $false ($mA.Moved.Count -eq 1 -and $mA.Problems.Count -eq 0 -and -not (Test-Path -LiteralPath $fA) -and [IO.File]::ReadAllText((Join-Path $Tmp "dest-attack/probe-O-$gone-4545.csv")) -ceq "real`n")
    # 别的程序正占着写：钉不住就不动，并判红
    $dirW = Join-Path $Tmp 'logs-writer'; $null = New-Item -ItemType Directory -Path $dirW
    $fW = Join-Path $dirW "probe-I-$gone-5656.csv"; [IO.File]::WriteAllText($fW, "real`n")
    $writer = [IO.File]::Open($fW, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]'ReadWrite, Delete')
    try { $mW = Move-ReaperProbeLogs -LogDir $dirW -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-writer') -PinTries 1 } finally { $writer.Dispose() }
    & $add '收日志：别的程序正开着写（钉不住）就不动，并判红' $true ((Test-Path -LiteralPath $fW) -and $mW.Moved.Count -eq 0 -and @($mW.Problems | Where-Object { $_ -like '*钉不住原文件*' }).Count -eq 1)
    # 硬链接：日志名其实是别处一个文件的另一个名字（探针新建的日志只有一个名字）：不动，那个文件也不受影响
    $dirK = Join-Path $Tmp 'logs-hardlink'; $null = New-Item -ItemType Directory -Path $dirK
    $precious = Join-Path $Tmp 'precious.txt'; [IO.File]::WriteAllText($precious, "precious`n")
    $fK = Join-Path $dirK "probe-O-$gone-6767.csv"
    $hardOk = try { $null = New-Item -ItemType HardLink -Path $fK -Value $precious; $true } catch { $false }
    $mK = Move-ReaperProbeLogs -LogDir $dirK -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-hardlink')
    & $add "收日志：有多个硬链接的同名文件不动，别处的那个名字和内容都还在，并判红$($hardOk ? '' : '（本机建不了硬链接，这条没能真跑）')" $true (
        (-not $hardOk) -or ($mK.Moved.Count -eq 0 -and (Test-Path -LiteralPath $fK) -and [IO.File]::ReadAllText($precious) -ceq "precious`n" -and @($mK.Problems | Where-Object { $_ -like '*硬链接*' }).Count -eq 1))
    # 钉住这一步自己：联接（目录）拒绝并说明理由；普通文件钉得上；属性判定的纯函数
    $pinJunc = if ($juncOk) { Open-ReaperPinnedFile -Path $junc -Tries 1 } else { $null }
    & $add "钉住：联接（重解析的目录）钉不上$($juncOk ? '' : '（本机建不了联接，这条没能真跑）')" $true ((-not $juncOk) -or (-not $pinJunc.Ok))
    # 重解析点文件（符号链接的替身）：带 OPEN_REPARSE_POINT 才打得开，打开后属性带 0x400，说明是链接；不带这个旗标会被系统拒绝（没有谁解得开）而说不出是链接
    $pinRp = if ($rpOk) { Open-ReaperPinnedFile -Path $rpFile -Tries 1 } else { $null }
    & $add "钉住：重解析点文件（链接）钉不上，并说明是链接$($rpOk ? '' : '（本机造不出重解析点文件，这条没能真跑）')" $true ((-not $rpOk) -or (-not $pinRp.Ok -and $pinRp.Why -like '*链接*'))
    # 列目录时它还是普通文件，钉住之前被换成链接（重解析点）：钉住这一步拒绝，原文件不动
    $dirR = Join-Path $Tmp 'logs-swap-link'; $null = New-Item -ItemType Directory -Path $dirR
    $fR = Join-Path $dirR "probe-O-$gone-3535.csv"; [IO.File]::WriteAllText($fR, "real`n")
    $mR = Move-ReaperProbeLogs -LogDir $dirR -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-swap-link') -PinTries 1 -BeforePin {
        param($src) Remove-Item -LiteralPath $src; $null = New-FakeReparseFile $src "real`n"
    }
    & $add "收日志：列目录之后原文件被换成链接就不动、不删，并判红$($rpOk ? '' : '（本机造不出重解析点文件，这条没能真跑）')" $true (
        (-not $rpOk) -or ($mR.Moved.Count -eq 0 -and $null -ne (Get-Item -LiteralPath $fR -Force -ErrorAction SilentlyContinue) -and @($mR.Problems | Where-Object { $_ -like '*钉不住原文件（是链接*' }).Count -eq 1))
    $pinOk = Open-ReaperPinnedFile -Path $precious -Tries 1
    try { & $add '钉住：普通文件钉得上，读得到长度与硬链接数' $false ($pinOk.Ok -and $pinOk.Length -eq 9 -and $pinOk.Links -eq ($hardOk ? 2 : 1) -and $pinOk.Stream.CanRead) } finally { Close-ReaperPinnedFile $pinOk }
    $pinMissing = Open-ReaperPinnedFile -Path (Join-Path $Tmp 'no-such-log.csv') -Tries 1
    & $add '钉住：文件不存在钉不上，并说明' $true (-not $pinMissing.Ok -and $pinMissing.Why -like '*不见了*')
    & $add '钉住：属性是目录（0x10）就拒收，说明是目录' $true ((Get-ReaperPinnedRefusal 0x10) -like '*目录*')
    & $add '钉住：属性是重解析点（0x400，符号链接）就拒收，说明是链接' $true ((Get-ReaperPinnedRefusal 0x400) -like '*链接*')
    & $add '钉住：普通文件的属性（0x20）不拒收' $false ($null -eq (Get-ReaperPinnedRefusal 0x20))
    # 写它的进程还在：不动
    $dirP = Join-Path $Tmp 'logs-alive'; $null = New-Item -ItemType Directory -Path $dirP
    $fP = Join-Path $dirP "probe-I-$PID-3333.csv"; [IO.File]::WriteAllText($fP, "live`n")
    $mP = Move-ReaperProbeLogs -LogDir $dirP -Pids @($PID) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-alive')
    & $add '收日志：写它的进程还没退出就不动，并判红' $true ((Test-Path -LiteralPath $fP) -and $mP.Moved.Count -eq 0 -and @($mP.Problems | Where-Object { $_ -like '*还没退出*' }).Count -eq 1)
    # 结果目录里已有同名文件：不覆盖、不删原文件
    $dirT = Join-Path $Tmp 'logs-target'; $null = New-Item -ItemType Directory -Path $dirT
    $fT = Join-Path $dirT "probe-O-$gone-4444.csv"; [IO.File]::WriteAllText($fT, "real`n")
    $destT = Join-Path $Tmp 'dest-target'; $null = New-Item -ItemType Directory -Path $destT
    [IO.File]::WriteAllText((Join-Path $destT "probe-O-$gone-4444.csv"), "earlier`n")
    $mT = Move-ReaperProbeLogs -LogDir $dirT -Pids @($gone) -SinceUtc $since -DestDir $destT
    & $add '收日志：结果目录里已有同名文件就不覆盖、不删原文件，并判红' $true ((Test-Path -LiteralPath $fT) -and [IO.File]::ReadAllText((Join-Path $destT "probe-O-$gone-4444.csv")) -ceq "earlier`n" -and @($mT.Problems | Where-Object { $_ -like '*同名*' }).Count -eq 1)
    # 纯函数：哪些条目算本轮自己的（修改时间要落在 [SinceUtc, UntilUtc] 里，两端都算）
    $until = [DateTime]::UtcNow.AddSeconds(5)
    $e = { param($name, [bool]$isFile = $true, [bool]$isLink = $false, $at = [DateTime]::UtcNow) [pscustomobject]@{ Name = $name; IsFile = $isFile; IsLink = $isLink; LastWriteUtc = $at } }
    $sel = Select-ReaperOwnProbeLogs -Entries @((& $e 'probe-I-77-abcd.csv'), (& $e 'probe-O-77-abcd.csv' $true $true), (& $e 'probe-I-78-abcd.csv'), (& $e 'probe-I-77-abcd.csv.tmp')) -Pids @(77) -SinceUtc $since -UntilUtc $until
    & $add '选取：符号链接（重解析点）不算普通文件，不碰' $true ($sel.Own.Count -eq 1 -and $sel.Own[0].Name -ceq 'probe-I-77-abcd.csv' -and @($sel.Skipped | Where-Object { $_ -like 'probe-O-77-abcd.csv*不是普通文件*' }).Count -eq 1)
    & $add '选取：没有给 pid 时一个都不选' $true ((Select-ReaperOwnProbeLogs -Entries @((& $e 'probe-I-77-abcd.csv')) -Pids @() -SinceUtc $since -UntilUtc $until).Own.Count -eq 0)
    $padded = Select-ReaperOwnProbeLogs -Entries @((& $e 'probe-I-077-abcd.csv'), (& $e 'probe-I-0077-abcd.csv'), (& $e 'probe-I-00-abcd.csv')) -Pids @(77, 0) -SinceUtc $since -UntilUtc $until
    & $add '选取：pid 补零的名字（077、0077、00）一个都不选，也不报理由（不是探针写的名字）' $true ($padded.Own.Count -eq 0 -and $padded.Skipped.Count -eq 0)
    $zero = Select-ReaperOwnProbeLogs -Entries @((& $e 'probe-I-0-abcd.csv')) -Pids @(0) -SinceUtc $since -UntilUtc $until
    & $add '选取：pid 就是 0 时（不补零）照常认' $false ($zero.Own.Count -eq 1)
    $long = try { Select-ReaperOwnProbeLogs -Entries @((& $e 'probe-I-12345678901-abcd.csv'), (& $e 'probe-I-1234567890123456789012345-abcd.csv')) -Pids @(77) -SinceUtc $since -UntilUtc $until } catch { $null }
    & $add '选取：pid 超过十位的名字（十一位、二十五位）不选，也不因为数太大而抛错' $true ($null -ne $long -and $long.Own.Count -eq 0)
    $edge = Select-ReaperOwnProbeLogs -Entries @((& $e 'probe-I-77-aaaa.csv' $true $false $since), (& $e 'probe-I-77-bbbb.csv' $true $false $until), (& $e 'probe-I-77-cccc.csv' $true $false $since.AddTicks(-1)), (& $e 'probe-I-77-dddd.csv' $true $false $until.AddTicks(1))) -Pids @(77) -SinceUtc $since -UntilUtc $until
    & $add '选取：修改时间恰等于下界或上界的算，早一刻（下界）或晚一刻（上界）的不算，各写明原因' $true (
        @($edge.Own | ForEach-Object { $_.Name }) -join ',' -ceq 'probe-I-77-aaaa.csv,probe-I-77-bbbb.csv' -and
        @($edge.Skipped | Where-Object { $_ -like 'probe-I-77-cccc.csv*早于*' }).Count -eq 1 -and @($edge.Skipped | Where-Object { $_ -like 'probe-I-77-dddd.csv*晚于*' }).Count -eq 1)
    $cases
}

function Get-CellCases {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $cell = {
        param([hashtable]$In = @{}, [hashtable]$Out = @{}, [hashtable]$Exp = @{}, [string[]]$Extra = @())
        $logs = @((Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role I @In)), (Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role O -Pings @() @Out)))
        foreach ($x in $Extra) { $logs += Read-ReaperProbeLog -Text $x }
        $want = @{ Pid = 4242; Case = 'CB-01'; Variant = 'steady'; Sr = 48000; Block = 512; MinBlocks = 32; NeedPing = $true }
        foreach ($k in $Exp.Keys) { $want[$k] = $Exp[$k] }
        Test-ReaperProbeCell -Logs $logs -Expect $want
    }
    $hit = { param($r, [string]$like) @($r.Bad | Where-Object { $_ -like $like }).Count -gt 0 }
    $good = & $cell
    & $add "一格：两端齐全、Ping 恰为 D、块长对、配对、不丢记录时没有问题（$($good.Bad -join '；')）" $false ($good.Bad.Count -eq 0 -and $good.Facts.ping -ceq '4800' -and $good.Facts.d -ceq '4800' -and $good.Facts.blockMatches)
    & $add '一格：Ping 求出的延迟不等于 D 判红' $true (& $hit (& $cell -In @{ Pings = @(4801) }) '*Ping 求出的延迟是 4801*')
    & $add '一格：两次 Ping 有一次不等于 D 也判红' $true (& $hit (& $cell -In @{ Pings = @(4800, 4799) }) '*Ping 求出的延迟是*')
    & $add '一格：没有 Ping 结果判红' $true (& $hit (& $cell -In @{ Pings = @() }) '*没有 Ping*')
    $twoD = (New-FakeProbeLogText -Role I) -replace '(?m)^(B,5,(?:[^,]*,){15})4800$', '${1}4000'
    $rTwoD = Test-ReaperProbeCell -Logs @((Read-ReaperProbeLog -Text $twoD), (Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role O -Pings @()))) -Expect @{ Pid = 4242; Case = 'CB-01'; Variant = 'steady'; Sr = 48000; Block = 512; MinBlocks = 32; NeedPing = $true }
    & $add '一格：块记录里的 D 有两个取值判红' $true (& $hit $rTwoD '*D 应恰好一个值*')
    & $add '一格：D 不是探针默认的 100 ms 判红（state 模板与探针对不上）' $true (& $hit (& $cell -In @{ D = 4000; Pings = @(4000) } -Out @{ D = 4000 }) '*D 是 4000 样本，应为探针默认的 100 ms（4800 样本）*')
    & $add '一格：S 行里有丢弃计数判红' $true (& $hit (& $cell -Out @{ Dropped = 3 }) '*Out 丢了记录*')
    & $add '一格：块序号有缺口判红' $true (& $hit (& $cell -In @{ GapAt = 10 }) '*In 丢了记录*缺口 1*')
    & $add '一格：没有 paired 事件判红' $true (& $hit (& $cell -Out @{ PeerPid = 0 }) '*Out 没有配对*')
    & $add '一格：paired 的对端不是本进程判红' $true (& $hit (& $cell -In @{ PeerPid = 999 }) '*In 没有配对*')
    & $add '一格：有块长超过声明的最大块长判红' $true (& $hit (& $cell -In @{ NOverride = 1024; NOverrideAt = 5 }) '*超过声明的最大块长*')
    & $add '一格：播放中的块长不是设定的块长判红' $true (& $hit (& $cell -In @{ N = 256; NDecl = 256 } -Out @{ N = 256; NDecl = 256 }) '*应恰为设定的 512*')
    & $add '一格：块数不够判红' $true (& $hit (& $cell -In @{ Blocks = 10 }) '*In 只有 10 块记录*')
    & $add '一格：播放中的块不够判红（稳态格）' $true (& $hit (& $cell -In @{ PlayingFrom = 20 } -Out @{ PlayingFrom = 20 }) '*播放中的块不足 32*')
    & $add '一格：日志没有 Z 行（没有正常收尾）判红' $true (& $hit (& $cell -Out @{ NoClose = $true }) '*Out 日志没有正常收尾*')
    $cut = (New-FakeProbeLogText -Role O -Pings @()).TrimEnd("`n")
    $rCut = Test-ReaperProbeCell -Logs @((Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role I)), (Read-ReaperProbeLog -Text $cut)) -Expect @{ Pid = 4242; Case = 'CB-01'; Variant = 'steady'; Sr = 48000; Block = 512; MinBlocks = 32; NeedPing = $true }
    & $add '一格：末行没写完（没有换行结尾）也算没收尾' $true (& $hit $rCut '*Out 日志没有正常收尾*')
    & $add '一格：测试格不对判红' $true (& $hit (& $cell -In @{ Case = 'CB-02'; Variant = 'toggle' }) '*In 的测试格是*应恰好一格 CB-01/steady*')
    & $add '一格：测试格的采样率不对判红' $true (& $hit (& $cell -In @{ Sr = 44100 }) '*In 测试格的采样率是 44100*')
    & $add '一格：同一次启动有两份 In 判红' $true (& $hit (& $cell -Extra @((New-FakeProbeLogText -Role I))) '*In 2、Out 1*')
    & $add '一格：日志都不是这次启动的 pid 判红' $true (& $hit (& $cell -Exp @{ Pid = 1 }) '*In 0、Out 0*')
    # 播放中的块的 nonrt：宿主对插件报实时是 0、离线是 1，应恰为场景要的那个值（只看正在走带的块）
    & $add '一格：要离线（nonrt 恰为 1）而播放中的块都是实时（0）判红' $true (& $hit (& $cell -Exp @{ PlayingNonRt = 1 }) '*播放中的块的 nonrt 是 [[]0[]]，应恰为 1*')
    & $add '一格：要实时（nonrt 恰为 0）而播放中的块都是离线（1）判红' $true (& $hit (& $cell -In @{ NonRt = 1 } -Out @{ NonRt = 1 } -Exp @{ PlayingNonRt = 0 }) '*播放中的块的 nonrt 是 [[]1[]]，应恰为 0*')
    & $add '一格：播放中的块 nonrt 一部分 0 一部分 1（宿主中途换了模式）也判红，两个方向都是' $true (
        (& $hit (& $cell -In @{ NonRtFrom = 10; NonRtTo = 40 } -Exp @{ PlayingNonRt = 0 }) '*播放中的块的 nonrt 是 [[]0/1[]]*') -and (& $hit (& $cell -In @{ NonRtFrom = 10; NonRtTo = 40 } -Exp @{ PlayingNonRt = 1 }) '*播放中的块的 nonrt 是 [[]0/1[]]*'))
    & $add '一格：要离线、播放中的块都是离线（1）时不报' $false ((& $cell -In @{ NonRt = 1 } -Out @{ NonRt = 1 } -Exp @{ PlayingNonRt = 1 }).Bad.Count -eq 0)
    & $add '一格：要实时、播放中的块都是实时（0）时不报' $false ((& $cell -Exp @{ PlayingNonRt = 0 }).Bad.Count -eq 0)
    & $add '一格：没走带的块（playing 为 0）的 nonrt 不算：停着的块是 1、播放中的块是 0，要实时时不报' $false (
        (& $cell -In @{ Blocks = 60; PlayingFrom = 20; NonRtFrom = 0; NonRtTo = 20 } -Out @{ Blocks = 60; PlayingFrom = 20; NonRtFrom = 0; NonRtTo = 20 } -Exp @{ PlayingNonRt = 0 }).Bad.Count -eq 0)
    & $add '一格：没有给 PlayingNonRt 就不查它（第一段的格不受影响）' $false ((& $cell -In @{ NonRt = 1 } -Out @{ NonRt = 1 }).Bad.Count -eq 0)
    $withPlay = & $cell -In @{ Blocks = 94 } -Out @{ Blocks = 94 }
    & $add '一格：事实里记播放中的块数与播放时长（块数 × 块长 ÷ 采样率，94 块 × 512 ÷ 48000 ≈ 1 s），文字里也写出来' $false (
        $withPlay.Facts.In.playingBlocks -eq 94 -and $withPlay.Facts.Out.playingBlocks -eq 94 -and $withPlay.Facts.In.playingSeconds -eq 1.0 -and (& (Get-Module Scenarios) { param($f) Format-ProbeFacts $f } $withPlay.Facts) -like '*（播放中 94/94 块，约 1 s）*')
    $withDriver = & $cell -Exp @{ Driver = [ordered]@{ exit_bound = 'after_render'; render_end_to_exit_s = '6.500' } }
    $driverFacts = $withDriver.Facts.Contains('driver') ? $withDriver.Facts.driver : $null # 没记进去时是 $null，不让严格模式在这里抛错（要的是一条干净的红）
    & $add '一格：驱动记下的时间（exit_bound、render_end_to_exit_s）原样进事实' $false ($null -ne $driverFacts -and $driverFacts.exit_bound -ceq 'after_render' -and $driverFacts.render_end_to_exit_s -ceq '6.500' -and $withDriver.Facts.In.playingNonrt -ceq '0')
    $rUnread = Test-ReaperProbeCell -Logs @((Read-ReaperProbeLog -Text ((New-FakeProbeLogText -Role I) + "B,1,2`n")), (Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role O -Pings @()))) -Expect @{ Pid = 4242; Case = 'CB-01'; Variant = 'steady'; Sr = 48000; Block = 512; MinBlocks = 32; NeedPing = $true }
    & $add '一格：有读不懂的行判红（格式交给分析器）' $true (& $hit $rUnread '*In 有 1 行读不懂*')
    # 读日志：列数不对（少了或多了）、不是整数都只记作读不懂，不抛错
    $r = Read-ReaperProbeLog -Text ((New-FakeProbeLogText -Role I) + "E,1,ping`nB,x,1,I,512,512,0,0,1,0,0,1,0000abcd,0,0,0,0,4800`nB,99,1,I,512,512,0,0,1,0,0,1,0000abcd,0,0,0,0,4800,7`n")
    & $add '读日志：列数少了、多了、取值不是整数的行都记作读不懂，不抛错' $true ($r.Unread -eq 3)
    $cases
}

function Get-AnalyzerCases([string]$RepoRoot, [string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $fake = { param([int]$exit, [hashtable]$v) [pscustomobject]@{ Exit = $exit; Err = 'x'; ParseError = ''; Json = [pscustomobject]@{ items = @($v.Keys | Sort-Object | ForEach-Object { [pscustomobject]@{ id = $_; verdict = $v[$_] } }); f1 = [pscustomobject]@{ ok = $false } } } }
    $sum = Get-ReaperAnalyzerSummary (& $fake 1 @{ A1 = 'PASS'; A4 = 'PASS'; A7 = 'FAIL'; A8 = 'PASS' }) -MustPass 'A1', 'A4', 'A8'
    & $add '分析器：不计入本格的项（A7）FAIL 不让这一格变红，摘要里照录并打星号' $false ($sum.Fails.Count -eq 0 -and $sum.Short.Count -eq 0 -and $sum.Text -match 'A7=FAIL\*' -and $sum.Text -match 'A1=PASS ')
    $sum = Get-ReaperAnalyzerSummary (& $fake 1 @{ A1 = 'FAIL'; A4 = 'PASS'; A8 = 'PASS' }) -MustPass 'A1', 'A4', 'A8'
    & $add '分析器：计入本格的项 FAIL 记进 Fails' $true (@($sum.Fails) -join ',' -ceq 'A1')
    $sum = Get-ReaperAnalyzerSummary (& $fake 3 @{ A1 = 'INCOMPLETE'; A4 = 'PASS'; A8 = 'PASS' }) -MustPass 'A1', 'A4', 'A8'
    & $add '分析器：计入本格的项数据不全记进 Short（这一格 INCOMPLETE，不当 PASS）' $true (@($sum.Short) -join ',' -ceq 'A1' -and $sum.Fails.Count -eq 0)
    $sum = Get-ReaperAnalyzerSummary (& $fake 3 @{ A4 = 'PASS'; A8 = 'PASS' }) -MustPass 'A1', 'A4', 'A8'
    & $add '分析器：计入本格的项在结果里根本没有也记进 Short' $true (@($sum.Short) -join ',' -ceq 'A1')
    $sum = Get-ReaperAnalyzerSummary ([pscustomobject]@{ Exit = 2; Err = '日志格式错误：日志#1 第 3 行 列 n'; ParseError = ''; Json = $null }) -MustPass 'A1'
    & $add '分析器：退出码 2（拒收真日志）记为 Rejected，并带上分析器给的文件序号、行号、列名' $true ($sum.Rejected -and $sum.Text -match '日志#1 第 3 行 列 n')
    $sum = Get-ReaperAnalyzerSummary ([pscustomobject]@{ Exit = 4; Err = '内部错误'; ParseError = ''; Json = $null }) -MustPass 'A1', 'A8'
    & $add '分析器：内部错误（退出码 4）不是拒收，要计入的项全记进 Short' $true (-not $sum.Rejected -and @($sum.Short).Count -eq 2)
    $sum = Get-ReaperAnalyzerSummary (& $fake 3 @{ A1 = 'INCOMPLETE'; A3 = 'PASS' })
    & $add '分析器：整轮那一次（没有要计入的项）只作记录，不打星号' $false ($sum.Text -match '只作记录' -and $sum.Text -notmatch '\*' -and $sum.Short.Count -eq 0)
    # 调用参数：脚本、目录、--json，每个非空的脱敏词各带一个 --redact（替身 .cmd 原样回显参数）
    $echo = Join-Path $Tmp 'fake-analyzer-echo.cmd'
    [IO.File]::WriteAllText($echo, "@echo %*`r`n@exit /b 0`r`n")
    $argsSeen = (Invoke-ReaperProbeAnalyzer -Python $echo -Script 'S.py' -Dir 'D' -Redact 'someone', '', 'somehost').Out.Trim()
    & $add "分析器：调用参数是 -I <脚本> <目录> --json，每个非空的脱敏词各带一个 --redact（$argsSeen）" $false ($argsSeen -ceq '-I S.py D --json --redact someone --redact somehost')
    $roundCtx = & (Get-Module Suite) { New-ReaperProbeRound ([pscustomobject]@{ ProbeLogDir = 'L'; Python = 'P'; Analyzer = 'A' }) ([pscustomobject]@{ group = 0 }) 'R' }
    & $add '分析器：一轮交给 --redact 的词是本机的用户名与主机名' $false (@($roundCtx.Redact) -ccontains $env:USERNAME -and @($roundCtx.Redact) -ccontains $env:COMPUTERNAME)
    # 比对行
    $nt = { param($bit, $lag, $whole, $head) "逐位相同：$bit`n对齐偏移（lag）：$lag 样本（正数表示 B 比 A 晚）`n整体残差（480000 帧）：峰值 $whole dBFS，RMS $whole dBFS`n开头 2.00 秒残差（96000 帧）：峰值 $head dBFS，RMS $head dBFS`n" }
    & $add '比对行：逐位相同的报告写成 case,variant,1,-inf,-inf,0' $false ((ConvertTo-ReaperCompareRow -Case 'CB-04' -Variant 'export-offline' -Out (& $nt '是' 0 '-inf' '-inf')) -ceq 'CB-04,export-offline,1,-inf,-inf,0')
    & $add '比对行：有差异的报告照实写残差与 lag' $false ((ConvertTo-ReaperCompareRow -Case 'CB-04' -Variant 'export-offline' -Out (& $nt '否' 3 '-96.5' '-101.25')) -ceq 'CB-04,export-offline,0,-96.5,-101.25,3')
    & $add '比对行：逐位相同却有残差（自相矛盾，分析器会判格式错）不写，返回空' $true ($null -eq (ConvertTo-ReaperCompareRow -Case 'CB-04' -Variant 'export-offline' -Out (& $nt '是' 0 '-90.0' '-inf')))
    & $add '比对行：报告里读不出 lag 不写，返回空' $true ($null -eq (ConvertTo-ReaperCompareRow -Case 'CB-04' -Variant 'export-offline' -Out ((& $nt '是' 0 '-inf' '-inf') -replace '对齐偏移[^\n]*\n', '')))
    # 契约对拍：不跑 Python，直接读分析器源码里的规格常量
    $py = [IO.File]::ReadAllText((Join-Path $RepoRoot 'scripts/probe/analyze-probe.py'))
    $head = [regex]::Match($py, '(?m)^COMPARE_HEAD = "([^"]+)"').Groups[1].Value
    & $add '契约：比对表头与 analyze-probe.py 的 COMPARE_HEAD 逐字相同' $false ($head -and $head -ceq (Get-ReaperCompareHead))
    $blockCols = [regex]::Match($py, 'BLOCK_COLS = \(\s*"([^"]+)"\.split\(\)').Groups[1].Value
    $mine = Get-ReaperProbeLogColumns
    $rowsOk = $blockCols -and ($mine.B -join ' ') -ceq $blockCols
    foreach ($m in [regex]::Matches($py, '(?m)^\s+"([HCSEZ])": "([^"]+)"\.split\(\),')) {
        if (($mine[$m.Groups[1].Value] -join ' ') -cne $m.Groups[2].Value) { $rowsOk = $false }
    }
    $seenTypes = @([regex]::Matches($py, '(?m)^\s+"([HCSEZ])": "([^"]+)"\.split\(\),') | ForEach-Object { $_.Groups[1].Value })
    & $add '契约：读日志用的各行列名与列序与 analyze-probe.py 的规格逐字相同（H C B S E Z）' $false ($rowsOk -and $seenTypes.Count -eq 5 -and $mine.Count -eq 6)
    $cases
}

function Get-RppProbeCases([string]$RepoRoot) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $dev = @(Get-ReaperPluginIdentities)
    $probe = @(Get-ReaperPluginIdentities -Probe)
    & $add '探针身份：两个探针插件的包名与类 ID，与正式的各不相同' $false ($probe.Count -eq 2 -and $probe[0].Bundle -ceq 'Rinne Probe Input.vst3' -and $probe[1].Bundle -ceq 'Rinne Probe Output.vst3' -and
        $probe[0].ClassId -cmatch '^[0-9A-F]{32}$' -and $probe[0].ClassId -cne $probe[1].ClassId -and $probe[0].ClassId -cne $dev[0].ClassId -and $probe[1].ClassId -cne $dev[1].ClassId)
    $decode = { param([string[]]$Lines) [Convert]::FromBase64String((($Lines[1..($Lines.Count - 2)]) -join '')) }
    $l0 = @(Get-ReaperProbeStateLines -Role input -Group 0)
    $lNone = @(Get-ReaperProbeStateLines -Role input)
    $b0 = & $decode $l0; $bNone = & $decode $lNone
    $diff = @(for ($i = 0; $i -lt $b0.Length; $i++) { if ($b0[$i] -ne $bNone[$i]) { $i } })
    & $add '探针状态：组 0 只改探针 state 的组字节（其余一个字节都不动），不设组时是 0xFF' $false ($b0.Length -eq $bNone.Length -and $diff.Count -eq 1 -and $diff[0] -eq 32 -and $b0[32] -eq 0 -and $bNone[32] -eq 0xFF)
    & $add '探针状态：每行不超过 128 个字符，首行是 REAPER 的插件头，末行与正式版相同' $false (@($l0 | Where-Object { $_.Length -gt 128 }).Count -eq 0 -and $l0[0] -match '^[A-Za-z0-9+/]+=*$' -and $l0[-1] -ceq 'AAAAAAAA')
    $lOut = @(Get-ReaperProbeStateLines -Role output -Group 7)
    & $add '探针状态：Output 的 role 字段是 2，组 7 写成 7' $false ((& $decode $lOut)[16] -eq 2 -and (& $decode $lOut)[32] -eq 7)
    $text = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -Probe -ProbeGroup 0
    $devText = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N'
    & $add '工程：-Probe 的链上只有两个探针插件（Input 在前），没有正式插件的类 ID' $false ($text.IndexOf('Rinne Probe Input') -lt $text.IndexOf('Rinne Probe Output') -and $text.IndexOf('Rinne Probe Input') -gt 0 -and
        $text -notmatch $dev[0].ClassId -and $text -notmatch $dev[1].ClassId -and $text -match $probe[0].ClassId)
    & $add '工程：正式的工程里没有探针插件' $false ($devText -notmatch 'Rinne Probe' -and $devText -notmatch $probe[0].ClassId)
    # 渲染速度（RENDER_1X）：0 全速离线；1 是 1x Offline（按实时速度跑，对插件仍报离线模式）；2 是 Online Render（对插件报实时模式）
    $speedOf = { param([string]$t) [regex]::Match($t, '(?m)^\s*RENDER_1X (\d+)\s*$').Groups[1].Value }
    $oneXText = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -OneX
    $onlineText = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -Online
    & $add '工程：渲染速度默认 0（全速离线）、-OneX 是 1（1x Offline）、-Online 是 2（Online Render）' $false ((& $speedOf $devText) -ceq '0' -and (& $speedOf $oneXText) -ceq '1' -and (& $speedOf $onlineText) -ceq '2')
    & $add '工程：-Online 与 -OneX 同时给抛错（两种速度只能选一个）' $true ($(try { $null = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -OneX -Online; $false } catch { $_.Exception.Message -like '*只能选一个*' }))
    $probeOnline = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -Probe -ProbeGroup 0 -Online
    & $add '工程：-Probe 与 -Online 一起给，链还是两个探针插件，渲染速度是 2' $false ((& $speedOf $probeOnline) -ceq '2' -and $probeOnline -match $probe[0].ClassId -and $probeOnline -notmatch $dev[0].ClassId)
    $saved = & (Get-Module Rpp) { $script:ProbePlugins.input.State }
    try {
        & (Get-Module Rpp) { $b = [Convert]::FromBase64String($script:ProbePlugins.input.State); $b[8 + 28] = 0x00; $b[8 + 29] = 0x00; $b[8 + 30] = 0x80; $b[8 + 31] = 0x3F; $script:ProbePlugins.input.State = [Convert]::ToBase64String($b) }
        & $add '探针状态：模板里 feedback 不是 0 就抛错（烟测一律用探针默认的 0）' $true ($(try { $null = Get-ReaperProbeStateLines -Role input -Group 0; $false } catch { $_.Exception.Message -like '*feedback 不是 0*' }))
        & (Get-Module Rpp) { $b = [Convert]::FromBase64String($script:ProbePlugins.input.State); $b[8] = 0x58; $script:ProbePlugins.input.State = [Convert]::ToBase64String($b) }
        & $add '探针状态：模板不是 abi 0 的探针 state（魔数不对）就抛错' $true ($(try { $null = Get-ReaperProbeStateLines -Role input -Group 0; $false } catch { $_.Exception.Message -like '*不是 abi 0 的探针 state*' }))
    } finally { & (Get-Module Rpp) { param($v) $script:ProbePlugins.input.State = $v } $saved }
    # 规则表与隐藏参数的编号
    $cpp = [IO.File]::ReadAllText((Join-Path $RepoRoot 'src/core/diag/ProbeLog.cpp'))
    $stmt = [regex]::Match($cpp, 'kCaseKeys\[\] =([^;]+);')
    $keys = -join @([regex]::Matches($stmt.Groups[1].Value, '"([^"]*)"') | ForEach-Object { $_.Groups[1].Value })
    $g3 = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -Probe -ProbeGroup 3
    $g3Missing = @(@(Get-ReaperProbeStateLines -Role input -Group 3) + @(Get-ReaperProbeStateLines -Role output -Group 3) | Where-Object { -not $g3.Contains($_) })
    & $add '工程：-ProbeGroup 写进两端探针 state 的组字节（两端的状态行与 Get-ReaperProbeStateLines 逐行相同）' $false ($g3Missing.Count -eq 0)
    & $add '规则表：与 src/core/diag/ProbeLog.cpp 的 kCaseKeys 逐字相同' $false ($stmt.Success -and $keys -ceq (Get-ReaperProbeCaseKeys))
    $ix = Get-ReaperProbeAutoIndex -Case 'CB-01' -Variant 'steady'
    $ex = Get-ReaperProbeAutoIndex -Case 'CB-04' -Variant 'export-offline'
    $rt = Get-ReaperProbeAutoIndex -Case 'CB-04' -Variant 'export-rt'
    $lu = Get-ReaperProbeAutoIndex -Case 'LU-01' -Variant '-'
    & $add '隐藏参数：按首现顺序编号（CB-01/steady = 1/1，CB-04/export-offline = 4/7，export-rt = 8，LU-01/- 的 variant 是 0），上限 21 / 33' $false (
        $ix.Case -eq 1 -and $ix.Variant -eq 1 -and $ex.Case -eq 4 -and $ex.Variant -eq 7 -and $rt.Variant -eq 8 -and $lu.Case -eq 13 -and $lu.Variant -eq 0 -and $ix.CaseMax -eq 21 -and $ix.VariantMax -eq 33)
    & $add '隐藏参数：表外的组合（CB-01 配 toggle）抛错，不凑合着写' $true ($(try { $null = Get-ReaperProbeAutoIndex -Case 'CB-01' -Variant 'toggle'; $false } catch { $_.Exception.Message -like '*不在探针的规则表里*' }))
    $cases
}

function Get-ProbeConfigCases([string]$RepoRoot, [string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $cfgPath = Join-Path $RepoRoot 'tests/reaper/scenarios.json'
    $mutated = { param($edit) $c = Get-ReaperSmokeConfig $cfgPath; & $edit $c; Test-ReaperScenarioConfig $c }
    $config = Get-ReaperSmokeConfig $cfgPath
    & $add 'probe 一节：至少有 48 kHz 的 64 / 512 / 1024 三格稳态与一格导出，全部经结构校验' $false ((Test-ReaperScenarioConfig $config).Count -eq 0 -and
        @($config.probe.scenarios | Where-Object { $_.kind -eq 'probe-play' -and $_.sr -eq 48000 -and $_.block -in 64, 512, 1024 }).Count -eq 3 -and @($config.probe.scenarios | Where-Object { $_.kind -eq 'probe-export' }).Count -ge 1)
    & $add 'probe 一节：探针场景写进第一段判红' $true ((& $mutated { param($c) $c.scenarios[0].kind = 'probe-play' }) -match '只能写在 probe 一节')
    & $add 'probe 一节：第一段的场景写进 probe 一节判红' $true ((& $mutated { param($c) $c.probe.scenarios[0].kind = 'render' }) -match 'probe 一节只能写')
    & $add 'probe 一节：缺 group 判红' $true ((& $mutated { param($c) $c.probe.PSObject.Properties.Remove('group') }) -match 'probe 一节缺字段 group')
    & $add 'probe 一节：group 不在 0..7 判红' $true ((& $mutated { param($c) $c.probe.group = 8 }) -match 'probe.group')
    & $add 'probe 一节：limits 缺 minBlocks 判红' $true ((& $mutated { param($c) $c.probe.limits.PSObject.Properties.Remove('minBlocks') }) -match 'probe.limits 缺 minBlocks')
    & $add 'probe 一节：表外的 (case, variant) 判红' $true ((& $mutated { param($c) $c.probe.scenarios[0].variant = 'toggle' }) -match '不在探针的规则表里')
    & $add 'probe 一节：recordSeconds 超出 1..3600 判红' $true (((& $mutated { param($c) $c.probe.scenarios[0].recordSeconds = 0 }) -match 'recordSeconds') -and ((& $mutated { param($c) $c.probe.scenarios[0].recordSeconds = 4000 }) -match 'recordSeconds'))
    & $add 'probe 一节：播放时长不短于记录窗口判红（Ping 要落在窗口里）' $true ((& $mutated { param($c) $c.probe.scenarios[0].playSeconds = $c.probe.scenarios[0].recordSeconds }) -match 'playSeconds 要短于')
    & $add 'probe 一节：场景的 group 不在 -1..7 判红' $true ((& $mutated { param($c) $c.probe.negative[0].group = 9 }) -match 'group 应为 -1')
    & $add 'probe 一节：analyzerMustPass 为空或不是 A1..A9 判红' $true (((& $mutated { param($c) $c.probe.scenarios[0].analyzerMustPass = @() }) -match 'analyzerMustPass') -and ((& $mutated { param($c) $c.probe.scenarios[0].analyzerMustPass = @('B1') }) -match 'analyzerMustPass'))
    & $add 'probe 一节：与第一段重复的 id 判红' $true ((& $mutated { param($c) $c.probe.scenarios[0].id = $c.scenarios[0].id }) -match 'id 重复')
    & $add 'probe 一节：探针反例 neg-probe-unpaired 不设组（-1），要求原因里有「没有配对」' $false (@($config.probe.negative | Where-Object { $_.id -eq 'neg-probe-unpaired' -and $_.group -eq -1 -and $_.expect -eq 'FAIL' -and $_.expectDetail -ceq '没有配对' }).Count -eq 1)
    & $add '一轮结局：场景全绿但探针日志目录里别人的文件变了判 FAIL' $true ((Get-RunVerdict -RowStatuses @('PASS') -CfgUnchanged $true -Residual 0 -DocsUnchanged $false) -eq 'FAIL')
    & $add '停手：探针日志目录里别人的文件变了，不再跑后续轮次' $true ((Get-ReaperRoundStop -CfgUnchanged $true -DocsUnchanged $false) -like '*探针日志目录*')
    & $add '停手：分析器拒收真日志，不再跑后续轮次' $true ((Get-ReaperRoundStop -CfgUnchanged $true -Rejected $true) -like '*拒收*')
    & $add '停手：默认资源目录变了，不再跑后续轮次' $true ((Get-ReaperRoundStop -CfgUnchanged $false) -like '*默认资源目录*')
    & $add '停手：都没变、也没拒收时接着跑' $false ((Get-ReaperRoundStop -CfgUnchanged $true -DocsUnchanged $true -Rejected $false) -ceq '')
    & $add '上下文：不给锁名时起 REAPER 持的是 gui' $false ((New-ReaperContext -ReaperExe x -NullTest x -WorkDir $Tmp -Driver x -Config ([pscustomobject]@{ limits = $null; signal = $null })).GuiLock -ceq 'gui')
    $cases
}

# 预检按风味：第二段只收探针构建、类 ID 按探针那一套比；另查 Python、分析器、文档目录
function Get-ProbePreflightCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $pwshExe = (Get-Process -Id $PID).Path
    $mi = { param($cid) "{`n  `"Classes`": [`n    {`n      `"CID`": `"$cid`",`n      `"Category`": `"Audio Module Class`",`n    },`n  ],`n}`n" }
    $root = Join-Path $Tmp 'probe-build'
    $ids = @(Get-ReaperPluginIdentities -Probe)
    $devIds = @(Get-ReaperPluginIdentities)
    $bundles = foreach ($e in $ids) { $d = Join-Path $root $e.Bundle; $null = New-Item -ItemType Directory -Force -Path (Join-Path $d 'Contents/Resources'); $d }
    $manifest = { param($i, $cid) [IO.File]::WriteAllText((Join-Path $bundles[$i] 'Contents/Resources/moduleinfo.json'), (& $mi $cid)) }
    & $manifest 0 $ids[0].ClassId; & $manifest 1 $ids[1].ClassId
    $info = { param($flavor) [IO.File]::WriteAllText((Join-Path $root 'rinne-build.json'), (@{ sha = 'abcdef0123'; flavor = $flavor; bundles = @($bundles) } | ConvertTo-Json)) }
    $fakePy = Join-Path $Tmp 'python-fake.cmd'; [IO.File]::WriteAllText($fakePy, "@echo Python 3.10.11`r`n")
    $oldPy = Join-Path $Tmp 'python-old.cmd'; [IO.File]::WriteAllText($oldPy, "@echo Python 2.7.18`r`n")
    $logs = Join-Path $Tmp 'probe-docs/Rinne Probe Logs'
    $marker = Join-Path $Tmp 'probe-USER_TESTING'
    $pre = { param($flavor = 'probe', $py = $fakePy, $dir = $logs, $analyzer = $null)
        $a = @{ ReaperExe = $pwshExe; BuildDir = $root; NullTest = $pwshExe; UserTestingMarker = $marker; Flavor = $flavor; Python = $py; ProbeLogDir = $dir }
        if ($analyzer) { $a.Analyzer = $analyzer }
        @((Get-ReaperPreflight @a).Reasons | Where-Object { $_ -notmatch '已有 reaper.exe' }) }
    & $info 'probe'
    $ok = @(& $pre)
    & $add "预检（第二段）：探针构建、类 ID 对、Python 3、分析器在、文档目录取得到时没有缺项（$($ok -join '；')）" $false ($ok.Count -eq 0)
    & $add '预检（第二段）：-Flavor probe 时 Python 版本照实读出' $false ((Get-ReaperPreflight -ReaperExe $pwshExe -BuildDir $root -NullTest $pwshExe -UserTestingMarker $marker -Flavor probe -Python $fakePy -ProbeLogDir $logs).PythonVersion -ceq '3.10.11')
    & $add '预检（第二段）：第一段（dev）拿探针构建来跑被拒' $true ((& $pre 'dev') -match '只测 dev')
    & $manifest 0 $devIds[0].ClassId
    & $add '预检（第二段）：探针包的清单里是正式插件的类 ID（产物混了）被拒' $true ((& $pre) -match '类 ID')
    & $manifest 0 $ids[0].ClassId
    & $info 'dev'
    & $add '预检（第二段）：-Probe 拿正式（dev）构建来跑被拒' $true ((& $pre) -match '第二段只测探针')
    & $info 'probe'
    & $add '预检（第二段）：找不到 Python 被拒' $true ((& $pre 'probe' (Join-Path $Tmp 'no-python.exe')) -match 'Python')
    & $add '预检（第二段）：解释器不是 Python 3 被拒' $true ((& $pre 'probe' $oldPy) -match '不是 Python 3')
    & $add '预检（第二段）：找不到 analyze-probe.py 被拒' $true ((& $pre 'probe' $fakePy $logs (Join-Path $Tmp 'no-analyzer.py')) -match 'analyze-probe.py')
    & $add '预检（第二段）：取不到文档目录被拒' $true ((& $pre 'probe' $fakePy '') -match '取不到用户文档目录')
    $denied = Join-Path $env:SystemDrive 'System Volume Information'
    $deniedOk = (Test-Path -LiteralPath $denied -PathType Container) -and $(try { $null = Get-ChildItem -LiteralPath $denied -Force -ErrorAction Stop; $false } catch { $true })
    & $add "预检（第二段）：探针日志目录列不出清单被拒（看不见不能当作没变）$($deniedOk ? '' : '（本机找不到拒绝访问的目录，这条没能真跑）')" $true ((-not $deniedOk) -or ((& $pre 'probe' $fakePy $denied) -match '列不出'))
    & $add '预检（第一段）：不查 Python 与文档目录' $false (@((Get-ReaperPreflight -ReaperExe $pwshExe -BuildDir $root -NullTest $pwshExe -UserTestingMarker $marker -Flavor dev -Python (Join-Path $Tmp 'no-python.exe') -ProbeLogDir '').Reasons | Where-Object { $_ -match 'Python|文档目录' }).Count -eq 0)
    $cases
}

# 一格的结论（Complete-ProbeCell）：收日志出错、REAPER 没正常结束、分析器拒收、自己的判据、计入本格的分析器项，各走各的结局。
# 分析器用替身（.cmd 原样吐出准备好的输出、按给定的退出码退出），不跑 Python。
function Get-CompleteCellCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $root = Join-Path $Tmp 'complete-cell'
    $null = New-Item -ItemType Directory -Force -Path $root
    $judge = {
        param([int]$Exit = 1, [string]$Verdicts = 'A1=PASS A4=PASS A8=PASS', [long[]]$Pings = @(4800), $Moves = @(), [string[]]$Whys = @(), [string[]]$ExtraBad = @())
        $round = Join-Path $root ([guid]::NewGuid().ToString('N').Substring(0, 8)); $cellDir = Join-Path $round 'cell'
        $null = New-Item -ItemType Directory -Force -Path $cellDir
        [IO.File]::WriteAllText((Join-Path $cellDir 'probe-I-4242-abcd.csv'), (New-FakeProbeLogText -Role I -Pings $Pings))
        [IO.File]::WriteAllText((Join-Path $cellDir 'probe-O-4242-ef01.csv'), (New-FakeProbeLogText -Role O -Pings @()))
        $items = @($Verdicts -split ' ' | ForEach-Object { $p = $_ -split '='; '{{"id":"{0}","verdict":"{1}"}}' -f $p[0], $p[1] }) -join ','
        $outFile = Join-Path $round 'fake-out.json'
        [IO.File]::WriteAllText($outFile, ($Exit -eq 2 ? '' : "{`"items`":[$items],`"f1`":{`"ok`":false}}"))
        $fake = Join-Path $round 'fake-analyzer.cmd'
        [IO.File]::WriteAllText($fake, "@type `"$outFile`"`r`n" + ($Exit -eq 2 ? "@echo format error: log 1 line 3 column n 1>&2`r`n" : '') + "@exit /b $Exit`r`n")
        $ctx = @{ Probe = @{ RoundDir = $round; Python = $fake; Analyzer = 'unused.py'; Redact = @(); Analyses = [ordered]@{}; Rejected = $false } }
        $expect = @{ Pid = 4242; Case = 'CB-01'; Variant = 'steady'; Sr = 48000; Block = 512; MinBlocks = 32; NeedPing = $true }
        $a = @{ Ctx = $ctx; Id = 'cell'; CellDir = $cellDir; Moves = $Moves; Whys = $Whys; Expects = @($expect); MustPass = @('A1', 'A4', 'A8'); ExtraBad = $ExtraBad }
        $o = & (Get-Module Scenarios) { param($splat) Complete-ProbeCell @splat } $a
        [pscustomobject]@{ Status = $o.Status; Detail = $o.Detail; Rejected = $ctx.Probe.Rejected; Raw = (Test-Path -LiteralPath (Join-Path $round 'analyze-cell.json')) }
    }
    $ok = & $judge
    & $add "一格的结论：日志对、计入的分析器项都 PASS 时 PASS，分析器原始输出存下（$($ok.Status)：$($ok.Detail)）" $false ($ok.Status -eq 'PASS' -and $ok.Raw -and $ok.Detail -match 'Ping 4800 样本 / D 4800 样本')
    $r = & $judge -Moves @([pscustomobject]@{ Problems = @('probe-I-1-abcd.csv：复制后哈希不同') })
    & $add '一格的结论：收日志出错判 FAIL，不再往下读' $true ($r.Status -eq 'FAIL' -and $r.Detail -like '收日志出错*哈希不同*' -and -not $r.Raw)
    $r = & $judge -Whys @('超时（120 s）')
    & $add '一格的结论：REAPER 这一趟没正常结束判 INCOMPLETE，原因照录' $true ($r.Status -eq 'INCOMPLETE' -and $r.Detail -like '*超时（120 s）*')
    $r = & $judge -Exit 2
    & $add '一格的结论：分析器拒收真日志判 INCOMPLETE，写明契约对不上，并让整套停下' $true ($r.Status -eq 'INCOMPLETE' -and $r.Detail -like '*拒收真日志*契约对不上*format error*' -and $r.Rejected)
    $r = & $judge -Pings @(4801)
    & $add '一格的结论：日志里的判据不过（Ping 不等于 D）判 FAIL' $true ($r.Status -eq 'FAIL' -and $r.Detail -like '*Ping 求出的延迟是 4801*')
    $r = & $judge -ExtraBad @('离线对在线对拍不通过')
    & $add '一格的结论：另给的不通过原因（对拍）判 FAIL' $true ($r.Status -eq 'FAIL' -and $r.Detail -like '离线对在线对拍不通过*')
    $r = & $judge -Verdicts 'A1=FAIL A4=PASS A8=PASS'
    & $add '一格的结论：计入本格的分析器项 FAIL 判 FAIL' $true ($r.Status -eq 'FAIL' -and $r.Detail -like '分析器判出不通过（A1）*')
    $r = & $judge -Exit 3 -Verdicts 'A1=PASS A4=INCOMPLETE A8=PASS'
    & $add '一格的结论：计入本格的分析器项数据不全判 INCOMPLETE（不当 PASS）' $true ($r.Status -eq 'INCOMPLETE' -and $r.Detail -like '*没下结论（A4）*')
    $r = & $judge -Verdicts 'A1=PASS A4=PASS A7=FAIL A8=PASS'
    & $add '一格的结论：不计入本格的分析器项（A7）FAIL 不让这一格变红' $false ($r.Status -eq 'PASS' -and $r.Detail -match 'A7=FAIL\*')
    $cases
}

# 进程采样遇到进程刚退出：只认这一种情况，原因照实写；别的错误照常抛
function Get-SampleCases {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $p = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path, '-NoProfile -NonInteractive -Command exit 0'))
    $p.WaitForExit()
    $s = try { Get-ReaperProcessSample $p } catch { $null }
    & $add '采样：进程已经退出时不抛错，记作 Exited（句柄与内存为空，不是 0）' $true ($s -and $s.Exited -and $null -eq $s.Handles -and $null -eq $s.PrivateMB)
    $live = Get-ReaperProcessSample (Get-Process -Id $PID)
    & $add '采样：活着的进程读得出句柄数与内存' $false (-not $live.Exited -and $live.Handles -gt 0 -and $live.PrivateMB -gt 0)
    & $add '采样：没有关联进程的对象照常抛错（不当作「进程在采样前退出」）' $true ($(try { $null = Get-ReaperProcessSample ([Diagnostics.Process]::new()); $false } catch { $true }))
    $marks = [ordered]@{ warm = $live }
    & $add '采样：要的标记都取到时没有问题' $false ($null -eq (Get-ReaperMarkProblem -Samples ([ordered]@{ warm = $live; loop = $live }) -Need 'warm', 'loop'))
    & $add '采样：标记因为进程在采样前退出没取到，原因照实写「进程在采样前退出」' $true ((Get-ReaperMarkProblem -Samples $marks -LostAt 'loop' -Need 'warm', 'loop') -like '进程在采样前退出（loop 处*')
    & $add '采样：标记没取到又不是因为进程退出，写「没取到」，不冒充进程退出' $true ((Get-ReaperMarkProblem -Samples $marks -Need 'warm', 'loop') -like '没取到 loop 处*')
    $cases
}

# ---- 替身（子进程自测的接线用例也用）：分析器、rinne_nulltest、探针构建目录 ----

# 假的 Python：`--version` 报 3.10.11，别的参数一律原样吐出准备好的 JSON 并按给定退出码退出（不跑真的分析器）
function New-FakePython {
    param([Parameter(Mandatory)][string]$Dir, [int]$ExitCode = 1, [string]$Json = '')
    if (-not $Json) {
        $Json = '{"items":[{"id":"A1","verdict":"PASS"},{"id":"A3","verdict":"PASS"},{"id":"A4","verdict":"PASS"},{"id":"A7","verdict":"FAIL"},{"id":"A8","verdict":"PASS"}],"f1":{"ok":false}}'
    }
    $null = New-Item -ItemType Directory -Force -Path $Dir
    $jsonFile = Join-Path $Dir 'fake-analyzer.json'
    [IO.File]::WriteAllText($jsonFile, $Json)
    $cmd = Join-Path $Dir 'python-fake.cmd'
    [IO.File]::WriteAllText($cmd, "@echo off`r`nif `"%~1`"==`"--version`" (`r`n  echo Python 3.10.11`r`n  exit /b 0`r`n)`r`ntype `"$jsonFile`"`r`nexit /b $ExitCode`r`n")
    $analyzer = Join-Path $Dir 'analyze-fake.py'
    [IO.File]::WriteAllText($analyzer, "# fake`n")
    [pscustomobject]@{ Python = $cmd; Analyzer = $analyzer }
}

# 比对报告的文本（格式与 rinne_nulltest compare 的输出相同，ConvertTo-ReaperCompareRow 与 Get-CompareVerdict 读它）
function Get-FakeCompareText {
    param([string]$Bit = '是', [string]$Lag = '0', [string]$Whole = '-inf', [string]$Head = '-inf')
    "逐位相同：$Bit`n对齐偏移（lag）：$Lag 样本（正数表示 B 比 A 晚）`n整体残差（480000 帧）：峰值 $Whole dBFS，RMS $Whole dBFS`n开头 2.00 秒残差（96000 帧）：峰值 $Head dBFS，RMS $Head dBFS`n"
}

# 假的 rinne_nulltest：make-signal 造一个空文件，compare 吐出环境变量 FAKE_COMPARE_FILE 指的报告（没设就吐一份逐位相同的）
function New-FakeNullTest {
    param([Parameter(Mandatory)][string]$Dir)
    $null = New-Item -ItemType Directory -Force -Path $Dir
    $same = Join-Path $Dir 'compare-same.txt'
    [IO.File]::WriteAllText($same, (Get-FakeCompareText), [Text.UTF8Encoding]::new($false))
    $cmd = Join-Path $Dir 'nulltest-fake.cmd'
    [IO.File]::WriteAllText($cmd, "@echo off`r`nif `"%~1`"==`"make-signal`" (`r`n  type nul > `"%~3`"`r`n  exit /b 0`r`n)`r`nif `"%~1`"==`"compare`" (`r`n  if defined FAKE_COMPARE_FILE (type `"%FAKE_COMPARE_FILE%`") else (type `"$same`")`r`n  exit /b 0`r`n)`r`nexit /b 1`r`n")
    $cmd
}

# 合成一份探针构建目录：rinne-build.json（flavor 为 probe）和两个带类 ID 清单的 VST3 包，预检和入口接线用例用
function New-FakeProbeBuild {
    param([Parameter(Mandatory)][string]$Root)
    $bundles = foreach ($id in @(Get-ReaperPluginIdentities -Probe)) {
        $bundle = Join-Path $Root $id.Bundle
        $res = Join-Path $bundle 'Contents/Resources'; $bin = Join-Path $bundle 'Contents/x86_64-win'
        $null = New-Item -ItemType Directory -Force -Path $res, $bin
        [IO.File]::WriteAllText((Join-Path $res 'moduleinfo.json'), "{`n  `"Classes`": [`n    {`n      `"CID`": `"$($id.ClassId)`",`n      `"Category`": `"Audio Module Class`",`n    },`n  ],`n}`n")
        [IO.File]::WriteAllText((Join-Path $bin $id.Bundle), 'bits')
        $bundle
    }
    [IO.File]::WriteAllText((Join-Path $Root 'rinne-build.json'), (@{ sha = 'abcdef0123'; flavor = 'probe'; bundles = @($bundles) } | ConvertTo-Json))
    [pscustomobject]@{ Root = $Root; Bundles = @($bundles) }
}

# 整轮合并分析（Invoke-ReaperProbeWhole）：反例格（Excluded）的日志不并进 all-logs，各格的日志与比对行都并进去
function Get-WholeCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $root = Join-Path $Tmp 'whole'
    $fake = New-FakePython -Dir (Join-Path $root 'fake')
    $round = Join-Path $root 'round'
    $put = { param($cell, $name, $text) $d = Join-Path $round "probe-logs/$cell"; $null = New-Item -ItemType Directory -Force -Path $d; [IO.File]::WriteAllText((Join-Path $d $name), $text) }
    & $put 'cell-a' 'probe-I-11-aaaa.csv' (New-FakeProbeLogText -Role I -LogPid 11)
    & $put 'cell-a' 'probe-O-11-bbbb.csv' (New-FakeProbeLogText -Role O -LogPid 11 -Pings @())
    & $put 'cell-a' 'compare-reaper.csv' "case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples`nCB-04,export-offline,1,-inf,-inf,0`n"
    & $put 'neg-cell' 'probe-I-12-cccc.csv' (New-FakeProbeLogText -Role I -LogPid 12 -PeerPid 0)
    & $put 'neg-cell' 'probe-O-12-dddd.csv' (New-FakeProbeLogText -Role O -LogPid 12 -PeerPid 0 -Pings @())
    $probe = @{ RoundDir = $round; Python = $fake.Python; Analyzer = $fake.Analyzer; Redact = @(); Excluded = [Collections.Generic.List[string]]::new(); Rejected = $false }
    $probe.Excluded.Add('neg-cell')
    $w = & (Get-Module Suite) { param($p) Invoke-ReaperProbeWhole $p } $probe
    $names = @(Get-ChildItem -LiteralPath (Join-Path $round 'all-logs') -File | ForEach-Object { $_.Name } | Sort-Object)
    & $add "整轮分析：反例格（Excluded）的日志不并进 all-logs，别的格的日志与比对行都并进去（$($names -join ' ')）" $true (
        ($names -join '|') -ceq 'compare-cell-a-reaper.csv|probe-I-11-aaaa.csv|probe-O-11-bbbb.csv' -and @($w.cells) -join ',' -ceq 'cell-a' -and @($w.excluded) -join ',' -ceq 'neg-cell')
    & $add '整轮分析：原始输出写进 analyze.json，退出码照实记' $false ((Test-Path -LiteralPath (Join-Path $round 'analyze.json')) -and $w.exit -eq 1 -and $w.summary -like '*只作记录*')
    $probeNone = @{ RoundDir = (Join-Path $root 'round2'); Python = $fake.Python; Analyzer = $fake.Analyzer; Redact = @(); Excluded = [Collections.Generic.List[string]]::new(); Rejected = $false }
    $d2 = Join-Path $root 'round2/probe-logs/only-neg'; $null = New-Item -ItemType Directory -Force -Path $d2
    [IO.File]::WriteAllText((Join-Path $d2 'probe-I-13-eeee.csv'), (New-FakeProbeLogText -Role I -LogPid 13))
    $probeNone.Excluded.Add('only-neg')
    $w2 = & (Get-Module Suite) { param($p) Invoke-ReaperProbeWhole $p } $probeNone
    & $add '整轮分析：只剩反例格时没有日志可分析，不起分析器、不建 all-logs' $false ($null -eq $w2.exit -and $w2.summary -like '没有日志可分析*' -and -not (Test-Path -LiteralPath (Join-Path $root 'round2/all-logs')))
    $cases
}

function Get-ProbeSelfTestCases {
    param([Parameter(Mandatory)][string]$Tmp, [Parameter(Mandatory)][string]$RepoRoot)
    $all = [Collections.Generic.List[object]]::new()
    foreach ($fn in { Get-CollectCases $Tmp }, { Get-CellCases }, { Get-AnalyzerCases $RepoRoot $Tmp }, { Get-RppProbeCases $RepoRoot }, { Get-ProbeConfigCases $RepoRoot $Tmp },
                    { Get-ProbePreflightCases $Tmp }, { Get-CompleteCellCases $Tmp }, { Get-WholeCases $Tmp }, { Get-SampleCases }) {
        foreach ($c in (& $fn)) { $all.Add($c) }
    }
    $all
}

Export-ModuleMember -Function Get-ProbeSelfTestCases, New-FakeProbeLogText, New-FakePython, New-FakeNullTest, New-FakeProbeBuild, Get-FakeCompareText
