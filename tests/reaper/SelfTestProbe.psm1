# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 烟测第二段（探针）的自测用例：不启动 REAPER。SelfTest.psm1 调 Get-ProbeSelfTestCases，结果并进同一张表。
# 每个用例是 (标签, 是否反例, 是否按预期)：反例（Bad）是「必须被拦住 / 必须判红」的情形，Passed 表示它确实被拦住了。
#   收日志：别的 pid、名字不对（含 pid 补零）、时间不对（早于启动或晚于现在）、哈希不对、目录与联接、硬链接、子目录里的、进程还在、
#           目标已有同名、列目录之后原文件变了、被别的程序占着，一律不删；钉住期间别人追加、覆盖、改名、删除都被拒；删的就是复制出去的那个对象
#   清单：本轮新出现的自己的文件之外，增、删、改任何一项都判变；开头就在清单里的名字，哪怕是「自己的」也照常比；
#         值里记重解析点标记与硬链接数（换成同大小同时间的链接也看得出来）；落盘的清单与变化报告不留别人的文件名，只留每轮随机盐的散列
#   写日志的进程还在不在：只按 pid 查会被 pid 复用骗到，所以再比持有者的开始时间与日志最后一次写入的时刻（不和启动时刻比）
#   pid 没人持有时，同号的别的进程（Cubase 里的探针）写的日志靠钉住之后的两道判据挡住：H 行的 host_kind 要是 other、role / pid / tok4 与文件名一致，
#   创建时间要落在这次启动到它退出（加余量）之间；H 行读不出也不动。清单里硬链接数一侧取不到（?）时只比前三段、不判变、另记提示
#   块长上限逐块比 n 与该块自己的 n_decl（两次 prepare 时 n_decl 不止一个值）
#   -Python：路径或 PATH 上的命令名；停走带后留给自动 Ping 的时间与 limits.afterRenderSeconds（驱动里同一个值）
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
          [int]$NonRtFrom = -1, [int]$NonRtTo = -1, # NonRtFrom..NonRtTo-1 号块的 nonrt 是 1，别的块取 NonRt（造「播放中的块 nonrt 不一致」）
          [string]$Tok4 = 'abcd', [ValidateSet('cubase', 'luna', 'other')][string]$HostKind = 'other',
          [int]$SecondFrom = -1, [int]$N2 = -1, [int]$NDecl2 = -1) # 第 SecondFrom 块起 n 取 N2、n_decl 取 NDecl2（造「两次 prepare，n_decl 不止一个值」）
    $peer = ($PeerPid -lt 0) ? $LogPid : $PeerPid
    $sb = [Text.StringBuilder]::new()
    $null = $sb.Append("rinne-probe-log,1`nH,$Role,vst3,$HostKind,win,x64,$LogPid,1,$Tok4,00000000`nE,10,prepare,$Sr,$NDecl`nC,100,$Case,$Variant,-,na,$Sr`n")
    if ($SecondFrom -ge 0) { $null = $sb.Append("E,150,prepare,$Sr,$NDecl2`n") }
    if ($PeerPid -ne 0) { $null = $sb.Append("E,101,paired,$peer,A`n") }
    $seq = 0
    for ($k = 0; $k -lt $Blocks; $k++) {
        if ($k -eq $GapAt) { $seq++ }
        $second = ($SecondFrom -ge 0 -and $k -ge $SecondFrom)
        $nHere = ($second -and $N2 -ge 0) ? $N2 : $N
        $declHere = ($second -and $NDecl2 -ge 0) ? $NDecl2 : $NDecl
        $nBlock = ($k -eq $NOverrideAt) ? $NOverride : $nHere # 不叫 $n：PowerShell 的变量名不分大小写，会把参数 $N 改掉
        $nonRtHere = ($k -ge $NonRtFrom -and $k -lt $NonRtTo) ? 1 : $NonRt
        $null = $sb.Append("B,$seq,$(2 * $k + 1),$Role,$nBlock,$declHere,0,0,$([int]($k -ge $PlayingFrom)),$nonRtHere,0,$(200 + $k),0000abcd,0,$($k * $N),0,0,$D`n")
        $seq++
    }
    $null = $sb.Append("S,5000,$Blocks,$Dropped,$N,0,0`n")
    if ($Role -eq 'I') { foreach ($p in $Pings) { $null = $sb.Append("E,6000,ping,$p,`n") } }
    if (-not $NoClose) { $null = $sb.Append("Z,7000,$Blocks,$Dropped`n") }
    $sb.ToString()
}

