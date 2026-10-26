# SPDX-License-Identifier: GPL-3.0-or-later
#
# 第二段：探针日志的收取与读出。这里的函数自己不启动 REAPER。
# 探针不读环境变量，日志一律写在用户文档目录下的「Rinne Probe Logs」，文件名 probe-<I|O>-<pid>-<tok4>.csv。
# 维护者接着要在 Cubase 里往同一个目录写上机数据，REAPER 留下的日志不能混进去，所以每个 REAPER 进程退出后，
# 把它自己的日志搬走：复制到本轮的结果目录，核对 sha256 相同，再只删那一个原文件。
#   Get-ReaperProbeLogDir          文档目录下的 Rinne Probe Logs 的路径（只取路径，不建目录）
#   Get-ReaperProbeLogListing      目录里全部条目的清单（相对名 → 大小|修改时间刻度|重解析点 0/1|硬链接数，子目录记 dir，重解析点的目录记 dir|link；硬链接数一时取不到记 ?）
#   Compare-ReaperProbeLogListing  前后两份清单：除本轮新出现的自己的文件外必须完全相同；变化的名字不回显别人的文件名（只写散列与变化类型）；
#                                  某一项一侧的硬链接数是 ? 时只比前三段，不判变，另记一笔提示（Compare-ReaperListingValue）
#   ConvertTo-ReaperSafeListing    清单落盘前的改写：不是探针日志命名的条目，名字换成「每轮随机盐 + 名字」的短散列（盐不落盘）
#   Get-ReaperPidHolder / Get-ReaperWriterState  pid 现在属于哪个进程、它能不能是写这份日志的进程（pid 可能已被复用）
#   Select-ReaperOwnProbeLogs      纯函数：一组条目里哪些是本轮自己的
#   Get-ReaperCreationRefusal      纯函数：钉住的文件的创建时间要落在「这次启动前一刻 ~ 这次启动的 REAPER 退出 + 2 s」里
#   Get-ReaperHeaderRefusal        纯函数：钉住的文件的 H 行（第 2 行）要是 REAPER 里的探针写的（host_kind=other）、与文件名的 role / pid / tok4 一致
#   Move-ReaperProbeLogs           钉住原文件、复制、核对哈希、按同一个句柄删它；哈希不同不删并判红
#   Open-ReaperPinnedFile          以「别人不能写、不能改名、不能删」的方式打开一个普通文件，拿到的句柄就是之后读、算哈希、删的那个对象
#   Read-ReaperProbeLog            纯函数：从一份日志文本读出头、测试格、块长、配对、Ping、D、丢记录与收尾
#   Get-ReaperProbeLogColumns      各行类型的列名与列序（与 analyze-probe.py 的规格逐字相同，自测对拍）
# 删除的规矩（缺一条就不删）：条目直接在日志目录里（不进子目录）；是普通文件（目录、链接一律不碰，硬链接数大于 1 的也不碰）；名字恰好是
# probe-<I|O>-<pid>-<tok4>.csv，pid 写成不补零的十进制；pid 是本轮自己启动、并且已经退出的 REAPER（这个 pid 现在属于别的进程、
# 而那个进程比这份日志最后一次写入还晚开始，是 pid 被复用，写日志的进程其实已经退出，照常搬，并记一笔；读不到开始时间按「还在」处理）；修改时间落在
# 这次启动到现在之间（既不早于启动，也不晚于现在）；原文件钉住之后，大小与修改时间还是列目录时看到的那个；
# 钉住的这个对象的创建时间落在这次启动到这次启动的 REAPER 退出（加 2 s 余量）之间；它的 H 行（第 2 行，从同一个句柄读）是 host_kind=other
# （REAPER；Cubase 里的探针写 cubase）、role / pid / tok4 与文件名一致，读不出 H 行也不动；
# 复制件的 sha256 与钉住的原文件相同。删除走钉住的那个句柄（FileDispositionInfo），不再按路径：从读内容、算哈希到删除，
# 中间没有别的对象能顶替它（共享方式只放行读，别人写不了、改不了名、删不了）。删的只会是已经原样复制出去的那个文件，
# 也不进子目录、不递归、不删目录。
# 清单比较：本轮新出现的自己的文件（清单开头本来没有的名字）不参与比较；开头就在清单里的名字，哪怕是「自己的」，也照常比，
# 变了或没了就是别人的文件被动了——时间与 pid 的筛选万一选错，这一道照样会红。硬链接数是尽力取的：某一项一侧没取到（?）时只比大小、
# 修改时间与重解析点，不判变（前三段任何一段变了照样判变），另在结果的提示里记一笔；两侧都取到才比硬链接数。
# 清单里记全部条目（别人的文件也记，比较要看全部），但结果目录、控制台、结果 json 里不留别人的文件名：落盘的清单和比较报出来的
# 变化，凡不是探针日志命名的条目，名字都换成短散列。
# 写它的进程还在不在：只按 pid 查会被 pid 复用骗到（REAPER 退出后，同一个 pid 可能已经分给了别的进程）。所以再看现在持有这个 pid 的进程
# 是什么时候开始的：它比这份日志最后一次被写入还晚开始，就不可能是写它的那个。拿它的开始时间去比「这次启动的时刻」是不行的——
# 真正的 REAPER 恰恰就是这次启动之后才开始的。取不到开始时间时按「还在」处理（不动文件），并在原因里写明可能是 pid 复用。
# pid 没人持有（Gone）时 pid 判据说明不了什么：同号的别的进程（例如 Cubase 里的探针）若在窗口里写了日志、又已经退出，名字、pid、修改时间都对得上。
# 所以钉住之后再看这个对象自己：H 行里的 host_kind 与创建时间（写它的进程已退出的都要过，包括上面照常搬走的 pid 被复用的那一种）。
# 创建时间的上界是这次启动的 REAPER 退出的时刻（加 2 s 余量）：Process.ExitTime 在真机上偶尔读出 1601 年，Host.psm1 的 Resolve-ReaperExitedUtc
# 已把这样的值换成检测到退出的当下，这里拿到的就是可用的上界。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 钉住文件用的几个系统调用。CreateFile 带 DELETE，共享方式只放行读，并带 OPEN_REPARSE_POINT（链接被当成链接本身，不被跟到目标上）。
# 类型名带版本后缀：同一个会话里再导入改过的本文件时，旧类型不会被悄悄沿用。
if (-not ('RinneProbeFileV3' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RinneProbeFileV3 {
    [StructLayout(LayoutKind.Sequential)] struct FILETIME { public uint Low; public uint High; }
    [StructLayout(LayoutKind.Sequential)] struct BY_HANDLE_FILE_INFORMATION {
        public uint FileAttributes; public FILETIME CreationTime; public FILETIME LastAccessTime; public FILETIME LastWriteTime;
        public uint VolumeSerialNumber; public uint FileSizeHigh; public uint FileSizeLow; public uint NumberOfLinks; public uint FileIndexHigh; public uint FileIndexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out BY_HANDLE_FILE_INFORMATION info);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle handle, int infoClass, ref byte info, uint size);
    public sealed class Opened {
        public SafeFileHandle Handle; public int Error; public uint Attributes; public long Length; public long LastWriteUtcTicks; public long CreationUtcTicks; public string FileId; public uint Links;
    }
    public static Opened Open(string path) {
        var r = new Opened();
        // GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES；共享只放行读；OPEN_EXISTING；FILE_FLAG_OPEN_REPARSE_POINT
        SafeFileHandle h = CreateFileW(path, 0x80000000u | 0x00010000u | 0x80u, 1u, IntPtr.Zero, 3u, 0x00200000u, IntPtr.Zero);
        if (h.IsInvalid) { r.Error = Marshal.GetLastWin32Error(); h.Dispose(); return r; }
        BY_HANDLE_FILE_INFORMATION i;
        if (!GetFileInformationByHandle(h, out i)) { r.Error = Marshal.GetLastWin32Error(); h.Dispose(); return r; }
        r.Handle = h; r.Attributes = i.FileAttributes; r.Links = i.NumberOfLinks;
        r.Length = ((long)i.FileSizeHigh << 32) | i.FileSizeLow;
        r.LastWriteUtcTicks = DateTime.FromFileTimeUtc(((long)i.LastWriteTime.High << 32) | i.LastWriteTime.Low).Ticks;
        r.CreationUtcTicks = DateTime.FromFileTimeUtc(((long)i.CreationTime.High << 32) | i.CreationTime.Low).Ticks;
        r.FileId = i.VolumeSerialNumber.ToString("x8") + "-" + i.FileIndexHigh.ToString("x8") + i.FileIndexLow.ToString("x8");
        return r;
    }
    // FILE_DISPOSITION_INFO（信息类 4）：在这个句柄上标记删除，最后一个句柄关闭时生效。只删这个句柄指的对象。
    public static bool MarkDelete(SafeFileHandle h) { byte yes = 1; return SetFileInformationByHandle(h, 4, ref yes, 1); }
}
'@
}

