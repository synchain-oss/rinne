# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 本机烟测的安全护栏。这里的函数只吃路径与文本，自己不启动 REAPER。
# 存在的理由：REAPER 对不存在的 -cfgfile 路径不报错，而是悄悄改用 %APPDATA%\REAPER，把维护者自己的配置写掉。
# 所以启动前必须同时满足：配置目录是显式给出的绝对路径、不在（也不指向）%APPDATA% 之下、目录里有 doctor.ps1 -Init 写的
# 哨兵文件、reaper.ini 在。任何一条不满足就抛出以 REFUSED: 开头的错误，调用方不得启动 REAPER。
#   Assert-ReaperCfgDir        校验配置目录，返回 @{ Dir; Ini }；New-ReaperCfgDir 给 doctor -Init 建目录与哨兵
#   Initialize-ReaperCfg       校验并把 ini 路径钉成只读常量；之后 -cfgfile 的实参只能由 Get-ReaperCfgArgument 产生
#   Set-ReaperIniValue         只改私有 ini 里的一个键（字节原样往返，不改别的行）
#   Get-ReaperDefaultCfgSnapshot / Compare-ReaperDefaultCfgSnapshot
#                              只读地取 %APPDATA%\REAPER 下全部条目的大小与修改时间，前后比较
#   Get-ReaperFileIdentity     目录的文件身份（卷序列号 + 文件 ID）：路径写法再怎么绕，指到同一个目录身份就相同
#   Find-ReaperVariableCaseCollisions / Find-ReaperControlChars / Test-ReaperScriptLint
#                              变量名不得只靠大小写区分（PowerShell 变量名不分大小写，$c 会盖掉 $C）；脚本里不得夹不可见的控制字符

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 文件身份取自打开的句柄（FILE_ID_INFO），所以 8.3 短名、大小写、尾部分隔符、subst 盘符、junction 与符号链接的写法都归一到同一个值；
# 比按字符串比前缀可靠。不带 OPEN_REPARSE_POINT，链接会被跟到目标上。
if (-not ('RinneReaperFileId' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RinneReaperFileId {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandleEx(IntPtr handle, int infoClass, byte[] buffer, uint size);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    public static string Get(string path) {
        // FILE_READ_ATTRIBUTES、共享全开、OPEN_EXISTING、FILE_FLAG_BACKUP_SEMANTICS（目录要它）
        IntPtr h = CreateFileW(path, 0x80, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (h == new IntPtr(-1)) {
            int err = Marshal.GetLastWin32Error();
            // 2、3：路径不存在，正常（还没建的配置目录）；别的错误取不到身份，不能当作「不是同一个」
            return (err == 2 || err == 3) ? null : "ERROR:" + err;
        }
        try {
            byte[] b = new byte[24]; // FILE_ID_INFO：8 字节卷序列号 + 16 字节文件 ID
            if (!GetFileInformationByHandleEx(h, 18, b, (uint)b.Length)) return "ERROR:" + Marshal.GetLastWin32Error();
            return BitConverter.ToString(b);
        } finally { CloseHandle(h); }
    }
}
'@
}

$script:SentinelName = '.rinne-reaper-cfg'
$script:SentinelMagic = 'rinne-reaper-private-cfg v1'

function Get-ReaperSentinel { [pscustomobject]@{ Name = $script:SentinelName; Magic = $script:SentinelMagic } }

function Stop-Refuse([string]$Why) { throw [System.InvalidOperationException]::new("REFUSED: $Why") }

# 路径指到的目录（或文件）的文件身份字符串；路径不存在返回 $null；存在却打不开返回 'ERROR:<错误码>'（调用方要当作「证明不了」处理）。
function Get-ReaperFileIdentity([string]$Path) { [RinneReaperFileId]::Get($Path) }

# 逐级解析 junction 与符号链接，返回真实路径；路径不存在的部分照字面接上。
function Resolve-ReaperRealPath([string]$Path, [int]$Depth = 0) {
    if ($Depth -gt 32) { Stop-Refuse "链接套得太深：$Path" }
    $full = [IO.Path]::GetFullPath($Path)
    $cur = [IO.Path]::GetPathRoot($full)
    foreach ($part in @($full.Substring($cur.Length) -split '[\\/]' | Where-Object { $_ })) {
        $next = [IO.Path]::Join($cur, $part)
        $attr = try { [IO.File]::GetAttributes($next) } catch { $null }
        if ($null -ne $attr -and ($attr -band [IO.FileAttributes]::ReparsePoint)) {
            $info = ($attr -band [IO.FileAttributes]::Directory) ? [IO.DirectoryInfo]::new($next) : [IO.FileInfo]::new($next)
            $target = $info.ResolveLinkTarget($true)
            if ($target) { $cur = Resolve-ReaperRealPath $target.FullName ($Depth + 1); continue }
        }
        $cur = $next
    }
    $cur
}

# $Path 等于 $Root 或在它下面（不分大小写，两种斜杠都认）。
function Test-ReaperUnder([string]$Path, [string]$Root) {
    $p = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/') + '\'
    $r = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + '\'
    $p.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)
}

# 路径本身的检查，不看里面有什么：显式的绝对路径，不在（也不指向）AppData 之下。返回真实路径。
# 三道，缺一不可：
#   1. 只收盘符开头的绝对路径。\\?\ 前缀、\\.\ 设备路径、\\server\share（包括指回本机的 \\localhost\C$\ 管理共享）写的是同一个目录，
#      却躲得过按字符串的比较，所以一律不收；
#   2. 字符串比较：展开 .. 与 junction / 符号链接之后，不在 AppData 的字符串前缀之下；
#   3. 文件身份比较：配置目录自己和它的每一级上级（直到盘符根），只要有一个与 AppData 是同一个目录（8.3 短名、subst 盘符等字符串认不出的写法），
#      就拒绝；存在却取不到身份（拒绝访问等）也拒绝，证明不了就不放行。路径还不存在的部分（取到 $null）正常跳过。
function Assert-ReaperOutsideDefaultDir([string]$CfgDir) {
    if ([string]::IsNullOrWhiteSpace($CfgDir)) { Stop-Refuse '没有给 -CfgDir（只接受显式的绝对路径）' }
    if (-not [IO.Path]::IsPathFullyQualified($CfgDir)) { Stop-Refuse '-CfgDir 必须是绝对路径' }
    if ($CfgDir -notmatch '^[A-Za-z]:[\\/]') { Stop-Refuse '-CfgDir 只接受本机盘符开头的绝对路径（C:\…）：\\?\ 前缀、\\.\ 设备路径、UNC 与 \\localhost\C$\ 这类管理共享写法一律不收' }
    $appData = $env:APPDATA
    if ([string]::IsNullOrWhiteSpace($appData)) { Stop-Refuse 'APPDATA 没有设置，无法证明配置目录在它之外' }
    $full = [IO.Path]::GetFullPath($CfgDir).TrimEnd('\', '/')
    $real = (Resolve-ReaperRealPath $full).TrimEnd('\', '/')
    $bases = @($appData, (Resolve-ReaperRealPath $appData))
    foreach ($candidate in @($full, $real)) {
        foreach ($base in $bases) {
            if (Test-ReaperUnder $candidate $base) { Stop-Refuse '配置目录在 AppData 之下（或指向那里）：永不碰 %APPDATA%\REAPER' }
        }
    }
    $baseId = Get-ReaperFileIdentity $appData
    if (-not $baseId -or $baseId.StartsWith('ERROR:')) { Stop-Refuse 'AppData 目录打不开，取不到它的文件身份，无法证明配置目录在它之外' }
    foreach ($start in @($full, $real)) {
        # 一直查到盘符根：subst 盘符可以让根目录本身就是 AppData
        for ($p = $start; $p; $p = [IO.Path]::GetDirectoryName($p)) {
            $id = Get-ReaperFileIdentity $p
            if ($id -and $id.StartsWith('ERROR:')) { Stop-Refuse "配置目录或它的某一级上级取不到文件身份（$id），无法证明它不是 AppData" }
            if ($id -ceq $baseId) { Stop-Refuse '配置目录在 AppData 之下（或指向那里；按文件身份认出的）：永不碰 %APPDATA%\REAPER' }
        }
    }
    $real
}

function Assert-ReaperCfgDir {
    param([string]$CfgDir)
    $real = Assert-ReaperOutsideDefaultDir $CfgDir
    if (-not (Test-Path -LiteralPath $real -PathType Container)) { Stop-Refuse '配置目录不存在' }
    $sentinel = Join-Path $real $script:SentinelName
    $first = (Test-Path -LiteralPath $sentinel -PathType Leaf) ? "$(Get-Content -LiteralPath $sentinel -TotalCount 1)".Trim() : ''
    if ($first -cne $script:SentinelMagic) { Stop-Refuse '配置目录里没有哨兵文件（先用 doctor.ps1 -Init 初始化）' }
    $ini = Join-Path $real 'reaper.ini'
    if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) { Stop-Refuse '配置目录里没有 reaper.ini' }
    [pscustomobject]@{ Dir = $real; Ini = $ini }
}

# doctor.ps1 -Init 用：路径检查通过后建目录、私有插件目录、最小的 reaper.ini（Dummy 设备）和哨兵文件。已有的 ini 不动。
function New-ReaperCfgDir {
    param([string]$CfgDir)
    $real = Assert-ReaperOutsideDefaultDir $CfgDir
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $real 'plugins')
    $ini = Join-Path $real 'reaper.ini'
    if (-not (Test-Path -LiteralPath $ini)) {
        [IO.File]::WriteAllText($ini, "[REAPER]`npromptendrec=0`n[audioconfig]`nmode=4`ndummy_srate=48000`ndummy_blocksize=512`n", [Text.UTF8Encoding]::new($false))
    }
    [IO.File]::WriteAllText((Join-Path $real $script:SentinelName), "$($script:SentinelMagic)`n", [Text.UTF8Encoding]::new($false))
    Assert-ReaperCfgDir $real
}

# 把 ini 路径钉成只读常量（Constant 连 -Force 都改不掉）：以后任何给它赋值的写法，包括只差大小写的同名变量，都是硬错误。
function Initialize-ReaperCfg {
    param([string]$CfgDir)
    $g = Assert-ReaperCfgDir $CfgDir
    $held = Get-Variable -Name RinneReaperCfgIni -Scope Script -ErrorAction SilentlyContinue
    if ($held) {
        if ($held.Value -cne $g.Ini) { Stop-Refuse '本进程已经钉死了另一个配置目录' }
    } else {
        New-Variable -Name RinneReaperCfgIni -Value $g.Ini -Option Constant -Scope Script
    }
    $g
}

# -cfgfile 的实参唯一的来源：重新校验目录，再用常量拼出带引号的路径。
function Get-ReaperCfgArgument {
    if (-not (Get-Variable -Name RinneReaperCfgIni -Scope Script -ErrorAction SilentlyContinue)) { Stop-Refuse '还没有 Initialize-ReaperCfg' }
    $g = Assert-ReaperCfgDir (Split-Path -Parent $script:RinneReaperCfgIni)
    if ($g.Ini -cne $script:RinneReaperCfgIni) { Stop-Refuse '配置目录的真实路径变了' }
    '"{0}"' -f $script:RinneReaperCfgIni
}

# ini 文本里改一个键（纯函数，自测直接用它）：节没有就在末尾补，键没有就补在节的最后一个非空行后。保留原来的换行风格。
function Set-ReaperIniText {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text, [Parameter(Mandatory)][string]$Section, [Parameter(Mandatory)][string]$Key, [Parameter(Mandatory)][string]$Value)
    $eol = $Text.Contains("`r`n") ? "`r`n" : "`n"
    $lines = [Collections.Generic.List[string]]::new([string[]]($Text -split '\r?\n'))
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    $head = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[$Section]") { $head = $i; break } }
    if ($head -lt 0) {
        $lines.Add("[$Section]"); $lines.Add("$Key=$Value")
    } else {
        $end = $head + 1
        while ($end -lt $lines.Count -and -not $lines[$end].TrimStart().StartsWith('[')) { $end++ }
        $hit = -1
        for ($i = $head + 1; $i -lt $end; $i++) { if ($lines[$i] -imatch ('^' + [regex]::Escape($Key) + '=')) { $hit = $i; break } }
        if ($hit -ge 0) {
            if ($lines[$hit] -ceq "$Key=$Value") { return $Text }
            $lines[$hit] = "$Key=$Value"
        } else {
            $at = $end
            while ($at -gt $head + 1 -and $lines[$at - 1].Trim() -eq '') { $at-- }
            $lines.Insert($at, "$Key=$Value")
        }
    }
    ($lines -join $eol) + $eol
}

