# SPDX-License-Identifier: GPL-3.0-or-later
#
# 合并后构建的锁存标记：pr-merge 写，维护者脚本与 gates 查。合并后构建跑的是 PR 树里的代码，它写得到主 clone 共享的 .git
# （配置、hooks、忽略文件）与主 clone 的工作树；改过的 git 配置可能带着会执行代码的项，之后任何一次 git 都会执行它。
# - pr-merge 在构建之前写「进行中」标记（PR、时间、pid、构建前指纹摘要、一次性随机值），写不进去就不构建；构建期间一直握着它
#   （只放行读：Windows 上构建删不掉、改不了、也换不成目录）。构建后的核对全部通过、标记内容也与写入时一字不差，才删掉它；
#   有一项不过就经握着的句柄改写成「已篡改」。父进程被结束、核对之前抛错，标记都留着。
# - 标记在时，导入维护者模块（maintainer/lib/RinneGh.psm1，全部包装脚本都导入它）与 gates.ps1 开头就拒绝，之后一条 git、gh
#   都不跑，直到维护者核对主 clone 与共享 .git 之后手动删掉。构建期间标记也在，别的 worktree 里的包装脚本与 gates 同样被拒，
#   要等构建结束。
# - 位置只从调用方脚本或模块文件自己的路径推，不读环境变量与 git config（构建改得了它们）：仓根的 .git 是目录，仓根就是主
#   clone；是文件（worktree），就按其中的 gitdir 与那里的 commondir 找到主 clone 的 .git，gitdir 必须是 <主 clone>/.git/worktrees/
#   下的一个目录，否则报错（当作锁存）。标记在主 clone 的上一级（工作区根），在主 clone 与 wt/ 之外。
#   那个位置上有任何东西（文件、目录、隐藏的都算）、或者连属性都读不了，都算锁存：构建伪造一个只会造成拒绝服务。
# 边界：这是尽力而为，不是隔离。锁存防的是「构建期间被篡改之后，后续调用在不知情下继续跑 git」；同一用户身份下的后台进程
# 可以在 pr-merge 退出之后直接删掉标记，也可以改 wt/ 下各 worktree 里的脚本（包括这份检查），这一点挡不住；直接敲的 git
# 命令、单独跑的 build.ps1 与 node 检查也不经过这里。别的平台上握着句柄不妨碍删除，只剩内容核对。彻底的做法是独立 clone，
# 或者换一个账户、容器来跑构建。
#   Assert-RinneNoLatch [-RepoRoot <仓根>]    标记在就抛错（消息以「已锁存」开头）
#   Test-PremergeLatch                        自测用例（gates 的自测调用）

Set-StrictMode -Version Latest

$script:Override = $null
$script:Hold = $true
$script:LatchName = '.rinne-premerge-latch.json'
$script:ModuleRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

<# 自测把标记换到一个临时位置；传空还原。 #>
function Set-RinneLatchOverride { param([string]$Path) $script:Override = $Path ? $Path : $null }

<# 自测用：构建期间不握着标记（模拟握不住它的平台），好验证后面的内容核对。 #>
function Set-RinneLatchHold { param([bool]$On) $script:Hold = $On }

<#
  标记的位置：<主 clone 的上一级>/.rinne-premerge-latch.json。RepoRoot 缺省是本模块所在的仓。没有 .git 时按主 clone 算；
  .git 文件读不懂、gitdir 不存在或不在 <主 clone>/.git/worktrees/ 下时抛错（调用方当作锁存）。