# 按文件名造一份头两行合格的日志文本（H 行与名字一致、host_kind 默认 other）。Mark 是之后一行的内容，只为区分不同文件。
function New-LogForName {
    param([Parameter(Mandatory)][string]$Name, [string]$Mark = 'x', [string]$HostKind = 'other')
    $m = [regex]::Match($Name, '^probe-([IO])-([0-9]+)-([0-9a-f]{4})\.csv$')
    if (-not $m.Success) { throw "不是日志名：$Name" }
    "rinne-probe-log,1`nH,$($m.Groups[1].Value),vst3,$HostKind,win,x64,$($m.Groups[2].Value),1,$($m.Groups[3].Value),00000000`nE,10,note,$Mark,`n"
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
    $own = @{ "probe-I-$gone-abcd.csv" = (New-LogForName "probe-I-$gone-abcd.csv" 'own-in'); "probe-O-$gone-ef01.csv" = (New-LogForName "probe-O-$gone-ef01.csv" 'own-out') }
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
    & $add '清单：子目录也记一项（dir），子目录里的文件记大小、修改时间、重解析点标记与硬链接数' $false ($before['sub'] -ceq 'dir' -and $before[$subFile] -cmatch '^[0-9]+\|[0-9]+\|0\|1$' -and $before["probe-I-$gone-beef.csv"] -ceq 'dir')
    # 哈希不同：复制件被换了内容，原文件不删、判红
    $dirH = Join-Path $Tmp 'logs-hash'; $null = New-Item -ItemType Directory -Path $dirH
    $fH = Join-Path $dirH "probe-I-$gone-1111.csv"; [IO.File]::WriteAllText($fH, (New-LogForName "probe-I-$gone-1111.csv" 'real'))
    $mH = Move-ReaperProbeLogs -LogDir $dirH -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-hash') -AfterCopy { param($src, $dst) [IO.File]::WriteAllText($dst, "corrupted`n") }
    & $add '收日志：复制件的 sha256 与原文件不同就不删原文件，并判红' $true ((Test-Path -LiteralPath $fH) -and $mH.Moved.Count -eq 0 -and @($mH.Problems | Where-Object { $_ -like '*哈希不同*' }).Count -eq 1)
    # 列目录之后、钉住之前原文件变了：大小变了、只是修改时间变了，都不动
    $dirC = Join-Path $Tmp 'logs-changed'; $null = New-Item -ItemType Directory -Path $dirC
    $fC = Join-Path $dirC "probe-O-$gone-2222.csv"; [IO.File]::WriteAllText($fC, (New-LogForName "probe-O-$gone-2222.csv" 'real'))
    $mC = Move-ReaperProbeLogs -LogDir $dirC -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-changed') -BeforePin { param($src) [IO.File]::AppendAllText($src, "more`n") }
    & $add '收日志：列目录之后原文件变长了就不删，并判红' $true ((Test-Path -LiteralPath $fC) -and $mC.Moved.Count -eq 0 -and @($mC.Problems | Where-Object { $_ -like '*列目录之后原文件变了*' }).Count -eq 1)
    $dirM = Join-Path $Tmp 'logs-touched'; $null = New-Item -ItemType Directory -Path $dirM
    $fM = Join-Path $dirM "probe-O-$gone-2323.csv"; [IO.File]::WriteAllText($fM, (New-LogForName "probe-O-$gone-2323.csv" 'real'))
    $mM = Move-ReaperProbeLogs -LogDir $dirM -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp 'dest-touched') -BeforePin { param($src) (Get-Item -LiteralPath $src).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(-5) }
    & $add '收日志：列目录之后只是修改时间变了（大小没变）也不删，并判红' $true ((Test-Path -LiteralPath $fM) -and $mM.Moved.Count -eq 0 -and @($mM.Problems | Where-Object { $_ -like '*列目录之后原文件变了*' }).Count -eq 1)
    # 列目录之后、钉住之前，原文件被同长度、同修改时间的另一个文件顶替：复制出去的、核对的、删掉的，是同一个（钉住的那个）对象
    $dirS = Join-Path $Tmp 'logs-swap'; $null = New-Item -ItemType Directory -Path $dirS
    $fS = Join-Path $dirS "probe-I-$gone-3434.csv"; [IO.File]::WriteAllText($fS, (New-LogForName "probe-I-$gone-3434.csv" 'real'))
    $destS = Join-Path $Tmp 'dest-swap'
    $mS = Move-ReaperProbeLogs -LogDir $dirS -Pids @($gone) -SinceUtc $since -DestDir $destS -BeforePin {
        param($src) $t = (Get-Item -LiteralPath $src).LastWriteTimeUtc; Remove-Item -LiteralPath $src; [IO.File]::WriteAllText($src, (New-LogForName (Split-Path -Leaf $src) 'SWAP')); (Get-Item -LiteralPath $src).LastWriteTimeUtc = $t
    }
    $copyS = Join-Path $destS "probe-I-$gone-3434.csv"
    & $add '收日志：钉住之前原文件被同长度同时间的另一个文件顶替：复制出去的正是被删掉的那个，原文件没了、没有问题' $true ($mS.Moved.Count -eq 1 -and -not (Test-Path -LiteralPath $fS) -and (Test-Path -LiteralPath $copyS) -and [IO.File]::ReadAllText($copyS) -ceq (New-LogForName "probe-I-$gone-3434.csv" 'SWAP') -and $mS.Problems.Count -eq 0)
    # 钉住期间别人动不了原文件：追加、覆盖、改名、删除都被拒（共享方式只放行读），正常搬走不受影响
    $dirA = Join-Path $Tmp 'logs-attack'; $null = New-Item -ItemType Directory -Path $dirA
    $fA = Join-Path $dirA "probe-O-$gone-4545.csv"; [IO.File]::WriteAllText($fA, (New-LogForName "probe-O-$gone-4545.csv" 'real'))
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
    & $add '收日志：被攻击也不影响正常搬走：复制件与原文件内容相同、原文件删了' $false ($mA.Moved.Count -eq 1 -and $mA.Problems.Count -eq 0 -and -not (Test-Path -LiteralPath $fA) -and [IO.File]::ReadAllText((Join-Path $Tmp "dest-attack/probe-O-$gone-4545.csv")) -ceq (New-LogForName "probe-O-$gone-4545.csv" 'real'))
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
    & $add '一格：有块长超过它自己声明的最大块长判红' $true (& $hit (& $cell -In @{ NOverride = 1024; NOverrideAt = 5 }) '*1 块的块长超过它自己声明的最大块长*')
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
    $spareMsg = '*停走带之后留给自动 Ping 的时间*'
    & $add 'probe 一节：播放时长不短于记录窗口判红（停走带之后留给 Ping 的时间不够）' $true ((& $mutated { param($c) $c.probe.scenarios[0].playSeconds = $c.probe.scenarios[0].recordSeconds }) -like $spareMsg)
    # 停走带之后留给自动 Ping 的时间 = recordSeconds + marginSeconds - leadSeconds - playSeconds，不少于 limits.afterRenderSeconds（驱动在导出格里渲染返回后再等的同一个值）
    $firstPlay = @($config.probe.scenarios | Where-Object { $_.kind -eq 'probe-play' })[0]
    $spareNow = [double]$firstPlay.recordSeconds + [double]$config.probe.limits.marginSeconds - [double]$config.probe.limits.leadSeconds - [double]$firstPlay.playSeconds
    & $add "probe 一节：afterRenderSeconds 是 6.5，现有的稳态格恰好留 $spareNow s，全部过校验" $false (
        [double]$config.probe.limits.afterRenderSeconds -eq 6.5 -and $spareNow -eq 6.5 -and @($config.probe.scenarios | Where-Object { $_.kind -eq 'probe-play' -and ([double]$_.playSeconds -ne 8 -or [int]$_.recordSeconds -ne 14) }).Count -eq 0)
    $r9 = & $mutated { param($c) $c.probe.scenarios[0].playSeconds = 9 }
    & $add 'probe 一节：播放 9 s（停走带后只剩 5.5 s，不够 6.5 s）判红，文案写出实际余量与要求，且不再说「Ping 也要落在记录窗口里」' $true (
        ($r9 -like $spareMsg) -and ($r9 -like '*只有 5.5 s*') -and ($r9 -like '*afterRenderSeconds 的 6.5 s*') -and -not ($r9 -like '*落在记录窗口*'))
    & $add 'probe 一节：播放 8.1 s（余量 6.4 s）判红，播放 8 s（余量恰为 6.5 s）不红' $true (
        ((& $mutated { param($c) $c.probe.scenarios[0].playSeconds = 8.1 }) -like $spareMsg) -and ((& $mutated { param($c) $c.probe.scenarios[0].playSeconds = 8 }).Count -eq 0))
    & $add 'probe 一节：余量里算上 marginSeconds 与 leadSeconds（播放 12 s 配 margin 6 s 刚好够；lead 加到 3.5 s 就不够）' $true (
        ((& $mutated { param($c) $c.probe.scenarios[0].playSeconds = 12; $c.probe.limits.marginSeconds = 6 }).Count -eq 0) -and
        ((& $mutated { param($c) $c.probe.limits.leadSeconds = 3.5 }) -like $spareMsg))
    & $add 'probe 一节：要的秒数读场景表的 afterRenderSeconds，不是写死的 6.5（改成 7 现有格不够，改成 6 就够）' $true (
        ((& $mutated { param($c) $c.probe.limits.afterRenderSeconds = 7 }) -like '*afterRenderSeconds 的 7 s*') -and ((& $mutated { param($c) $c.probe.limits.afterRenderSeconds = 6 }).Count -eq 0))
    & $add 'probe 一节：limits 缺 afterRenderSeconds、或它不是数判红' $true (
        ((& $mutated { param($c) $c.probe.limits.PSObject.Properties.Remove('afterRenderSeconds') }) -like '*probe.limits 缺 afterRenderSeconds*') -and ((& $mutated { param($c) $c.probe.limits.afterRenderSeconds = 'abc' }) -like '*afterRenderSeconds 应为不小于 0 的数*'))
    & $add 'probe 一节：playSeconds 不是数判红（不抛错）' $true ((& $mutated { param($c) $c.probe.scenarios[0].playSeconds = 'abc' }) -like '*playSeconds 应为不小于 0 的数*')
    # 单一真源：任务里的 afterRenderSeconds 就是场景表的那个值；Lua 驱动里不再写字面量，读 job.afterRenderSeconds
    $jobCtx = New-ReaperContext -ReaperExe x -NullTest x -WorkDir $Tmp -Driver x -Config ([pscustomobject]@{ limits = [pscustomobject]@{ leadSeconds = 1.5; marginSeconds = 2; afterRenderSeconds = 7.25 }; signal = $null })
    $job = & (Get-Module Scenarios) { param($c, $s) Get-ProbeLuaJob $c $s 'steady' 'x.rpp' } $jobCtx $firstPlay
    $jobReal = & (Get-Module Scenarios) { param($c, $s) Get-ProbeLuaJob $c $s 'steady' 'x.rpp' } (New-ReaperContext -ReaperExe x -NullTest x -WorkDir $Tmp -Driver x -Config $config.probe) $firstPlay
    & $add 'Lua 任务：afterRenderSeconds 取自场景表 limits（7.25 进 7.25；现有场景表进 6.5）' $false ($job.afterRenderSeconds -eq 7.25 -and $jobReal.afterRenderSeconds -eq [double]$config.probe.limits.afterRenderSeconds)
    $lua = [IO.File]::ReadAllText((Join-Path $RepoRoot 'tests/reaper/lua/driver.lua'))
    & $add 'Lua 驱动：渲染返回后再等的秒数读 job.afterRenderSeconds，脚本里没有写死的数字（没有 afterRenderSeconds = <数>，也没有 6.5）' $true (
        ($lua -match 'job\.afterRenderSeconds') -and ($lua -notmatch 'afterRenderSeconds\s*=\s*[0-9]') -and ($lua -notmatch '(?<![0-9.])6\.5(?![0-9])'))
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

