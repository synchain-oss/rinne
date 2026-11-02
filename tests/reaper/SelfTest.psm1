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
Import-Module (Join-Path $PSScriptRoot 'SelfTestProbe.psm1') -ErrorAction Stop # 第二段（探针）的用例
Import-Module (Join-Path $script:Scripts '../lib/TempDir.psm1') -ErrorAction Stop

function Test-Refused([scriptblock]$Action, [string]$Like = '*') {
    try { $null = & $Action; $false } catch { $_.Exception.Message -like 'REFUSED:*' -and $_.Exception.Message -like $Like }
}

function Test-Usage([scriptblock]$Action, [string]$Like = '*') {
    try { $null = & $Action; $false } catch { $_.Exception.Message -like 'USAGE:*' -and $_.Exception.Message -like $Like }
}

function Write-Bytes([string]$Path, [byte[]]$Bytes) { [IO.File]::WriteAllBytes($Path, $Bytes) }

$script:ScratchPrefix = 'rinne-reaper-selftest-' # 自测 scratch 目录的前缀（New-RinneTempDir 在它后面接 32 位随机十六进制）
$script:SubstLetters = 'Z', 'Y', 'X', 'W', 'V', 'U' # 自测借来做 subst 的盘符（从前往后取第一个空闲的），开头清残留也只看这几个

# subst 映射对整个登录会话可见：自测被硬结束（Ctrl+C 两次、看门狗、关终端）时 finally 里的 subst /d 不会跑，盘符就一直指向已被清掉的
# 临时目录，下次自测还会把这个字母当成已占用。所以自测开头先清。只清同时满足两条的：盘符在 Z…U 里；目标路径是 -Prefix 后面紧跟 32 位
# 十六进制（自测 scratch 目录的命名）的目录或它之下。别的映射（用户自己的）一个不碰。返回处理过的映射（字母、去向、是否清掉），
# 由调用方打印——去向只进本机日志。
function Get-SelfTestStalePrefix { [IO.Path]::Join([IO.Path]::GetTempPath(), $script:ScratchPrefix) } # 自测 scratch 目录路径的前缀，后面接 32 位十六进制

function Clear-ReaperSelftestSubst {
    param([string]$Prefix = (Get-SelfTestStalePrefix), [string[]]$Letters = $script:SubstLetters)
    $done = [Collections.Generic.List[object]]::new()
    $listing = try { (& subst.exe 2>&1 | Out-String) } catch { '' }
    $prefixFull = [IO.Path]::GetFullPath($Prefix)
    $owned = '^' + [regex]::Escape($prefixFull) + '[0-9a-f]{32}(\\|$)'
    foreach ($line in ($listing -split "`r?`n")) {
        $m = [regex]::Match($line, '^([A-Za-z]):\\: => (.+)$')
        if (-not $m.Success) { continue }
        $driveLetter = $m.Groups[1].Value.ToUpperInvariant()
        if ($driveLetter -notin $Letters) { continue }
        $target = $m.Groups[2].Value.Trim()
        $full = try { [IO.Path]::GetFullPath($target) } catch { '' }
        if (-not $full -or $full -inotmatch $owned) { continue }
        & subst.exe "${driveLetter}:" /d 2>&1 | Out-Null
        $done.Add([pscustomobject]@{ Letter = $driveLetter; Target = $target; Cleared = ($LASTEXITCODE -eq 0) })
    }
    $done.ToArray() # 逐项输出（不加逗号包一层）：调用方用 @() 接，没有残留时就是空数组
}

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
    $letter = @($script:SubstLetters) | Where-Object { $_ -notin $used } | Select-Object -First 1
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
    foreach ($c in (Get-InitCases $Tmp)) { $cases.Add($c) }
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

# doctor.ps1 -Init（New-ReaperCfgDir）只接管三种目录：还不存在的、空的、以前 -Init 过的（有哨兵）。已有内容又没有哨兵的一律拒绝，
# 什么都不写：前后按「名字、大小、修改时间、内容散列」比一遍整棵树。
function Get-TreeFingerprint([string]$TreeDir) {
    @(Get-ChildItem -LiteralPath $TreeDir -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($TreeDir.Length)
        $_.PSIsContainer ? "$rel|dir" : ('{0}|{1}|{2}|{3}' -f $rel, $_.Length, $_.LastWriteTimeUtc.Ticks, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)
    }) -join "`n"
}

function Get-InitCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    $sentinel = (Get-ReaperSentinel).Name
    $refusedUntouched = {
        param([string]$TreeDir)
        $before = Get-TreeFingerprint $TreeDir
        $refused = Test-Refused { New-ReaperCfgDir $TreeDir } '*已有内容却没有哨兵*'
        $refused -and (Get-TreeFingerprint $TreeDir) -ceq $before -and -not (Test-Path -LiteralPath (Join-Path $TreeDir $sentinel))
    }
    # 一份便携版 REAPER 的样子：有 reaper.ini、有 plugins 子目录
    $portable = Join-Path $Tmp 'init-portable'
    $null = New-Item -ItemType Directory -Path (Join-Path $portable 'plugins')
    [IO.File]::WriteAllText((Join-Path $portable 'reaper.ini'), "[REAPER]`nvstpath64=elsewhere`n[audioconfig]`nmode=3`n")
    [IO.File]::WriteAllText((Join-Path $portable 'plugins/user.dll'), 'bits')
    & $add '-Init：已有内容又没有哨兵的目录（例如便携版 REAPER 的目录）被拒绝，什么都没写（树前后逐项相同，没建哨兵）' $true (& $refusedUntouched $portable)
    $hiddenOnly = Join-Path $Tmp 'init-hidden-only'
    $null = New-Item -ItemType Directory -Path $hiddenOnly
    $hiddenFile = Join-Path $hiddenOnly 'desktop.ini'
    [IO.File]::WriteAllText($hiddenFile, 'x')
    (Get-Item -LiteralPath $hiddenFile -Force).Attributes = 'Hidden'
    & $add '-Init：只有一个隐藏文件的目录也算有内容，被拒绝且什么都没写' $true (& $refusedUntouched $hiddenOnly)
    $subOnly = Join-Path $Tmp 'init-subdir-only'
    $null = New-Item -ItemType Directory -Path (Join-Path $subOnly 'empty-sub')
    & $add '-Init：只有一个空子目录的目录也算有内容，被拒绝且什么都没写' $true (& $refusedUntouched $subOnly)
    $wrongMagic = Join-Path $Tmp 'init-wrong-sentinel'
    $null = New-Item -ItemType Directory -Path $wrongMagic
    [IO.File]::WriteAllText((Join-Path $wrongMagic $sentinel), "something else`n")
    [IO.File]::WriteAllText((Join-Path $wrongMagic 'reaper.ini'), "[REAPER]`n")
    $before = Get-TreeFingerprint $wrongMagic
    & $add '-Init：哨兵内容不对的目录不算以前 -Init 过的，被拒绝且什么都没写' $true ((Test-Refused { New-ReaperCfgDir $wrongMagic } '*已有内容却没有哨兵*') -and (Get-TreeFingerprint $wrongMagic) -ceq $before)
    $aFile = Join-Path $Tmp 'init-a-file'
    [IO.File]::WriteAllText($aFile, 'x')
    & $add '-Init：-CfgDir 指向一个文件被拒绝，文件没被动' $true ((Test-Refused { New-ReaperCfgDir $aFile } '*是一个文件*') -and [IO.File]::ReadAllText($aFile) -ceq 'x')
    $empty = Join-Path $Tmp 'init-empty'
    $null = New-Item -ItemType Directory -Path $empty
    $made = try { New-ReaperCfgDir $empty } catch { $null }
    & $add '-Init：空目录照常接管（建 plugins、reaper.ini、哨兵）' $false ($made -and (Test-Path -LiteralPath (Join-Path $empty $sentinel)) -and (Test-Path -LiteralPath (Join-Path $empty 'plugins') -PathType Container) -and (Test-Path -LiteralPath (Join-Path $empty 'reaper.ini')))
    [IO.File]::WriteAllText((Join-Path $empty 'reaper.ini'), "[REAPER]`nkept=1`n")
    $again = try { New-ReaperCfgDir $empty } catch { $null }
    & $add '-Init：以前 -Init 过的目录（有哨兵）照常可用，已有的 reaper.ini 不动' $false ($again -and [IO.File]::ReadAllText((Join-Path $empty 'reaper.ini')) -ceq "[REAPER]`nkept=1`n")
    $fresh = Join-Path $Tmp 'init-not-yet'
    & $add '-Init：还不存在的目录照常新建' $false ($(try { New-ReaperCfgDir $fresh } catch { $null }) -and (Test-Path -LiteralPath (Join-Path $fresh $sentinel)))
    # 走真正的入口：doctor.ps1 -Init 对已有内容又没有哨兵的目录退出码 64，什么都没写
    $viaDoctor = Join-Path $Tmp 'init-doctor-portable'
    $null = New-Item -ItemType Directory -Path $viaDoctor
    [IO.File]::WriteAllText((Join-Path $viaDoctor 'reaper.ini'), "[REAPER]`nportable=1`n")
    $before = Get-TreeFingerprint $viaDoctor
    $r = Invoke-Native -Exe (Get-Process -Id $PID).Path -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $script:Scripts 'doctor.ps1'), '-CfgDir', $viaDoctor, '-Init')
    & $add '-Init 入口：doctor.ps1 -Init 对已有内容又没有哨兵的目录退出码 64，什么都没写' $true ($r.Exit -eq 64 -and $r.Out -match 'REFUSED:' -and (Get-TreeFingerprint $viaDoctor) -ceq $before)
    $cases
}