# 清单要记硬链接数：只要 FILE_READ_ATTRIBUTES（读属性不和任何共享方式冲突），共享全放行，不占着别人的文件，也不挡别人写。
# 带 OPEN_REPARSE_POINT（链接按链接本身看）与 BACKUP_SEMANTICS（目录也打得开）。打不开返回 -1。
if (-not ('RinneProbeInfoV1' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RinneProbeInfoV1 {
    [StructLayout(LayoutKind.Sequential)] struct FILETIME { public uint Low; public uint High; }
    [StructLayout(LayoutKind.Sequential)] struct BY_HANDLE_FILE_INFORMATION {
        public uint FileAttributes; public FILETIME CreationTime; public FILETIME LastAccessTime; public FILETIME LastWriteTime;
        public uint VolumeSerialNumber; public uint FileSizeHigh; public uint FileSizeLow; public uint NumberOfLinks; public uint FileIndexHigh; public uint FileIndexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out BY_HANDLE_FILE_INFORMATION info);
    public static long Links(string path) {
        using (SafeFileHandle h = CreateFileW(path, 0x80u, 7u, IntPtr.Zero, 3u, 0x00200000u | 0x02000000u, IntPtr.Zero)) {
            if (h.IsInvalid) return -1;
            BY_HANDLE_FILE_INFORMATION i;
            if (!GetFileInformationByHandle(h, out i)) return -1;
            return i.NumberOfLinks;
        }
    }
}
'@
}

$script:LogDirName = 'Rinne Probe Logs'
# pid 写成不补零的十进制（探针从不写补零的名字；补零的名字不是它写的）
$script:NamePattern = '^probe-([IO])-(0|[1-9][0-9]{0,9})-([0-9a-f]{4})\.csv$'
$script:UntilSlackSeconds = 2 # 修改时间的上界留一点余量（文件系统时间戳的粒度）
# 行类型 → 列名（第一列的行类型不算）。按名字取列，不写死下标；与 scripts/probe/analyze-probe.py 的 SPEC rows 逐字相同（自测对拍）
$script:Columns = [ordered]@{
    H = 'role format host_kind os arch pid ppid tok4 build' -split ' '
    C = 't_ns case variant buf guard sr' -split ' '
    B = 'seq call_seq role n n_decl host_ns host_ns_valid playing nonrt bypassed mono_ns thread m w off rs d' -split ' '
    S = 't_ns blocks dropped max_n resync trips' -split ' '
    E = 't_ns name a b' -split ' '
    Z = 't_ns blocks_total dropped_total' -split ' '
}
function Get-ReaperProbeLogColumns { $script:Columns }

# 探针（JUCE 的 userDocumentsDirectory）与这里取的是同一个「文档」：本机上它可能在 OneDrive 下。取不到返回空串。
function Get-ReaperProbeLogDir {
    param([string]$DocumentsDir = ([Environment]::GetFolderPath('MyDocuments')))
    if ([string]::IsNullOrWhiteSpace($DocumentsDir)) { return '' }
    Join-Path $DocumentsDir $script:LogDirName
}

# 清单里一个条目的值（纯函数）。文件：大小|修改时间刻度|是否重解析点（1/0）|硬链接数（取不到记 ?）；子目录：dir，是重解析点的目录（联接）
# 记 dir|link。别人的文件被换成大小、修改时间都相同的链接时，重解析点标记或硬链接数会变，前后比较就看得出来。
function Format-ReaperListingValue {
    param([bool]$IsDir, [long]$Length = 0, [long]$Ticks = 0, [bool]$IsReparse = $false, [long]$Links = -1)
    if ($IsDir) { return ($IsReparse ? 'dir|link' : 'dir') }
    '{0}|{1}|{2}|{3}' -f $Length, $Ticks, ($IsReparse ? 1 : 0), ($Links -ge 0 ? "$Links" : '?')
}

# 目录里全部条目（递归）：值的写法见 Format-ReaperListingValue。目录不存在返回空表；列不出来就抛错（调用方判红并停手，
# 不能把「看不见」当作「没变」）。硬链接数是尽力而为：某一项取不到记 ?（不抛错）。
function Get-ReaperProbeLogListing {
    param([Parameter(Mandatory)][string]$Dir)
    $out = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return $out }
    $prefix = [IO.Path]::GetFullPath($Dir).TrimEnd('\') + '\'
    foreach ($e in Get-ChildItem -LiteralPath $Dir -Recurse -Force -ErrorAction Stop) {
        $reparseFlag = [bool]($e.Attributes -band [IO.FileAttributes]::ReparsePoint)
        $linkCount = $e.PSIsContainer ? -1 : [RinneProbeInfoV1]::Links($e.FullName)
        $out[$e.FullName.Substring($prefix.Length)] = Format-ReaperListingValue -IsDir $e.PSIsContainer -Length ($e.PSIsContainer ? 0 : $e.Length) -Ticks $e.LastWriteTimeUtc.Ticks -IsReparse $reparseFlag -Links $linkCount
    }
    $out
}

# 每轮一把随机盐（只在内存里，不落盘、不进结果）：同一轮里前后两份清单与变化报告用同一把，名字的散列能对上；换一轮就对不上了，
# 也没法拿字典去撞。
function New-ReaperListingSalt { [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(16)).ToLowerInvariant() }

# 一个条目名在落盘/报告里怎么写：名字恰好是探针日志命名的（顶层的 probe-<I|O>-<pid>-<tok4>.csv，维护者自己的上机日志也是这个名字）原样写；
# 其余（别人的文件、子目录、子目录里的东西）一律写成 other-<盐加名字的 sha256 前 8 位>，不可逆。
function Get-ReaperListingShownName {
    param([Parameter(Mandatory)][string]$EntryName, [Parameter(Mandatory)][string]$Salt)
    if ($EntryName -cmatch $script:NamePattern) { return $EntryName }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { 'other-' + [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes("$Salt|$EntryName"))).Substring(0, 8).ToLowerInvariant() } finally { $sha.Dispose() }
}