# 清单的值与落盘：值里记重解析点标记与硬链接数；落盘的清单与变化报告不留别人的文件名
function Get-ListingCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $dir = Join-Path $Tmp 'listing'
    $null = New-Item -ItemType Directory -Force -Path $dir
    $stamp = [DateTime]::new(2026, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
    $mk = { param($path, $text) [IO.File]::WriteAllText($path, $text); (Get-Item -LiteralPath $path).LastWriteTimeUtc = $stamp }
    $field = { param($v, $i) @($v -split '\|')[$i] }
    # 值的写法（纯函数）
    & $add '清单值：文件写成 大小|修改时间刻度|重解析点(0/1)|硬链接数' $false ((Format-ReaperListingValue -IsDir $false -Length 6 -Ticks 99 -IsReparse $false -Links 1) -ceq '6|99|0|1')
    & $add '清单值：重解析点写 1，硬链接数取不到写 ?' $false ((Format-ReaperListingValue -IsDir $false -Length 6 -Ticks 99 -IsReparse $true -Links -1) -ceq '6|99|1|?')
    & $add '清单值：目录写 dir，是重解析点的目录（联接）写 dir|link' $false ((Format-ReaperListingValue -IsDir $true) -ceq 'dir' -and (Format-ReaperListingValue -IsDir $true -IsReparse $true) -ceq 'dir|link')
    $same = Format-ReaperListingValue -IsDir $false -Length 6 -Ticks 99 -IsReparse $false -Links 1
    $asLink = Format-ReaperListingValue -IsDir $false -Length 6 -Ticks 99 -IsReparse $true -Links 1
    $asHard = Format-ReaperListingValue -IsDir $false -Length 6 -Ticks 99 -IsReparse $false -Links 2
    & $add '清单值：大小与修改时间都相同，只有重解析点标记不同，比较判变' $true (-not (Compare-ReaperProbeLogListing -Before ([ordered]@{ 'x.csv' = $same }) -After ([ordered]@{ 'x.csv' = $asLink })).Same)
    & $add '清单值：大小与修改时间都相同，只有硬链接数不同，比较判变' $true (-not (Compare-ReaperProbeLogListing -Before ([ordered]@{ 'x.csv' = $same }) -After ([ordered]@{ 'x.csv' = $asHard })).Same)
    # 真文件：普通文件记 0|1；换成指向同一份内容的硬链接（大小、修改时间都相同）后硬链接数变成 2，比较看得出来
    $ld = Join-Path $dir 'real'; $null = New-Item -ItemType Directory -Path $ld
    & $mk (Join-Path $ld 'a.csv') "hello`n"
    $null = New-Item -ItemType Directory -Path (Join-Path $ld 'sub')
    $l1 = Get-ReaperProbeLogListing -Dir $ld
    & $add '清单：普通文件记 大小|刻度|0|1，普通子目录记 dir' $false ($l1['a.csv'] -cmatch '^6\|[0-9]+\|0\|1$' -and $l1['sub'] -ceq 'dir')
    [IO.File]::Delete((Join-Path $ld 'a.csv'))
    & $mk (Join-Path $ld 'p.csv') "hello`n"
    $hardOk = try { $null = New-Item -ItemType HardLink -Path (Join-Path $ld 'a.csv') -Value (Join-Path $ld 'p.csv'); $true } catch { $false }
    $l2 = Get-ReaperProbeLogListing -Dir $ld
    $cmpHard = Compare-ReaperProbeLogListing -Before ([ordered]@{ 'a.csv' = $l1['a.csv']; 'sub' = $l1['sub'] }) -After ([ordered]@{ 'a.csv' = $l2['a.csv']; 'sub' = $l2['sub'] })
    & $add "清单：别人的文件被换成大小、修改时间都相同的硬链接：大小与刻度没变、硬链接数从 1 变 2，比较判变$($hardOk ? '' : '（本机建不了硬链接，这条没能真跑）')" $true (
        (-not $hardOk) -or ((& $field $l1['a.csv'] 0) -ceq (& $field $l2['a.csv'] 0) -and (& $field $l1['a.csv'] 1) -ceq (& $field $l2['a.csv'] 1) -and (& $field $l2['a.csv'] 3) -ceq '2' -and -not $cmpHard.Same))
    # 重解析点文件（符号链接的替身）与联接
    $rd = Join-Path $dir 'reparse'; $null = New-Item -ItemType Directory -Path $rd
    & $mk (Join-Path $rd 'r.csv') "hello`n"
    $r1 = Get-ReaperProbeLogListing -Dir $rd
    [IO.File]::Delete((Join-Path $rd 'r.csv'))
    $rpOk = New-FakeReparseFile (Join-Path $rd 'r.csv') "hello`n"
    $r2 = Get-ReaperProbeLogListing -Dir $rd
    & $add "清单：普通文件被换成同大小的重解析点文件：重解析点标记从 0 变 1，比较判变$($rpOk ? '' : '（本机造不出重解析点文件，这条没能真跑）')" $true (
        (-not $rpOk) -or ((& $field $r1['r.csv'] 2) -ceq '0' -and (& $field $r2['r.csv'] 2) -ceq '1' -and -not (Compare-ReaperProbeLogListing -Before $r1 -After $r2).Same))
    $jt = Join-Path $dir 'junction-target'; $null = New-Item -ItemType Directory -Path $jt
    $jd = Join-Path $dir 'junc'; $null = New-Item -ItemType Directory -Path $jd
    $null = New-Item -ItemType Directory -Path (Join-Path $jd 'plain')
    $juncOk = try { $null = New-Item -ItemType Junction -Path (Join-Path $jd 'link') -Value $jt; $true } catch { $false }
    $j1 = Get-ReaperProbeLogListing -Dir $jd
    & $add "清单：联接记 dir|link，普通子目录仍记 dir$($juncOk ? '' : '（本机建不了联接，这条没能真跑）')" $false ((-not $juncOk) -or ($j1['link'] -ceq 'dir|link' -and $j1['plain'] -ceq 'dir'))
    # 落盘的清单：不是探针日志命名的条目只留散列
    $salt = New-ReaperListingSalt
    $named = 'probe-I-4242-abcd.csv'
    $listing = [ordered]@{ 'secret-song.wav' = '5|10|0|1'; 'Cubase Project' = 'dir'; 'Cubase Project\take-1.wav' = '7|11|0|1'; $named = '9|12|0|1'; 'notes.txt' = '3|13|0|1' }
    $safe = ConvertTo-ReaperSafeListing -Listing $listing -Salt $salt
    $json = $safe | ConvertTo-Json -Depth 5
    & $add '落盘清单：别人的文件名、子目录名、子目录里的文件名一个都不出现在写出去的 json 里' $true ($json -notmatch 'secret|song|Cubase|take-1|notes')
    & $add '落盘清单：探针日志命名的条目原样写，其余都是 other-<8 位十六进制>，条目数不变，大小、时间、链接标记原样' $true (
        $safe.Count -eq 5 -and $safe.Contains($named) -and $safe[$named] -ceq '9|12|0|1' -and @($safe.Keys | Where-Object { $_ -cmatch '^other-[0-9a-f]{8}$' }).Count -eq 4 -and
        @($safe.Values | Where-Object { $_ -ceq '5|10|0|1' }).Count -eq 1 -and @($safe.Values | Where-Object { $_ -ceq 'dir' }).Count -eq 1)
    & $add '落盘清单：同一把盐下同一个名字的散列不变（前后两份能对上），不同的名字不同' $false (
        (Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt) -ceq (Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt) -and
        (Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt) -cne (Get-ReaperListingShownName -EntryName 'notes.txt' -Salt $salt))
    $salt2 = New-ReaperListingSalt
    & $add '落盘清单：换一把盐（每轮一把随机盐）同一个名字的散列就不同；盐是 32 位十六进制，两次取的不一样' $true (
        $salt -cmatch '^[0-9a-f]{32}$' -and $salt -cne $salt2 -and (Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt) -cne (Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt2))
    & $add '落盘清单：盐本身不出现在写出去的 json 里' $true ($json -notmatch [regex]::Escape($salt))
    # 变化报告：别人的文件只写散列与变化类型；探针日志命名的条目照写名字
    $after = [ordered]@{ 'Cubase Project' = 'dir'; 'Cubase Project\take-1.wav' = '7|11|0|1'; $named = '9|12|0|1'; 'notes.txt' = '4|13|0|1'; 'new-secret.wav' = '1|1|0|1' }
    $cmp = Compare-ReaperProbeLogListing -Before $listing -After $after -Salt $salt
    $changedText = $cmp.Changed -join ' ; '
    & $add "变化报告：别人的文件删了、改了、多了，原因文字里只有 removed/changed/added 加散列，没有文件名（$changedText）" $true (
        -not $cmp.Same -and $cmp.Changed.Count -eq 3 -and $changedText -notmatch 'secret|song|notes|Cubase' -and
        $cmp.Changed -ccontains ('removed:' + (Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt)) -and
        $cmp.Changed -ccontains ('changed:' + (Get-ReaperListingShownName -EntryName 'notes.txt' -Salt $salt)) -and
        $cmp.Changed -ccontains ('added:' + (Get-ReaperListingShownName -EntryName 'new-secret.wav' -Salt $salt)))
    & $add '变化报告：散列与落盘清单里的名字对得上（同一把盐），能从 json 里找到是哪一项' $false ($safe.Contains((Get-ReaperListingShownName -EntryName 'notes.txt' -Salt $salt)))
    $cmpNoSalt = Compare-ReaperProbeLogListing -Before $listing -After $after
    & $add '变化报告：不给盐也照样遮名字（文件名不会因为调用方忘了传盐而漏出去）' $true (-not $cmpNoSalt.Same -and ($cmpNoSalt.Changed -join ' ') -notmatch 'secret|song|notes' -and $cmpNoSalt.Changed.Count -eq 3)
    $cmpOwnName = Compare-ReaperProbeLogListing -Before ([ordered]@{ $named = '9|12|0|1' }) -After ([ordered]@{ $named = '9|13|0|1' }) -Salt $salt
    & $add '变化报告：探针日志命名的条目变了照写名字（那是探针自己的名字）' $false ($cmpOwnName.Changed -ccontains "changed:$named")
    $cases
}

