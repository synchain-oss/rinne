# SPDX-License-Identifier: GPL-3.0-or-later
#
# 自测用的临时目录：递归删除只删本进程经 New-RinneTempDir 建出来的目录，删之前按真实路径核对。
# - New-RinneTempDir 在系统临时目录下一层建目录（名字是前缀 + 32 位随机十六进制，同名的已经在就换一个，建不出来抛错），
#   所以「是本进程建的」不取决于调用方的 ErrorActionPreference；建好就记下它的真实路径。不用 .NET 7 才有的
#   [IO.Directory]::CreateTempSubdirectory：脚本的 #Requires 只到 7.0，这里自己拼路径、用 CreateDirectory 建。
# - 真实路径逐级解析：路径上每一级是 junction 或符号链接的，换成它最终指向的位置再往下走（Windows 与别的平台同一套，macOS
#   的 /var 指向 /private/var 这类系统自带的链接也照样解析）。
# - Remove-RinneTempDir 每次动手之前重新解析：真实路径必须是建的时候记下的那一个，而且就在系统临时目录（同样按真实路径）
#   下一层、名字以建时的前缀开头；有一项不符就不删，返回原因。里面的 junction 与符号链接只删链接本身，不进去。删不掉（子进程
#   还握着里面的文件）时按 -WaitSeconds 重试，等不到返回原因。
# - 量子进程管道的自测会留下一个活十几秒、握着临时目录里日志的孙进程：Get-RinneLingerCommand 给出它的命令（pwsh
#   -EncodedCommand 的参数），活到时限或者停止文件出现就退出。量完写停止文件，孙进程退出之后目录才删得掉。
# - 本模块自己的 $ErrorActionPreference 是 Stop：调用方把偏好设成 SilentlyContinue 时，核对途中的错误（例如解析链接时抛错）
#   照样中止这一次调用，不会被吞掉之后接着走到删除。抛到调用方的错误按调用方的偏好处理（带参数属性的函数在调用方那一层按语句
#   报错，SilentlyContinue 时返回空）。
#   New-RinneTempDir -Prefix <rinne-…->                  返回目录的完整路径
#   Remove-RinneTempDir -Path <目录> [-WaitSeconds <秒>]  删掉了（或本来就没有）返回 $null，不删或删不掉返回原因
#   Test-RinneInTempDir -Path <路径> -Dir <目录>          路径（按真实路径）在本进程建的那个目录里面（不含它本身）
#   Get-RinneLingerCommand -Seconds <秒> -StopFile <文件>
#   Test-RinneTempDir                                     自测用例（gates 的自测调用）

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Cmp = $IsWindows ? [StringComparer]::OrdinalIgnoreCase : [StringComparer]::Ordinal
# 本进程建的目录：真实路径 → 前缀
$script:Made = [Collections.Generic.Dictionary[string, string]]::new($script:Cmp)

<# 真实路径：逐级把 junction、符号链接换成最终指向的位置。路径不存在的部分照字面接上。 #>
function Resolve-RinneRealPath {
    param([Parameter(Mandatory)][string]$Path, [int]$Depth = 0)
    if ($Depth -gt 32) { throw "链接套得太深，解析不出真实路径：$Path" }
    $full = [IO.Path]::GetFullPath($Path)
    $cur = [IO.Path]::GetPathRoot($full)
    foreach ($part in @($full.Substring($cur.Length) -split '[\\/]' | Where-Object { $_ })) {
        $next = [IO.Path]::Join($cur, $part)
        $attr = try { [IO.File]::GetAttributes($next) } catch { $null }
        if ($null -ne $attr -and ($attr -band [IO.FileAttributes]::ReparsePoint)) {
            $info = ($attr -band [IO.FileAttributes]::Directory) ? [IO.DirectoryInfo]::new($next) : [IO.FileInfo]::new($next)
            $target = $info.ResolveLinkTarget($true)
            if ($target) { $cur = Resolve-RinneRealPath $target.FullName ($Depth + 1); continue }
        }
        $cur = $next
    }
    $cur
}