# 清单落盘前的改写：比较要看全部条目，所以内存里的清单不改；写进结果目录的这一份，不是探针日志命名的条目只留散列（大小、修改时间、
# 链接标记照写，排查时要看）。条目数不变；两个名字撞上同一个散列时（概率可忽略）后一个加序号，不丢条目。
function ConvertTo-ReaperSafeListing {
    param([Parameter(Mandatory)]$Listing, [Parameter(Mandatory)][string]$Salt)
    $out = [ordered]@{}
    foreach ($k in $Listing.Keys) {
        $shown = Get-ReaperListingShownName -EntryName $k -Salt $Salt
        for ($n = 2; $out.Contains($shown); $n++) { $shown = (Get-ReaperListingShownName -EntryName $k -Salt $Salt) + "-$n" }
        $out[$shown] = $Listing[$k]
    }
    $out
}

# 纯函数：同一个名字前后两份清单的值算不算一样。文件的值是「大小|刻度|重解析点|硬链接数」，硬链接数是尽力取的，一时取不到记 ?
# （OneDrive 正在同步、杀毒软件短暂独占……）。一侧是 ? 时只比前三段，不因为「这一次没取到」判变，返回 LinkUnknown 让调用方另记一笔；
# 两侧都取到才比硬链接数。注意这条只放宽「取不到」本身：前三段任何一段变了，不管硬链接数取没取到，照样判变。目录（dir、dir|link）等别的写法整串比。
function Compare-ReaperListingValue {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Before, [Parameter(Mandatory)][AllowEmptyString()][string]$After)
    $shape = '^([0-9]+\|[0-9]+\|[01])\|([0-9]+|\?)$'
    $b = [regex]::Match($Before, $shape)
    $a = [regex]::Match($After, $shape)
    if (-not ($b.Success -and $a.Success)) { return [pscustomobject]@{ Same = ($Before -ceq $After); LinkUnknown = $false } }
    if ($b.Groups[1].Value -cne $a.Groups[1].Value) { return [pscustomobject]@{ Same = $false; LinkUnknown = $false } }
    $unknown = ($b.Groups[2].Value -ceq '?') -or ($a.Groups[2].Value -ceq '?')
    [pscustomobject]@{ Same = ($unknown -or ($Before -ceq $After)); LinkUnknown = $unknown }
}