# 写日志的进程还在不在：pid 可能已被复用。拿「现在持有这个 pid 的进程是什么时候开始的」和「这份日志最后一次写入的时刻」比，
# 不拿它和「这次启动的时刻」比（真正的 REAPER 恰恰是这次启动之后才开始的）
function Get-WriterCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $now = [DateTime]::UtcNow
    $mtime = $now.AddMinutes(-10) # 日志最后一次写入
    $holder = { param($exists, $start) [pscustomobject]@{ Exists = $exists; StartUtc = $start } }
    & $add '写日志的进程：没有这个进程是 Gone' $false ((Get-ReaperWriterState -Holder (& $holder $false $null) -LastWriteUtc $mtime) -ceq 'Gone')
    & $add '写日志的进程：持有者比日志最后一次写入还晚开始（pid 被复用）是 Reused' $true ((Get-ReaperWriterState -Holder (& $holder $true $now) -LastWriteUtc $mtime) -ceq 'Reused')
    & $add '写日志的进程：持有者在日志最后一次写入之前就开始了是 Alive（写它的进程还在）' $true ((Get-ReaperWriterState -Holder (& $holder $true $mtime.AddMinutes(-5)) -LastWriteUtc $mtime) -ceq 'Alive')
    & $add '写日志的进程：开始时间比最后一次写入晚不过时钟余量（2 s）仍是 Alive，超过一刻才是 Reused' $true (
        (Get-ReaperWriterState -Holder (& $holder $true $mtime.AddSeconds(2)) -LastWriteUtc $mtime) -ceq 'Alive' -and (Get-ReaperWriterState -Holder (& $holder $true $mtime.AddSeconds(2).AddTicks(1)) -LastWriteUtc $mtime) -ceq 'Reused')
    & $add '写日志的进程：有进程但读不到开始时间是 Unknown（没法排除复用，按还在处理）' $true ((Get-ReaperWriterState -Holder (& $holder $true $null) -LastWriteUtc $mtime) -ceq 'Unknown')
    # 真进程：自己、已退出的、刚起的子进程
    $self = Get-ReaperPidHolder $PID
    $selfStart = (Get-Process -Id $PID).StartTime.ToUniversalTime()
    & $add '持有者：本进程的 pid 查得到，开始时间与进程自己报的一致' $false ($self.Exists -and $null -ne $self.StartUtc -and [math]::Abs(($self.StartUtc - $selfStart).TotalSeconds) -lt 1)
    $gone = Get-ExitedPid
    & $add '持有者：已退出进程的 pid 查不到' $false (-not (Get-ReaperPidHolder $gone).Exists)
    # 搬走的接线：持有者由 ProcessInfo 注入
    $since = $now.AddHours(-1)
    $run = {
        param([string]$Name, $Holder)
        $d = Join-Path $Tmp "writer-$Name"; $null = New-Item -ItemType Directory -Force -Path $d
        $f = Join-Path $d "probe-I-$gone-1a2b.csv"; [IO.File]::WriteAllText($f, (New-LogForName "probe-I-$gone-1a2b.csv" 'log')); (Get-Item -LiteralPath $f).LastWriteTimeUtc = $mtime
        $h = $Holder
        $m = Move-ReaperProbeLogs -LogDir $d -Pids @($gone) -SinceUtc $since -DestDir (Join-Path $Tmp "writer-$Name-dest") -ProcessInfo ({ param($p) $h }.GetNewClosure())
        [pscustomobject]@{ M = $m; File = $f; Dest = (Join-Path $Tmp "writer-$Name-dest/probe-I-$gone-1a2b.csv") }
    }
    $rGone = & $run 'gone' (& $holder $false $null)
    & $add '搬走：写它的进程已退出（pid 没人持有）：照常搬走，没有问题，没有备注' $false ($rGone.M.Moved.Count -eq 1 -and $rGone.M.Problems.Count -eq 0 -and $rGone.M.Notes.Count -eq 0 -and -not (Test-Path -LiteralPath $rGone.File) -and (Test-Path -LiteralPath $rGone.Dest))
    $rReuse = & $run 'reused' (& $holder $true $now)
    & $add "搬走：pid 被别的进程复用（持有者比日志最后一次写入还晚开始）：不判假的「进程还在」，照常搬走，备注里写明 pid 被复用" $false (
        $rReuse.M.Moved.Count -eq 1 -and $rReuse.M.Problems.Count -eq 0 -and $rReuse.M.Notes.Count -eq 1 -and $rReuse.M.Notes[0] -like '*pid 被复用*' -and -not (Test-Path -LiteralPath $rReuse.File) -and (Test-Path -LiteralPath $rReuse.Dest))
    $rAlive = & $run 'alive' (& $holder $true $mtime.AddMinutes(-5))
    & $add '搬走：持有者在日志最后一次写入之前就开始了（写它的进程还在）：不动，判红，原文件还在' $true (
        $rAlive.M.Moved.Count -eq 0 -and (Test-Path -LiteralPath $rAlive.File) -and @($rAlive.M.Problems | Where-Object { $_ -like '*还没退出*' }).Count -eq 1 -and $rAlive.M.Notes.Count -eq 0)
    $rEdge = & $run 'edge' (& $holder $true $mtime.AddSeconds(1))
    & $add '搬走：开始时间只比最后一次写入晚 1 s（在时钟余量里）按还在处理：不动，判红' $true ($rEdge.M.Moved.Count -eq 0 -and (Test-Path -LiteralPath $rEdge.File) -and $rEdge.M.Problems.Count -eq 1)
    $rUnknown = & $run 'unknown' (& $holder $true $null)
    & $add '搬走：读不到持有者的开始时间：按还在处理（不动文件），原因里写明没法排除 pid 被复用' $true (
        $rUnknown.M.Moved.Count -eq 0 -and (Test-Path -LiteralPath $rUnknown.File) -and @($rUnknown.M.Problems | Where-Object { $_ -like '*还没退出*pid 被复用*' }).Count -eq 1)
    # 真进程：这次启动之后才开始、还在写日志的进程不能被当成「复用」（拿开始时间去比启动时刻会把它错判成复用，然后删掉它还在写的日志）
    $sinceLaunch = [DateTime]::UtcNow.AddSeconds(-10) # 启动时刻取在子进程开始之前，比时钟余量（2 s）早得多
    $child = [Diagnostics.Process]::Start([Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path, '-NoProfile -NonInteractive -Command Start-Sleep -Seconds 120'))
    try {
        $dc = Join-Path $Tmp 'writer-child'; $null = New-Item -ItemType Directory -Force -Path $dc
        $fc = Join-Path $dc "probe-O-$($child.Id)-3c4d.csv"; [IO.File]::WriteAllText($fc, "still writing`n")
        $mc = Move-ReaperProbeLogs -LogDir $dc -Pids @($child.Id) -SinceUtc $sinceLaunch -DestDir (Join-Path $Tmp 'writer-child-dest')
        & $add '搬走：这次启动之后才开始、正在跑的进程（开始时间晚于启动时刻）：写它的进程还在，不动，判红（不能因为它比启动时刻晚就当成 pid 复用）' $true (
            $mc.Moved.Count -eq 0 -and (Test-Path -LiteralPath $fc) -and @($mc.Problems | Where-Object { $_ -like '*还没退出*' }).Count -eq 1 -and $mc.Notes.Count -eq 0)
    } finally { if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() } }
    # 真进程：本进程的 pid，日志最后一次写入早于本进程开始（同一个 pid 以前属于别的进程）：算复用
    $dr = Join-Path $Tmp 'writer-self-reused'; $null = New-Item -ItemType Directory -Force -Path $dr
    $fr = Join-Path $dr "probe-I-$PID-5e6f.csv"; [IO.File]::WriteAllText($fr, (New-LogForName "probe-I-$PID-5e6f.csv" 'old owner')); (Get-Item -LiteralPath $fr).LastWriteTimeUtc = $selfStart.AddHours(-1)
    $mr = Move-ReaperProbeLogs -LogDir $dr -Pids @($PID) -SinceUtc $selfStart.AddHours(-3) -DestDir (Join-Path $Tmp 'writer-self-reused-dest')
    & $add '搬走：真进程（本进程）的 pid，日志最后一次写入早于它开始之前一小时：算 pid 复用，照常搬走并留备注' $false ($mr.Moved.Count -eq 1 -and $mr.Notes.Count -eq 1 -and -not (Test-Path -LiteralPath $fr))
    # 备注记进这一轮的上下文（Save-ProbeLogs 是各个场景处理器收日志的入口）
    $ds = Join-Path $Tmp 'writer-save'; $null = New-Item -ItemType Directory -Force -Path $ds
    $fs = Join-Path $ds "probe-O-$PID-7a8b.csv"; [IO.File]::WriteAllText($fs, (New-LogForName "probe-O-$PID-7a8b.csv" 'old owner')); (Get-Item -LiteralPath $fs).LastWriteTimeUtc = $selfStart.AddHours(-1)
    $saveCtx = @{ Probe = @{ LogDir = $ds; Own = [Collections.Generic.List[string]]::new(); Problems = [Collections.Generic.List[string]]::new(); Notes = [Collections.Generic.List[string]]::new(); Moved = 0 } }
    $null = & (Get-Module Scenarios) { param($c, $run, $cell) Save-ProbeLogs $c $run $cell } $saveCtx ([pscustomobject]@{ Status = 'Exited'; Pid = $PID; StartedUtc = $selfStart.AddHours(-3) }) (Join-Path $Tmp 'writer-save-dest')
    & $add '搬走：Save-ProbeLogs 把 pid 复用的备注记进这一轮的 Ctx.Probe.Notes，搬走数照记，没有问题' $false (
        $saveCtx.Probe.Moved -eq 1 -and $saveCtx.Probe.Notes.Count -eq 1 -and $saveCtx.Probe.Notes[0] -like '*pid 被复用*' -and $saveCtx.Probe.Problems.Count -eq 0)
    # 读开始时间出错（拒绝访问等）：进程还在就是 Exists 为真、StartUtc 为空；出错的当口进程已退出就算不存在。Get-Process 在模块里临时换成替身。
    $holderOf = {
        param([bool]$Exited)
        & (Get-Module ProbeLogs) {
            param($exited)
            $script:FakeExited = $exited
            function script:Get-Process {
                param($Id, $ErrorAction)
                $o = [pscustomobject]@{ Id = $Id }
                Add-Member -InputObject $o -MemberType ScriptProperty -Name StartTime -Value { throw [ComponentModel.Win32Exception]::new(5) }
                Add-Member -InputObject $o -MemberType ScriptProperty -Name HasExited -Value { $script:FakeExited }
                $o
            }
            try { Get-ReaperPidHolder 1234 } finally { Remove-Item -LiteralPath Function:\script:Get-Process -ErrorAction SilentlyContinue }
        } $Exited
    }
    $hAlive = & $holderOf $false
    $hExited = & $holderOf $true
    & $add '持有者：读开始时间出错而进程还在：Exists 为真、StartUtc 为空（Unknown，不动文件）' $true ($hAlive.Exists -and $null -eq $hAlive.StartUtc)
    & $add '持有者：读开始时间出错的当口进程已退出：算不存在' $false (-not $hExited.Exists)
    $cases
}