# 自测开头清 subst 残留（Clear-ReaperSelftestSubst）：用真的 subst 造映射，清完看它还在不在。这台机器上 Z…U 里往往只剩一个空闲盘符，
# 所以一次只用一个字母，四种映射轮流造、轮流撤；造的映射和目录自己收拾，不碰别的。
function Get-SubstResidueCases([string]$Tmp) {
    $cases = [Collections.Generic.List[object]]::new()
    $add = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    # 开头清理用的前缀必须就是自测真正建 scratch 目录的命名（$Tmp 正是这一次自测的 scratch 目录），否则真的残留认不出来
    $fullPrefix = [IO.Path]::GetFullPath((Get-SelfTestStalePrefix))
    & $add 'subst 残留：开头清理用的前缀就是自测 scratch 目录的命名（前缀加 32 位十六进制）' $false ([IO.Path]::GetFullPath($Tmp) -imatch ('^' + [regex]::Escape($fullPrefix) + '[0-9a-f]{32}$'))
    $used = @([IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpperInvariant() })
    $slot = @($script:SubstLetters | Where-Object { $_ -notin $used }) | Select-Object -First 1
    if (-not $slot) {
        & $add '（Z…U 里没有空闲盘符：subst 残留清理没能真跑）' $false $true
        return $cases
    }
    $hex = { [guid]::NewGuid().ToString('N') }
    $stalePrefix = Join-Path $Tmp 'stale-'
    $listed = { (& subst.exe 2>&1 | Out-String) }
    # 把 $slot 指到 $Place（要先建），可选地再删掉 $Place（目标没了、映射还在：和自测被硬结束之后的样子一样），然后清一遍，返回清理的报告和映射还在不在
    $attempt = {
        param([string]$Place, [bool]$Dangling, [string[]]$Scope)
        $null = New-Item -ItemType Directory -Path $Place
        try {
            & subst.exe "${slot}:" $Place 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { return $null }
            if ($Dangling) { Remove-Item -LiteralPath $Place }
            $report = @(Clear-ReaperSelftestSubst -Prefix $stalePrefix -Letters $Scope)
            [pscustomobject]@{ Report = $report; Still = [bool]((& $listed) -match ('(?m)^' + $slot + ':\\: =>')) }
        } finally {
            & subst.exe "${slot}:" /d 2>&1 | Out-Null # 只撤销这里自己造的
            if (Test-Path -LiteralPath $Place) { Remove-Item -LiteralPath $Place } # 空目录，不递归
        }
    }
    $r = & $attempt "$stalePrefix$(& $hex)" $true $script:SubstLetters
    & $add 'subst 残留：Z…U 里指向自测 scratch 目录（前缀加 32 位十六进制）的残留映射被清掉，并报出字母和去向' $false ($r -and -not $r.Still -and $r.Report.Count -eq 1 -and $r.Report[0].Letter -ceq $slot -and $r.Report[0].Cleared -and $r.Report[0].Target -like "*$stalePrefix*")
    $r = & $attempt (Join-Path $Tmp "keep-$(& $hex)") $false $script:SubstLetters
    & $add 'subst 残留：指向别处的映射（用户自己的）不碰' $true ($r -and $r.Still -and $r.Report.Count -eq 0)
    $r = & $attempt "${stalePrefix}notes" $false $script:SubstLetters
    & $add 'subst 残留：前缀之后不是 32 位十六进制的不碰' $true ($r -and $r.Still -and $r.Report.Count -eq 0)
    $r = & $attempt "$stalePrefix$(& $hex)" $false @('Q') # 清理的盘符范围里没有这个字母（范围默认是 Z…U）
    & $add 'subst 残留：不在清理范围内的盘符不碰（哪怕指向自测 scratch 目录）' $true ($r -and $r.Still -and $r.Report.Count -eq 0)
    & $add 'subst 残留：清理范围就是 Z…U' $false (($script:SubstLetters -join '') -ceq 'ZYXWVU')
    & $add 'subst 残留：没有残留时什么都不做' $false (@(Clear-ReaperSelftestSubst -Prefix $stalePrefix).Count -eq 0)
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
    # 目录里有叫 Keys、Count、Values 的文件：快照字典里就有同名条目，属性读返回的是条目而不是集合，前后比较会悄悄比错
    $namesRoot = Join-Path $Tmp 'snap-names'
    $null = New-Item -ItemType Directory -Path $namesRoot
    foreach ($fileName in 'Keys', 'Count', 'Values') { Set-Content -LiteralPath (Join-Path $namesRoot $fileName) -Value $fileName }
    $n0 = Get-ReaperDefaultCfgSnapshot -Root $namesRoot
    $nSame = Compare-ReaperDefaultCfgSnapshot -Before $n0 -After (Get-ReaperDefaultCfgSnapshot -Root $namesRoot)
    & $add '清单：目录里有叫 Keys、Count、Values 的文件，没变时不报，条目数是 3' $false ($nSame.Same -and $nSame.Count -eq 3)
    Add-Content -LiteralPath (Join-Path $namesRoot 'Count') -Value 'more'
    $n1 = Get-ReaperDefaultCfgSnapshot -Root $namesRoot
    $nChanged = Compare-ReaperDefaultCfgSnapshot -Before $n0 -After $n1
    & $add '清单：叫 Count 的文件变了照样判变，只报这一项' $true (-not $nChanged.Same -and @($nChanged.Changed).Count -eq 1 -and $nChanged.Changed[0] -ceq 'changed:Count')
    Remove-Item -LiteralPath (Join-Path $namesRoot 'Keys')
    Set-Content -LiteralPath (Join-Path $namesRoot 'Values2') -Value 'v2'
    $nMoved = Compare-ReaperDefaultCfgSnapshot -Before $n1 -After (Get-ReaperDefaultCfgSnapshot -Root $namesRoot)
    & $add '清单：叫 Keys 的文件被删、新增一个文件，照样各报一项' $true (-not $nMoved.Same -and (@($nMoved.Changed | Sort-Object) -join ',') -ceq 'added:Values2,removed:Keys')
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
        foreach ($name in $Files.get_Keys()) { [IO.File]::WriteAllText((Join-Path $dir $name), $Files[$name]) }
        try { Test-ReaperScriptLint -Dirs $dir } finally { foreach ($name in $Files.get_Keys()) { Remove-Item -LiteralPath (Join-Path $dir $name) }; Remove-Item -LiteralPath $dir }
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
    # 被扫文本里有名为 keys / values / count 的变量时，扫描自己用的字典里就有同名条目，属性读返回的是条目而不是集合：整份文件等于没扫。
    # 用例都是「文本里同时有这类变量和只差大小写的另一对变量」，必须仍然报出那一对（且只报这一对）
    $pair = "`$a = 1`n`$A = 2`n"
    $pairOnly = { param($Found) @($Found).Count -eq 1 -and $Found[0] -cmatch '^变量名只差大小写：(\$a / \$A|\$A / \$a)$' }
    & $add '碰撞扫描：文本里有 $keys 变量时，另一对只差大小写的变量照样报出' $true (& $pairOnly (Find-ReaperVariableCaseCollisions -Text "`$keys = 1`n$pair"))
    & $add '碰撞扫描：文本里有 $Values 变量时，另一对只差大小写的变量照样报出' $true (& $pairOnly (Find-ReaperVariableCaseCollisions -Text "`$Values = 1`n$pair"))
    & $add '碰撞扫描：文本里有 $Count 变量时，另一对只差大小写的变量照样报出' $true (& $pairOnly (Find-ReaperVariableCaseCollisions -Text "`$Count = 1`n$pair"))
    & $add '碰撞扫描：$keys、$values、$Count 三个都在时，另一对只差大小写的变量照样报出' $true (& $pairOnly (Find-ReaperVariableCaseCollisions -Text "`$keys = 1`n`$values = 2`n`$Count = 3`n$pair"))
    $selfPair = @(Find-ReaperVariableCaseCollisions -Text "`$keys = 1`n`$Keys = 2`n")
    & $add '碰撞扫描：$keys 与 $Keys 自己只差大小写，也报出来' $true ($selfPair.Count -eq 1 -and $selfPair[0] -cmatch '^变量名只差大小写：(\$keys / \$Keys|\$Keys / \$keys)$')
    & $add '碰撞扫描：有 $keys、$values、$count 变量但没有碰撞时不报' $false ((Find-ReaperVariableCaseCollisions -Text "`$keys = 1`n`$values = 2`n`$count = 3`n`$keys = 4`n").Count -eq 0)
    & $add 'lint：字典的 .Keys 当属性读判红（外部数据当键时会被同名条目遮住）' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$h = @{}`nforeach (`$k in `$h.Keys) { `$k }`n" })) -match '第 3 行：\.Keys 当属性读')
    & $add 'lint：字典的 .Values 当属性读判红' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$h = @{}`n@(`$h.Values)`n" })) -match '第 3 行：\.Values 当属性读')
    & $add 'lint：小写的 .keys 同样判红（成员名不分大小写）' $true ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n`$h = @{}`n@(`$h.keys)`n" })) -match '当属性读')
    & $add 'lint：.get_Keys()、.get_Values()、.GetEnumerator() 不判红，注释与字符串里提到 .Keys 也不算' $false ((& $lint ($hostOk + @{ 'x.ps1' = "$strict`n# 不读 .Keys`n`$h = @{}`n@(`$h.get_Keys()); @(`$h.get_Values()); `$h.GetEnumerator()`n'.Keys 与 .Values'`n" })).Count -eq 0)
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
    $weak = "A：a.wav（48000 Hz，2 声道，480000 帧，10.000 秒）`nB：b.wav（48000 Hz，2 声道，480000 帧，10.000 秒）`n结果：FAIL`n原因：信号太弱，无法判定：A 的峰值 -inf dBFS 低于 -60 dBFS，里面没有声音，对拍没有意义`n"
    $weakVerdict = Get-CompareVerdict 2 $weak
    & $add '对拍结论：对拍工具给了拒绝理由（A 里没有声音）时判 FAIL，并把理由写进结论（没有比对行也说得清）' $true ($weakVerdict.Status -eq 'FAIL' -and $weakVerdict.Detail -like '*对拍工具说：信号太弱，无法判定：A 的峰值 -inf dBFS*没有声音*')
    & $add '对拍结论：对拍工具没给理由时，结论里不带「对拍工具说」' $false ((Get-CompareVerdict 0 $good).Detail -notlike '*对拍工具说*' -and (Get-CompareVerdict 0 (& $nt '否' '0' '-6.94')).Detail -notlike '*对拍工具说*')
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
    # 键是外面给的名字：叫 Keys、Values、Count 的键，字典的属性读返回的是它们的值而不是集合，整张表会写错
    & $add 'Lua 任务：键叫 Keys、Values、Count 时一个不少、值对得上' $false ((ConvertTo-ReaperLuaTable @{ Keys = 'k'; Values = 'v'; Count = 3; p = 'x' }) -ceq "return {[`"Count`"]=3,[`"Keys`"]=`"k`",[`"p`"]=`"x`",[`"Values`"]=`"v`"}`n")
    $cfgPath = Join-Path $RepoRoot 'tests/reaper/scenarios.json'
    $config = Get-ReaperSmokeConfig $cfgPath
    & $add 'scenarios.json 通过结构校验' $false ((Test-ReaperScenarioConfig $config).Count -eq 0)
    $mutated = { param($edit) $c = Get-ReaperSmokeConfig $cfgPath; & $edit $c; Test-ReaperScenarioConfig $c }
    & $add '校验：重复 id 判红' $true ((& $mutated { param($c) $c.scenarios[1].id = $c.scenarios[0].id }) -match '重复')
    & $add '校验：不认识的 kind 判红' $true ((& $mutated { param($c) $c.scenarios[0].kind = 'nope' }) -match '不认识')
    & $add '校验：缺必填字段判红' $true ((& $mutated { param($c) $c.scenarios[0].PSObject.Properties.Remove('sr') }) -match '缺字段')
    & $add '校验：反例的 expect 不认识判红' $true ((& $mutated { param($c) $c.negative[0].expect = 'MAYBE' }) -match 'expect')
    & $add '校验：hang 放进正例判红' $true ((& $mutated { param($c) $c.scenarios[0].kind = 'hang' }) -match '只能做反例')
    & $add '校验：反例缺 expectDetail 判红' $true ((& $mutated { param($c) $c.negative[0].PSObject.Properties.Remove('expectDetail') }) -match '缺 expectDetail')
    & $add '校验：反例的 expectDetail 不是合法的正则判红' $true ((& $mutated { param($c) $c.negative[0].expectDetail = '(' }) -match 'expectDetail 不是合法')
    # 反例要同时对上结局和原因。只比结局会被「没启动、没执行」空过：gui 锁等不到时 REAPER 没有启动，看门狗反例照样是 INCOMPLETE，却没验证到看门狗
    $negs = @{}
    foreach ($n in $config.negative) { $negs[$n.id] = $n }
    $verdict = { param($id, $observed, $detail) Get-NegativeVerdict -Expect $negs[$id].expect -ExpectDetail $negs[$id].expectDetail -Observed $observed -Detail $detail }
    $hang = { param($state, $exitCode = 0) $w = Get-HostFailure ([pscustomobject]@{ Status = $state; Seconds = 8.2; ExitCode = $exitCode; Modal = 'license-nag'; Windows = @('About REAPER') }); "$w；进程已结束，gui 锁已放开" } # 与 Invoke-HangScenario 拼出来的原因同一个样子
    & $add '反例判定（真实的 scenarios.json）：看门狗超时的反例，超时被结束才是 PASS' $false ((& $verdict 'neg-timeout' 'INCOMPLETE' (& $hang 'Timeout')).Status -eq 'PASS')
    & $add '反例判定（真实的 scenarios.json）：到点被杀的反例，到点被结束才是 PASS' $false ((& $verdict 'neg-killed' 'INCOMPLETE' (& $hang 'Killed')).Status -eq 'PASS')
    $busy = & $verdict 'neg-timeout' 'INCOMPLETE' 'gui 锁等不到；REAPER 没有启动，看门狗没能验证'
    & $add '反例判定：gui 锁等不到（REAPER 没有启动）也是 INCOMPLETE，但不能让看门狗反例判 PASS，整行 INCOMPLETE 并写明没能验证' $true ($busy.Status -eq 'INCOMPLETE' -and $busy.Detail -match '没能验证')
    & $add '反例判定：到点被杀的反例同样不能被 gui 锁等不到空过' $true ((& $verdict 'neg-killed' 'INCOMPLETE' (& $hang 'LockBusy')).Status -eq 'INCOMPLETE')
    & $add '反例判定：看门狗反例被对话框卡住、或 REAPER 自己崩溃退出（INCOMPLETE 但不是看门狗按时限处理）也不算验证过' $true (((& $verdict 'neg-timeout' 'INCOMPLETE' (& $hang 'Modal')).Status -eq 'INCOMPLETE') -and ((& $verdict 'neg-timeout' 'INCOMPLETE' (& $hang 'Exited' 3)).Status -eq 'INCOMPLETE') -and ((& $verdict 'neg-killed' 'INCOMPLETE' (& $hang 'Modal')).Status -eq 'INCOMPLETE'))
    & $add '反例判定：超时与到点被杀不能互相冒充' $true (((& $verdict 'neg-timeout' 'INCOMPLETE' (& $hang 'Killed')).Status -eq 'INCOMPLETE') -and ((& $verdict 'neg-killed' 'INCOMPLETE' (& $hang 'Timeout')).Status -eq 'INCOMPLETE'))
    & $add '反例判定：偏 1 个样本的反例要看到 lag=1' $false ((& $verdict 'neg-offset-1-sample' 'FAIL' (Get-CompareVerdict 0 (& $nt '否' '1' '-inf')).Detail).Status -eq 'PASS')
    & $add '反例判定：偏 1 个样本的反例，对拍报告读不出来（也是 FAIL，但没有 lag）不算验证过' $true ((& $verdict 'neg-offset-1-sample' 'FAIL' (Get-CompareVerdict 0 '').Detail).Status -eq 'INCOMPLETE')
    & $add '反例判定：轨道 -6 dB 的反例要看到 lag=0 加前 2 s 峰值约 -6.9' $false ((& $verdict 'neg-track-minus-6db' 'FAIL' (Get-CompareVerdict 0 (& $nt '否' '0' '-6.94')).Detail).Status -eq 'PASS')
    & $add '反例判定：轨道 -6 dB 的反例，换成 lag=1 的 FAIL 不能冒充' $true ((& $verdict 'neg-track-minus-6db' 'FAIL' (Get-CompareVerdict 0 (& $nt '否' '1' '-inf')).Detail).Status -eq 'INCOMPLETE')
    & $add '反例判定：观察到 INCOMPLETE（反例没能执行，例如参照渲染没成）而期望是 FAIL，整行 INCOMPLETE，不判 FAIL' $true ((& $verdict 'neg-offset-1-sample' 'INCOMPLETE' '参照渲染没成：超时').Status -eq 'INCOMPLETE')
    & $add '反例判定：该变红的没有变红（观察到 PASS，期望 FAIL）仍是 FAIL' $true ((& $verdict 'neg-offset-1-sample' 'PASS' '逐位相同=是 lag=0 前2s峰值=-inf').Status -eq 'FAIL')
    $refusal = { param($block) try { $null = & $block; 'NOTHROW' } catch { $_.Exception.Message -replace '\s+', ' ' } } # 与 Invoke-GuardNegative 记下的原因同一个样子
    $msgDefault = & $refusal { Assert-ReaperCfgDir (Get-ReaperDefaultCfgDir) }
    $bareDir = Join-Path $Tmp 'neg-bare'
    $null = New-Item -ItemType Directory -Path $bareDir
    Set-Content -LiteralPath (Join-Path $bareDir 'reaper.ini') -Value '[REAPER]'
    $msgBare = & $refusal { Assert-ReaperCfgDir $bareDir }
    Remove-Item -LiteralPath (Join-Path $bareDir 'reaper.ini'); Remove-Item -LiteralPath $bareDir
    & $add '反例判定（真实的 scenarios.json）：缺哨兵的守卫反例，拒绝的原因是哨兵才是 PASS；默认资源目录的拒绝理由冒充不了' $true (((& $verdict 'neg-sentinel-missing' 'REFUSED' $msgBare).Status -eq 'PASS') -and ((& $verdict 'neg-sentinel-missing' 'REFUSED' $msgDefault).Status -eq 'INCOMPLETE'))
    & $add '反例判定（真实的 scenarios.json）：默认资源目录的守卫反例，拒绝的原因是「永不碰」才是 PASS；缺哨兵的拒绝理由冒充不了' $true (((& $verdict 'neg-default-dir' 'REFUSED' $msgDefault).Status -eq 'PASS') -and ((& $verdict 'neg-default-dir' 'REFUSED' $msgBare).Status -eq 'INCOMPLETE'))
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
    & $add '反例判定（真实的 scenarios.json）：USER_TESTING 的守卫反例要看到标记这条原因；因为别的缺项（没有 reaper.exe 等）凑出来的 SKIPPED 不算' $true (
        ((& $verdict 'neg-user-testing' 'SKIPPED' "launches=0；$($withMarker.Reasons -join '；')").Status -eq 'PASS') -and
        ((& $verdict 'neg-user-testing' 'SKIPPED' 'launches=0；找不到 reaper.exe；构建目录里没有 rinne-build.json（先用 gates 构建）').Status -eq 'INCOMPLETE'))
    Remove-Item -LiteralPath $marker
    # 接线：看语法树里的命令调用，不比文本。入口里反例这一行必须经 Invoke-ReaperNegativeRow（结局加原因一起比）；自测入口要在建 scratch 目录之前先清 subst 残留
    $callsIn = {
        param($file, $functionName)
        $parsed = [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$null, [ref]$null)
        $fn = $parsed.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName }, $true)
        @($fn.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { [pscustomobject]@{ Name = $_.GetCommandName(); At = $_.Extent.StartOffset } })
    }
    $smokeCalls = (& $callsIn (Join-Path $script:Scripts 'Suite.psm1') 'Invoke-ReaperSmoke').Name
    & $add '接线：入口 Invoke-ReaperSmoke 的反例这一行经 Invoke-ReaperNegativeRow 判定，不在入口里自己跑守卫、自己比结局' $false (('Invoke-ReaperNegativeRow' -in $smokeCalls) -and ('Invoke-GuardNegative' -notin $smokeCalls))
    $selfCalls = & $callsIn (Join-Path $PSScriptRoot 'SelfTest.psm1') 'Invoke-ReaperSelfTest'
    $clearAt = @($selfCalls | Where-Object { $_.Name -eq 'Clear-ReaperSelftestSubst' } | ForEach-Object { $_.At })
    $makeAt = @($selfCalls | Where-Object { $_.Name -eq 'New-RinneTempDir' } | ForEach-Object { $_.At })
    & $add '接线：自测入口在建 scratch 目录之前先清 subst 残留' $false ($clearAt.Count -eq 1 -and $makeAt.Count -eq 1 -and $clearAt[0] -lt $makeAt[0])
    # 加载来源 1：vstpath64 的新值（纯函数，表驱动）。REAPER 对同名的 VST3 包只留扫描顺序里最后的那个，私有目录必须排在最后，系统 VST3 目录显式写在它前面
    $sysDirs = '%COMMONPROGRAMFILES%\VST3;%LOCALAPPDATA%\Programs\Common\VST3'
    $privateDir = 'C:\cfg\plugins'
    $commonExpanded = [Environment]::ExpandEnvironmentVariables('%COMMONPROGRAMFILES%') + '\VST3'
    $pathTable = @(
        @{ Name = '没有 vstpath64 这一行（当前值为空）'; Current = ''; Want = "$sysDirs;$privateDir" }
        @{ Name = '只有私有目录'; Current = $privateDir; Want = "$sysDirs;$privateDir" }
        @{ Name = '已经是新顺序（重复安装）原样不动'; Current = "$sysDirs;$privateDir"; Want = "$sysDirs;$privateDir" }
        @{ Name = '私有目录大小写写法不同，换成给定的写法并排到最后'; Current = "c:\CFG\Plugins;$sysDirs"; Want = "$sysDirs;$privateDir" }
        @{ Name = '私有目录带结尾反斜杠'; Current = "$privateDir\;$sysDirs"; Want = "$sysDirs;$privateDir" }
        @{ Name = '私有目录用正斜杠并带结尾斜杠'; Current = 'C:/cfg/plugins/'; Want = "$sysDirs;$privateDir" }
        @{ Name = '私有目录夹在两个系统目录中间'; Current = '%COMMONPROGRAMFILES%\VST3;C:\cfg\plugins;%LOCALAPPDATA%\Programs\Common\VST3'; Want = "$sysDirs;$privateDir" }
        @{ Name = '别的目录原样保留、相对顺序不变'; Current = "D:\mine;$privateDir;E:\other"; Want = "D:\mine;E:\other;$sysDirs;$privateDir" }
        @{ Name = '系统目录已按展开后的写法写过，不再追加另一种写法'; Current = "$commonExpanded;$privateDir"; Want = "$commonExpanded;%LOCALAPPDATA%\Programs\Common\VST3;$privateDir" }
        @{ Name = '重复项与空段只留一份'; Current = "D:\mine;;d:\MINE\;$privateDir;"; Want = "D:\mine;$sysDirs;$privateDir" }
    )
    foreach ($tableRow in $pathTable) {
        $pathGot = Get-ReaperPluginPathValue -Current $tableRow.Current -Private $privateDir
        & $add "vstpath64：$($tableRow.Name)" $false ($pathGot -ceq $tableRow.Want)
        & $add "vstpath64：再算一遍结果不变，最后一段是私有目录、私有目录只出现一次（$($tableRow.Name)）" $false (
            ((Get-ReaperPluginPathValue -Current $pathGot -Private $privateDir) -ceq $pathGot) -and ($pathGot.Split(';')[-1] -ceq $privateDir) -and (@($pathGot.Split(';') | Where-Object { $_ -ieq $privateDir }).Count -eq 1))
    }
    $oldLayout = Get-ReaperPluginPathValue -Current "$privateDir;$sysDirs" -Private $privateDir
    & $add 'vstpath64：旧写法（私有目录排在最前）必须被改写，不能原样放行' $true ($oldLayout -cne "$privateDir;$sysDirs" -and $oldLayout.Split(';')[0] -cne $privateDir)

    # 加载来源 1b：扫描缓存里被测插件的条目要去掉。缓存里留着系统目录发布版的时间戳时，REAPER 不管 vstpath64 怎么排都用发布版（真机上见过），
    # 而且赢一次就一直赢；所以装插件时清掉被测插件的条目，让 REAPER 重扫，由私有目录排在最后来定
    $cacheNames = 'Rinne Input.vst3', 'Rinne Output.vst3'
    $cacheOther = "Other.vst3=0102030405060708,11{FF,Other plugin`n"
    $cacheStale = "[vstcache]`nRinne_Output.vst3=525EE8DD736ADD01,1668839456{AB,Rinne Output (Synchain)`n" + $cacheOther + "Rinne_Input.vst3=BCE5AEDD736ADD01,1769505170{AB,Rinne Input (Synchain)`n"
    $cacheCut = Remove-ReaperScanCacheEntries -Text $cacheStale -FileNames $cacheNames
    & $add '扫描缓存：键是文件名里除字母、数字、点以外的字符换成 _' $false (
        (Get-ReaperScanCacheKey 'Rinne Input.vst3') -ceq 'Rinne_Input.vst3' -and (Get-ReaperScanCacheKey 'a-b (c).vst3') -ceq 'a_b__c_.vst3' -and $null -eq (Get-ReaperScanCacheKey ('插件' + '.vst3')))
    & $add '扫描缓存：留着发布版时间戳的两个被测条目都被去掉，别的条目一字不动，记下去掉 2 行' $true (
        $cacheCut.Text -ceq ("[vstcache]`n" + $cacheOther) -and $cacheCut.Removed -eq 2 -and $cacheCut.Unkeyed -eq 0)
    & $add '扫描缓存：再清一遍不再有变化（去掉 0 行，文本原样）' $false ((Remove-ReaperScanCacheEntries -Text $cacheCut.Text -FileNames $cacheNames) | ForEach-Object { $_.Removed -eq 0 -and $_.Text -ceq $cacheCut.Text })
    & $add '扫描缓存：外壳插件的子条目（键<序号=）跟着去掉，键只是前缀相同的别的文件不动' $true (
        (Remove-ReaperScanCacheEntries -Text "[vstcache]`nRinne_Input.vst3=1,1{A,x`nRinne_Input.vst3<1=2,2{B,y`nRinne_Input.vst3.bak=3,3{C,z`nRinne_Input2.vst3=4,4{D,w`n" -FileNames $cacheNames).Text -ceq "[vstcache]`nRinne_Input.vst3.bak=3,3{C,z`nRinne_Input2.vst3=4,4{D,w`n")
    & $add '扫描缓存：键的大小写不同也认（Windows 文件名不分大小写）' $true (
        (Remove-ReaperScanCacheEntries -Text "[vstcache]`nrinne_input.vst3=1,1{A,x`n" -FileNames $cacheNames).Removed -eq 1)
    & $add '扫描缓存：只动 [vstcache] 一节，别的节里同名的键不动' $true (
        (Remove-ReaperScanCacheEntries -Text "[other]`nRinne_Input.vst3=a`n[vstcache]`nRinne_Input.vst3=1,1{A,x`n[xvst3_compat]`nRinne_Input.vst3=b`n" -FileNames $cacheNames).Text -ceq "[other]`nRinne_Input.vst3=a`n[vstcache]`n[xvst3_compat]`nRinne_Input.vst3=b`n")
    $cacheCrlf = Remove-ReaperScanCacheEntries -Text $cacheStale.Replace("`n", "`r`n") -FileNames $cacheNames
    & $add '扫描缓存：CRLF 换行的缓存去掉条目后仍是 CRLF，别的行不变' $true ($cacheCrlf.Text -ceq ("[vstcache]`n" + $cacheOther).Replace("`n", "`r`n") -and $cacheCrlf.Removed -eq 2)
    & $add '扫描缓存：条目在最后一行且没有结尾换行也能去掉，前一行的换行保留' $true (
        (Remove-ReaperScanCacheEntries -Text "[vstcache]`nOther.vst3=1,1{A,x`nRinne_Input.vst3=1,1{A,x" -FileNames $cacheNames).Text -ceq "[vstcache]`nOther.vst3=1,1{A,x`n")
    & $add '扫描缓存：没有 [vstcache] 一节、空文本、没有文件名，都原样返回、去掉 0 行' $false (
        ((Remove-ReaperScanCacheEntries -Text "[x]`nRinne_Input.vst3=1`n" -FileNames $cacheNames) | ForEach-Object { $_.Removed -eq 0 -and $_.Text -ceq "[x]`nRinne_Input.vst3=1`n" }) -and
        ((Remove-ReaperScanCacheEntries -Text '' -FileNames $cacheNames) | ForEach-Object { $_.Removed -eq 0 -and $_.Text -ceq '' }) -and
        ((Remove-ReaperScanCacheEntries -Text $cacheStale -FileNames @()) | ForEach-Object { $_.Removed -eq 0 -and $_.Text -ceq $cacheStale }))
    $cacheWide = Remove-ReaperScanCacheEntries -Text $cacheStale -FileNames @('插件.vst3')
    & $add '扫描缓存：文件名含非 ASCII 字符时没有已知的键，记进 Unkeyed、不猜着删' $true ($cacheWide.Unkeyed -eq 1 -and $cacheWide.Removed -eq 0 -and $cacheWide.Text -ceq $cacheStale)

    # 加载来源 2：被测的 .vst3 模块是不是都从私有目录里这次复制进去的那份加载的。纯函数，同路径比较与 sha256 用注入的替身
    $inPath = 'C:\cfg\plugins\Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3'
    $outPath = 'C:\cfg\plugins\Rinne Output.vst3\Contents\x86_64-win\Rinne Output.vst3'
    $sysInPath = 'C:\Program Files\Common Files\VST3\Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3'
    $verdictWant = @{ 'Rinne Input.vst3' = @{ Path = $inPath; Sha256 = ('a' * 64) }; 'Rinne Output.vst3' = @{ Path = $outPath; Sha256 = ('b' * 64) } }
    $liveHashes = @{ $inPath = ('a' * 64); $outPath = ('b' * 64) }
    $sameFake = { param($first, $second) [string]::Equals($first, $second, [StringComparison]::OrdinalIgnoreCase) }
    $hashFake = { param($file) $liveHashes.ContainsKey($file) ? $liveHashes[$file] : $null }.GetNewClosure()
    $hashDrifted = { param($file) 'c' * 64 }
    $hashUnreadable = { param($file) $null }
    $modVerdict = { param($seen, $scans = 3, $hashOf = $hashFake, $want = $verdictWant) Get-ReaperModuleVerdict -Expected $want -Seen $seen -Scans $scans -SameFile $sameFake -HashOf $hashOf }
    $seenGood = @{ 'Rinne Input.vst3' = @($inPath); 'Rinne Output.vst3' = @($outPath) }
    $good = & $modVerdict $seenGood
    & $add '加载来源：两个插件都只从私有目录加载、sha256 对得上才是 Proven，没有原因' $false (
        $good.Verdict -ceq 'Proven' -and $good.Reasons.Count -eq 0 -and $good.Binaries['Rinne Input.vst3'].State -ceq 'Private' -and $good.Binaries['Rinne Output.vst3'].State -ceq 'Private')
    & $add '加载来源：路径写法只差大小写（同一个文件）仍是 Proven，用同文件判定而不是比字符串' $false ((& $modVerdict @{ 'Rinne Input.vst3' = @($inPath.ToUpperInvariant()); 'Rinne Output.vst3' = @($outPath) }).Verdict -ceq 'Proven')
    $foreign = & $modVerdict @{ 'Rinne Input.vst3' = @($sysInPath); 'Rinne Output.vst3' = @($outPath) }
    & $add '加载来源：Input 是从系统 VST3 目录加载的同名模块，判 Foreign 并点名' $true (
        $foreign.Verdict -ceq 'Foreign' -and $foreign.Binaries['Rinne Input.vst3'].State -ceq 'Foreign' -and ($foreign.Reasons -match 'Rinne Input.vst3 从被测副本以外'))
    & $add '加载来源：私有副本也见到了、但同名的别处模块同样加载过，仍判 Foreign' $true ((& $modVerdict @{ 'Rinne Input.vst3' = @($inPath, $sysInPath); 'Rinne Output.vst3' = @($outPath) }).Verdict -ceq 'Foreign')
    $unseen = & $modVerdict @{}
    & $add '加载来源：一个被测模块都没见到，判 Unseen 并点名两个' $true ($unseen.Verdict -ceq 'Unseen' -and @($unseen.Reasons -match '没有在进程的模块列表里见到').Count -eq 2)
    & $add '加载来源：只见到 Input 没见到 Output，判 Unseen' $true ((& $modVerdict @{ 'Rinne Input.vst3' = @($inPath) }).Verdict -ceq 'Unseen')
    & $add '加载来源：见到的是别的模块（同名以外的文件）不算见到被测模块，判 Unseen' $true ((& $modVerdict @{ 'Rinne Input.vst3' = @(); 'Rinne Output.vst3' = @() }).Verdict -ceq 'Unseen')
    $drifted = & $modVerdict $seenGood 3 $hashDrifted
    & $add '加载来源：路径对、但文件现在的 sha256 与复制时不同，判 Changed' $true ($drifted.Verdict -ceq 'Changed' -and $drifted.Binaries['Rinne Input.vst3'].State -ceq 'Changed' -and ($drifted.Reasons -match 'sha256'))
    & $add '加载来源：路径对、但文件读不出来（算不出 sha256），判 Changed，不当作对得上' $true ((& $modVerdict $seenGood 3 $hashUnreadable).Verdict -ceq 'Changed')
    $unreadable = & $modVerdict $seenGood 0
    & $add '加载来源：一次也没读到模块列表（Scans=0）判 Unreadable，即便 Seen 看着是对的' $true ($unreadable.Verdict -ceq 'Unreadable' -and ($unreadable.Reasons -match '一次也没读到'))
    & $add '加载来源：没有被测副本的清单判 NoExpected，不判 Proven' $true ((& $modVerdict @{} 3 $hashFake @{}).Verdict -ceq 'NoExpected')
    & $add '加载来源：Foreign 排在 Unseen 之前（一个别处加载、一个没见到，整体判 Foreign）' $true ((& $modVerdict @{ 'Rinne Input.vst3' = @($sysInPath) }).Verdict -ceq 'Foreign')

    # 同一个文件的判定与 sha256 用真文件：路径只差大小写、硬链接是同一个文件，复制出来的另一份不是
    $sameDir = Join-Path $Tmp 'same-file'
    $null = New-Item -ItemType Directory -Path $sameDir
    $fileA = Join-Path $sameDir 'a.bin'; $fileCopy = Join-Path $sameDir 'copy.bin'; $fileLink = Join-Path $sameDir 'link.bin'
    [IO.File]::WriteAllText($fileA, 'same bytes')
    Copy-Item -LiteralPath $fileA -Destination $fileCopy
    $null = New-Item -ItemType HardLink -Path $fileLink -Target $fileA
    & $add '同一个文件：路径只差大小写是同一个' $false (Test-ReaperSameFile $fileA $fileA.ToUpperInvariant())
    & $add '同一个文件：硬链接指向同一个文件' $false (Test-ReaperSameFile $fileA $fileLink)
    & $add '同一个文件：内容相同的另一份拷贝不是同一个' $true (-not (Test-ReaperSameFile $fileA $fileCopy))
    & $add '同一个文件：有一边不存在、或为空，不算同一个' $true (-not (Test-ReaperSameFile $fileA (Join-Path $sameDir 'nope.bin')) -and -not (Test-ReaperSameFile $fileA '') -and -not (Test-ReaperSameFile $null $fileA))
    $goneFile = Join-Path $sameDir 'gone.bin'
    & $add '同一个文件：文件已经不存在时（读不出文件身份），完整路径只差大小写仍认作同一个，差一个字符不认' $false ((Test-ReaperSameFile $goneFile $goneFile.ToUpperInvariant()) -and -not (Test-ReaperSameFile $goneFile ($goneFile + 'x')))
    & $add 'sha256：与 Get-FileHash 一致（小写），读不出的文件返回 $null' $false (
        ((Get-ReaperFileSha256 $fileA) -ceq (Get-FileHash -LiteralPath $fileA -Algorithm SHA256).Hash.ToLowerInvariant()) -and $null -eq (Get-ReaperFileSha256 (Join-Path $sameDir 'nope.bin')))

    # 结果里写的模块位置：私有目录、系统 VST3 目录用占位前缀，别处只留文件名，盘符路径与用户名目录不进结果
    $shownPlugins = Join-Path $Tmp 'shown-plugins'
    $shownLeaf = 'Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3'
    & $add '模块位置：私有插件目录下的写成 <cfg>\plugins\…' $false ((ConvertTo-ReaperModuleShown (Join-Path $shownPlugins $shownLeaf) $shownPlugins) -ceq "<cfg>\plugins\$shownLeaf")
    & $add '模块位置：系统 VST3 目录下的写成 <CommonProgramFiles>\…' $false ((ConvertTo-ReaperModuleShown (Join-Path $env:CommonProgramFiles "VST3\$shownLeaf") $shownPlugins) -ceq "<CommonProgramFiles>\VST3\$shownLeaf")
    $localRoot = [Environment]::GetFolderPath('LocalApplicationData')
    & $add '模块位置：本机用户的 VST3 目录下的写成 <LocalProfile>\…，不带用户名' $false ((ConvertTo-ReaperModuleShown (Join-Path $localRoot "Programs\Common\VST3\$shownLeaf") $shownPlugins) -ceq "<LocalProfile>\Programs\Common\VST3\$shownLeaf")
    $elsewhere = ConvertTo-ReaperModuleShown 'D:\work\someone\build\Rinne Input.vst3' $shownPlugins
    & $add '模块位置：别处的只留文件名，不留盘符路径与用户名' $true ($elsewhere -ceq '<其他目录>\Rinne Input.vst3' -and $elsewhere -notmatch 'someone|D:')

    # 场景处理完后核对加载来源（Add-ReaperModuleEvidence）：用真文件，路径与 sha256 都按真的算
    $evPlugins = Join-Path $Tmp 'ev-plugins'
    $evFiles = @{}
    foreach ($evLeaf in 'Rinne Input.vst3', 'Rinne Output.vst3') {
        $evFile = Join-Path $evPlugins (Join-Path $evLeaf "Contents\x86_64-win\$evLeaf")
        $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $evFile)
        [IO.File]::WriteAllText($evFile, "binary of $evLeaf")
        $evFiles[$evLeaf] = @{ Path = $evFile; Sha256 = (Get-FileHash -LiteralPath $evFile -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $evElsewhere = Join-Path $Tmp 'ev-elsewhere\Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3'
    $evSeenGood = @{ 'Rinne Input.vst3' = @($evFiles['Rinne Input.vst3'].Path); 'Rinne Output.vst3' = @($evFiles['Rinne Output.vst3'].Path) }
    $evLaunch = { param($seen, $scans = 3, $loads = $true, $status = 'Exited') [pscustomobject]@{ Label = 'ev'; LoadsPlugins = $loads; Status = $status; Modules = $seen; ModuleScans = $scans } }
    $evRun = {
        param($launches, $status = 'PASS', $from = 0, $facts = $null, $installed = $evFiles)
        $evLog = [Collections.Generic.List[object]]::new()
        foreach ($oneLaunch in @($launches)) { $evLog.Add($oneLaunch) }
        $evOutcome = [pscustomobject]@{ Status = $status; Detail = '场景自己的说明' }
        if ($facts) { $evOutcome | Add-Member -NotePropertyName Facts -NotePropertyValue $facts }
        Add-ReaperModuleEvidence -Ctx @{ Installed = $installed; PluginsDir = $evPlugins; LaunchLog = $evLog } -Outcome $evOutcome -From $from
    }
    $addSafe = { param($Label, $Bad, [scriptblock]$Check) $ok = $false; try { $ok = [bool](& $Check) } catch { $ok = $false }; & $add $Label $Bad $ok } # 取不到 Facts 之类的成员时算这一条没过，不让整个自测抛错
    $evGood = & $evRun (& $evLaunch $evSeenGood)
    & $addSafe '加载来源核对：来源对得上，场景的 PASS 原样保留，说明不改，证据写进 Facts.modules' $false {
        $evGood.Status -ceq 'PASS' -and $evGood.Detail -ceq '场景自己的说明' -and $evGood.Facts.modules.Count -eq 1 -and $evGood.Facts.modules[0].verdict -ceq 'Proven' -and
        $evGood.Facts.modules[0].binaries['Rinne Input.vst3'].state -ceq 'Private' -and $evGood.Facts.modules[0].binaries['Rinne Input.vst3'].sha256 -ceq $evFiles['Rinne Input.vst3'].Sha256.Substring(0, 12)}
    & $addSafe '加载来源核对：证据里的位置是 <cfg>\plugins\…，没有盘符路径' $false {
        $evGood.Facts.modules[0].binaries['Rinne Input.vst3'].loaded[0] -like '<cfg>\plugins\Rinne Input.vst3\*' -and (ConvertTo-Json $evGood.Facts -Depth 8) -notmatch '[A-Za-z]:\\\\'}
    $evForeign = & $evRun (& $evLaunch @{ 'Rinne Input.vst3' = @($evElsewhere); 'Rinne Output.vst3' = @($evFiles['Rinne Output.vst3'].Path) })
    & $addSafe '加载来源核对：Input 是别处的同名模块，场景的 PASS 改判 INCOMPLETE，原因点名，场景自己的说明保留' $true {
        $evForeign.Status -ceq 'INCOMPLETE' -and $evForeign.Detail -match '插件加载来源没核实' -and $evForeign.Detail -match '被测副本以外' -and $evForeign.Detail -match '场景自己的说明' -and $evForeign.Facts.modules[0].verdict -ceq 'Foreign'}
    & $addSafe '加载来源核对：别处的同名模块在结果里不带盘符路径' $false {(ConvertTo-Json $evForeign.Facts -Depth 8) -notmatch '[A-Za-z]:\\\\'}
    & $add '加载来源核对：场景的 FAIL 同样改判 INCOMPLETE（测的可能是另一份构建，红的原因不一定在被测副本上）' $true ((& $evRun (& $evLaunch @{ 'Rinne Input.vst3' = @($evElsewhere); 'Rinne Output.vst3' = @($evFiles['Rinne Output.vst3'].Path) }) 'FAIL').Status -ceq 'INCOMPLETE')
    $evUnseen = & $evRun (& $evLaunch @{})
    & $addSafe '加载来源核对：一个被测模块都没见到，场景的 PASS 改判 INCOMPLETE' $true {$evUnseen.Status -ceq 'INCOMPLETE' -and $evUnseen.Detail -match '没有在进程的模块列表里见到' -and $evUnseen.Facts.modules[0].verdict -ceq 'Unseen'}
    $evWrongSha = @{ 'Rinne Input.vst3' = @{ Path = $evFiles['Rinne Input.vst3'].Path; Sha256 = ('f' * 64) }; 'Rinne Output.vst3' = $evFiles['Rinne Output.vst3'] }
    $evChanged = & $evRun (& $evLaunch $evSeenGood) 'PASS' 0 $null $evWrongSha
    & $addSafe '加载来源核对：路径对、但 sha256 与复制时算的不同，场景的 PASS 改判 INCOMPLETE' $true {$evChanged.Status -ceq 'INCOMPLETE' -and $evChanged.Facts.modules[0].verdict -ceq 'Changed'}
    & $add '加载来源核对：读不到进程的模块列表（ModuleScans=0），场景的 PASS 改判 INCOMPLETE' $true ((& $evRun (& $evLaunch $evSeenGood 0)).Status -ceq 'INCOMPLETE')
    $evNoLaunch = & $evRun @()
    & $add '加载来源核对：这个场景里没有一次会加载插件的启动被核对，场景的 PASS 改判 INCOMPLETE（没有证明）' $true ($evNoLaunch.Status -ceq 'INCOMPLETE' -and $evNoLaunch.Detail -match '没有一次会加载插件')
    & $add '加载来源核对：只有不加载插件的启动（参照渲染）不算证明过，改判 INCOMPLETE' $true ((& $evRun (& $evLaunch @{} 0 $false)).Status -ceq 'INCOMPLETE')
    & $add '加载来源核对：不加载插件的参照渲染在前、加载插件的启动对得上在后，场景的 PASS 保留' $false ((& $evRun @((& $evLaunch @{} 0 $false), (& $evLaunch $evSeenGood))).Status -ceq 'PASS')
    & $add '加载来源核对：两次加载插件的启动，第二次是别处的模块，整个场景改判 INCOMPLETE' $true ((& $evRun @((& $evLaunch $evSeenGood), (& $evLaunch @{ 'Rinne Input.vst3' = @($evElsewhere); 'Rinne Output.vst3' = @($evFiles['Rinne Output.vst3'].Path) }))).Status -ceq 'INCOMPLETE')
    & $add '加载来源核对：锁等不到（LockBusy，REAPER 没有启动）的那次不计入，不拖累其余启动的核对' $false ((& $evRun @((& $evLaunch $evSeenGood), (& $evLaunch @{} 0 $true 'LockBusy'))).Status -ceq 'PASS')
    & $add '加载来源核对：只看本场景开始之后的启动（From），之前场景里的坏启动不连累' $false ((& $evRun @((& $evLaunch @{} 0), (& $evLaunch $evSeenGood)) 'PASS' 1).Status -ceq 'PASS')
    $evOwn = & $evRun (& $evLaunch @{ 'Rinne Input.vst3' = @($evElsewhere); 'Rinne Output.vst3' = @($evFiles['Rinne Output.vst3'].Path) }) 'INCOMPLETE'
    & $addSafe '加载来源核对：场景自己已经是 INCOMPLETE 的不改它的说明（不盖掉超时、卡对话框这些自己的原因），但证据照写' $false {$evOwn.Status -ceq 'INCOMPLETE' -and $evOwn.Detail -ceq '场景自己的说明' -and $evOwn.Facts.modules[0].verdict -ceq 'Foreign'}
    $evKeep = & $evRun (& $evLaunch $evSeenGood) 'PASS' 0 ([ordered]@{ other = 1 })
    & $addSafe '加载来源核对：场景已有的 Facts 保留，modules 加在旁边' $false {$evKeep.Facts.other -eq 1 -and $evKeep.Facts.modules.Count -eq 1}
    # 接线：不带插件链的参照渲染必须声明「不加载插件」（-LoadsPlugins $false），别的渲染调用不能带；否则参照渲染会被当成加载被测插件的启动，
    # 每个渲染场景都因为「没见到被测模块」判 INCOMPLETE。看语法树里的命令调用，不比文本
    $scenarioFile = Join-Path $script:Scripts 'Scenarios.psm1'
    $scenarioAst = [System.Management.Automation.Language.Parser]::ParseFile($scenarioFile, [ref]$null, [ref]$null)
    $renderCalls = @($scenarioAst.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Invoke-RenderProject' }, $true))
    $referenceFn = $scenarioAst.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-RenderReference' }, $true)
    $loadsArgs = foreach ($call in $renderCalls) {
        $elements = $call.CommandElements
        for ($at = 0; $at -lt $elements.Count - 1; $at++) {
            if ($elements[$at] -is [System.Management.Automation.Language.CommandParameterAst] -and $elements[$at].ParameterName -eq 'LoadsPlugins') {
                [pscustomobject]@{ Value = $elements[$at + 1].Extent.Text; Inside = ($call.Extent.StartOffset -ge $referenceFn.Extent.StartOffset -and $call.Extent.EndOffset -le $referenceFn.Extent.EndOffset) }
            }
        }
    }
    & $add '接线：参照渲染（Get-RenderReference）的 Invoke-RenderProject 带 -LoadsPlugins $false，且只有这一处声明不加载插件' $false (
        $renderCalls.Count -ge 3 -and @($loadsArgs).Count -eq 1 -and @($loadsArgs)[0].Value -ceq '$false' -and @($loadsArgs)[0].Inside)
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
    # 上一次自测若被硬结束，会留下指向已清掉的临时目录的 subst 盘符：先清（只清指向自测 scratch 目录的，别的映射不碰），清了什么写进本机日志
    foreach ($stale in (Clear-ReaperSelftestSubst)) {
        Write-Host ('  开头清理上次自测残留的 subst 映射 {0}: -> {1}{2}' -f $stale.Letter, $stale.Target, ($stale.Cleared ? '' : '（没能清掉）'))
    }
    $scratch = New-RinneTempDir -Prefix $script:ScratchPrefix
    $savedRoaming = $env:APPDATA
    $cases = [Collections.Generic.List[object]]::new()
    try {
        $fakeRoam = Join-Path $scratch 'Roaming-Fake-AppData'
        $null = New-Item -ItemType Directory -Path (Join-Path $fakeRoam 'REAPER')
        $env:APPDATA = $fakeRoam # 之后所有「默认资源目录」都指向这个假的；子进程继承
        foreach ($group in @{ Fn = { Get-GuardCases $scratch } }, @{ Fn = { Get-SubstResidueCases $scratch } }, @{ Fn = { Get-IniCases } }, @{ Fn = { Get-SnapshotCases $scratch } },
                           @{ Fn = { Get-LintCases $scratch $RepoRoot $CollisionSample } }, @{ Fn = { Get-PureCases $scratch $RepoRoot } }, @{ Fn = { Get-ProbeSelfTestCases -Tmp $scratch -RepoRoot $RepoRoot } },
                           @{ Fn = { Get-ChildCases $scratch $RepoRoot } }) {
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