# 前后两份清单。Own 是本轮认作自己的文件名（搬走的与没搬走的都算）：其中清单开头本来就没有的，是本轮新出现的文件，不参与比较
# （它们搬走了，或者留着、另有判红）；清单开头就有的名字不除外，照常比——自己的文件不可能在本轮开始之前就在，
# 这样的名字出现在 Own 里说明时间与 pid 的筛选选错了，它被搬走或被动了就是变化，一轮判 FAIL。
function Compare-ReaperProbeLogListing {
    param([Parameter(Mandatory)]$Before, [Parameter(Mandatory)]$After, [string[]]$Own = @(), [string]$Salt = '')
    if (-not $Salt) { $Salt = New-ReaperListingSalt } # 不给盐也照样遮：报告里不留别人的文件名
    $skip = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($n in $Own) { if (-not $Before.Contains($n)) { $null = $skip.Add($n) } }
    $changed = [Collections.Generic.List[string]]::new()
    $shown = { param($entry) Get-ReaperListingShownName -EntryName $entry -Salt $Salt }
    $linkUnknown = [Collections.Generic.List[string]]::new()
    foreach ($k in $Before.Keys) {
        if (-not $After.Contains($k)) { $changed.Add("removed:$(& $shown $k)"); continue }
        $v = Compare-ReaperListingValue -Before $Before[$k] -After $After[$k]
        if (-not $v.Same) { $changed.Add("changed:$(& $shown $k)") }
        elseif ($v.LinkUnknown) { $linkUnknown.Add((& $shown $k)) }
    }
    foreach ($k in $After.Keys) { if (-not $skip.Contains($k) -and -not $Before.Contains($k)) { $changed.Add("added:$(& $shown $k)") } }
    # 硬链接数没取到的条目不判变，只在提示里记一笔（名字照样只写散列）；提示最多列 8 项，数目写全
    $notes = [Collections.Generic.List[string]]::new()
    if ($linkUnknown.Count) {
        $listed = (@($linkUnknown | Select-Object -First 8) -join '、') + ($linkUnknown.Count -gt 8 ? '……' : '')
        $notes.Add("硬链接数没取到：$($linkUnknown.Count) 项的前后清单里至少一侧是 ?（一时打不开，例如 OneDrive 正在同步、杀毒软件短暂独占），这几项只比了大小、修改时间与重解析点，没比硬链接数（$listed）")
    }
    [pscustomobject]@{ Same = ($changed.Count -eq 0); Count = $Before.Count; Changed = $changed.ToArray(); LinkUnknown = $linkUnknown.ToArray(); Notes = $notes.ToArray() }
}

# 纯函数。Entries 的每一项要有 Name、IsFile、IsLink、LastWriteUtc（[datetime]，UTC）。返回本轮自己的那些（原样），
# 以及每个被排除的条目为什么被排除（Name 不是日志名的不报：目录里本来就可能有别的文件）。
# 修改时间要落在 [SinceUtc, UntilUtc] 里：启动之前的是同号的旧进程留下的，晚于现在的不是这次启动的进程能写出来的。
function Select-ReaperOwnProbeLogs {
    param([object[]]$Entries = @(), [int[]]$Pids = @(), [Parameter(Mandatory)][datetime]$SinceUtc, [Parameter(Mandatory)][datetime]$UntilUtc)
    $mine = [Collections.Generic.List[object]]::new()
    $skipped = [Collections.Generic.List[string]]::new()
    foreach ($e in $Entries) {
        $m = [regex]::Match([string]$e.Name, $script:NamePattern)
        if (-not $m.Success) { continue }
        $logPid = [long]$m.Groups[2].Value
        if ($logPid -notin $Pids) { continue } # 别的进程（包括用户自己的宿主）的日志：一眼都不多看
        if (-not $e.IsFile -or $e.IsLink) { $skipped.Add("$($e.Name)：不是普通文件，不碰"); continue }
        if ($e.LastWriteUtc -lt $SinceUtc) { $skipped.Add("$($e.Name)：修改时间早于这次启动（同号的旧进程留下的），不碰"); continue }
        if ($e.LastWriteUtc -gt $UntilUtc) { $skipped.Add("$($e.Name)：修改时间晚于现在（这次启动的进程写不出来），不碰"); continue }
        $mine.Add($e)
    }
    [pscustomobject]@{ Own = $mine.ToArray(); Skipped = $skipped.ToArray() }
}

function Get-ReaperDirEntries([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return @() }
    foreach ($i in Get-ChildItem -LiteralPath $Dir -Force -ErrorAction Stop) { # 不递归：探针只往这一层写
        [pscustomobject]@{
            Name = $i.Name; FullName = $i.FullName; IsFile = -not $i.PSIsContainer
            IsLink = [bool]($i.Attributes -band [IO.FileAttributes]::ReparsePoint); LastWriteUtc = $i.LastWriteTimeUtc
            Length = ($i.PSIsContainer ? -1 : $i.Length)
        }
    }
}

# 纯函数：句柄上读到的属性 → 不能当普通文件收的理由（目录、重解析点即链接），可以收返回 $null
function Get-ReaperPinnedRefusal([uint32]$Attributes) {
    if ($Attributes -band 0x10) { return '是目录' }
    if ($Attributes -band 0x400) { return '是链接（重解析点）' }
    $null
}