# -Python 可以是路径，也可以是 PATH 上的命令名：裸名先按命令解析，解析出来的路径再走原来的判断（包括 py 启动器的 -3）
function Get-PythonCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $bin = Join-Path $Tmp 'pybin'; $null = New-Item -ItemType Directory -Force -Path $bin
    foreach ($spec in @(@('zzpyfake.cmd', '3.10.11'), @('zzpyold.cmd', '2.7.18'), @('py.cmd', '3.10.11'))) { [IO.File]::WriteAllText((Join-Path $bin $spec[0]), "@echo Python $($spec[1])`r`n") }
    $savedPath = $env:PATH
    $env:PATH = "$bin;$savedPath"
    try {
        $bare = Resolve-ReaperPython 'zzpyfake'
        & $add "-Python 裸命令名（zzpyfake，在 PATH 上）：解析成现成文件的绝对路径，版本读得出（$($bare.Exe)）" $false (
            -not $bare.Why -and [IO.Path]::IsPathRooted($bare.Exe) -and (Test-Path -LiteralPath $bare.Exe -PathType Leaf) -and (Split-Path -Leaf $bare.Exe) -ieq 'zzpyfake.cmd' -and $bare.Version -ceq '3.10.11')
        $byPath = Resolve-ReaperPython (Join-Path $bin 'zzpyfake.cmd')
        & $add '-Python 给文件路径：原样用，不走 PATH 解析' $false (-not $byPath.Why -and $byPath.Exe -ceq (Join-Path $bin 'zzpyfake.cmd') -and $byPath.Version -ceq '3.10.11')
        $bareOld = Resolve-ReaperPython 'zzpyold'
        & $add '-Python 裸命令名解析到的不是 Python 3：照样被拒（版本照核）' $true ($bareOld.Why -like '*不是 Python 3*')
        $none = Resolve-ReaperPython 'zzpy-no-such-command'
        & $add '-Python 裸命令名在 PATH 上找不到：被拒，原因写明找不到' $true ($none.Why -like '*找不到 Python 解释器*' -and -not $none.Exe)
        $wild = Resolve-ReaperPython 'zzpyf*'
        & $add '-Python 带通配符的名字不当命令名解析（即使通配会匹配到 PATH 上的文件）：被拒' $true ($wild.Why -like '*找不到 Python 解释器*')
        # py 启动器的 -3 按解析出来的路径判：裸名 py 在这里解析到 py.cmd（不是 py.exe），所以不带 -3；真正的 py.exe 路径才带
        $bareLauncher = Resolve-ReaperPython 'py'
        & $add '-Python py（解析到 py.cmd，不是启动器 py.exe）：前导参数按解析后的路径判，不带 -3' $true ((Split-Path -Leaf $bareLauncher.Exe) -ieq 'py.cmd' -and @($bareLauncher.Lead).Count -eq 0)
        & $add 'py 启动器前导参数：py.exe / py（不分大小写）带 -3，别的解释器不带' $false (
            (@(Get-ReaperPythonLead 'C:\Windows\py.exe') -join ',') -ceq '-3' -and (@(Get-ReaperPythonLead 'D:\x\PY.EXE') -join ',') -ceq '-3' -and @(Get-ReaperPythonLead 'C:\Python\python.exe').Count -eq 0 -and @(Get-ReaperPythonLead 'C:\x\py.cmd').Count -eq 0)
        # 预检里同样可用
        $marker = Join-Path $Tmp 'py-USER_TESTING'
        $pre = Get-ReaperPreflight -ReaperExe ((Get-Process -Id $PID).Path) -BuildDir (Join-Path $Tmp 'no-build') -NullTest ((Get-Process -Id $PID).Path) -UserTestingMarker $marker -Flavor probe -Python 'zzpyfake' -ProbeLogDir (Join-Path $Tmp 'py-docs/Rinne Probe Logs')
        & $add '预检（第二段）：-Python 给裸命令名：解释器解析成路径、版本读出，没有 Python 方面的缺项' $false (
            $pre.PythonVersion -ceq '3.10.11' -and [IO.Path]::IsPathRooted($pre.Python) -and @($pre.Reasons | Where-Object { $_ -match 'Python 解释器' }).Count -eq 0)
    } finally { $env:PATH = $savedPath }
    $cases
}