# 改私有 ini 的一个键。按 Latin-1 读写，字节原样往返；只在文本真的变了时才写。返回是否改了。
function Set-ReaperIniValue {
    param([Parameter(Mandatory)][string]$Section, [Parameter(Mandatory)][string]$Key, [Parameter(Mandatory)][string]$Value)
    $null = Get-ReaperCfgArgument
    $iniPath = $script:RinneReaperCfgIni
    $enc = [Text.Encoding]::Latin1
    $old = $enc.GetString([IO.File]::ReadAllBytes($iniPath))
    $new = Set-ReaperIniText -Text $old -Section $Section -Key $Key -Value $Value
    if ($new -ceq $old) { return $false }
    [IO.File]::WriteAllBytes($iniPath, $enc.GetBytes($new))
    $true
}

# REAPER 不给 -cfgfile 时用的默认资源目录，即 %APPDATA%\REAPER。只用来取清单和造「必须拒绝」的反例，永远不写。
function Get-ReaperDefaultCfgDir { Join-Path $env:APPDATA 'REAPER' }

# 默认资源目录下全部条目的清单，只读。文件记「大小|修改时间刻度」；目录只记「在」：NTFS 在父目录列表里给的子目录时间戳更新滞后，
# 同一个目录连取两次可能不同，记下来会误报。目录里的增删改都会从文件项上看出来。目录不存在返回空表。Root 只给自测用。
function Get-ReaperDefaultCfgSnapshot {
    param([string]$Root = (Get-ReaperDefaultCfgDir))
    $snap = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return $snap }
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    foreach ($e in Get-ChildItem -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue) {
        $snap[$e.FullName.Substring($prefix.Length)] = $e.PSIsContainer ? 'dir' : ('{0}|{1}' -f $e.Length, $e.LastWriteTimeUtc.Ticks)
    }
    $snap
}

