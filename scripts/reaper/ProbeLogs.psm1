# SPDX-License-Identifier: GPL-3.0-or-later
#
# 第二段：探针日志的收取与读出。这里的函数自己不启动 REAPER。
# 探针不读环境变量，日志一律写在用户文档目录下的「Rinne Probe Logs」，文件名 probe-<I|O>-<pid>-<tok4>.csv。
# 维护者接着要在 Cubase 里往同一个目录写上机数据，REAPER 留下的日志不能混进去，所以每个 REAPER 进程退出后，
# 把它自己的日志搬走：复制到本轮的结果目录，核对 sha256 相同，再只删那一个原文件。
#   Get-ReaperProbeLogDir          文档目录下的 Rinne Probe Logs 的路径（只取路径，不建目录）
#   Get-ReaperProbeLogListing      目录里全部条目的清单（相对名 → 大小|修改时间刻度，子目录记 dir）
#   Compare-ReaperProbeLogListing  前后两份清单：除本轮自己的文件外必须完全相同
#   Select-ReaperOwnProbeLogs      纯函数：一组条目里哪些是本轮自己的
#   Move-ReaperProbeLogs           复制、核对哈希、只删那一个原文件；哈希不同不删并判红
#   Read-ReaperProbeLog            纯函数：从一份日志文本读出头、测试格、块长、配对、Ping、D、丢记录与收尾
#   Get-ReaperProbeLogColumns      各行类型的列名与列序（与 analyze-probe.py 的规格逐字相同，自测对拍）
# 删除的规矩（缺一条就不删）：条目直接在日志目录里（不进子目录）；是普通文件（目录、链接一律不碰）；名字恰好是
# probe-<I|O>-<pid>-<tok4>.csv；pid 是本轮自己启动、并且已经退出的 REAPER；修改时间不早于那次启动；复制件的 sha256 与原文件相同；
# 删之前再看一眼大小与修改时间没变。删用 [IO.File]::Delete：只删得了单个文件，删不了目录，也不递归。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:LogDirName = 'Rinne Probe Logs'
$script:NamePattern = '^probe-([IO])-([0-9]{1,10})-([0-9a-f]{4})\.csv$'
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

# 目录里全部条目（递归）：文件记「大小|修改时间刻度」，子目录只记 dir。目录不存在返回空表；列不出来就抛错（调用方判红并停手，
# 不能把「看不见」当作「没变」）。
function Get-ReaperProbeLogListing {
    param([Parameter(Mandatory)][string]$Dir)
    $out = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return $out }
    $prefix = [IO.Path]::GetFullPath($Dir).TrimEnd('\') + '\'
    foreach ($e in Get-ChildItem -LiteralPath $Dir -Recurse -Force -ErrorAction Stop) {
        $out[$e.FullName.Substring($prefix.Length)] = $e.PSIsContainer ? 'dir' : ('{0}|{1}' -f $e.Length, $e.LastWriteTimeUtc.Ticks)
    }
    $out
}

# 前后两份清单。Own 是本轮自己的文件名（搬走的与没搬走的都算）：它们不参与比较，别的条目增、删、改一项就是 Same = $false。
function Compare-ReaperProbeLogListing {
    param([Parameter(Mandatory)]$Before, [Parameter(Mandatory)]$After, [string[]]$Own = @())
    $skip = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($n in $Own) { $null = $skip.Add($n) }
    $changed = [Collections.Generic.List[string]]::new()
    foreach ($k in $Before.Keys) {
        if ($skip.Contains($k)) { continue }
        if (-not $After.Contains($k)) { $changed.Add("removed:$k") }
        elseif ($After[$k] -cne $Before[$k]) { $changed.Add("changed:$k") }
    }
    foreach ($k in $After.Keys) { if (-not $skip.Contains($k) -and -not $Before.Contains($k)) { $changed.Add("added:$k") } }
    [pscustomobject]@{ Same = ($changed.Count -eq 0); Count = $Before.Count; Changed = $changed.ToArray() }
}