# 钉住一个文件：打开时共享方式只放行读，别的程序在这期间写不了、改不了名、删不了；带 OPEN_REPARSE_POINT，链接被当成链接本身而不是被跟到
# 目标上。返回 @{ Ok; Why; Stream（只读）; Handle; Length; LastWriteUtc; CreationUtc; FileId; Links }。被别的程序占着（共享冲突）时稍等几次再试。
# 用完要 Close-ReaperPinnedFile；标记过删除的，在最后一个句柄关闭时才真的删。
function Open-ReaperPinnedFile {
    param([Parameter(Mandatory)][string]$Path, [int]$Tries = 10)
    $failed = { param($why) [pscustomobject]@{ Ok = $false; Why = $why; Stream = $null; Handle = $null; Length = -1; LastWriteUtc = [DateTime]::MinValue; CreationUtc = [DateTime]::MinValue; FileId = ''; Links = 0 } }
    for ($try = 1; $true; $try++) {
        $o = [RinneProbeFileV3]::Open($Path)
        if (-not $o.Error) { break }
        if ($o.Error -ne 32 -or $try -ge $Tries) {
            $why = switch ($o.Error) { 2 { '文件不见了' } 3 { '路径不见了' } 5 { '拒绝访问（可能是目录）' } 32 { '被别的程序占着' } default { "打不开（系统错误 $($o.Error)）" } }
            return (& $failed $why)
        }
        Start-Sleep -Milliseconds 300
    }
    $refusal = Get-ReaperPinnedRefusal $o.Attributes
    if ($refusal) { $o.Handle.Dispose(); return (& $failed $refusal) }
    $stream = [IO.FileStream]::new($o.Handle, [IO.FileAccess]::Read, 4096, $false)
    [pscustomobject]@{
        Ok = $true; Why = ''; Stream = $stream; Handle = $o.Handle; Length = $o.Length
        LastWriteUtc = [DateTime]::new($o.LastWriteUtcTicks, [DateTimeKind]::Utc); CreationUtc = [DateTime]::new($o.CreationUtcTicks, [DateTimeKind]::Utc); FileId = $o.FileId; Links = $o.Links
    }
}

function Close-ReaperPinnedFile($Pinned) {
    if ($Pinned.Stream) { $Pinned.Stream.Dispose() }
    if ($Pinned.Handle) { $Pinned.Handle.Dispose() }
}

# 纯函数：钉住的文件的创建时间 → 不能当本轮日志收的理由；可以收返回 $null。创建时间要落在 [SinceUtc, ExitedUtc + 余量] 里：
# SinceUtc 是这次启动前一刻，ExitedUtc 是这次启动的 REAPER 进程退出的时刻（取不到就是收日志的当下）。pid 复用只能发生在原进程退出之后，
# 复用者新建的文件创建时间一定晚于它；启动之前创建的是更早的进程留下的。比修改时间硬：修改时间谁都能改（复用者在窗口里追加写一笔就落进窗口），
# 创建时间在文件系统里只有新建时才定。
# NTFS 文件名隧道：同一目录里同名文件被删或改名之后 15 s 内又以同名新建，新文件会继承旧文件的创建时间。继承只会让创建时间变早，所以：
#   下界一侧：旧文件在启动之前创建的，新文件继承了它，会被判「早于启动」，只会多拒绝，不会多放行；
#   上界一侧：复用者的文件若继承了更早的创建时间，这一道会被绕过——但前提是同一个文件名在 15 s 内刚被删过或改过名，而文件名里有这次启动的
#   pid 和探针随机取的 tok4，写出这个名字的只有这次启动的 REAPER 里的探针，它的日志在收日志之前不会被删或改名，所以这个缺口在这里够不着；
#   且这一道本来就不是唯一一道：H 行的 host_kind、pid、tok4 要对得上，pid 持有者的开始时间也要对得上。
function Get-ReaperCreationRefusal {
    param([Parameter(Mandatory)][datetime]$CreationUtc, [Parameter(Mandatory)][datetime]$SinceUtc, [Parameter(Mandatory)][datetime]$ExitedUtc)
    if ($CreationUtc -lt $SinceUtc) { return '创建时间早于这次启动（不是这次启动的进程新建的）' }
    if ($CreationUtc -gt $ExitedUtc.AddSeconds($script:UntilSlackSeconds)) { return '创建时间晚于这次启动的 REAPER 退出（只有 pid 被复用的进程才会在那之后新建这个文件）' }
    $null
}

# 从钉住的流里读文件开头（最多 MaxBytes 字节，按 ASCII 解码），读完把位置放回 0。读的就是之后算哈希、复制、删除的那个对象。
function Read-ReaperPinnedHead {
    param([Parameter(Mandatory)][IO.Stream]$Pinned, [int]$MaxBytes = 1024)
    $Pinned.Position = 0
    $buf = [byte[]]::new([int][Math]::Min([long]$MaxBytes, [Math]::Max(0L, $Pinned.Length)))
    $got = 0
    while ($got -lt $buf.Length) {
        $n = $Pinned.Read($buf, $got, $buf.Length - $got)
        if ($n -le 0) { break }
        $got += $n
    }
    $Pinned.Position = 0
    [Text.Encoding]::ASCII.GetString($buf, 0, $got)
}

