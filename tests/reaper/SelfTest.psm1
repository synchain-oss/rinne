# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 烟测自己的自测：不启动 REAPER，全部在假的 APPDATA 与临时目录里做，真实的 %APPDATA%\REAPER 只在最前最后各取一次清单比对。
# 每个用例是 (标签, 是否反例, 是否按预期)：反例（Bad）是「必须被拦住 / 必须判红」的情形，Passed 表示它确实被拦住了。
# 需要把配置目录钉进进程的用例（Initialize 之后不可逆）放在子进程 SelfTestChild.ps1 里做。
#   Invoke-ReaperSelfTest -RepoRoot <仓根> [-CollisionSample <一份含大小写碰撞变量的 .ps1，例如 spike 的旧脚本>]   返回退出码

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Scripts = Join-Path $PSScriptRoot '../../scripts/reaper'
Import-Module (Join-Path $script:Scripts 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts 'Scenarios.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts 'Suite.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Rpp.psm1') -ErrorAction Stop
Import-Module (Join-Path $script:Scripts '../lib/TempDir.psm1') -ErrorAction Stop

function Test-Refused([scriptblock]$Action, [string]$Like = '*') {
    try { $null = & $Action; $false } catch { $_.Exception.Message -like 'REFUSED:*' -and $_.Exception.Message -like $Like }
}

function Test-Usage([scriptblock]$Action, [string]$Like = '*') {
    try { $null = & $Action; $false } catch { $_.Exception.Message -like 'USAGE:*' -and $_.Exception.Message -like $Like }
}

function Write-Bytes([string]$Path, [byte[]]$Bytes) { [IO.File]::WriteAllBytes($Path, $Bytes) }

function Get-GuardCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $defaultDir = Get-ReaperDefaultCfgDir
    # 护栏有两层，拒绝时的理由不同：字符串层（展开 .. 与链接之后比前缀）和文件身份层。用例按理由断言，删掉任何一层都有用例变红
    $byString = '*（或指向那里）：永不碰*'
    $byIdentity = '*按文件身份认出的*'
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $defaultDir 'Data')
    & $add '没给 -CfgDir 被拒绝' $true (Test-Refused { Assert-ReaperCfgDir '' })
    & $add '相对路径被拒绝' $true (Test-Refused { Assert-ReaperCfgDir 'reaper-cfg' } '*绝对路径*')
    & $add '默认资源目录本身被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $defaultDir } $byString)
    & $add '默认资源目录下的子目录被拒绝（目录不必存在）' $true (Test-Refused { Assert-ReaperCfgDir (Join-Path $defaultDir 'Data\x') } $byString)
    & $add '大小写与正斜杠写法的默认资源目录被拒绝' $true (Test-Refused { Assert-ReaperCfgDir ($defaultDir.ToUpperInvariant() -replace '\\', '/') } $byString)
    $junction = Join-Path $Tmp 'link-to-default'
    $null = New-Item -ItemType Junction -Path $junction -Target $defaultDir
    try { & $add '指向默认资源目录的 junction 被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $junction } $byString) }
    finally { [IO.Directory]::Delete($junction) } # 只删链接本身
    # 路径写法绕过：下面写的都是同一个默认资源目录（或它的上级），必须一律拒绝。全是假 APPDATA 里的路径，不碰真实的。
    $roam = Split-Path -Parent $defaultDir
    $short = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($defaultDir).ShortPath
    $hasShort = $short -cne $defaultDir
    $noShort = $hasShort ? '' : '（此卷没有生成 8.3 短名，这条没能真跑）'
    & $add "8.3 短名写法的默认资源目录被拒绝（.NET 的 GetFullPath 先把它展开成长名；身份比较是第二道）$noShort" $true ((-not $hasShort) -or (Test-Refused { Assert-ReaperCfgDir $short } '*永不碰*'))
    & $add "8.3 短名写法下还不存在的子目录也被拒绝$noShort" $true ((-not $hasShort) -or (Test-Refused { Assert-ReaperCfgDir (Join-Path $short 'Data\not-yet') } '*永不碰*'))
    & $add '文件身份：8.3 短名与长名是同一个' $false ((-not $hasShort) -or ((Get-ReaperFileIdentity $short) -ceq (Get-ReaperFileIdentity $defaultDir)))
    & $add '文件身份：不同目录不同，不存在的路径取不到' $false ((Get-ReaperFileIdentity $defaultDir) -and (Get-ReaperFileIdentity $defaultDir) -cne (Get-ReaperFileIdentity $Tmp) -and $null -eq (Get-ReaperFileIdentity (Join-Path $Tmp 'nope-nope')))
    & $add "8.3 短名的小写写法也被拒绝（短名不分大小写）$noShort" $true ((-not $hasShort) -or (Test-Refused { Assert-ReaperCfgDir $short.ToLowerInvariant() } '*永不碰*'))
    & $add '\\?\ 前缀写法的默认资源目录被拒绝' $true (Test-Refused { Assert-ReaperCfgDir ('\\?\' + $defaultDir) } '*盘符*')
    & $add '\\.\ 设备路径与 \??\ 写法的默认资源目录被拒绝' $true ((Test-Refused { Assert-ReaperCfgDir ('\\.\' + $defaultDir) } '*盘符*') -and (Test-Refused { Assert-ReaperCfgDir ('\??\' + $defaultDir) } '*盘符*'))
    $admin = '\\localhost\' + $defaultDir.Substring(0, 1) + '$' + $defaultDir.Substring(2)
    & $add '\\localhost\C$\ 管理共享写法的默认资源目录被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $admin } '*盘符*')
    # 第一道（只收盘符开头）之外，文件身份这一层自己也认得这些写法：就算第一道被绕过，也还有一道。管理共享要本机开着才取得到身份，取不到时这一条不判。
    $idDefault = Get-ReaperFileIdentity $defaultDir
    $idAdmin = Get-ReaperFileIdentity $admin
    & $add '文件身份：\\?\ 前缀、\\.\ 设备路径写法与原目录是同一个；管理共享写法（本机取得到时）也是' $false (($idDefault -ceq (Get-ReaperFileIdentity ('\\?\' + $defaultDir))) -and ($idDefault -ceq (Get-ReaperFileIdentity ('\\.\' + $defaultDir))) -and ((-not $idAdmin) -or $idAdmin.StartsWith('ERROR:') -or ($idAdmin -ceq $idDefault)))
    & $add '尾部分隔符不改变结论' $true ((Test-Refused { Assert-ReaperCfgDir ($defaultDir + '\') } $byString) -and (Test-Refused { Assert-ReaperCfgDir ($defaultDir + '\\') } $byString))
    & $add '尾部的点与空格不改变结论（Win32 会把它们吃掉）' $true ((Test-Refused { Assert-ReaperCfgDir ($defaultDir + '.') } $byString) -and (Test-Refused { Assert-ReaperCfgDir ($defaultDir + ' ') } $byString))
    # subst 盘符：字符串里看不出它就是 APPDATA，只有认文件身份才拦得住。要一直查到盘符根，根目录本身就是 APPDATA。
    $used = @([IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpperInvariant() })
    $letter = @('Z', 'Y', 'X', 'W', 'V', 'U') | Where-Object { $_ -notin $used } | Select-Object -First 1
    $substOk = $false
    if ($letter) { & subst.exe "${letter}:" $roam 2>&1 | Out-Null; $substOk = ($LASTEXITCODE -eq 0) }
    if ($substOk) {
        try {
            & $add 'subst 盘符指到 APPDATA 时，盘符下的 \REAPER 被拒绝（配置目录与它的上级，直到盘符根，按文件身份比）' $true ((Test-Refused { Assert-ReaperCfgDir "${letter}:\REAPER" } $byIdentity) -and (Test-Refused { Assert-ReaperCfgDir "${letter}:\REAPER\Data\not-yet" } $byIdentity) -and (Test-Refused { Assert-ReaperCfgDir "${letter}:\" } $byIdentity))
            & $add '文件身份：subst 盘符根与它指向的目录是同一个' $false ((Get-ReaperFileIdentity "${letter}:\") -ceq (Get-ReaperFileIdentity $roam))
        } finally { & subst.exe "${letter}:" /d 2>&1 | Out-Null }
    } else {
        & $add '（这台机器上没能建 subst 盘符：subst 反例没能真跑）' $false $true
    }
    # 取不到身份就不放行：存在却打不开（这里用非法文件名触发 Win32 的非「不存在」错误）
    & $add '取不到文件身份（非法名字）时拒绝，不静默放行' $true ((Test-Refused { Assert-ReaperCfgDir (Join-Path $Tmp 'bad|name') } '*文件身份*') -and ((Get-ReaperFileIdentity (Join-Path $Tmp 'bad|name')) -like 'ERROR:*'))
    & $add '带 .. 绕回默认资源目录的写法被拒绝' $true ((Test-Refused { Assert-ReaperCfgDir "$defaultDir\Data\..\Data\x" } $byString) -and (Test-Refused { Assert-ReaperCfgDir ($roam + '\..\' + (Split-Path -Leaf $roam) + '\REAPER') } $byString))
    $junctionUp = Join-Path $Tmp 'link-to-roaming'
    $null = New-Item -ItemType Junction -Path $junctionUp -Target $roam
    try { & $add '指向 APPDATA 的 junction 之下的目录（含还不存在的）被拒绝' $true ((Test-Refused { Assert-ReaperCfgDir (Join-Path $junctionUp 'REAPER\Data') } $byString) -and (Test-Refused { Assert-ReaperCfgDir (Join-Path $junctionUp 'not\yet') } $byString)) }
    finally { [IO.Directory]::Delete($junctionUp) } # 只删链接本身
    $sibling = Join-Path $Tmp ((Split-Path -Leaf $roam) + '-Other')
    & $add '名字只是以 APPDATA 目录名开头的兄弟目录不被误拒' $false ((New-ReaperCfgDir $sibling).Dir -like '*-Other')
    & $add 'New-ReaperCfgDir 对默认资源目录也拒绝，且什么都没建' $true ((Test-Refused { New-ReaperCfgDir $defaultDir } $byString) -and -not (Test-Path -LiteralPath (Join-Path $defaultDir '.rinne-reaper-cfg')))
    $gone = Join-Path $Tmp 'not-there'
    & $add '目录不存在被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $gone } '*不存在*')
    $bare = Join-Path $Tmp 'bare'; $null = New-Item -ItemType Directory -Path $bare; Set-Content -LiteralPath (Join-Path $bare 'reaper.ini') -Value '[REAPER]'
    & $add '有 ini 没哨兵被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $bare } '*哨兵*')
    Set-Content -LiteralPath (Join-Path $bare '.rinne-reaper-cfg') -Value 'something else'
    & $add '哨兵内容不对被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $bare } '*哨兵*')
    $good = Join-Path $Tmp 'good'
    $made = New-ReaperCfgDir $good
    & $add '正常初始化的配置目录通过校验' $false ($made.Ini -like '*good\reaper.ini' -and (Assert-ReaperCfgDir $good).Dir -eq $made.Dir)
    Remove-Item -LiteralPath (Join-Path $good 'reaper.ini')
    & $add '哨兵在、ini 没了被拒绝' $true (Test-Refused { Assert-ReaperCfgDir $good } '*reaper.ini*')
    $cases
}

function Get-IniCases {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $crlf = "[REAPER]`r`nvstpath64=x`r`n[audioconfig]`r`nmode=3`r`n[nag]`r`nnag=1`r`n"
    $t = Set-ReaperIniText -Text $crlf -Section audioconfig -Key mode -Value 4
    & $add 'ini：改已有键，只动那一行，保留 CRLF' $false ($t -ceq $crlf.Replace('mode=3', 'mode=4'))
    $t = Set-ReaperIniText -Text $crlf -Section audioconfig -Key dummy_srate -Value 44100
    & $add 'ini：键不在就补在节的末尾，不串到下一节' $false ($t -ceq "[REAPER]`r`nvstpath64=x`r`n[audioconfig]`r`nmode=3`r`ndummy_srate=44100`r`n[nag]`r`nnag=1`r`n")
    & $add 'ini：值没变时原样返回' $false ((Set-ReaperIniText -Text $crlf -Section audioconfig -Key mode -Value 3) -ceq $crlf)
    $t = Set-ReaperIniText -Text "[REAPER]`nx=1`n" -Section audioconfig -Key mode -Value 4
    & $add 'ini：节不在就补节，LF 风格不变' $false ($t -ceq "[REAPER]`nx=1`n[audioconfig]`nmode=4`n")
    & $add 'ini：放在 [REAPER] 段下的 mode 不会被当成 [audioconfig] 的' $false ((Set-ReaperIniText -Text "[REAPER]`nmode=9`n[audioconfig]`n" -Section audioconfig -Key mode -Value 4) -ceq "[REAPER]`nmode=9`n[audioconfig]`nmode=4`n")
    $cases
}

function Get-SnapshotCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $root = Join-Path $Tmp 'snap'
    $null = New-Item -ItemType Directory -Path (Join-Path $root 'sub')
    Set-Content -LiteralPath (Join-Path $root 'a.ini') -Value 'aaaa'
    Set-Content -LiteralPath (Join-Path $root 'sub\b.ini') -Value 'bbbb'
    $s0 = Get-ReaperDefaultCfgSnapshot -Root $root
    & $add '清单：什么都没变时前后相同' $false ((Compare-ReaperDefaultCfgSnapshot -Before $s0 -After (Get-ReaperDefaultCfgSnapshot -Root $root)).Same)
    Add-Content -LiteralPath (Join-Path $root 'a.ini') -Value 'x'
    & $add '清单：大小变了判变' $true (-not (Compare-ReaperDefaultCfgSnapshot -Before $s0 -After (Get-ReaperDefaultCfgSnapshot -Root $root)).Same)
    $s1 = Get-ReaperDefaultCfgSnapshot -Root $root
    (Get-Item -LiteralPath (Join-Path $root 'sub\b.ini')).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-3)
    & $add '清单：只有修改时间变了也判变' $true (-not (Compare-ReaperDefaultCfgSnapshot -Before $s1 -After (Get-ReaperDefaultCfgSnapshot -Root $root)).Same)
    $s2 = Get-ReaperDefaultCfgSnapshot -Root $root
    Set-Content -LiteralPath (Join-Path $root 'new.ini') -Value n
    & $add '清单：新增文件判变' $true (-not (Compare-ReaperDefaultCfgSnapshot -Before $s2 -After (Get-ReaperDefaultCfgSnapshot -Root $root)).Same)
    $s3 = Get-ReaperDefaultCfgSnapshot -Root $root
    Remove-Item -LiteralPath (Join-Path $root 'new.ini')
    & $add '清单：删除文件判变' $true (-not (Compare-ReaperDefaultCfgSnapshot -Before $s3 -After (Get-ReaperDefaultCfgSnapshot -Root $root)).Same)
    $s4 = Get-ReaperDefaultCfgSnapshot -Root $root
    (Get-Item -LiteralPath (Join-Path $root 'sub')).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-5)
    & $add '清单：只有目录自己的时间戳变了不报（它滞后，会误报；目录里的变动看文件项）' $false ((Compare-ReaperDefaultCfgSnapshot -Before $s4 -After (Get-ReaperDefaultCfgSnapshot -Root $root)).Same)
    & $add '清单：目录不存在是空清单，不报错' $false ((Get-ReaperDefaultCfgSnapshot -Root (Join-Path $Tmp 'nope')).Count -eq 0)
    $cases
}

function Get-LintCases([string]$Tmp, [string]$RepoRoot, [string]$CollisionSample) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $word = '-cfg' + 'file'
    $strict = 'Set-StrictMode -Version Latest'
    $lint = {
        param($Files)
        $dir = Join-Path $Tmp ('lint-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $null = New-Item -ItemType Directory -Path $dir
        foreach ($name in $Files.Keys) { [IO.File]::WriteAllText((Join-Path $dir $name), $Files[$name]) }
        try { Test-ReaperScriptLint -Dirs $dir } finally { foreach ($name in $Files.Keys) { Remove-Item -LiteralPath (Join-Path $dir $name) }; Remove-Item -LiteralPath $dir }
    }
    $hostOk = @{ 'Host.psm1' = "$strict`n`$a = '$word'`n" }
    & $add 'lint：干净的样本没有问题' $false ((& $lint $hostOk).Count -eq 0)
    & $add 'lint：$c 与 $C 只差大小写判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$c = 1`n`$C = 2`n" })) -match '只差大小写')
    & $add 'lint：字符串里的 "$c" 与参数 $C 也算碰撞' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`nparam(`$C)`n`"`$c\reaper.ini`"`n" })) -match '只差大小写')
    & $add 'lint：缺 Set-StrictMode 判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "`$a = 1`n" })) -match 'StrictMode')
    & $add 'lint：配置文件参数的字面量出现在 Host 以外判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$b = '$word'`n" })) -match '只许在 Host')
    & $add 'lint：配置文件参数的字面量一处都没有判红' $true ((& $lint @{ 'x.ps1' = "$strict`n" }) -match '恰好出现一次')
    & $add 'lint：配置文件参数的字面量出现两次判红' $true ((& $lint @{ 'Host.psm1' = "$strict`n`$a = '$word'`n`$b = '$word'`n" }) -match '恰好出现一次')
    & $add 'lint：注释里提到配置文件参数不算' $false ((& $lint @{ 'Host.psm1' = "$strict`n# $word`n`$a = '$word'`n" }).Count -eq 0)
    & $add 'lint：Host 以外的文件提到 APPDATA 判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$p = `$env:APPDATA`n" })) -match 'APPDATA')
    $cr = [string][char]13; $esc = [string][char]27
    & $add 'lint：脚本里夹着不在行尾的 CR 判红（编辑器看不见，吞掉就改了正则的意思）' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$r = '(;|${cr}?`$)'`n" })) -match '控制字符')
    & $add 'lint：脚本里夹着别的 C0 控制字符（ESC）判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$r = 'a${esc}b'`n" })) -match '控制字符')
    & $add 'lint：.lua 与 .json 里夹着控制字符也判红' $true (((& $lint ($hostOk + @{ 'x.lua' = "local a = 'b${esc}c'`n" })) -match '控制字符') -and ((& $lint ($hostOk + @{ 'x.json' = "{ `"a`": `"b${esc}c`" }`n" })) -match '控制字符'))
    & $add 'lint：行尾 CRLF 与 TAB 不算控制字符' $false ((& $lint @{ 'Host.psm1' = "$strict${cr}`n`t`$a = '$word'${cr}`n" }).Count -eq 0)
    $ctl = Find-ReaperControlChars -Text "ab${cr}cd`nx${esc}y`n"
    & $add '控制字符扫描：报出行、列和码位' $true ($ctl.Count -eq 2 -and $ctl[0] -ceq '第 1 行第 3 列：U+000D' -and $ctl[1] -ceq '第 2 行第 2 列：U+001B')
    & $add '控制字符扫描：干净文本、空文本、CRLF 换行都没有问题' $false ((Find-ReaperControlChars -Text "a`tb${cr}`nc`n").Count -eq 0 -and (Find-ReaperControlChars -Text '').Count -eq 0)
    & $add 'lint：解析不了的脚本判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`nif (`n" })) -match '解析失败')
    if ($CollisionSample -and (Test-Path -LiteralPath $CollisionSample)) {
        $sample = [IO.File]::ReadAllText($CollisionSample)
        & $add 'lint：spike 的旧循环脚本（出事故的那份）被查出碰撞' $true ((Find-ReaperVariableCaseCollisions -Text $sample).Count -gt 0)
    }
    $real = Test-ReaperScriptLint -Dirs (Join-Path $RepoRoot 'scripts/reaper'), (Join-Path $RepoRoot 'tests/reaper')
    & $add "lint：scripts/reaper 与 tests/reaper 自己干净（$($real -join '；')）" $false ($real.Count -eq 0)
    $cases
}

function Get-PureCases([string]$Tmp, [string]$RepoRoot) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $v = { param($c, $vis, $t) (Get-ReaperModalVerdict -Class $c -Visible $vis -Title $t) }
    & $add '模态框：REAPER Query 点「否」' $false ((& $v '#32770' $true 'REAPER Query').Action -eq 'PressNo')
    & $add '模态框：渲染进度窗放行' $false ((& $v '#32770' $true 'Rendering to x.wav').Action -eq 'Benign')
    & $add '模态框：插件扫描进度窗放行' $false ((& $v '#32770' $true 'Scanning VST plugins...').Action -eq 'Benign')
    & $add '模态框：峰值文件进度窗放行' $false ((& $v '#32770' $true 'Building Peaks...').Action -eq 'Benign')
    & $add '模态框：评估许可提示窗不自动点掉，判卡死并点名' $true (($nag = & $v '#32770' $true 'About REAPER v7.82/win64 rev 1 (Oct') -and $nag.Action -eq 'Fatal' -and $nag.Name -eq 'license-nag')
    foreach ($pair in @(@('Project Load Warning', 'project-load-warning'), @('REAPER Error', 'reaper-error'), @('Select files to save or delete', 'record-save-prompt'), @('REAPER', 'no-audio-device'), @('Brand New Box', 'unknown-dialog'))) {
        $r = & $v '#32770' $true $pair[0]
        & $add "模态框：「$($pair[0])」判卡死（$($pair[1])）" $true ($r.Action -eq 'Fatal' -and $r.Name -eq $pair[1])
    }
    & $add '模态框：不可见的对话框不管' $false ((& $v '#32770' $false 'REAPER Error').Action -eq 'Ignore')
    & $add '模态框：主窗口等非对话框不管' $false ((& $v 'REAPERwnd' $true 'REAPER v7').Action -eq 'Ignore')
    & $add '标题里的路径被打码' $false ((ConvertTo-ReaperSafeTitle 'Error in D:\scratch\proj.rpp now') -notmatch 'scratch')
    & $add '结果文本里的盘符路径与 UNC 被打码' $false ((Hide-LocalPaths 'a D:\scratch\y.wav b \\host\share\z c') -notmatch 'scratch|host')
    $nt = { param($bit, $lag, $head) "逐位相同：$bit`n对齐偏移（lag）：$lag 样本（正数表示 B 比 A 晚）`n开头 2.00 秒残差（96000 帧）：峰值 $head dBFS，RMS $head dBFS`n" }
    $good = & $nt '是' '0' '-inf'
    & $add '对拍结论：四项都满足才 PASS' $false ((Get-CompareVerdict 0 $good).Status -eq 'PASS')
    & $add '对拍结论：只有逐位相同是「否」判 FAIL' $true ((Get-CompareVerdict 0 (& $nt '否' '0' '-inf')).Status -eq 'FAIL')
    & $add '对拍结论：只有 lag 不是 0 判 FAIL' $true ((Get-CompareVerdict 0 (& $nt '是' '1' '-inf')).Status -eq 'FAIL')
    & $add '对拍结论：只有前 2 s 残差不是 -inf 判 FAIL' $true ((Get-CompareVerdict 0 (& $nt '是' '0' '-60.0')).Status -eq 'FAIL')
    & $add '对拍结论：只有退出码是 2 判 FAIL' $true ((Get-CompareVerdict 2 $good).Status -eq 'FAIL')
    & $add '对拍结论：报告读不出来判 FAIL，不当 PASS' $true ((Get-CompareVerdict 0 '').Status -eq 'FAIL')
    & $add '对拍结论：退出码 1（没能比）是 INCOMPLETE，不是 FAIL 也不是 PASS' $true ((Get-CompareVerdict 1 '' '打不开文件').Status -eq 'INCOMPLETE')
    & $add '汇总：INCOMPLETE 不当 PASS' $true ((Get-WorstStatus @('PASS', 'INCOMPLETE')) -eq 'INCOMPLETE')
    & $add '汇总：FAIL 压过 INCOMPLETE' $true ((Get-WorstStatus @('INCOMPLETE', 'FAIL', 'PASS')) -eq 'FAIL')
    & $add '汇总：全 SKIPPED 才是 SKIPPED，混着 PASS 算 INCOMPLETE' $false ((Get-WorstStatus @('SKIPPED', 'SKIPPED')) -eq 'SKIPPED' -and (Get-WorstStatus @('PASS', 'SKIPPED')) -eq 'INCOMPLETE')
    & $add '汇总：空列表（什么都没跑）不能是 PASS' $true ((Get-WorstStatus @()) -eq 'INCOMPLETE' -and (Get-WorstStatus $null) -eq 'INCOMPLETE')
    & $add '一轮结局：没有任何场景结局的一轮不是 PASS' $true ((Get-RunVerdict -RowStatuses @() -CfgUnchanged $true -Residual 0) -eq 'INCOMPLETE')
    & $add '一轮结局：场景全绿但默认资源目录变了判 FAIL' $true ((Get-RunVerdict -RowStatuses @('PASS', 'PASS') -CfgUnchanged $false -Residual 0) -eq 'FAIL')
    & $add '一轮结局：场景全绿但结束后还留着 reaper.exe 判 FAIL' $true ((Get-RunVerdict -RowStatuses @('PASS') -CfgUnchanged $true -Residual 1) -eq 'FAIL')
    & $add '一轮结局：全绿、目录没变、没有残留才是 PASS' $false ((Get-RunVerdict -RowStatuses @('PASS', 'PASS') -CfgUnchanged $true -Residual 0) -eq 'PASS')
    $chain = New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -Bypass @('output') -TrackVolume 0.5 -Channels 1 -SampleRate 44100 -OneX
    & $add '工程：Input 在 Output 前' $false ($chain.IndexOf('Rinne Input') -lt $chain.IndexOf('Rinne Output'))
    & $add '工程：旁路行只在被旁路的那个插件前' $false (([regex]::Matches($chain, 'BYPASS 1 0 0').Count -eq 1) -and ($chain.IndexOf('BYPASS 1 0 0') -gt $chain.IndexOf('Rinne Input')))
    & $add '工程：采样率、声道、1x 与轨道音量写进去了' $false ($chain -match 'RENDER_FMT 0 1 44100' -and $chain -match 'RENDER_1X 1' -and $chain -match 'VOLPAN 0.5 ')
    & $add '工程：空链没有 FXCHAIN' $false ((New-ReaperProjectText -SourceWav 'S' -RenderDir 'D' -RenderName 'N' -Chain @()) -notmatch 'FXCHAIN')
    & $add 'Lua 任务：反斜杠与引号被转义' $false ((ConvertTo-ReaperLuaTable @{ p = 'a\b"c' }) -ceq "return {[`"p`"]=`"a\\b\`"c`"}`n")
    $cfgPath = Join-Path $RepoRoot 'tests/reaper/scenarios.json'
    $config = Get-ReaperSmokeConfig $cfgPath
    & $add 'scenarios.json 通过结构校验' $false ((Test-ReaperScenarioConfig $config).Count -eq 0)
    $mutated = { param($edit) $c = Get-ReaperSmokeConfig $cfgPath; & $edit $c; Test-ReaperScenarioConfig $c }
    & $add '校验：重复 id 判红' $true ((& $mutated { param($c) $c.scenarios[1].id = $c.scenarios[0].id }) -match '重复')
    & $add '校验：不认识的 kind 判红' $true ((& $mutated { param($c) $c.scenarios[0].kind = 'nope' }) -match '不认识')
    & $add '校验：缺必填字段判红' $true ((& $mutated { param($c) $c.scenarios[0].PSObject.Properties.Remove('sr') }) -match '缺字段')
    & $add '校验：反例的 expect 不认识判红' $true ((& $mutated { param($c) $c.negative[0].expect = 'MAYBE' }) -match 'expect')
    & $add '校验：hang 放进正例判红' $true ((& $mutated { param($c) $c.scenarios[0].kind = 'hang' }) -match '只能做反例')
    # 选择场景：-Only 写错、什么都没选中是用法错误，不能落成零个场景的 PASS
    $picked = Select-ReaperScenarios -Config $config
    & $add '选择：不给 -Only 时正例与反例都选' $false ($picked.Positive.Count -eq $config.scenarios.Count -and $picked.Negative.Count -eq $config.negative.Count)
    $one = Select-ReaperScenarios -Config $config -Only '^null-offline-48000-512$' -NoNegative
    & $add '选择：-Only 选中一个正例，-NoNegative 时不跑反例' $false ($one.Positive.Count -eq 1 -and $one.Negative.Count -eq 0)
    & $add '选择：-Only 一个场景都没选中是用法错误' $true (Test-Usage { Select-ReaperScenarios -Config $config -Only '^no-such-scenario$' } '*没有选中任何场景*')
    & $add '选择：-Only 只选中反例、又给了 -NoNegative，也是用法错误' $true (Test-Usage { Select-ReaperScenarios -Config $config -Only '^neg-timeout$' -NoNegative } '*-NoNegative*')
    & $add '选择：-Only 不是合法的正则是用法错误' $true (Test-Usage { Select-ReaperScenarios -Config $config -Only '(' } '*正则*')
    # 工程模板里的类 ID 与构建产物的 VST3 清单比对
    $ids = @(Get-ReaperPluginIdentities)
    & $add '类 ID：工程模板里是 Input 与 Output 两个插件，各有包名和 32 位类 ID' $false ($ids.Count -eq 2 -and $ids[0].Bundle -ceq 'Rinne Input.vst3' -and $ids[1].Bundle -ceq 'Rinne Output.vst3' -and $ids[0].ClassId -cmatch '^[0-9A-F]{32}$' -and $ids[0].ClassId -cne $ids[1].ClassId)
    $mi = { param($cid) "{`n  `"Classes`": [`n    {`n      `"CID`": `"$cid`",`n      `"Category`": `"Audio Module Class`",`n    },`n    {`n      `"CID`": `"00000000000000000000000000000000`",`n      `"Category`": `"Component Controller Class`",`n    },`n  ],`n}`n" }
    & $add '类 ID：从清单文本里取 Audio Module Class 那一条，大小写不敏感，不取 Controller 那条' $false ((Get-ReaperAudioClassId -ModuleInfoText (& $mi 'abcdef019182faeb536e6368526e6e69')) -ceq 'ABCDEF019182FAEB536E6368526E6E69')
    & $add '类 ID：清单里没有 Audio Module Class 或格式读不出就取不到' $true ($null -eq (Get-ReaperAudioClassId -ModuleInfoText '{ "Classes": [ { "CID": "00000000000000000000000000000000", "Category": "Component Controller Class" } ] }') -and $null -eq (Get-ReaperAudioClassId -ModuleInfoText ''))
    # 预检
    $fake = Join-Path $Tmp 'build'; $null = New-Item -ItemType Directory -Path $fake
    $pwshExe = (Get-Process -Id $PID).Path
    $bundleIn = Join-Path $Tmp 'Rinne Input.vst3'; $bundleOut = Join-Path $Tmp 'Rinne Output.vst3'
    $writeManifest = { param($dir, $cid) $res = Join-Path $dir 'Contents/Resources'; $null = New-Item -ItemType Directory -Force -Path $res; [IO.File]::WriteAllText((Join-Path $res 'moduleinfo.json'), (& $mi $cid)) }
    & $writeManifest $bundleIn $ids[0].ClassId
    & $writeManifest $bundleOut $ids[1].ClassId
    $marker = Join-Path $Tmp 'USER_TESTING'
    $info = { param($flavor, $sha = 'abcdef0123456789') [IO.File]::WriteAllText((Join-Path $fake 'rinne-build.json'), (@{ sha = $sha; flavor = $flavor; bundles = @($bundleIn, $bundleOut) } | ConvertTo-Json)) }
    & $info 'dev'
    $pre = Get-ReaperPreflight -ReaperExe $pwshExe -BuildDir $fake -NullTest $pwshExe -UserTestingMarker $marker
    $mine = @($pre.Reasons | Where-Object { $_ -notmatch '已有 reaper.exe' })
    & $add "预检：材料齐全时没有缺项（$($mine -join '；')）" $false ($mine.Count -eq 0 -and $pre.Sha7 -eq 'abcdef0')
    $preWith = { Get-ReaperPreflight -ReaperExe $pwshExe -BuildDir $fake -NullTest $pwshExe -UserTestingMarker $marker }
    $reasonsNow = { try { @((& $preWith).Reasons) } catch { @("THREW") } } # 抛异常就只留一个标记，不把异常消息当作原因（消息里可能碰巧带着 JSON 之类的字样）
    $reasonsFor = { param($sha) & $info 'dev' $sha; & $reasonsNow }
    & $add '预检：sha 缺失判缺，不抛异常' $true ((& $reasonsFor '') -match 'sha')
    [IO.File]::WriteAllText((Join-Path $fake 'rinne-build.json'), (@{ flavor = 'dev'; bundles = @($bundleIn, $bundleOut) } | ConvertTo-Json))
    & $add '预检：rinne-build.json 里干脆没有 sha 字段判缺，不抛异常' $true ((& $reasonsNow) -match 'sha')
    & $add '预检：sha 短于 7 位判缺，不抛异常' $true ((& $reasonsFor 'abc123') -match 'sha')
    & $add '预检：sha 不是十六进制判缺（它会拼进结果文件名）' $true ((& $reasonsFor 'zzzzzzzzzz..\x') -match 'sha')
    [IO.File]::WriteAllText((Join-Path $fake 'rinne-build.json'), '{ not json')
    & $add '预检：rinne-build.json 不是合法 JSON 判缺，不抛异常' $true ((& $reasonsNow) -match 'JSON')
    & $writeManifest $bundleIn ('A' * 32)
    & $info 'dev'
    & $add '预检：构建清单里的类 ID 与工程模板对不上，整套 SKIPPED 并点名' $true ((& $preWith).Reasons -match '类 ID 是 A{32}')
    & $writeManifest $bundleIn $ids[0].ClassId
    Remove-Item -LiteralPath (Join-Path $bundleOut 'Contents/Resources/moduleinfo.json')
    & $add '预检：插件包里没有 moduleinfo.json 判缺' $true ((& $preWith).Reasons -match 'moduleinfo.json')
    & $writeManifest $bundleOut $ids[1].ClassId
    & $add '预检：类 ID 对上后恢复无缺项' $false (@((& $preWith).Reasons | Where-Object { $_ -notmatch '已有 reaper.exe' }).Count -eq 0)
    & $info 'release'
    & $add '预检：release 构建被拒（只测 D 前缀）' $true ((Get-ReaperPreflight -ReaperExe $pwshExe -BuildDir $fake -NullTest $pwshExe -UserTestingMarker $marker).Reasons -match 'flavor')
    & $add '预检：reaper.exe 不在判缺' $true ((Get-ReaperPreflight -ReaperExe (Join-Path $Tmp 'no.exe') -BuildDir $fake -NullTest $pwshExe -UserTestingMarker $marker).Reasons -match 'reaper.exe')
    & $add '预检：没有构建清单判缺' $true ((Get-ReaperPreflight -ReaperExe $pwshExe -BuildDir (Join-Path $Tmp 'nobuild') -NullTest $pwshExe -UserTestingMarker $marker).Reasons -match 'rinne-build')
    Set-Content -LiteralPath $marker -Value x
    $withMarker = Get-ReaperPreflight -ReaperExe (Join-Path $Tmp 'no.exe') -BuildDir (Join-Path $Tmp 'nobuild') -UserTestingMarker $marker
    & $add 'USER_TESTING 标记存在时预检只给这一条原因（整套 SKIPPED）' $true ($withMarker.Reasons.Count -eq 1 -and $withMarker.Reasons[0] -match 'USER_TESTING')
    Remove-Item -LiteralPath $marker
    $cases
}

function Get-ChildCases([string]$Tmp, [string]$RepoRoot) {
    $cfgA = Join-Path $Tmp 'child-cfg-a'; $cfgB = Join-Path $Tmp 'child-cfg-b'
    $null = New-ReaperCfgDir $cfgA; $null = New-ReaperCfgDir $cfgB
    $work = Join-Path $Tmp 'child-work'; $null = New-Item -ItemType Directory -Path $work
    $r = Invoke-Native -Exe (Get-Process -Id $PID).Path -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'SelfTestChild.ps1'),
        '-RepoRoot', $RepoRoot, '-CfgA', $cfgA, '-CfgB', $cfgB, '-Work', $work)
    $cases = [Collections.Generic.List[object]]::new()
    foreach ($line in ($r.Out -split "`r?`n")) {
        $f = $line -split "`t", 4
        if ($f.Count -eq 4 -and $f[0] -eq 'RESULT') { $cases.Add([pscustomobject]@{ Label = $f[3]; Bad = ($f[1] -eq '1'); Passed = ($f[2] -eq '1') }) }
    }
    if ($r.Exit -ne 0 -or $cases.Count -eq 0) { $cases.Add([pscustomobject]@{ Label = "子进程自测异常（退出码 $($r.Exit)）：$(($r.Err + $r.Out).Trim() -replace '\s+', ' ')"; Bad = $false; Passed = $false }) }
    $cases
}

function Invoke-ReaperSelfTest {
    param([Parameter(Mandatory)][string]$RepoRoot, [string]$CollisionSample = '')
    $realDefault = Get-ReaperDefaultCfgDir
    $before = Get-ReaperDefaultCfgSnapshot
    $scratch = New-RinneTempDir -Prefix 'rinne-reaper-selftest-'
    $savedRoaming = $env:APPDATA
    $cases = [Collections.Generic.List[object]]::new()
    try {
        $fakeRoam = Join-Path $scratch 'Roaming-Fake-AppData'
        $null = New-Item -ItemType Directory -Path (Join-Path $fakeRoam 'REAPER')
        $env:APPDATA = $fakeRoam # 之后所有「默认资源目录」都指向这个假的；子进程继承
        foreach ($group in @{ Fn = { Get-GuardCases $scratch } }, @{ Fn = { Get-IniCases } }, @{ Fn = { Get-SnapshotCases $scratch } },
                           @{ Fn = { Get-LintCases $scratch $RepoRoot $CollisionSample } }, @{ Fn = { Get-PureCases $scratch $RepoRoot } }, @{ Fn = { Get-ChildCases $scratch $RepoRoot } }) {
            foreach ($c in (& $group.Fn)) { $cases.Add($c) }
        }
    } finally {
        $env:APPDATA = $savedRoaming
        $why = Remove-RinneTempDir -Path $scratch -WaitSeconds 10
        if ($why) { Write-Host "  临时目录没删掉：$why" }
    }
    $after = Get-ReaperDefaultCfgSnapshot
    $cmp = Compare-ReaperDefaultCfgSnapshot -Before $before -After $after
    $cases.Add([pscustomobject]@{ Label = "自测前后真实的默认资源目录没变（$($cmp.Count) 项）"; Bad = $false; Passed = $cmp.Same })
    $badCount = 0; $goodCount = 0; $red = 0
    foreach ($c in $cases) {
        if ($c.Bad) { $badCount++ } else { $goodCount++ }
        if (-not $c.Passed) { $red++ }
        Write-Host ('  [{0}] {1}{2}' -f ($c.Passed ? 'PASS' : 'FAIL'), ($c.Bad ? '反例：' : ''), $c.Label)
    }
    Write-Host ("REAPER 烟测自测：{0}（反例 {1}、正例 {2}、未通过 {3}）" -f ($red -eq 0 ? '通过' : '失败'), $badCount, $goodCount, $red)
    $red -eq 0 ? 0 : 1
}

Export-ModuleMember -Function Invoke-ReaperSelfTest