<#
  在系统临时目录下一层新建一个目录（名字是 <前缀><32 位随机十六进制>），记下它的真实路径，返回完整路径。
  名字靠随机数，同名的已经在就换一个；这里不用 .NET 7 才有的 [IO.Directory]::CreateTempSubdirectory，
  脚本的下限仍是 PowerShell 7.0。
#>
function New-RinneTempDir {
    param([Parameter(Mandatory)][ValidatePattern('^rinne-[a-z0-9-]+-$')][string]$Prefix)
    $root = [IO.Path]::GetTempPath()
    for ($i = 0; $i -lt 100; $i++) {
        $dir = [IO.Path]::Join($root, $Prefix + [guid]::NewGuid().ToString('N'))
        if (Test-Path -LiteralPath $dir) { continue }
        $null = [IO.Directory]::CreateDirectory($dir)
        $script:Made[(Resolve-RinneRealPath $dir)] = $Prefix
        return $dir
    }
    throw "在 $root 下建不出 $Prefix 开头的临时目录（同名的太多）"
}

<# 这个目录能不能删：能删返回 @{ Real }，不能删返回原因（字符串）。 #>
function Get-RinneTempDirCheck {
    param([Parameter(Mandatory)][string]$Path)
    $real = Resolve-RinneRealPath $Path
    $prefix = $null
    if (-not $script:Made.TryGetValue($real, [ref]$prefix)) { return "不是本进程用 New-RinneTempDir 建的目录（按真实路径比：$real）" }
    $root = Resolve-RinneRealPath ([IO.Path]::GetTempPath())
    if (-not $script:Cmp.Equals([IO.Path]::GetDirectoryName($real), $root)) { return "真实路径 $real 不在系统临时目录 $root 下一层" }
    if (-not ([IO.Path]::GetFileName($real)).StartsWith($prefix, [StringComparison]::Ordinal)) { return "目录名不以建时的前缀 $prefix 开头：$real" }
    @{ Real = $real }
}

<# Path 的真实路径在 Dir 里面（不含 Dir 本身），而且 Dir 是本进程建的、照样过上面的核对。自测的假实现挪动路径之前用它。 #>
function Test-RinneInTempDir {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Dir)
    $c = Get-RinneTempDirCheck $Dir
    if ($c -is [string]) { return $false }
    (Resolve-RinneRealPath $Path).StartsWith($c.Real + [IO.Path]::DirectorySeparatorChar, $IsWindows ? [StringComparison]::OrdinalIgnoreCase : [StringComparison]::Ordinal)
}

<# 删掉本进程建的临时目录（连同里面的东西）。删掉了或本来就没有返回 $null；不删、删不掉返回原因。 #>
function Remove-RinneTempDir {
    param([AllowNull()][AllowEmptyString()][string]$Path, [double]$WaitSeconds = 0)
    if (-not $Path) { return $null }
    $deadline = [datetime]::UtcNow.AddSeconds($WaitSeconds)
    while ($true) {
        try { $null = [IO.File]::GetAttributes($Path) } catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] { return $null }
        $c = Get-RinneTempDirCheck $Path
        if ($c -is [string]) { return "不删 $($Path)：$c" }
        try {
            Remove-Item -LiteralPath $c.Real -Recurse -Force -ErrorAction Stop
            $null = $script:Made.Remove($c.Real)
            return $null
        } catch {
            if ([datetime]::UtcNow -ge $deadline) { return "删不掉 $($Path)：$($_.Exception.Message)" }
            Start-Sleep -Milliseconds 200
        }
    }
}

<#
  孙进程的命令（pwsh -EncodedCommand 的参数）：最多活 Seconds 秒，停止文件出现就提前退出。编码后的串里没有引号、空格与
  补位的 =（UTF-16 的字节数凑成 3 的倍数），经 cmd /c start 转一手也不会被改。