# 纯函数。Entries 的每一项要有 Name、IsFile、IsLink、LastWriteUtc（[datetime]，UTC）。返回本轮自己的那些（原样），
# 以及每个被排除的条目为什么被排除（Name 不是日志名的不报：目录里本来就可能有别的文件）。
function Select-ReaperOwnProbeLogs {
    param([object[]]$Entries = @(), [int[]]$Pids = @(), [Parameter(Mandatory)][datetime]$SinceUtc)
    $mine = [Collections.Generic.List[object]]::new()
    $skipped = [Collections.Generic.List[string]]::new()
    foreach ($e in $Entries) {
        $m = [regex]::Match([string]$e.Name, $script:NamePattern)
        if (-not $m.Success) { continue }
        $logPid = [long]$m.Groups[2].Value
        if ($logPid -notin $Pids) { continue } # 别的进程（包括用户自己的宿主）的日志：一眼都不多看
        if (-not $e.IsFile -or $e.IsLink) { $skipped.Add("$($e.Name)：不是普通文件，不碰"); continue }
        if ($e.LastWriteUtc -lt $SinceUtc) { $skipped.Add("$($e.Name)：修改时间早于这次启动（同号的旧进程留下的），不碰"); continue }
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

function Get-ReaperSha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# 把 Pids 里（已退出的）进程在 LogDir 里留下的日志搬到 DestDir。返回 @{ Moved（搬走并删了原文件的名字）; Kept（自己的、但留在原处的，
# 每项带原因）; Skipped（没认作自己的同 pid 条目与原因）; Own（本轮自己的全部名字）; Problems（要判红的）}。
# Copier 只给自测注入（收到源与目标路径，负责产出目标文件），用来造「哈希不同」。
function Move-ReaperProbeLogs {
    param(
        [Parameter(Mandatory)][string]$LogDir, [Parameter(Mandatory)][int[]]$Pids, [Parameter(Mandatory)][datetime]$SinceUtc,
        [Parameter(Mandatory)][string]$DestDir, [scriptblock]$Copier = $null
    )
    $moved = [Collections.Generic.List[string]]::new()
    $kept = [Collections.Generic.List[string]]::new()
    $problems = [Collections.Generic.List[string]]::new()
    $listed = @(Get-ReaperDirEntries $LogDir)
    $pick = Select-ReaperOwnProbeLogs -Entries $listed -Pids $Pids -SinceUtc $SinceUtc
    foreach ($e in $pick.Own) {
        $logPid = [int][regex]::Match($e.Name, $script:NamePattern).Groups[2].Value
        if (Get-Process -Id $logPid -ErrorAction SilentlyContinue) { $kept.Add("$($e.Name)：进程 $logPid 还在，不动"); $problems.Add("$($e.Name)：写它的进程还没退出"); continue }
        $null = New-Item -ItemType Directory -Force -Path $DestDir
        $target = Join-Path $DestDir $e.Name
        if (Test-Path -LiteralPath $target) { $kept.Add("$($e.Name)：目标已有同名文件，不覆盖"); $problems.Add("$($e.Name)：结果目录里已有同名文件"); continue }
        if ($Copier) { & $Copier $e.FullName $target } else { Copy-Item -LiteralPath $e.FullName -Destination $target }
        $srcHash = Get-ReaperSha256 $e.FullName
        $dstHash = (Test-Path -LiteralPath $target -PathType Leaf) ? (Get-ReaperSha256 $target) : ''
        if ($srcHash -cne $dstHash) { $kept.Add("$($e.Name)：复制件的 sha256 与原文件不同，原文件不删"); $problems.Add("$($e.Name)：复制后哈希不同"); continue }
        # 删之前再核一次：还是那个普通文件，大小与修改时间都没变
        $now = Get-Item -LiteralPath $e.FullName -Force
        if ($now.PSIsContainer -or ($now.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $now.Length -ne $e.Length -or $now.LastWriteTimeUtc -ne $e.LastWriteUtc) {
            $kept.Add("$($e.Name)：复制之后原文件变了，不删"); $problems.Add("$($e.Name)：复制之后原文件变了"); continue
        }
        [IO.File]::Delete($e.FullName) # 只删这一个文件
        $moved.Add($e.Name)
    }
    [pscustomobject]@{ Moved = $moved.ToArray(); Kept = $kept.ToArray(); Skipped = $pick.Skipped; Own = @($pick.Own | ForEach-Object { $_.Name }); Problems = $problems.ToArray() }
}

# 纯函数：一份日志的文本 → 读得出的事实。只读第二段要的几项，格式判定交给 analyze-probe.py，这里读不懂的行不抛错，只计数。
#   Header（role pid tok4 build host_kind）、Cases（每个 C 行）、Blocks / PlayingBlocks、NValues（播放中块长的取值）、NDecl、MaxN、
#   DValues（块记录里的 D）、Pings（ping 事件的样本数）、PairedPids（paired 事件的对端 pid）、Prepares（采样率/块长）、
#   SeqGaps（块序号缺口数）、Dropped（S 行与 Z 行里最大的丢弃计数）、NonRt（块里的 nonrt 取值）、Complete（有 Z 行且以换行结尾）
function Read-ReaperProbeLog {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $facts = [ordered]@{
        Header = $null; Cases = [Collections.Generic.List[object]]::new(); Blocks = 0; PlayingBlocks = 0
        NValues = [Collections.Generic.SortedSet[int]]::new(); NDecl = [Collections.Generic.SortedSet[int]]::new(); MaxN = 0
        DValues = [Collections.Generic.SortedSet[long]]::new(); Pings = [Collections.Generic.List[long]]::new()
        PairedPids = [Collections.Generic.List[long]]::new(); Prepares = [Collections.Generic.List[string]]::new()
        SeqGaps = 0; Dropped = 0; NonRt = [Collections.Generic.SortedSet[int]]::new(); Complete = $false; Unread = 0
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
                    if ($f[$bPlaying] -ceq '1') { $facts.PlayingBlocks++; $null = $facts.NValues.Add($n) }
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
    Move-ReaperProbeLogs, Read-ReaperProbeLog, Get-ReaperProbeLogColumns