# 清单里硬链接数取不到（记 ?）：一侧是 ? 的条目只比前三段，不判变，另记一笔提示；两侧都取到才比硬链接数
function Get-LinkUnknownCases {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $v = { param($a, $b) Compare-ReaperListingValue -Before $a -After $b }
    # 纯函数：值与值
    $q1 = & $v '6|99|0|1' '6|99|0|?'
    $q2 = & $v '6|99|0|?' '6|99|0|1'
    $q3 = & $v '6|99|0|?' '6|99|0|?'
    & $add '清单值比较：前一份是 1、后一份取不到（?），其余相同：算一样，另标硬链接数没取到' $false ($q1.Same -and $q1.LinkUnknown)
    & $add '清单值比较：前一份取不到、后一份是 1：同样算一样；两份都取不到也算一样，都标没取到' $false ($q2.Same -and $q2.LinkUnknown -and $q3.Same -and $q3.LinkUnknown)
    $k1 = & $v '6|99|0|1' '6|99|0|1'
    & $add '清单值比较：两份都取到且相同：算一样，不标没取到' $false ($k1.Same -and -not $k1.LinkUnknown)
    & $add '清单值比较：两份都取到、硬链接数不同（1 变 2）：判变（只有一侧取不到才放宽）' $true (-not (& $v '6|99|0|1' '6|99|0|2').Same -and -not (& $v '6|99|0|1' '6|99|0|2').LinkUnknown)
    foreach ($row in @(@('大小', '6|99|0|?', '7|99|0|1'), @('修改时间', '6|99|0|?', '6|100|0|1'), @('重解析点标记', '6|99|0|?', '6|99|1|1'),
                       @('大小（取不到在后一份）', '6|99|0|1', '7|99|0|?'), @('修改时间（取不到在后一份）', '6|99|0|1', '6|98|0|?'), @('重解析点标记（取不到在后一份）', '6|99|1|1', '6|99|0|?'))) {
        $c = & $v $row[1] $row[2]
        & $add "清单值比较：一侧硬链接数取不到，但$($row[0])变了：照样判变（只放宽「取不到」本身）" $true (-not $c.Same -and -not $c.LinkUnknown)
    }
    & $add '清单值比较：一侧是文件的值、另一侧是目录（dir、dir|link）：判变；目录与目录整串比' $true (-not (& $v '6|99|0|?' 'dir').Same -and -not (& $v 'dir' '6|99|0|?').Same -and -not (& $v 'dir' 'dir|link').Same -and (& $v 'dir' 'dir').Same -and (& $v 'dir|link' 'dir|link').Same)
    # 整份清单的比较
    $salt = New-ReaperListingSalt
    $b = [ordered]@{ 'secret-song.wav' = '5|10|0|1'; 'notes.txt' = '3|13|0|1'; 'sub' = 'dir' }
    $aSame = [ordered]@{ 'secret-song.wav' = '5|10|0|?'; 'notes.txt' = '3|13|0|1'; 'sub' = 'dir' }
    $cmp = Compare-ReaperProbeLogListing -Before $b -After $aSame -Salt $salt
    $shown = Get-ReaperListingShownName -EntryName 'secret-song.wav' -Salt $salt
    & $add '清单比较：前一份 1、后一份 ?、其余相同：不判变，提示里记一笔，名字只写散列' $false (
        $cmp.Same -and $cmp.Changed.Count -eq 0 -and @($cmp.LinkUnknown).Count -eq 1 -and $cmp.LinkUnknown[0] -ceq $shown -and @($cmp.Notes).Count -eq 1 -and $cmp.Notes[0] -like '*硬链接数没取到：1 项*' -and
        $cmp.Notes[0] -like "*$shown*" -and $cmp.Notes[0] -notmatch 'secret|song')
    $aBig = [ordered]@{ 'secret-song.wav' = '6|10|0|?'; 'notes.txt' = '3|13|0|1'; 'sub' = 'dir' }
    $cmpBig = Compare-ReaperProbeLogListing -Before $b -After $aBig -Salt $salt
    & $add '清单比较：后一份是 ? 但大小变了：仍判变（changed 加散列），没有「没取到」的提示' $true (-not $cmpBig.Same -and $cmpBig.Changed.Count -eq 1 -and $cmpBig.Changed[0] -ceq "changed:$shown" -and @($cmpBig.Notes).Count -eq 0)
    $aGone = [ordered]@{ 'notes.txt' = '3|13|0|1'; 'sub' = 'dir' }
    & $add '清单比较：取不到的条目整个没了：判变（removed）' $true (-not (Compare-ReaperProbeLogListing -Before $aSame -After $aGone -Salt $salt).Same)
    & $add '清单比较：别的条目变了、取不到的条目没变：只判那个变的，取不到的仍在提示里' $true (
        (& { $r = Compare-ReaperProbeLogListing -Before $b -After ([ordered]@{ 'secret-song.wav' = '5|10|0|?'; 'notes.txt' = '4|13|0|1'; 'sub' = 'dir' }) -Salt $salt; (-not $r.Same) -and $r.Changed.Count -eq 1 -and $r.Changed[0] -like 'changed:other-*' -and @($r.LinkUnknown).Count -eq 1 }))
    & $add '清单比较：没有任何 ? 时 LinkUnknown 与 Notes 都是空（正常路径不多出提示）' $false (
        (& { $r = Compare-ReaperProbeLogListing -Before $b -After $b -Salt $salt; $r.Same -and @($r.LinkUnknown).Count -eq 0 -and @($r.Notes).Count -eq 0 }))
    # 很多项取不到：数目写全，名字最多列 8 个
    $many = [ordered]@{}; $manyQ = [ordered]@{}
    1..11 | ForEach-Object { $many["f$_.csv"] = "1|$_|0|1"; $manyQ["f$_.csv"] = "1|$_|0|?" }
    $cmpMany = Compare-ReaperProbeLogListing -Before $many -After $manyQ -Salt $salt
    & $add '清单比较：11 项取不到：不判变，提示写「11 项」，名字最多列 8 个后加省略号' $false (
        $cmpMany.Same -and @($cmpMany.LinkUnknown).Count -eq 11 -and $cmpMany.Notes[0] -like '*硬链接数没取到：11 项*……*' -and ([regex]::Matches($cmpMany.Notes[0], 'other-[0-9a-f]{8}')).Count -eq 8)
    $cases
}