# 纯函数：文件开头的文本（Head）和文件名 → 它的 H 行（第 2 行）是不是这次启动的 REAPER 写出来的。对得上返回 $null，否则返回理由。
# 要求：第 1 行是魔数、第 2 行是写完的 H 行（探针日志格式：LF 结尾，列数与列序见 $script:Columns）；host_kind 是 ExpectHostKind
# （REAPER 不是 Cubase 也不是 LUNA，探针按 other 写；Cubase 里的探针写 cubase，pid 被复用时就是靠它认出别人的日志）；role、pid、tok4 与文件名一致。
# 理由里不回显文件里的任意内容：host_kind 只在它属于枚举（cubase / luna / other）时才写出来。
function Get-ReaperHeaderRefusal {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Head, [Parameter(Mandatory)][string]$LogName, [string]$ExpectHostKind = 'other')
    $nm = [regex]::Match($LogName, $script:NamePattern)
    if (-not $nm.Success) { return '名字不是探针日志的名字' }
    if (($Head.Split("`n").Count - 1) -lt 2) { return '读不出 H 行（文件太短，第 2 行没写完）' }
    $lines = $Head.Split("`n")
    if ($lines[0] -cne 'rinne-probe-log,1') { return '读不出 H 行（第 1 行不是探针日志的魔数）' }
    $cols = [string[]]$script:Columns['H']
    $f = $lines[1].Split(',')
    if ($f.Count -ne $cols.Count + 1 -or $f[0] -cne 'H') { return '读不出 H 行（第 2 行不是 H 行，或列数不对）' }
    $at = { param($col) $f[1 + [array]::IndexOf($cols, $col)] }
    $kind = & $at 'host_kind'
    if ($kind -cne $ExpectHostKind) {
        $shown = ($kind -cin 'cubase', 'luna', 'other') ? $kind : '不在枚举里的值'
        return "H 行的 host_kind 是 $shown，不是 $ExpectHostKind（不是 REAPER 里的探针写的）"
    }
    if ((& $at 'role') -cne $nm.Groups[1].Value) { return 'H 行的 role 与文件名不符' }
    if ((& $at 'pid') -cne $nm.Groups[2].Value) { return 'H 行的 pid 与文件名不符' }
    if ((& $at 'tok4') -cne $nm.Groups[3].Value) { return 'H 行的 tok4 与文件名不符' }
    $null
}

function Get-ReaperStreamSha256([IO.Stream]$Source) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [Convert]::ToHexString($sha.ComputeHash($Source)) } finally { $sha.Dispose() }
}

function Get-ReaperSha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# 这个 pid 现在属于谁。返回 @{ Exists; StartUtc }：没有这个进程、或查的当口它退出了，Exists 为假；有进程却读不到开始时间
# （例如拒绝访问），Exists 为真、StartUtc 为 $null。
function Get-ReaperPidHolder([int]$ProcessId) {
    $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $p) { return [pscustomobject]@{ Exists = $false; StartUtc = $null } }
    try { return [pscustomobject]@{ Exists = $true; StartUtc = $p.StartTime.ToUniversalTime() } }
    catch {
        $exited = try { [bool]$p.HasExited } catch { $false }
        return [pscustomobject]@{ Exists = (-not $exited); StartUtc = $null }
    }
}

# 纯函数：现在持有这个 pid 的进程（Holder，见上）能不能是写这份日志（最后一次写入在 LastWriteUtc）的进程。
#   Gone     没有这个进程：写它的进程已经退出
#   Reused   有进程，但它比这份日志最后一次写入还晚开始（留 UntilSlackSeconds 的时钟余量）：不可能是写它的，pid 被复用了
#   Alive    有进程，开始时间不晚于最后一次写入：写它的进程还在（真正的 REAPER 是这次启动之后才开始的，所以不能拿「这次启动的时刻」比）
#   Unknown  有进程但读不到它的开始时间：没法排除 pid 复用，按「还在」处理（不动文件）
function Get-ReaperWriterState {
    param([Parameter(Mandatory)]$Holder, [Parameter(Mandatory)][datetime]$LastWriteUtc)
    if (-not $Holder.Exists) { return 'Gone' }
    if ($null -eq $Holder.StartUtc) { return 'Unknown' }
    if ($Holder.StartUtc -gt $LastWriteUtc.AddSeconds($script:UntilSlackSeconds)) { return 'Reused' }
    'Alive'
}