function Compare-ReaperDefaultCfgSnapshot {
    param([Parameter(Mandatory)]$Before, [Parameter(Mandatory)]$After)
    $changed = [Collections.Generic.List[string]]::new()
    foreach ($k in $Before.Keys) {
        if (-not $After.Contains($k)) { $changed.Add("removed:$k") }
        elseif ($After[$k] -cne $Before[$k]) { $changed.Add("changed:$k") }
    }
    foreach ($k in $After.Keys) { if (-not $Before.Contains($k)) { $changed.Add("added:$k") } }
    [pscustomobject]@{ Same = ($changed.Count -eq 0); Count = $Before.Count; Changed = $changed.ToArray() }
}

# 一段脚本文本里只差大小写的变量名（整个文件范围，比 PowerShell 的作用域规则更严）。解析失败也算问题，不放过。
function Find-ReaperVariableCaseCollisions {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $tokens = $null; $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$parseErrors)
    $out = [Collections.Generic.List[string]]::new()
    foreach ($e in $parseErrors) { $out.Add("解析失败：$($e.Message)") }
    $spellings = @{}
    $isVar = { param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] }
    foreach ($v in $ast.FindAll($isVar, $true)) {
        $name = $v.VariablePath.UserPath -replace '^(?:script|global|local|private):', ''
        $lower = $name.ToLowerInvariant()
        if (-not $spellings.ContainsKey($lower)) { $spellings[$lower] = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal) }
        $null = $spellings[$lower].Add($name)
    }
    foreach ($lower in $spellings.Keys) {
        if ($spellings[$lower].Count -gt 1) { $out.Add('变量名只差大小写：' + (($spellings[$lower] | Sort-Object | ForEach-Object { '$' + $_ }) -join ' / ')) }
    }
    , $out.ToArray()
}