#>
function Get-RinneLingerCommand {
    param([Parameter(Mandatory)][int]$Seconds, [Parameter(Mandatory)][string]$StopFile)
    $cmd = "`$d = [datetime]::UtcNow.AddSeconds($Seconds); while ([datetime]::UtcNow -lt `$d -and -not [IO.File]::Exists('$($StopFile -replace "'", "''")')) { Start-Sleep -Milliseconds 100 }"
    while ($cmd.Length % 3) { $cmd += ' ' }
    [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
}

<#
  自测：新建（系统临时目录下一层、带前缀、两次不重名）、删除（连同文件与子目录；里面指向别处的链接只删链接）、拒绝（不是
  经 New-RinneTempDir 建的；建完被换成指向别处的链接；系统临时目录换了位置）、删不掉时重试到句柄放开、「在里面」的判断、
  调用方的偏好吞不掉核对途中的错误、孙进程的命令。返回 { Label; Bad; Passed }。
#>
function Test-RinneTempDir {
    $check = { param($Label, $Bad, $Flagged) [pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = $(if ($Bad) { [bool]$Flagged } else { -not [bool]$Flagged }) } }
    $root = Resolve-RinneRealPath ([IO.Path]::GetTempPath())
    $link = { param($at, $to) $null = New-Item -ItemType ($IsWindows ? 'Junction' : 'SymbolicLink') -Path $at -Target $to }
    $saved = @{ TMP = $env:TMP; TEMP = $env:TEMP; TMPDIR = $env:TMPDIR }
    $dirs, $foreign, $c = [Collections.Generic.List[string]]::new(), $null, $null
    try {
        $a, $b = (New-RinneTempDir -Prefix 'rinne-tempdir-selftest-'), (New-RinneTempDir -Prefix 'rinne-tempdir-selftest-')
        $dirs.AddRange([string[]]@($a, $b))
        $one = { param($d) $script:Cmp.Equals([IO.Path]::GetDirectoryName((Resolve-RinneRealPath $d)), $root) -and (Split-Path -Leaf $d).StartsWith('rinne-tempdir-selftest-') }
        & $check '新建：在系统临时目录下一层、名字带前缀，两次不重名' $false (-not ((Test-Path -LiteralPath $a -PathType Container) -and (& $one $a) -and (& $one $b) -and $a -ne $b))

        # 里面有文件、子目录，还有一个指向别处（另一个本进程建的目录）的链接：全删掉，链接指向的东西不动
        $null = New-Item -ItemType Directory -Path (Join-Path $a 'sub/deeper')
        Set-Content -LiteralPath (Join-Path $a 'sub/deeper/f.txt') -Value 'x'
        Set-Content -LiteralPath (Join-Path $b 'keep.txt') -Value 'x'
        & $link (Join-Path $a 'out') $b
        $r = Remove-RinneTempDir -Path $a
        & $check '删除：本进程建的目录连同文件、子目录删掉；里面指向别处的链接只删链接' $false (-not (
                $null -eq $r -and -not (Test-Path -LiteralPath $a) -and (Test-Path -LiteralPath (Join-Path $b 'keep.txt'))))
        & $check '删除：目录已经不在时什么都不做' $false ($null -ne (Remove-RinneTempDir -Path $a))

        # 名字形态一样、也在系统临时目录下一层，但不是经 New-RinneTempDir 建的：不删
        $foreign = [IO.Path]::Join([IO.Path]::GetTempPath(), 'rinne-tempdir-selftest-' + [guid]::NewGuid().ToString('N'))
        $null = [IO.Directory]::CreateDirectory($foreign)
        $r = Remove-RinneTempDir -Path $foreign
        & $check '拒绝：不是经 New-RinneTempDir 建的目录不删' $true ($r -match '不是本进程' -and (Test-Path -LiteralPath $foreign))
        $victim = Join-Path $b 'victim'
        $null = New-Item -ItemType Directory -Path $victim
        Set-Content -LiteralPath (Join-Path $victim 'keep.txt') -Value 'x'
        $r = Remove-RinneTempDir -Path $victim
        & $check '拒绝：本进程建的目录里面的子目录不删（不是建的那一个）' $true ($r -match '不是本进程' -and (Test-Path -LiteralPath (Join-Path $victim 'keep.txt')))

        # 建完之后被换成指向别处的链接（原目录挪开）：真实路径变了，不删，链接指向的东西不动
        $c = New-RinneTempDir -Prefix 'rinne-tempdir-selftest-'
        $dirs.Add($c)
        Move-Item -LiteralPath $c -Destination "$c.moved"
        & $link $c $victim
        $r = Remove-RinneTempDir -Path $c
        & $check '拒绝：建完被换成指向别处的链接（真实路径变了）不删' $true ($r -match '不是本进程' -and (Test-Path -LiteralPath (Join-Path $victim 'keep.txt')))
        # 还原：只删链接本身（不递归），把原目录挪回来
        Remove-Item -LiteralPath $c -Force
        Move-Item -LiteralPath "$c.moved" -Destination $c

        # 系统临时目录换了位置（TMP、TEMP、TMPDIR 指到别处）：目录不在它下一层了，不删；还原之后照常删
        $env:TMP = $env:TEMP = $env:TMPDIR = $b
        try { $r = Remove-RinneTempDir -Path $c } finally { $env:TMP, $env:TEMP, $env:TMPDIR = $saved.TMP, $saved.TEMP, $saved.TMPDIR }
        & $check '拒绝：真实路径不在当前的系统临时目录下一层时不删' $true ($r -match '下一层' -and (Test-Path -LiteralPath $c))

        if ($IsWindows) {
            # 子进程握着里面的文件（不放行删除）：不等就删不掉；等它放开之后删掉
            Set-Content -LiteralPath (Join-Path $c 'held.txt') -Value 'x'
            $holder = "`$f = [IO.File]::Open('$((Join-Path $c 'held.txt') -replace "'", "''")', 'Open', 'Read', 'None'); Start-Sleep -Milliseconds 2500; `$f.Dispose()"
            $p = Start-Process -FilePath ([Environment]::ProcessPath) -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($holder))) -PassThru -NoNewWindow
            $deadline = [datetime]::UtcNow.AddSeconds(20)
            while ([datetime]::UtcNow -lt $deadline) { try { [IO.File]::Open((Join-Path $c 'held.txt'), 'Open', 'Read', 'None').Dispose(); Start-Sleep -Milliseconds 50 } catch { break } }
            $now = Remove-RinneTempDir -Path $c
            $later = Remove-RinneTempDir -Path $c -WaitSeconds 20
            $null = $p.WaitForExit(20000)
            & $check '删除：子进程握着里面的文件时不等就删不掉、返回原因' $true ($now -match '删不掉')
            & $check '删除：按 WaitSeconds 重试，句柄放开之后删掉' $false (-not ($null -eq $later -and -not (Test-Path -LiteralPath $c)))
        }

        # 在里面：本进程建的目录里面的路径算（还不存在也算）；目录本身、经里面的链接指到外面的、不是本进程建的目录里面的都不算
        $toForeign = Join-Path $b 'to-foreign'
        & $link $toForeign $foreign
        try {
            $inside = (Test-RinneInTempDir -Path (Join-Path $b 'wt/x') -Dir $b) -and -not (Test-RinneInTempDir -Path $b -Dir $b)
            $outside = (Test-RinneInTempDir -Path (Join-Path $toForeign 'x') -Dir $b) -or (Test-RinneInTempDir -Path (Join-Path $foreign 'x') -Dir $foreign)
        } finally { Remove-Item -LiteralPath $toForeign -Force }
        & $check '在里面：本进程建的目录里面的路径算' $false (-not $inside)
        & $check '在里面：经里面的链接指到外面的、不是本进程建的目录里面的都不算' $true (-not $outside)

        # 调用方把 ErrorActionPreference 设成 SilentlyContinue：没设偏好的模块里，核对途中的错误被吞掉之后接着往下走，落到删除。子进程
        # 里试，它的系统临时目录在 $b 里面：建一个目录、放一个文件，原目录挪开，原位换成成环的链接（解析真实路径时抛错），再删它。
        # 子进程退出之后断言链接原样在、文件还在；它留下的东西都随 $b 删掉（链接只删链接本身）。输出里认的字样运行时才拼出来
        $kind = $IsWindows ? 'Junction' : 'SymbolicLink'
        $child = @"
`$global:ErrorActionPreference = 'SilentlyContinue'
`$env:TMP = `$env:TEMP = `$env:TMPDIR = '$($b -replace "'", "''")'
Import-Module '$($PSCommandPath -replace "'", "''")'
`$d = New-RinneTempDir -Prefix 'rinne-tempdir-selftest-'
'pa' + 'th=' + `$d
Set-Content -LiteralPath (Join-Path `$d 'keep.txt') -Value 'x'
Move-Item -LiteralPath `$d -Destination "`$d.moved"
`$null = New-Item -ItemType Directory -Path "`$d.cyc"
`$null = New-Item -ItemType $kind -Path `$d -Target "`$d.cyc"
[IO.Directory]::Delete("`$d.cyc")
`$null = New-Item -ItemType $kind -Path "`$d.cyc" -Target `$d
if ([IO.File]::GetAttributes(`$d) -band [IO.FileAttributes]::ReparsePoint) { 'cycle' + '-ready' }
`$null = Remove-RinneTempDir -Path `$d
"@
        $o = & ([Environment]::ProcessPath) -NoProfile -NonInteractive -EncodedCommand ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))) 2>&1 | Out-String
        $cd = [regex]::Match($o, '(?m)^path=(.+?)\r?$').Groups[1].Value
        $cattr = $(if ($cd) { try { [IO.File]::GetAttributes($cd) } catch { $null } })
        & $check '调用方把 ErrorActionPreference 设成 SilentlyContinue：解析真实路径时抛错照样中止，不删，链接原样在' $true (
            $o -match 'cycle-ready' -and $cd.StartsWith($b) -and $null -ne $cattr -and ($cattr -band [IO.FileAttributes]::ReparsePoint) -and
            (Test-Path -LiteralPath (Join-Path "$cd.moved" 'keep.txt')))

        $cmd = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String((Get-RinneLingerCommand -Seconds 7 -StopFile "x'y")))
        & $check '孙进程的命令：编码后不带 =，活到时限或停止文件出现（单引号转义）' $false (-not (
                -not (Get-RinneLingerCommand -Seconds 7 -StopFile "x'y").Contains('=') -and $cmd -match 'AddSeconds\(7\)' -and $cmd.Contains("Exists('x''y')")))
    } catch {
        & $check "临时目录自测抛错：$($_.Exception.Message)" $true $false
    } finally {
        $env:TMP, $env:TEMP, $env:TMPDIR = $saved.TMP, $saved.TEMP, $saved.TMPDIR
        # 换成链接那一段中途出错时，原目录还挪在旁边：挪回来再删（只删经 New-RinneTempDir 建的那一个）
        if ($c -and (Test-Path -LiteralPath "$c.moved") -and -not (Test-Path -LiteralPath $c)) { Move-Item -LiteralPath "$c.moved" -Destination $c }
        $left = @(@(foreach ($d in $dirs) { Remove-RinneTempDir -Path $d -WaitSeconds 5 }) | Where-Object { $_ })
        # 不是经 New-RinneTempDir 建的那个：空目录，不递归删
        if ($foreign) { try { [IO.Directory]::Delete($foreign) } catch { $left += "删不掉 $($foreign)：$($_.Exception.Message)" } }
        & $check '自测建的临时目录都删掉了' $false (@($left).Count -gt 0)
    }
}

Export-ModuleMember -Function New-RinneTempDir, Remove-RinneTempDir, Test-RinneInTempDir, Get-RinneLingerCommand, Test-RinneTempDir