# 把 Pids 里（已退出的）进程在 LogDir 里留下的日志搬到 DestDir。返回 @{ Moved（搬走并删了原文件的名字）; Kept（自己的、但留在原处的，
# 每项带原因）; Skipped（没认作自己的同 pid 条目与原因）; Own（本轮自己的全部名字）; Problems（要判红的）; Notes（照常搬走、但值得记一笔的，
# 例如 pid 已被别的进程复用）}。写它的进程还在不在，见 Get-ReaperWriterState。
# UntilUtc 不给就取现在（再留 2 s 余量）：修改时间的上界。ExitedUtc 是这次启动的 REAPER 进程退出的时刻，不给就取现在：钉住的文件的创建时间要落在
# [SinceUtc, ExitedUtc + 2 s]（见 Get-ReaperCreationRefusal），H 行要对得上（见 Get-ReaperHeaderRefusal）。ProcessInfo、BeforePin、AfterCopy 只给自测注入：ProcessInfo 代替 Get-ReaperPidHolder
# （收到 pid，返回 @{ Exists; StartUtc }）；BeforePin 收到原文件路径，在列完目录、钉住之前调用
# （自测在这里换掉原文件）；AfterCopy 收到原文件与复制件的路径，在复制件写完、原文件仍被钉住时调用（自测在这里弄坏复制件，
# 或者试着改动被钉住的原文件）。
function Move-ReaperProbeLogs {
    param(
        [Parameter(Mandatory)][string]$LogDir, [Parameter(Mandatory)][int[]]$Pids, [Parameter(Mandatory)][datetime]$SinceUtc,
        [Parameter(Mandatory)][string]$DestDir, [datetime]$UntilUtc = [datetime]::MinValue, [datetime]$ExitedUtc = [datetime]::MinValue, [scriptblock]$BeforePin = $null, [scriptblock]$AfterCopy = $null, [int]$PinTries = 10, [scriptblock]$ProcessInfo = $null
    )
    if ($UntilUtc -eq [datetime]::MinValue) { $UntilUtc = [DateTime]::UtcNow.AddSeconds($script:UntilSlackSeconds) }
    if ($ExitedUtc -eq [datetime]::MinValue) { $ExitedUtc = [DateTime]::UtcNow } # 没给退出时刻（只有自测会不给）：取现在，创建时间的上界就是现在加余量
    $moved = [Collections.Generic.List[string]]::new()
    $kept = [Collections.Generic.List[string]]::new()
    $problems = [Collections.Generic.List[string]]::new()
    $notes = [Collections.Generic.List[string]]::new()
    $listed = @(Get-ReaperDirEntries $LogDir)
    $pick = Select-ReaperOwnProbeLogs -Entries $listed -Pids $Pids -SinceUtc $SinceUtc -UntilUtc $UntilUtc
    $keep = { param($name, $why) $kept.Add("${name}：$why"); $problems.Add("${name}：$why") }
    foreach ($e in $pick.Own) {
        $logPid = [int][regex]::Match($e.Name, $script:NamePattern).Groups[2].Value
        $pidHolder = $ProcessInfo ? (& $ProcessInfo $logPid) : (Get-ReaperPidHolder $logPid)
        $writer = Get-ReaperWriterState -Holder $pidHolder -LastWriteUtc $e.LastWriteUtc
        if ($writer -in 'Alive', 'Unknown') {
            $maybe = ($writer -eq 'Unknown') ? '（读不到它的开始时间，没法排除 pid 被复用，按还在处理）' : ''
            $kept.Add("$($e.Name)：进程 $logPid 还在，不动$maybe"); $problems.Add("$($e.Name)：写它的进程还没退出$maybe"); continue
        }
        if ($writer -eq 'Reused') { $notes.Add("$($e.Name)：进程号 $logPid 现在是另一个进程（它比这份日志最后一次写入还晚开始，写日志的进程已经退出，pid 被复用），照常搬走") }
        $null = New-Item -ItemType Directory -Force -Path $DestDir
        $target = Join-Path $DestDir $e.Name
        if (Test-Path -LiteralPath $target) { & $keep $e.Name '结果目录里已有同名文件，不覆盖'; continue }
        if ($BeforePin) { & $BeforePin $e.FullName }
        $pin = Open-ReaperPinnedFile -Path $e.FullName -Tries $PinTries
        if (-not $pin.Ok) { & $keep $e.Name "钉不住原文件（$($pin.Why)），不动"; continue }
        try {
            # 钉住的对象还得是列目录时看到的那个（大小与修改时间相同；列目录时的修改时间已经过了窗口筛选）
            if ($pin.Length -ne $e.Length -or $pin.LastWriteUtc -ne $e.LastWriteUtc) { & $keep $e.Name '列目录之后原文件变了（大小或修改时间不同），不动'; continue }
            if ($pin.Links -gt 1) { & $keep $e.Name "有 $($pin.Links) 个硬链接（探针新建的日志只有一个名字），不动"; continue }
            # 两道只有真日志过得去的判据，都对钉住的这个对象：创建时间在这次启动的存活区间里；H 行（第 2 行）是 REAPER 里的探针写的、与文件名一致
            $creationWhy = Get-ReaperCreationRefusal -CreationUtc $pin.CreationUtc -SinceUtc $SinceUtc -ExitedUtc $ExitedUtc
            if ($creationWhy) { & $keep $e.Name "$creationWhy，不动"; continue }
            $headerWhy = Get-ReaperHeaderRefusal -Head (Read-ReaperPinnedHead $pin.Stream) -LogName $e.Name
            if ($headerWhy) { & $keep $e.Name "$headerWhy，不动"; continue }
            $srcHash = Get-ReaperStreamSha256 $pin.Stream
            $pin.Stream.Position = 0
            $out = [IO.FileStream]::new($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None) # 已有同名文件就抛错，不覆盖
            try { $pin.Stream.CopyTo($out); $out.Flush($true) } finally { $out.Dispose() }
            if ($AfterCopy) { & $AfterCopy $e.FullName $target }
            $dstHash = (Test-Path -LiteralPath $target -PathType Leaf) ? (Get-ReaperSha256 $target) : ''
            if ($srcHash -cne $dstHash) { & $keep $e.Name '复制件的 sha256 与原文件不同，原文件不删（复制后哈希不同）'; continue }
            if (-not [RinneProbeFileV3]::MarkDelete($pin.Handle)) { & $keep $e.Name '标记删除失败，原文件不删'; continue }
        } catch {
            & $keep $e.Name "复制出错：$($_.Exception.Message -replace '\s+', ' ')"
            continue
        } finally { Close-ReaperPinnedFile $pin }
        # 句柄关了，标了删除的对象才真的没有：等一下确认名字不在了（别的程序晚一点才放手时会慢一拍）
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        while ((Test-Path -LiteralPath $e.FullName) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
        if (Test-Path -LiteralPath $e.FullName) { $problems.Add("$($e.Name)：复制与核对都过了，但原文件没能删掉"); continue }
        $moved.Add($e.Name)
    }
    [pscustomobject]@{ Moved = $moved.ToArray(); Kept = $kept.ToArray(); Skipped = $pick.Skipped; Own = @($pick.Own | ForEach-Object { $_.Name }); Problems = $problems.ToArray(); Notes = $notes.ToArray() }
}

# 纯函数：一份日志的文本 → 读得出的事实。只读第二段要的几项，格式判定交给 analyze-probe.py，这里读不懂的行不抛错，只计数。
#   Header（role pid tok4 build host_kind）、Cases（每个 C 行）、Blocks / PlayingBlocks、NValues（播放中块长的取值）、NDecl、MaxN、OverBlocks / OverExample（n 大于该块自己的 n_decl 的块数与第一例）、
#   DValues（块记录里的 D）、Pings（ping 事件的样本数）、PairedPids（paired 事件的对端 pid）、Prepares（采样率/块长）、
#   SeqGaps（块序号缺口数）、Dropped（S 行与 Z 行里最大的丢弃计数）、NonRt（块里的 nonrt 取值）、PlayingNonRt（播放中的块里的 nonrt 取值：
#   宿主对插件报的是实时还是离线，只看正在走带的块）、Complete（有 Z 行且以换行结尾）
function Read-ReaperProbeLog {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $facts = [ordered]@{
        Header = $null; Cases = [Collections.Generic.List[object]]::new(); Blocks = 0; PlayingBlocks = 0
        NValues = [Collections.Generic.SortedSet[int]]::new(); NDecl = [Collections.Generic.SortedSet[int]]::new(); MaxN = 0; OverBlocks = 0; OverExample = ''
        DValues = [Collections.Generic.SortedSet[long]]::new(); Pings = [Collections.Generic.List[long]]::new()
        PairedPids = [Collections.Generic.List[long]]::new(); Prepares = [Collections.Generic.List[string]]::new()
        SeqGaps = 0; Dropped = 0; NonRt = [Collections.Generic.SortedSet[int]]::new(); PlayingNonRt = [Collections.Generic.SortedSet[int]]::new()
        Complete = $false; Unread = 0
    }
    # 列号：行类型之后的第几列（f[0] 是行类型）
    $at = @{}
    foreach ($type in $script:Columns.Keys) { $i = 1; foreach ($name in $script:Columns[$type]) { $at["$type.$name"] = $i; $i++ } }
    $want = @{}
    foreach ($type in $script:Columns.Keys) { $want[$type] = $script:Columns[$type].Count + 1 }
    # 块记录最多（64 样本的一格上万行）：几列的下标先取出来
    $bSeq = $at['B.seq']; $bN = $at['B.n']; $bDecl = $at['B.n_decl']; $bPlaying = $at['B.playing']; $bNonRt = $at['B.nonrt']; $bD = $at['B.d']
    $lines = $Text -split "`n"
    $endsWithLf = $Text.EndsWith("`n")
    $lastSeq = -1L
    foreach ($line in $lines) {
        if ($line -eq '' -or $line -ceq 'rinne-probe-log,1') { continue }
        $f = $line -split ','
        if (-not $want.ContainsKey($f[0]) -or $f.Count -ne $want[$f[0]]) { $facts.Unread++; continue } # 列数不对：交给分析器判格式错，这里只记一笔
        $v = { param($name) $f[$at["$($f[0]).$name"]] }
        try {
            switch -CaseSensitive ($f[0]) {
                'H' { $facts.Header = [pscustomobject]@{ Role = (& $v role); HostKind = (& $v host_kind); Pid = [long](& $v pid); Tok4 = (& $v tok4); Build = (& $v build) } }
                'C' { $facts.Cases.Add([pscustomobject]@{ Case = (& $v case); Variant = (& $v variant); Buf = (& $v buf); Guard = (& $v guard); Sr = [int](& $v sr) }) }
                'B' {
                    $seq = [long]$f[$bSeq]; $n = [int]$f[$bN]; $nDecl = [int]$f[$bDecl]; $nonRt = [int]$f[$bNonRt]; $d = [long]$f[$bD]
                    $facts.Blocks++
                    if ($lastSeq -ge 0 -and $seq -ne $lastSeq + 1) { $facts.SeqGaps++ }
                    $lastSeq = $seq
                    $null = $facts.NDecl.Add($nDecl); $null = $facts.NonRt.Add($nonRt)
                    if ($n -gt $facts.MaxN) { $facts.MaxN = $n }
                    # 块长上限逐块比：每个 B 行带自己的 n 与 n_decl（一份日志里可以有多次 prepare，n_decl 不止一个值），不拿全文件的最大值去比某一个声明
                    if ($n -gt $nDecl) { $facts.OverBlocks++; if (-not $facts.OverExample) { $facts.OverExample = "块序号 $seq 的 n=$n 大于它自己的 n_decl=$nDecl" } }
                    if ($f[$bPlaying] -ceq '1') { $facts.PlayingBlocks++; $null = $facts.NValues.Add($n); $null = $facts.PlayingNonRt.Add($nonRt) }
                    $null = $facts.DValues.Add($d)
                }
                'S' { $dropped = [long](& $v dropped); if ($dropped -gt $facts.Dropped) { $facts.Dropped = $dropped } }
                'Z' { $facts.Complete = $endsWithLf; $dropped = [long](& $v dropped_total); if ($dropped -gt $facts.Dropped) { $facts.Dropped = $dropped } }
                'E' {
                    switch -CaseSensitive (& $v name) {
                        'ping' { $facts.Pings.Add([long](& $v a)) }
                        'paired' { $facts.PairedPids.Add([long](& $v a)) }
                        'prepare' { $facts.Prepares.Add("$(& $v a)/$(& $v b)") }
                    }
                }
            }
        } catch [Management.Automation.RuntimeException] {
            $facts.Unread++ # 不是整数（转换失败）：同样交给分析器判格式错，这里只记一笔
        }
    }
    [pscustomobject]$facts
}

Export-ModuleMember -Function Get-ReaperProbeLogDir, Get-ReaperProbeLogListing, Compare-ReaperProbeLogListing, Select-ReaperOwnProbeLogs,
    Move-ReaperProbeLogs, Read-ReaperProbeLog, Get-ReaperProbeLogColumns, Open-ReaperPinnedFile, Close-ReaperPinnedFile, Get-ReaperPinnedRefusal,
    Format-ReaperListingValue, New-ReaperListingSalt, Get-ReaperListingShownName, ConvertTo-ReaperSafeListing, Get-ReaperPidHolder, Get-ReaperWriterState,
    Compare-ReaperListingValue, Get-ReaperCreationRefusal, Get-ReaperHeaderRefusal, Read-ReaperPinnedHead