# 文本里夹的不可见控制字符，每处一条描述（第几行第几列、码位）。TAB、LF 和紧挨在 LF 前的 CR（Windows 换行）不算；
# 其余 C0 控制字符、DEL，以及不在行尾的 CR 都算。这类字符在编辑器和 diff 里看不见，一旦被吞掉，正则或字符串的意思就变了。
function Find-ReaperControlChars {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $out = [Collections.Generic.List[string]]::new()
    foreach ($m in [regex]::Matches($Text, '[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]|\r(?!\n)')) {
        $leading = $Text.Substring(0, $m.Index)
        $line = ($leading.Length - $leading.Replace("`n", '').Length) + 1
        $col = $m.Index - $leading.LastIndexOf("`n")
        $out.Add(('第 {0} 行第 {1} 列：U+{2:X4}' -f $line, $col, [int][char]$m.Value))
    }
    , $out.ToArray()
}

# 扫一组目录里的 .ps1 / .psm1。除了变量大小写碰撞，还要：每个文件都 Set-StrictMode -Version Latest；-cfgfile 字面量全局恰好
# 一处且在 Host.psm1 里；APPDATA 只许在 Guard 与自测里出现（别的文件连读都不该读）。注释不算。
# 另外 .ps1 / .psm1 / .lua / .json 都不许夹不可见的控制字符。
function Test-ReaperScriptLint {
    param([Parameter(Mandatory)][string[]]$Dirs)
    $problems = [Collections.Generic.List[string]]::new()
    $sites = 0
    $isStrict = { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -ieq 'Set-StrictMode' -and $n.Extent.Text -imatch '-Version\s+Latest' }
    $word = 'cfg' + 'file' # 分开拼：本文件自己的判据字面量不能算作一处 -cfgfile
    $isCfg = {
        param($n)
        ($n -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $n.Value -imatch $word) -or
        ($n -is [System.Management.Automation.Language.ExpandableStringExpressionAst] -and $n.Value -imatch $word) -or
        ($n -is [System.Management.Automation.Language.CommandParameterAst] -and $n.ParameterName -ieq $word)
    }.GetNewClosure()
    foreach ($dir in $Dirs) {
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { $problems.Add("目录不存在：$(Split-Path -Leaf $dir)"); continue }
        foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $_.Extension -in '.lua', '.json' }) {
            foreach ($c in (Find-ReaperControlChars -Text ([IO.File]::ReadAllText($f.FullName)))) { $problems.Add("$($f.Name)：夹着不可见的控制字符（$c）") }
        }
        foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $_.Extension -in '.ps1', '.psm1' }) {
            $source = [IO.File]::ReadAllText($f.FullName)
            foreach ($c in (Find-ReaperControlChars -Text $source)) { $problems.Add("$($f.Name)：夹着不可见的控制字符（$c）") }
            foreach ($c in (Find-ReaperVariableCaseCollisions -Text $source)) { $problems.Add("$($f.Name)：$c") }
            $tokens = $null; $parseErrors = $null
            $ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
            if (@($ast.FindAll($isStrict, $true)).Count -eq 0) { $problems.Add("$($f.Name)：缺 Set-StrictMode -Version Latest") }
            $n = @($ast.FindAll($isCfg, $true)).Count
            $sites += $n
            if ($n -gt 0 -and $f.Name -ne 'Host.psm1') { $problems.Add("$($f.Name)：-$word 只许在 Host.psm1 里拼") }
            if ($f.Name -notin 'Guard.psm1', 'SelfTest.psm1' -and @($tokens | Where-Object { $_.Kind -ne 'Comment' -and $_.Text -imatch 'appdata' }).Count -gt 0) {
                $problems.Add("$($f.Name)：只有 Guard.psm1 与自测可以提到 APPDATA")
            }
        }
    }
    if ($sites -ne 1) { $problems.Add("-$word 字面量应当恰好出现一次，实际 $sites 次") }
    , $problems.ToArray()
}

Export-ModuleMember -Function Get-ReaperSentinel, Get-ReaperFileIdentity, Resolve-ReaperRealPath, Test-ReaperUnder, Assert-ReaperCfgDir, New-ReaperCfgDir, Initialize-ReaperCfg,
    Get-ReaperCfgArgument, Set-ReaperIniText, Set-ReaperIniValue, Get-ReaperDefaultCfgDir, Get-ReaperDefaultCfgSnapshot, Compare-ReaperDefaultCfgSnapshot,
    Find-ReaperVariableCaseCollisions, Find-ReaperControlChars, Test-ReaperScriptLint