#>
function Get-RinneLatchPath {
    param([string]$RepoRoot = $script:ModuleRepo)
    if ($script:Override) { return $script:Override }
    $main = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/')
    $dotgit = Join-Path $main '.git'
    if (Test-Path -LiteralPath $dotgit -PathType Leaf) {
        $line = "$(@(Get-Content -LiteralPath $dotgit -TotalCount 1)[0])"
        if ($line -notmatch '^gitdir:\s*(\S.*?)\s*$') { throw "$dotgit 不是 gitdir 文件，推不出锁存标记的位置" }
        # gitdir 可以是相对路径（相对于 worktree 根），commondir 相对于 gitdir；git worktree add 写的 commondir 是 ../..
        $gitdir = [IO.Path]::GetFullPath($Matches[1], $main).TrimEnd('\', '/')
        $cd = Join-Path $gitdir 'commondir'
        if (-not (Test-Path -LiteralPath $cd -PathType Leaf)) { throw "$dotgit 的 gitdir 不存在或不是 worktree 的管理目录（$gitdir），推不出锁存标记的位置" }
        $common = [IO.Path]::GetFullPath("$(@(Get-Content -LiteralPath $cd -TotalCount 1)[0])".Trim(), $gitdir).TrimEnd('\', '/')
        $under = [IO.Path]::GetFullPath((Split-Path -Parent $gitdir)).TrimEnd('\', '/') -ieq (Join-Path $common 'worktrees')
        if ((Split-Path -Leaf $common) -ne '.git' -or -not (Test-Path -LiteralPath $common -PathType Container) -or -not $under) {
            throw "$dotgit 的 gitdir 不在 <主 clone>/.git/worktrees/ 下（$gitdir），推不出锁存标记的位置"
        }
        $main = Split-Path -Parent $common
    }
    Join-Path (Split-Path -Parent $main) $script:LatchName
}

<# 那个位置上有没有东西：不存在才算没有；有文件、目录，或者读不了属性（权限、路径坏了），都算有。 #>
function Test-RinneLatchPresent {
    param([Parameter(Mandatory)][string]$Path)
    try { $null = [IO.File]::GetAttributes($Path); $true }
    catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] { $false }
    catch { $true }
}

<# 读标记的原文：与 pr-merge 握着的句柄兼容（它只放行读）。读不出来就抛错。 #>
function Read-RinneLatchText {
    param([Parameter(Mandatory)][string]$Path)
    $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
    try { [IO.StreamReader]::new($fs, [Text.UTF8Encoding]::new($false)).ReadToEnd() } finally { $fs.Dispose() }
}

<# 给人看的一句：进行中（pid 还在不在）还是已篡改、哪个 PR、何时。内容是构建写得到的，只取认得出的字段、去掉控制字符。 #>
function Get-RinneLatchSummary {
    param([Parameter(Mandatory)][string]$Path)
    try {
        if (-not [IO.File]::Exists($Path)) { return '那个位置上是别的东西（目录之类）' }
        $j = Read-RinneLatchText $Path | ConvertFrom-Json -AsHashtable
        $pr = [int]$j['pr']; $procId = [int]$j['pid']
        $at = ([string]$j['at']) -replace '[^0-9A-Za-z:.+-]', ''
        $at = $at.Substring(0, [Math]::Min(40, $at.Length))
        switch ([string]$j['state']) {
            'building' {
                $alive = $procId -gt 0 -and $null -ne (Get-Process -Id $procId -ErrorAction SilentlyContinue)
                "合并后构建进行中或没有走完（PR #$pr，$at 起，pid $procId $(if ($alive) { '还在：等它结束' } else { '已不在：父进程没走到核对' })）"
            }
            'tampered' { "合并后构建改过主 clone 的判据文件（PR #$pr，$at）" }
            default { '内容认不出' }
        }
    } catch { '内容读不出来' }
}

<# 标记在就抛错：在任何 git、gh 之前调用。 #>
function Assert-RinneNoLatch {
    param([string]$RepoRoot = $script:ModuleRepo)
    $latch = Get-RinneLatchPath -RepoRoot $RepoRoot
    if (-not (Test-RinneLatchPresent $latch)) { return }
    throw "已锁存（$latch）：$(Get-RinneLatchSummary $latch)。一条 git、gh 都不跑；构建还在跑就等它结束，否则维护者核对主 clone、共享 .git 的配置、hooks 与忽略文件之后，手动删掉这个文件才放行"
}

<#
  构建之前写「进行中」标记。位置上已经有东西、或者写不进去，就抛错（调用方不跑构建；写了一半的文件留着，照样锁存）。
  返回 { Path; Pr; Text; Stream }：Text 是写进去的原文（核对时比它），Stream 是一直握着的句柄（Set-RinneLatchHold $false
  时为空）。用完必须 Clear-RinneLatch、Set-RinneLatchTampered 或 Close-RinneLatch 之一。
#>
function New-RinneLatch {
    param([Parameter(Mandatory)][int]$Pr, [Parameter(Mandatory)][string]$Digest, [string]$RepoRoot = $script:ModuleRepo)
    $path = Get-RinneLatchPath -RepoRoot $RepoRoot
    $text = [ordered]@{ state = 'building'; pr = $Pr; at = [datetime]::UtcNow.ToString('o'); pid = $PID; fingerprint = $Digest; nonce = [guid]::NewGuid().ToString('N') } |
        ConvertTo-Json -Compress
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
    $fs = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    try { $fs.Write($bytes, 0, $bytes.Length); $fs.Flush($true) } catch { $fs.Dispose(); throw }
    if (-not $script:Hold) { $fs.Dispose(); $fs = $null }
    [pscustomobject]@{ Path = $path; Pr = $Pr; Text = $text; Stream = $fs }
}

<# 标记还在、内容（含随机值）与写入时一字不差；不见了、换成了目录、读不出来都不算。 #>
function Test-RinneLatchIntact {
    param([Parameter(Mandatory)]$Latch)
    try { (Read-RinneLatchText $Latch.Path) -ceq $Latch.Text } catch { $false }
}

<# 放开句柄，标记文件留着（核对没走完时用）。 #>
function Close-RinneLatch {
    param([Parameter(Mandatory)]$Latch)
    if ($Latch.Stream) { $Latch.Stream.Dispose(); $Latch.Stream = $null }
}

<# 核对全部通过之后删标记；标记被动过（Test-RinneLatchIntact 不过）就不删、返回 $false，句柄照样握着，由调用方处理。 #>
function Clear-RinneLatch {
    param([Parameter(Mandatory)]$Latch)
    if (-not (Test-RinneLatchIntact $Latch)) { return $false }
    Close-RinneLatch $Latch
    [IO.File]::Delete($Latch.Path)
    $true
}

<#
  构建改了判据文件或动了标记：改写成「已篡改」并记下变了的项，然后放开句柄。握着句柄时经它写（根目录被设成不可写也写得进去）；
  没握着、经它写不进去、或者按路径读回来不是这份（别的平台上句柄可能指着已被删掉的旧文件），再按路径写。都写不进去时返回
  报错文字，写明位置上还有没有东西。
#>
function Set-RinneLatchTampered {
    param([Parameter(Mandatory)]$Latch, [string[]]$Changed = @())
    $text = [ordered]@{ state = 'tampered'; pr = $Latch.Pr; at = [datetime]::UtcNow.ToString('o'); changed = @($Changed) } | ConvertTo-Json -Depth 3
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
    if ($Latch.Stream) {
        try { $Latch.Stream.SetLength(0); $Latch.Stream.Write($bytes, 0, $bytes.Length); $Latch.Stream.Flush($true) } catch { $null = $_ }
        Close-RinneLatch $Latch
        try { if ((Read-RinneLatchText $Latch.Path) -ceq $text) { return $null } } catch { $null = $_ }
    }
    try { [IO.File]::WriteAllText($Latch.Path, $text, [Text.UTF8Encoding]::new($false)); $null }
    catch {
        $left = (Test-RinneLatchPresent $Latch.Path) ? '位置上的东西还在，照样锁存' : '位置上什么都没有，下一次调用看不到锁存'
        "锁存标记写不进去（$($_.Exception.Message.Trim())）：$left"
    }
}

<#
  自测：位置的推导（主 clone；worktree 的 gitdir 绝对、相对两种写法；gitdir 不存在、不在 .git/worktrees/ 下；RINNE_ROOT 与
  git config rinne.root 都指到别处也不影响）、有无的判断（文件、目录、隐藏文件、读不了属性都算）、进行中标记的写入、握着时
  删不掉改不了、核对清除、篡改时的改写；入口：子进程跑一份同样布局的副本，标记在时每个入口都在 git、gh 之前拒绝（PATH 最前面
  放假的 git、gh，被调用就留下记录；没有标记时同一个入口确实会调到它们，作对照）。返回 { Label; Bad; Passed }。
#>
function Test-PremergeLatch {
    # 与维护者脚本的自测同一写法：第三个参数是「判红了没有」，坏样例要判红、正例不能判红
    $check = { param($Label, $Bad, $Flagged) [pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = $(if ($Bad) { [bool]$Flagged } else { -not [bool]$Flagged }) } }
    $thrown = { param($sb) try { & $sb | Out-Null; '' } catch { $_.Exception.Message } }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "rinne-latch-selftest-$PID-$(Get-Random)"
    $saved = @{ Path = $env:PATH; Root = $env:RINNE_ROOT; LogDir = $env:RINNE_LOG_DIR; Override = $script:Override; Hold = $script:Hold }
    $held, $made = $null, $false
    try {
        # 不带 -Force：目录已经在（不是本进程建的）就抛错，finally 里也就不删它
        $null = New-Item -ItemType Directory -Path $tmp
        $made = $true
        $script:Override, $script:Hold = $null, $true
        # 工作区根的两种配置（环境变量与 git config rinne.root）都指到别处的诱饵目录，诱饵目录里另放一个标记
        $decoy = Join-Path $tmp 'decoy'
        $env:RINNE_ROOT = $decoy
        $ws = Join-Path $tmp 'ws'
        $main = Join-Path $ws 'rinne'
        $null = New-Item -ItemType Directory -Force -Path $main, $decoy
        $null = git init -q $main 2>&1
        $null = git -C $main config rinne.root ($decoy -replace '\\', '/') 2>&1
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $main '.git/worktrees/x'), (Join-Path $ws 'other/x')
        foreach ($w in 'x', 'y', 'z', 'n', 'o') { $null = New-Item -ItemType Directory -Force -Path (Join-Path $ws "wt/$w") }
        Set-Content -LiteralPath (Join-Path $main '.git/worktrees/x/commondir') -Value '../..'
        Set-Content -LiteralPath (Join-Path $ws 'other/x/commondir') -Value '../..'
        Set-Content -LiteralPath (Join-Path $ws 'wt/x/.git') -Value "gitdir: $((Join-Path $main '.git/worktrees/x') -replace '\\', '/')"
        Set-Content -LiteralPath (Join-Path $ws 'wt/y/.git') -Value 'gitdir: ../../rinne/.git/worktrees/x'
        Set-Content -LiteralPath (Join-Path $ws 'wt/z/.git') -Value 'not a gitdir line'
        Set-Content -LiteralPath (Join-Path $ws 'wt/n/.git') -Value "gitdir: $((Join-Path $main '.git/worktrees/missing') -replace '\\', '/')"
        Set-Content -LiteralPath (Join-Path $ws 'wt/o/.git') -Value "gitdir: $((Join-Path $ws 'other/x') -replace '\\', '/')"
        Set-Content -LiteralPath (Join-Path $decoy $script:LatchName) -Value '{}'
        $want = [IO.Path]::GetFullPath((Join-Path $ws $script:LatchName))
        & $check '位置：主 clone 的上一级（RINNE_ROOT 与 git config rinne.root 指到别处也不影响）' $false ((Get-RinneLatchPath -RepoRoot $main) -ne $want)
        & $check '位置：worktree 按 gitdir 与 commondir 找到主 clone（绝对、相对两种写法）' $false (
            (Get-RinneLatchPath -RepoRoot (Join-Path $ws 'wt/x')) -ne $want -or (Get-RinneLatchPath -RepoRoot (Join-Path $ws 'wt/y')) -ne $want)
        & $check '位置：.git 文件读不懂时抛错（当作锁存）' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot (Join-Path $ws 'wt/z') }) -match 'gitdir')
        & $check '位置：gitdir 指着不存在的目录时抛错' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot (Join-Path $ws 'wt/n') }) -match '不存在')
        & $check '位置：gitdir 不在 <主 clone>/.git/worktrees/ 下时抛错' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot (Join-Path $ws 'wt/o') }) -match 'worktrees')
        & $check '有无：没有标记时放行（诱饵目录里的标记不算）' $false ((& $thrown { Assert-RinneNoLatch -RepoRoot $main }) -ne '')
        Set-Content -LiteralPath $want -Value '{}'
        & $check '有无：标记在时拒绝（从 worktree 里查也一样）' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot (Join-Path $ws 'wt/x') }) -match '^已锁存')
        (Get-Item -LiteralPath $want).Attributes = 'Hidden'
        & $check '有无：隐藏的标记也算' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot $main }) -match '^已锁存')
        Remove-Item -LiteralPath $want -Force
        $null = New-Item -ItemType Directory -Path $want
        & $check '有无：那个位置上是目录也算' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot $main }) -match '^已锁存.*别的东西')
        Remove-Item -LiteralPath $want -Force
        # 读不了属性（这里用一个坏路径；权限不够时一样）：当作有，宁可拒绝
        $script:Override = "$want$([char]0)x"
        & $check '有无：读不了那个位置的属性时也算锁存' $true ((& $thrown { Assert-RinneNoLatch -RepoRoot $main }) -match '^已锁存')
        $script:Override = $null

        # 进行中标记：写入（握着）、读得到、不覆盖已有的、握着时删不掉改不了、原样时清除
        $held = New-RinneLatch -Pr 7 -Digest 'abc' -RepoRoot $main
        $j = Read-RinneLatchText $want | ConvertFrom-Json
        & $check '进行中标记：写下 PR、时间、pid、构建前指纹摘要与 32 位十六进制的随机值，写在由仓路径推出的位置' $false (-not (
                $held.Path -eq $want -and $j.state -eq 'building' -and $j.pr -eq 7 -and $j.at -and $j.pid -eq $PID -and $j.fingerprint -eq 'abc' -and "$($j.nonce)" -match '^[0-9a-f]{32}$'))
        & $check '进行中标记：pid 还在时提示等它结束' $false ((& $thrown { Assert-RinneNoLatch -RepoRoot $main }) -notmatch '进行中.*PR #7.*pid \d+ 还在：等它结束')
        & $check '进行中标记：位置上已经有东西时不覆盖、抛错' $true ((Test-Throws { New-RinneLatch -Pr 8 -Digest 'x' -RepoRoot $main }) -and (Read-RinneLatchText $want) -ceq $held.Text)
        if ($IsWindows) {
            $del, $over = (Test-Throws { Remove-Item -LiteralPath $want -ErrorAction Stop }), (Test-Throws { [IO.File]::WriteAllText($want, '{}') })
            & $check '进行中标记：握着的时候别的进程删不掉、改不了（也就换不成目录）' $true ($del -and $over -and (Test-RinneLatchIntact $held))
        }
        & $check '进行中标记：原样还在时清除' $false (-not ((Clear-RinneLatch $held) -and -not (Test-Path -LiteralPath $want)))
        # 没人握着的旧标记（上一次留下的已篡改标记之类）：上面那条在 Windows 上靠共享冲突就会抛错，这条才看得出是不是覆盖
        Set-Content -LiteralPath $want -Value '{}' -NoNewline
        $stray = $null
        $refused = try { $stray = New-RinneLatch -Pr 8 -Digest 'x' -RepoRoot $main; $false } catch { $true }
        if ($stray) { Close-RinneLatch $stray }
        & $check '进行中标记：位置上已有没人握着的旧标记时不覆盖、抛错' $true ($refused -and (Read-RinneLatchText $want) -ceq '{}')
        Remove-Item -LiteralPath $want
        $held = New-RinneLatch -Pr 7 -Digest 'abc' -RepoRoot $main
        $null = Set-RinneLatchTampered -Latch $held -Changed @('.git/config', '锁存标记')
        $j = Read-RinneLatchText $want | ConvertFrom-Json
        & $check '已篡改：经握着的句柄改写成 tampered，记下变了的项，句柄放开' $false (-not ($j.state -eq 'tampered' -and $j.pr -eq 7 -and @($j.changed) -contains '.git/config' -and -not $held.Stream))
        & $check '已篡改：拒绝的消息写明改过判据文件' $false ((& $thrown { Assert-RinneNoLatch -RepoRoot $main }) -notmatch '^已锁存.*改过主 clone 的判据文件')
        Remove-Item -LiteralPath $want
        if ($IsWindows) {
            # 构建把标记设成不可写（加一条拒绝写的权限）：按路径写不进去，经握着的句柄照样改写成功
            $held = New-RinneLatch -Pr 7 -Digest 'abc' -RepoRoot $main
            $deny = [Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.WindowsIdentity]::GetCurrent().User, 'Write', 'Deny')
            $acl = Get-Acl -LiteralPath $want; $acl.AddAccessRule($deny); Set-Acl -LiteralPath $want -AclObject $acl
            try {
                $blocked = Test-Throws { [IO.File]::WriteAllText($want, '{}') }
                $err = Set-RinneLatchTampered -Latch $held -Changed @('x')
                $ok = (Read-RinneLatchText $want) -match '"tampered"'
            } finally { $acl = Get-Acl -LiteralPath $want; $null = $acl.RemoveAccessRule($deny); Set-Acl -LiteralPath $want -AclObject $acl }
            & $check '已篡改：标记被设成不可写时，经握着的句柄照样改写成功' $true ($blocked -and -not $err -and $ok)
            Remove-Item -LiteralPath $want
        }
        # 握不住句柄的平台：靠内容核对。内容改了（随机值不同）、被删掉、换成目录，都判为被动过、不删
        $script:Hold = $false
        $held = New-RinneLatch -Pr 7 -Digest 'abc' -RepoRoot $main
        & $check '不握句柄：写完就放开' $false ($null -ne $held.Stream)
        Set-Content -LiteralPath $want -Value ($held.Text -replace '"nonce":"[0-9a-f]+"', '"nonce":"0"') -NoNewline
        & $check '不握句柄：内容被改过（随机值不同），不清除' $true (-not (Clear-RinneLatch $held) -and (Test-Path -LiteralPath $want))
        Remove-Item -LiteralPath $want
        & $check '不握句柄：被删掉了，判为被动过' $true (-not (Test-RinneLatchIntact $held))
        $null = Set-RinneLatchTampered -Latch $held -Changed @('锁存标记')
        & $check '不握句柄：被删掉之后按路径写回成 tampered' $true ((Test-Path -LiteralPath $want -PathType Leaf) -and (Read-RinneLatchText $want) -match '"tampered"')
        Remove-Item -LiteralPath $want
        $held = New-RinneLatch -Pr 7 -Digest 'abc' -RepoRoot $main
        Remove-Item -LiteralPath $want
        $null = New-Item -ItemType Directory -Path $want
        & $check '不握句柄：换成了目录，判为被动过、不删' $true (-not (Clear-RinneLatch $held) -and (Test-Path -LiteralPath $want -PathType Container))
        & $check '不握句柄：位置上是目录时写不进去，报出来、写明照样锁存' $true ((Set-RinneLatchTampered -Latch $held -Changed @('x')) -match '写不进去.*照样锁存')
        Remove-Item -LiteralPath $want -Force
        $script:Hold, $held = $true, $null

        # 入口：主 clone 与 worktree 两种布局各放一份副本；主 clone 的副本是真的仓，rinne.root 指到诱饵目录。假的 git、gh
        # 被调用就往 $called 里写一行
        $root = $script:ModuleRepo
        $lt = Join-Path $tmp 'lt'
        $copies = @((Join-Path $lt 'rinne'), (Join-Path $lt 'wt/k'))
        foreach ($c in $copies) {
            $null = New-Item -ItemType Directory -Force -Path (Join-Path $c 'scripts/maintainer/lib'), (Join-Path $c 'scripts/lib')
            Copy-Item -Path (Join-Path $root 'scripts/maintainer/*.ps1') -Destination (Join-Path $c 'scripts/maintainer')
            Copy-Item -LiteralPath (Join-Path $root 'scripts/maintainer/lib/RinneGh.psm1') -Destination (Join-Path $c 'scripts/maintainer/lib')
            foreach ($f in 'StdHandles.psm1', 'PremergeLatch.psm1', 'Locks.psm1', 'Tiers.psm1', 'tier-rules.json') { Copy-Item -LiteralPath (Join-Path $root "scripts/lib/$f") -Destination (Join-Path $c 'scripts/lib') }
            Copy-Item -LiteralPath (Join-Path $root 'scripts/gates.ps1') -Destination (Join-Path $c 'scripts')
        }
        $null = git init -q (Join-Path $lt 'rinne') 2>&1
        $null = git -C (Join-Path $lt 'rinne') config rinne.root ($decoy -replace '\\', '/') 2>&1
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $lt 'rinne/.git/worktrees/k')
        Set-Content -LiteralPath (Join-Path $lt 'rinne/.git/worktrees/k/commondir') -Value '../..'
        Set-Content -LiteralPath (Join-Path $lt 'wt/k/.git') -Value "gitdir: $((Join-Path $lt 'rinne/.git/worktrees/k') -replace '\\', '/')"
        $bin, $called, $marker = (Join-Path $tmp 'bin'), (Join-Path $tmp 'called.txt'), (Join-Path $lt $script:LatchName)
        $null = New-Item -ItemType Directory -Force -Path $bin
        foreach ($n in 'git', 'gh') {
            if ($IsWindows) { Set-Content -LiteralPath (Join-Path $bin "$n.cmd") -Value "@echo %0 %*>>`"$called`"`r`n@exit /b 1" }
            else {
                # 别的平台上 PATH 不认 .cmd：写成带可执行位的 sh 脚本
                $f = Join-Path $bin $n
                Set-Content -LiteralPath $f -Value "#!/bin/sh`necho `"`$0 `$*`" >> '$called'`nexit 1"
                & chmod +x $f
            }
        }
        $cid = 'RN' + '-99a'
        $run = {
            param($where, [string[]]$argv)
            Remove-Item -LiteralPath $called -ErrorAction SilentlyContinue
            # 结果 json 不落到调用方的日志目录里
            $env:PATH, $env:RINNE_LOG_DIR = "$bin$([IO.Path]::PathSeparator)$($saved.Path)", (Join-Path $tmp 'logs')
            try { $o = & ([Environment]::ProcessPath) -NoProfile -NonInteractive -File (Join-Path $lt "$where/$($argv[0])") @($argv | Select-Object -Skip 1) 2>&1 | Out-String; $code = $LASTEXITCODE }
            finally { $env:PATH, $env:RINNE_LOG_DIR = $saved.Path, $saved.LogDir }
            [pscustomobject]@{ Code = $code; Out = $o; Called = (Test-Path -LiteralPath $called) }
        }
        # 对照：推出的位置上没有标记（只有诱饵目录里有）时，包装脚本一导入就会去调 gh（核对身份），gates 一开头就会调 git
        $c0, $c1 = (& $run 'rinne' @('scripts/maintainer/rn-push.ps1')), (& $run 'rinne' @('scripts/gates.ps1', '-Quick'))
        & $check '入口对照：推出的位置上没有标记时，假的 gh、git 确实会被调到（诱饵目录里的标记不算）' $false (-not ($c0.Called -and $c1.Called))
        $entries = @(
            @('rinne', 'scripts/maintainer/pr-merge.ps1', '-Check', '-Pr', '3', '-Card', $cid),
            @('rinne', 'scripts/maintainer/pr-merge.ps1', '-After', '-Pr', '3', '-Card', $cid),
            @('rinne', 'scripts/maintainer/rn-push.ps1'),
            @('rinne', 'scripts/maintainer/pr-open.ps1', '-Head', 'feat/x', '-Title', 'x', '-BodyFile', 'x'),
            @('rinne', 'scripts/maintainer/pr-comment.ps1', '-List', '-Pr', '3'),
            @('rinne', 'scripts/maintainer/dispatch.ps1', '-Ref', 'feature/v1', '-Sha', ('c' * 40)),
            @('rinne', 'scripts/maintainer/new-worktree.ps1', '-Card', $cid, '-Slug', 'x'),
            @('rinne', 'scripts/gates.ps1', '-Quick'),
            @('wt/k', 'scripts/maintainer/rn-push.ps1'),
            @('wt/k', 'scripts/gates.ps1', '-Quick'))
        Remove-Item -LiteralPath (Join-Path $decoy $script:LatchName)
        Set-Content -LiteralPath $marker -Value '{}'
        foreach ($e in $entries) {
            $r = & $run $e[0] @($e | Select-Object -Skip 1)
            & $check "入口：标记在时 $($e[0])/$($e[1]) $(@($e | Select-Object -Skip 2) -join ' ') 在 git、gh 之前就拒绝（rinne.root 指到别处也一样）" $true ($r.Code -ne 0 -and $r.Out -match '已锁存' -and -not $r.Called)
        }
        Remove-Item -LiteralPath $marker
        $null = New-Item -ItemType Directory -Path $marker
        $r = & $run 'wt/k' @('scripts/maintainer/pr-comment.ps1', '-List', '-Pr', '3')
        & $check '入口：标记位置上是目录时，worktree 里的 pr-comment 照样拒绝' $true ($r.Code -ne 0 -and $r.Out -match '已锁存' -and -not $r.Called)
    } catch {
        & $check "锁存自测抛错：$($_.Exception.Message)" $true $false
    } finally {
        if ($held) { Close-RinneLatch $held }
        $env:PATH, $env:RINNE_ROOT, $script:Override, $script:Hold = $saved.Path, $saved.Root, $saved.Override, $saved.Hold
        # 只删本进程刚建的那个临时目录：解析后在系统临时目录之下、名字是上面起的
        $full = [IO.Path]::GetFullPath($tmp)
        if ($made -and $full.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $full) -like "rinne-latch-selftest-$PID-*") {
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Test-Throws { param([scriptblock]$Script) try { & $Script | Out-Null; $false } catch { $true } }

Export-ModuleMember -Function Set-RinneLatchOverride, Set-RinneLatchHold, Get-RinneLatchPath, Assert-RinneNoLatch, Read-RinneLatchText,
    New-RinneLatch, Test-RinneLatchIntact, Close-RinneLatch, Clear-RinneLatch, Set-RinneLatchTampered, Test-PremergeLatch