# 收日志在钉住之后的两道判据（都对钉住的那个对象）：H 行（第 2 行，从同一个句柄读）要是 REAPER 里的探针写的、与文件名一致；
# 创建时间要落在这次启动的存活区间里。pid 没人持有（Gone）时 pid 判据说明不了什么，同号的别的进程写的日志就靠这两道挡住
function Get-PinGuardCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $gone = Get-ExitedPid
    $now = [DateTime]::UtcNow
    $since = $now.AddMinutes(-10)
    $root = Join-Path $Tmp 'pinguard'
    $script:pinSeq = 0
    # 造一个日志目录放一份文件，收一次。Created / Exited 不给就是：创建时间是现在、退出时刻取现在（真日志的样子）
    $try = {
        param([string]$Name, [string]$Text, $Created = $null, $Exited = $null, [scriptblock]$BeforePin = $null)
        $script:pinSeq++
        $d = Join-Path $root "c$($script:pinSeq)"; $null = New-Item -ItemType Directory -Force -Path $d
        $f = Join-Path $d $Name; [IO.File]::WriteAllText($f, $Text)
        if ($null -ne $Created) { [IO.File]::SetCreationTimeUtc($f, $Created) }
        $hashBefore = Get-Sha $f
        $moveArgs = @{ LogDir = $d; Pids = @($gone); SinceUtc = $since; DestDir = (Join-Path $root "c$($script:pinSeq)-dest") }
        if ($null -ne $Exited) { $moveArgs.ExitedUtc = $Exited }
        if ($BeforePin) { $moveArgs.BeforePin = $BeforePin }
        $m = Move-ReaperProbeLogs @moveArgs
        [pscustomobject]@{ M = $m; File = $f; Dest = (Join-Path $root "c$($script:pinSeq)-dest/$Name"); HashBefore = $hashBefore }
    }
    $untouched = { param($r) (Test-Path -LiteralPath $r.File) -and (Get-Sha $r.File) -ceq $r.HashBefore -and -not (Test-Path -LiteralPath $r.Dest) -and $r.M.Moved.Count -eq 0 }
    $why = { param($r, [string]$like) @($r.M.Problems | Where-Object { $_ -like $like }).Count -eq 1 -and @($r.M.Kept | Where-Object { $_ -like $like }).Count -eq 1 }
    $nm = { param($role, $tok) "probe-$role-$gone-$tok.csv" }

    $real = & $try (& $nm 'I' 'aa01') (New-LogForName (& $nm 'I' 'aa01') 'real')
    & $add '头两行与创建时间：本轮真日志（H 行 host_kind=other、与文件名一致，创建时间在这次启动里）照常搬走，没有问题' $false (
        $real.M.Moved.Count -eq 1 -and $real.M.Problems.Count -eq 0 -and -not (Test-Path -LiteralPath $real.File) -and (Test-Path -LiteralPath $real.Dest) -and (Get-Sha $real.Dest) -ceq $real.HashBefore)
    # 比 1024 字节大的真日志：头只读开头那一段，读完位置放回 0，复制与哈希是整个文件
    $bigName = & $nm 'O' 'aa02'
    $big = & $try $bigName ((New-LogForName $bigName 'big') + ('S,5000,1,0,512,0,0' + "`n") * 300)
    & $add '头两行与创建时间：比 1024 字节大的真日志也整份搬走（读 H 行只读开头，读完位置放回开头，复制件与原文件 sha256 相同）' $false (
        $big.M.Moved.Count -eq 1 -and $big.M.Problems.Count -eq 0 -and (Test-Path -LiteralPath $big.Dest) -and (Get-Item -LiteralPath $big.Dest).Length -gt 4000 -and (Get-Sha $big.Dest) -ceq $big.HashBefore)

    # H 行的 host_kind：同 pid、名字对、时间在窗口里、pid 无人持有，但 H 行说它是 Cubase / LUNA 里的探针写的 → 不动
    $cubaseName = & $nm 'I' 'cb01'
    $cb = & $try $cubaseName (New-LogForName $cubaseName 'decoy' -HostKind cubase)
    & $add '头两行：同 pid 的诱饵（H 行 host_kind=cubase，名字对、时间在窗口里、pid 没人持有）不动，原文件一个字节没变，判红并写明 host_kind' $true ((& $untouched $cb) -and (& $why $cb '*host_kind 是 cubase*不是 other*'))
    $lunaName = & $nm 'O' 'cb02'
    $lu = & $try $lunaName (New-LogForName $lunaName 'decoy' -HostKind luna)
    & $add '头两行：host_kind=luna 的同 pid 诱饵也不动' $true ((& $untouched $lu) -and (& $why $lu '*host_kind 是 luna*'))
    $oddName = & $nm 'I' 'cb03'
    $odd = & $try $oddName ((New-LogForName $oddName 'decoy') -replace ',other,', ',secret-host-name,')
    & $add '头两行：host_kind 不在枚举里的诱饵不动，原因里不回显文件里的内容（只写「不在枚举里的值」）' $true ((& $untouched $odd) -and (& $why $odd '*host_kind 是 不在枚举里的值*') -and ($odd.M.Problems -join ' ') -notmatch 'secret')
    # H 行与文件名不符
    $pidName = & $nm 'I' 'cb04'
    $pidDecoy = & $try $pidName ((New-LogForName $pidName 'decoy') -replace ",$gone,1,", ",$($gone + 1),1,")
    & $add '头两行：H 行的 pid 与文件名不符（名字是本轮的 pid，里面写的是别的进程）不动' $true ((& $untouched $pidDecoy) -and (& $why $pidDecoy '*H 行的 pid 与文件名不符*'))
    $tokName = & $nm 'I' 'cb05'
    $tokDecoy = & $try $tokName ((New-LogForName $tokName 'decoy') -replace ',cb05,', ',cb06,')
    & $add '头两行：H 行的 tok4 与文件名不符不动' $true ((& $untouched $tokDecoy) -and (& $why $tokDecoy '*H 行的 tok4 与文件名不符*'))
    $roleName = & $nm 'I' 'cb07'
    $roleDecoy = & $try $roleName ((New-LogForName $roleName 'decoy') -replace '(?m)^H,I,', 'H,O,')
    & $add '头两行：H 行的 role 与文件名不符不动' $true ((& $untouched $roleDecoy) -and (& $why $roleDecoy '*H 行的 role 与文件名不符*'))
    # H 行读不出来：一律不动
    $okText = { param($name) New-LogForName $name 'x' }
    $unreadable = [ordered]@{
        '空文件' = { param($name) '' }
        '只有魔数一行' = { param($name) "rinne-probe-log,1`n" }
        'H 行没写完（没有换行）' = { param($name) ((& $okText $name) -split "`n")[0] + "`n" + (((& $okText $name) -split "`n")[1]) }
        '魔数不对' = { param($name) (& $okText $name) -replace 'rinne-probe-log,1', 'something-else,1' }
        '行尾是 CRLF' = { param($name) (& $okText $name) -replace "`n", "`r`n" }
        'H 行少一列' = { param($name) (& $okText $name) -replace ',00000000', '' }
        '第 2 行不是 H 行' = { param($name) (& $okText $name) -replace '(?m)^H,', 'E,' }
        '一堆不可读的字节' = { param($name) [string]::new([char[]](1..200 | ForEach-Object { [char]($_ + 100) })) }
    }
    $u = 0
    foreach ($what in $unreadable.Keys) {
        $u++
        $uName = & $nm 'O' ('dd{0:x2}' -f $u)
        $r = & $try $uName (& $unreadable[$what] $uName)
        & $add "头两行：$what，读不出合格的 H 行：不动，原文件没变，判红并写明读不出 H 行" $true ((& $untouched $r) -and (& $why $r '*读不出 H 行*'))
    }

    # 创建时间
    $exited5 = $now.AddMinutes(-5)
    $lateName = & $nm 'I' 'ee01'
    $late = & $try $lateName (New-LogForName $lateName 'decoy') -Exited $exited5
    & $add '创建时间：晚于这次启动的 REAPER 退出（H 行、名字、pid、修改时间都对得上，但文件是退出之后才新建的）不动，原文件没变，判红并写明创建时间' $true ((& $untouched $late) -and (& $why $late '*创建时间晚于这次启动的 REAPER 退出*'))
    $earlyName = & $nm 'O' 'ee02'
    $early = & $try $earlyName (New-LogForName $earlyName 'decoy') -Created $since.AddHours(-1)
    & $add '创建时间：早于这次启动（修改时间在窗口里，但文件是更早的进程建的）不动，原文件没变，判红并写明创建时间' $true ((& $untouched $early) -and (& $why $early '*创建时间早于这次启动*'))
    $inName = & $nm 'I' 'ee03'
    $inside = & $try $inName (New-LogForName $inName 'real') -Created $now.AddMinutes(-7) -Exited $now.AddMinutes(-6)
    & $add '创建时间：落在这次启动与它退出之间的真日志（退出是几分钟前的事）照常搬走' $false ($inside.M.Moved.Count -eq 1 -and $inside.M.Problems.Count -eq 0 -and -not (Test-Path -LiteralPath $inside.File) -and (Test-Path -LiteralPath $inside.Dest))
    $slackName = & $nm 'O' 'ee04'
    $slack = & $try $slackName (New-LogForName $slackName 'real') -Created $now.AddMinutes(-1) -Exited $now.AddMinutes(-1).AddSeconds(-1.5)
    & $add '创建时间：比退出晚 1.5 s（在 2 s 余量里）仍照常搬走' $false ($slack.M.Moved.Count -eq 1 -and $slack.M.Problems.Count -eq 0)
    $pastName = & $nm 'I' 'ee05'
    $past = & $try $pastName (New-LogForName $pastName 'decoy') -Created $now.AddMinutes(-1) -Exited $now.AddMinutes(-1).AddSeconds(-2.5)
    & $add '创建时间：比退出晚 2.5 s（超过 2 s 余量）不动' $true ((& $untouched $past) -and (& $why $past '*创建时间晚于*'))
    # 两道判据读的是钉住的那个对象：列目录时是 host_kind=other 的真日志，钉住之前被同长度、同修改时间的 Cubase 日志顶替 → 读到的是新对象，不动
    $swapName = & $nm 'I' 'ee06'
    $swap = & $try $swapName (New-LogForName $swapName 'real') -BeforePin {
        param($src)
        $t = (Get-Item -LiteralPath $src).LastWriteTimeUtc; Remove-Item -LiteralPath $src
        [IO.File]::WriteAllText($src, (New-LogForName (Split-Path -Leaf $src) 'rea' -HostKind cubase)); (Get-Item -LiteralPath $src).LastWriteTimeUtc = $t
    }
    & $add '头两行：列目录之后、钉住之前，原文件被同长度同修改时间的 Cubase 日志顶替：H 行读的是钉住的那个对象，不动、不删' $true (
        (Test-Path -LiteralPath $swap.File) -and $swap.M.Moved.Count -eq 0 -and -not (Test-Path -LiteralPath $swap.Dest) -and ([IO.File]::ReadAllText($swap.File) -cmatch ',cubase,') -and (& $why $swap '*host_kind 是 cubase*'))

    # 纯函数：创建时间的边界（两端都算在内）、H 行判据本身
    $e0 = $now.AddMinutes(-3)
    $edge = { param($c) Get-ReaperCreationRefusal -CreationUtc $c -SinceUtc $since -ExitedUtc $e0 }
    & $add '创建时间判据：恰等于启动前一刻、恰等于退出加 2 s 的算（两端闭区间）' $false (($null -eq (& $edge $since)) -and ($null -eq (& $edge $e0.AddSeconds(2))) -and ($null -eq (& $edge $e0)))
    & $add '创建时间判据：早一刻（启动前）、晚一刻（退出加 2 s 之后）的不算，各写明原因' $true (((& $edge $since.AddTicks(-1)) -like '*早于这次启动*') -and ((& $edge $e0.AddSeconds(2).AddTicks(1)) -like '*晚于这次启动的 REAPER 退出*'))
    $good = New-LogForName 'probe-I-12-ab01.csv' 'x'
    & $add 'H 行判据：合格的头两行返回空；ExpectHostKind 给 cubase 时 host_kind=cubase 的才合格' $false (
        ($null -eq (Get-ReaperHeaderRefusal -Head $good -LogName 'probe-I-12-ab01.csv')) -and ($null -eq (Get-ReaperHeaderRefusal -Head (New-LogForName 'probe-I-12-ab01.csv' 'x' -HostKind cubase) -LogName 'probe-I-12-ab01.csv' -ExpectHostKind cubase)) -and
        (Get-ReaperHeaderRefusal -Head $good -LogName 'probe-I-12-ab01.csv' -ExpectHostKind cubase) -like '*host_kind 是 other，不是 cubase*')
    & $add 'H 行判据：名字本身不是探针日志的名字也拒（不会拿别的文件的头去对）' $true ((Get-ReaperHeaderRefusal -Head $good -LogName 'notes.txt') -like '*名字不是探针日志的名字*')
    # 从钉住的流里读头：返回开头、位置放回 0；之后整份文件仍读得全
    $null = New-Item -ItemType Directory -Force -Path $root
    $pf = Join-Path $root 'pinned-head.csv'; [IO.File]::WriteAllText($pf, $good + ('Z' * 3000))
    $pinned = Open-ReaperPinnedFile -Path $pf -Tries 1
    try {
        $head = Read-ReaperPinnedHead $pinned.Stream
        $pos = $pinned.Stream.Position
        $all = [IO.StreamReader]::new($pinned.Stream, [Text.Encoding]::ASCII).ReadToEnd()
        & $add '读头：从钉住的流里读最多 1024 字节、位置放回 0，之后整份文件仍读得全' $false ($head.Length -eq 1024 -and $head.StartsWith($good) -and $pos -eq 0 -and $all.Length -eq $good.Length + 3000)
    } finally { Close-ReaperPinnedFile $pinned }

    # 接线：Save-ProbeLogs 把 Run 里记的退出时刻交给收日志
    $wireDir = Join-Path $root 'wire'; $null = New-Item -ItemType Directory -Force -Path $wireDir
    $wName = & $nm 'I' 'ff01'
    [IO.File]::WriteAllText((Join-Path $wireDir $wName), (New-LogForName $wName 'late'))
    $ctxOf = { @{ Probe = @{ LogDir = $wireDir; Own = [Collections.Generic.List[string]]::new(); Problems = [Collections.Generic.List[string]]::new(); Notes = [Collections.Generic.List[string]]::new(); Moved = 0 } } }
    $saveCtx = & $ctxOf
    $null = & (Get-Module Scenarios) { param($c, $run, $cell) Save-ProbeLogs $c $run $cell } $saveCtx ([pscustomobject]@{ Status = 'Exited'; Pid = $gone; StartedUtc = $since; ExitedUtc = $exited5 }) (Join-Path $root 'wire-dest-a')
    & $add '接线：Save-ProbeLogs 把 Run 里记的退出时刻交给收日志：文件是退出之后才建的，不搬，问题记进这一轮' $true (
        $saveCtx.Probe.Moved -eq 0 -and (Test-Path -LiteralPath (Join-Path $wireDir $wName)) -and $saveCtx.Probe.Problems.Count -eq 1 -and $saveCtx.Probe.Problems[0] -like '*创建时间晚于这次启动的 REAPER 退出*')
    $saveCtx2 = & $ctxOf
    $null = & (Get-Module Scenarios) { param($c, $run, $cell) Save-ProbeLogs $c $run $cell } $saveCtx2 ([pscustomobject]@{ Status = 'Exited'; Pid = $gone; StartedUtc = $since; ExitedUtc = [DateTime]::UtcNow }) (Join-Path $root 'wire-dest-b')
    & $add '接线：Run 里记的退出时刻在文件创建之后，同一份文件就照常搬走' $false ($saveCtx2.Probe.Moved -eq 1 -and $saveCtx2.Probe.Problems.Count -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $wireDir $wName)))
    $cases
}

