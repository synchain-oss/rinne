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
#   Find-ReaperVariableCaseCollisions / Test-ReaperScriptLint
#                              变量名不得只靠大小写区分（PowerShell 变量名不分大小写，$c 会盖掉 $C）

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SentinelName = '.rinne-reaper-cfg'
$script:SentinelMagic = 'rinne-reaper-private-cfg v1'

function Get-ReaperSentinel { [pscustomobject]@{ Name = $script:SentinelName; Magic = $script:SentinelMagic } }

function Stop-Refuse([string]$Why) { throw [System.InvalidOperationException]::new("REFUSED: $Why") }

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
function Assert-ReaperOutsideDefaultDir([string]$CfgDir) {
    if ([string]::IsNullOrWhiteSpace($CfgDir)) { Stop-Refuse '没有给 -CfgDir（只接受显式的绝对路径）' }
    if (-not [IO.Path]::IsPathFullyQualified($CfgDir)) { Stop-Refuse '-CfgDir 必须是绝对路径' }
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

# 扫一组目录里的 .ps1 / .psm1。除了变量大小写碰撞，还要：每个文件都 Set-StrictMode -Version Latest；-cfgfile 字面量全局恰好
# 一处且在 Host.psm1 里；APPDATA 只许在 Guard 与自测里出现（别的文件连读都不该读）。注释不算。
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
        foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $_.Extension -in '.ps1', '.psm1' }) {
            $source = [IO.File]::ReadAllText($f.FullName)
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

Export-ModuleMember -Function Get-ReaperSentinel, Resolve-ReaperRealPath, Test-ReaperUnder, Assert-ReaperCfgDir, New-ReaperCfgDir, Initialize-ReaperCfg,
    Get-ReaperCfgArgument, Set-ReaperIniText, Set-ReaperIniValue, Get-ReaperDefaultCfgDir, Get-ReaperDefaultCfgSnapshot, Compare-ReaperDefaultCfgSnapshot,
    Find-ReaperVariableCaseCollisions, Test-ReaperScriptLint