# 块长上限逐块比：每个 B 行带自己的 n 与 n_decl
function Get-BlockLimitCases {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $cell = {
        param([hashtable]$In = @{}, [hashtable]$Out = @{})
        $logs = @((Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role I @In)), (Read-ReaperProbeLog -Text (New-FakeProbeLogText -Role O -Pings @() @Out)))
        Test-ReaperProbeCell -Logs $logs -Expect @{ Pid = 4242; Case = 'CB-01'; Variant = 'steady'; Sr = 48000; Block = 512; MinBlocks = 32; NeedPing = $true }
    }
    $over = { param($r) , @($r.Bad | Where-Object { $_ -like '*块的块长超过它自己声明的最大块长*' }) } # 逗号：没有命中时也返回一个空数组，不是 $null（严格模式下 $null.Count 会抛错）
    # 两次 prepare：前 20 块 n=512、n_decl=512，后 20 块 n=1024、n_decl=1024（1024 块落在 n_decl=1024 的那一段）→ 不报
    $twoPrepare = & $cell -In @{ Blocks = 40; SecondFrom = 20; N2 = 1024; NDecl2 = 1024 }
    & $add '块长上限：两次 prepare（n_decl 先 512 后 1024），1024 的块落在 n_decl=1024 的那一段：不报（逐块比，不拿最小值去冤枉后一段）' $false ((& $over $twoPrepare).Count -eq 0 -and $twoPrepare.Facts.In.overBlocks -eq 0 -and $twoPrepare.Facts.In.nDecl -ceq '512/1024')
    # n_decl=512 的那一段里出现 1024 块（全文件最大的声明是 1024）→ 报：比最大值会放过它
    $inSegment = & $cell -In @{ Blocks = 40; NOverride = 1024; NOverrideAt = 5; SecondFrom = 20; N2 = 1024; NDecl2 = 1024 }
    $hitSeg = & $over $inSegment
    & $add '块长上限：n_decl=512 的那一段里出现 1024 的块（全文件最大的声明是 1024）：报，写出个数 1 与一例（块序号 5、n=1024 大于 n_decl=512）' $true (
        $hitSeg.Count -eq 1 -and $hitSeg[0] -like 'In 有 1 块*第一例：块序号 5 的 n=1024 大于它自己的 n_decl=512*全部声明值 512/1024*' -and $inSegment.Facts.In.overBlocks -eq 1)
    # 一整段都超：个数写全，一例是这一段的第一块
    $manyOver = & $cell -Out @{ Blocks = 40; SecondFrom = 20; N2 = 1024; NDecl2 = 512 }
    $hitMany = & $over $manyOver
    & $add '块长上限：后一段 20 块 n=1024 而它声明的 n_decl=512：个数写 20，一例是这一段的第一块（块序号 20）' $true ($hitMany.Count -eq 1 -and $hitMany[0] -like 'Out 有 20 块*第一例：块序号 20 的 n=1024 大于它自己的 n_decl=512*')
    & $add '块长上限：n 恰等于 n_decl 不报，大 1 报（边界）' $true ((& $over (& $cell -In @{ N = 512; NDecl = 512 })).Count -eq 0 -and (& $over (& $cell -In @{ NOverride = 513; NOverrideAt = 5 })).Count -eq 1)
    & $add '块长上限：一块都没有时不报，也不抛错' $true ((& $over (& $cell -In @{ Blocks = 0 })).Count -eq 0)
    # 读日志：OverBlocks 的数法
    $text = New-FakeProbeLogText -Role I -Blocks 10 -NOverride 600 -NOverrideAt 3
    $read = Read-ReaperProbeLog -Text $text
    & $add '读日志：OverBlocks 数的是 n 大于该块自己 n_decl 的块，一例写出块序号、n、n_decl；MaxN 与 NDecl 仍照记' $true (
        $read.OverBlocks -eq 1 -and $read.OverExample -ceq '块序号 3 的 n=600 大于它自己的 n_decl=512' -and $read.MaxN -eq 600 -and (@($read.NDecl) -join ',') -ceq '512')
    $cases
}

function Get-ProbeSelfTestCases {
    param([Parameter(Mandatory)][string]$Tmp, [Parameter(Mandatory)][string]$RepoRoot)
    $all = [Collections.Generic.List[object]]::new()
    foreach ($fn in { Get-CollectCases $Tmp }, { Get-CellCases }, { Get-AnalyzerCases $RepoRoot $Tmp }, { Get-RppProbeCases $RepoRoot }, { Get-ProbeConfigCases $RepoRoot $Tmp },
                    { Get-ProbePreflightCases $Tmp }, { Get-CompleteCellCases $Tmp }, { Get-WholeCases $Tmp }, { Get-SampleCases },
                    { Get-ListingCases $Tmp }, { Get-LinkUnknownCases }, { Get-WriterCases $Tmp }, { Get-PinGuardCases $Tmp }, { Get-BlockLimitCases }, { Get-PythonCases $Tmp }) {
        foreach ($c in (& $fn)) { $all.Add($c) }
    }
    $all
}

Export-ModuleMember -Function Get-ProbeSelfTestCases, New-FakeProbeLogText, New-LogForName, New-FakePython, New-FakeNullTest, New-FakeProbeBuild, Get-FakeCompareText
