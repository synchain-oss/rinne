#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/gates.json
#
# 本地 gates 运行器。子 PR 上的 CI 不编译，构建、ctest 与 pluginval 只在这里跑。
#   pwsh scripts/gates.ps1 [-Quick | -Standard | -Full] [-CardTier <档>] [-Base origin/feature/<名>] [-Fetch]
#        [-ResultJson <文件> | -LogDir <目录>] [-JucePath <目录>] [-BuildDir build] [-LockWaitMinutes 90] [-MemWaitMinutes 15]
#   pwsh scripts/gates.ps1 -SelfTest
# 定档见 lib/Tiers.psm1：diff（<Base>...HEAD）算出自动档，与 -CardTier 取高即最低档；不给档位就跑最低档，给的档更低时
# 照跑并另记一条 FAIL。-Base 只收 origin/feature/*。gate 来自 gates.d/NN-<名>.ps1，按文件名顺序各在自己的动态模块里
# 加载，每个文件定义 Register-RinneGates，返回若干 @{ Id; Tier; Name; Lock = none|build|gui|ipc; Run = { param($ctx) … } }，
# Run 返回 @{ Status = PASS|FAIL|SKIP|FLAKY; Reason; Lines }；可选的 Test-RinneGates <仓根> 返回自测用例。tier-rules.json 的 gates
# 是各档必须有的 gate，注册的档位必须与它一致；没有模块注册时，requires 在工作树或基线上存在就判 FAIL，否则记 SKIP。
# 锁见 lib/Locks.psm1：build 先等可用内存 ≥ 6000 MB（等满 -MemWaitMinutes 降为 -j2）再取构建槽，gui、ipc 各一把；等锁
# 超时记资源超时。USER_TESTING 标记（主 clone 上一级的 _logs/USER_TESTING，RINNE_USER_TESTING_MARKER 可再加一处）存在时
# Standard 及以上直接 DEFER。开跑与收尾各读一次 HEAD 与工作区，中途变了判 FAIL。结果 json 写到 -ResultJson，缺省
# <LogDir>/gates-<sha40>.json（LogDir 缺省 RINNE_LOG_DIR，再缺省仓内被忽略的 build/gates），完整输出写同名 .log。
# json 是自报的：pr-merge 只能把它与 head 绑定，证明不了它出自本脚本，所以合并时会用主 clone 的 Tiers.psm1 按 PR 的
# 文件清单重算最低档。字体检查要用的 Python：RINNE_PYTHON 没设时取 git config rinne.python（bootstrap-tools 建的 venv）
# 设进本进程的 RINNE_PYTHON，交给各 gate 起的检查。最后一行固定为 `PASS n / FAIL n / SKIP n / FLAKY n  EXIT=x  @<sha7>  <档>  <耗时>`，不含路径。
# 退出码：0 通过；1 有 FAIL 或工作区不干净；3 等锁超时；4 DEFER。
[CmdletBinding()]
param(
    [switch]$Quick, [switch]$Standard, [switch]$Full, [switch]$SelfTest, [switch]$Fetch,
    [ValidateSet('', 'Quick', 'Standard', 'Full')][string]$CardTier = '',
    [string]$Base = 'origin/feature/v1', [string]$ResultJson = '', [string]$LogDir = $env:RINNE_LOG_DIR,
    [string]$JucePath = '', [string]$BuildDir = 'build', [double]$LockWaitMinutes = 90, [double]$MemWaitMinutes = 15
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) } catch { $null = $_ }
$OutputEncoding = [Text.UTF8Encoding]::new($false)
$Root = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib/Locks.psm1') -Force -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'lib/Tiers.psm1') -Force -ErrorAction Stop
$Rules = Get-RinneTierRules
$Tiers = [string[]]$Rules.tiers
if (-not $env:RINNE_PYTHON) { $env:RINNE_PYTHON = "$(git -C $Root config --get rinne.python 2>$null)".Trim() }

function Get-RinneSetting([string]$EnvName, [string]$GitKey) {
    $v = [Environment]::GetEnvironmentVariable($EnvName)
    $v ? $v : "$(git -C $Root config --get $GitKey 2>$null)".Trim()
}

function Format-Duration([TimeSpan]$T) {
    $s = [int][Math]::Round($T.TotalSeconds)
    ($s -lt 60) ? "${s}s" : ('{0}m{1:00}s' -f [Math]::Floor($s / 60), ($s % 60))
}

function Format-Summary($Count, [int]$Exit, [string]$Sha, [string]$Tier, [TimeSpan]$T) {
    'PASS {0} / FAIL {1} / SKIP {2} / FLAKY {3}  EXIT={4}  @{5}  {6}  {7}' -f $Count.PASS, $Count.FAIL, $Count.SKIP, $Count.FLAKY,
    $Exit, ($Sha + '0000000').Substring(0, 7), $Tier, (Format-Duration $T)
}

# 打印前脱敏：仓根写成 .，用户目录写成 ~，两种斜杠都认。
function Format-Redacted([string]$Text) {
    foreach ($p in @(@($Root, '.'), @($HOME, '~'))) {
        if (-not $p[0]) { continue }
        foreach ($v in @(($p[0] -replace '/', '\'), ($p[0] -replace '\\', '/'))) { $Text = $Text -ireplace [regex]::Escape($v), $p[1] }
    }
    $Text
}

function Get-ExitCode($Count, [bool]$Dirty, [bool]$Resource, [bool]$Deferred) {
    if ($Deferred) { 4 } elseif ($Count.FAIL -gt 0 -or $Dirty) { 1 } elseif ($Resource) { 3 } else { 0 }
}

function New-Result([string]$Id, [string]$Name, [string]$Status, [string]$Reason, $Lines = @(), [bool]$Resource = $false, [double]$Seconds = 0) {
    [pscustomobject]@{ Id = $Id; Name = $Name; Status = $Status; Reason = $Reason; Lines = @($Lines); Resource = $Resource; Seconds = $Seconds }
}

function Import-GateModules([string]$Dir) {
    foreach ($f in Get-ChildItem -LiteralPath $Dir -Filter '*.ps1' -File | Sort-Object Name) {
        try {
            $mod = New-Module -Name "rinne-gates-$($f.BaseName)" -Function @() -ScriptBlock ([scriptblock]::Create((Get-Content -LiteralPath $f.FullName -Raw)))
            foreach ($g in @(& $mod { Register-RinneGates })) {
                [pscustomobject]@{ Id = $g.Id; Tier = $g.Tier; Name = $g.Name; Lock = $g.Lock ?? 'none'; Run = $g.Run; File = $f.Name; Module = $mod; Broken = $null }
            }
        } catch {
            [pscustomobject]@{ Id = "load:$($f.BaseName)"; Tier = $Tiers[0]; Name = '模块加载'; Lock = 'none'; Run = $null; File = $f.Name; Module = $null; Broken = $_.Exception.Message }
        }
    }
}

# 注册项逐条核对，有问题的不跑、各记一条 FAIL。返回 @{ Problems; Valid; Seen }。
function Test-Registrations($Regs) {
    $catalog, $seen, $valid = @{}, @{}, [System.Collections.Generic.List[object]]::new()
    foreach ($c in $Rules.gates) { $catalog[$c.id] = $c }
    $problems = foreach ($g in $Regs) {
        $want = $catalog["$($g.Id)"].tier
        if ($g.Broken) { "$($g.File)：$($g.Broken)" }
        elseif ($g.Id -cnotmatch '^[A-Za-z][\w-]*$' -or $g.Run -isnot [scriptblock]) { "$($g.File)：注册项缺合法的 Id 或 Run" }
        elseif ($seen.ContainsKey($g.Id)) { "$($g.File)：$($g.Id) 已经由 $($seen[$g.Id]) 注册过" }
        elseif ($g.Tier -cnotin $Tiers) { "$($g.File)：$($g.Id) 的档位 '$($g.Tier)' 不是 $($Tiers -join ' / ')" }
        elseif ($want -and $want -cne $g.Tier) { "$($g.File)：$($g.Id) 属于 $want 档，注册成了 $($g.Tier)" }
        elseif ($g.Lock -cnotin 'none', 'build', 'gui', 'ipc') { "$($g.File)：$($g.Id) 的锁 '$($g.Lock)' 不认识" }
        else { $seen[$g.Id] = $g.File; $valid.Add($g) }
    }
    @{ Problems = @($problems); Valid = $valid.ToArray(); Seen = $seen }
}

# 目录里属于本档、却没有模块注册的 gate。requires 在基线上存在也算在：PR 删掉构建输入不能换来 SKIP。
function Get-MissingGates($Seen, [int]$RunRank, [string]$BaseSha) {
    $there = { param($p) (Test-Path -LiteralPath (Join-Path $Root $p)) -or ($BaseSha -and $(git -C $Root cat-file -e "${BaseSha}:$p" 2>$null; $LASTEXITCODE -eq 0)) }
    foreach ($c in $Rules.gates | Where-Object { (Get-RinneTierRank $_.tier $Rules) -le $RunRank -and -not $Seen.ContainsKey($_.id) }) {
        $absent = @($c.requires | Where-Object { $_ -and -not (& $there $_) })
        if (-not $c.requires -or -not $absent.Count) { New-Result $c.id $c.what 'FAIL' "没有模块注册这道 gate（应由 $($c.by ?? 'gates.d/10-static.ps1') 提供）" }
        else { New-Result $c.id $c.what 'SKIP' "尚无 $($absent -join '、')：由 $($c.by) 提供，入库后自动生效" }
    }
}

function Invoke-Gate($G, $Ctx) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lock = $null
    try {
        if ($G.Lock -eq 'build') {
            $mem = Wait-RinneMemory -MinMB 6000 -TimeoutSeconds ($MemWaitMinutes * 60)
            if (-not $mem.Ok) { Write-Host "  [mem] 等了 $MemWaitMinutes 分钟，可用内存仍只有 $($mem.AvailMB) MB，降为 -j2" }
            $lock = Enter-RinneBuildSlot -TimeoutSeconds ($LockWaitMinutes * 60)
            $Ctx.Jobs = ($mem.Ok -and $lock) ? (Get-RinneJobs (Get-RinneAvailableMB)) : 2    # 拿到槽之后再按当时的内存折算
        } elseif ($G.Lock -ne 'none') {
            $lock = Enter-RinneLock (($G.Lock -eq 'ipc') ? 'ipc-tests' : $G.Lock) -TimeoutSeconds ($LockWaitMinutes * 60)
        }
        if ($G.Lock -ne 'none' -and -not $lock) { $r = @{ Status = 'SKIP'; Reason = "等 $($G.Lock) 锁超过 $LockWaitMinutes 分钟，资源超时"; Resource = $true } }
        else { $r = @(& $G.Run $Ctx) | Where-Object { $null -ne $_.Status } | Select-Object -Last 1 }
    } catch {
        $r = @{ Status = 'FAIL'; Reason = "抛错：$($_.Exception.Message)" }
    } finally {
        Exit-RinneLock $lock
    }
    if ($r.Status -cnotin 'PASS', 'FAIL', 'SKIP', 'FLAKY') { $r = @{ Status = 'FAIL'; Reason = "返回的状态 '$($r.Status)' 不认识" } }
    New-Result $G.Id ($G.Name ?? $G.Id) $r.Status $r.Reason $r.Lines ([bool]$r.Resource) ([Math]::Round($sw.Elapsed.TotalSeconds, 1))
}

function Write-ResultLine($R) {
    Write-Host (Format-Redacted ('[{0}] {1} {2}  {3}s  {4}' -f $R.Status, $R.Id, $R.Name, $R.Seconds, $R.Reason))
    if ($R.Status -eq 'FAIL') { $R.Lines | Select-Object -Last 40 | ForEach-Object { Write-Host (Format-Redacted "      $_") } }
}

function Invoke-SelfTest {
    $cases = [System.Collections.Generic.List[object]]::new()
    $check = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }
    Test-RinneTierRules $Rules | ForEach-Object { $cases.Add($_) }
    $regs = @(Import-GateModules (Join-Path $PSScriptRoot 'gates.d'))
    $quick = @($Rules.gates | Where-Object tier -ceq 'Quick' | ForEach-Object id | Sort-Object) -join ','
    $real = Test-Registrations @($regs | Where-Object File -like '10-*')
    & $check '10-static.ps1 的注册恰好是 Quick 档目录里的 gate' $false ((-not $real.Problems.Count) -and (@($real.Valid.Id | Sort-Object) -join ',') -ceq $quick)
    foreach ($m in @($regs.Module | Where-Object { $_ } | Sort-Object Name -Unique)) { & $m { param($r) if (Get-Command Test-RinneGates -ErrorAction Ignore) { Test-RinneGates $r } } $Root | ForEach-Object { $cases.Add($_) } }
    $bad = Test-Registrations @(@{ Id = 'G6'; Tier = 'Full' }, @{ Id = 'G7'; Tier = 'Standard'; Lock = 'disk' }, @{ Id = 'x'; Tier = 'Quick' }, @{ Id = 'x'; Tier = 'Quick' } |
        ForEach-Object { [pscustomobject]@{ Id = $_.Id; Tier = $_.Tier; Lock = $_.Lock ?? 'none'; Run = {}; File = 'x.ps1' } })
    & $check '注册的档位与目录不符、锁名不认识、Id 重复都判红' $true ($bad.Problems.Count -eq 3 -and $bad.Valid.Count -eq 1)
    $line = Format-Summary @{ PASS = 5; FAIL = 0; SKIP = 3; FLAKY = 1 } 0 ('a' * 40) 'Standard' ([TimeSpan]::FromSeconds(185))
    & $check '汇总行格式' $false ($line -ceq 'PASS 5 / FAIL 0 / SKIP 3 / FLAKY 1  EXIT=0  @aaaaaaa  Standard  3m05s')
    & $check '打印前把仓根与用户目录脱敏' $true ((Format-Redacted "$($Root -replace '\\', '/')/x $HOME\y") -ceq './x ~\y')
    $ec = foreach ($a in @(1, 0, 1, 0), @(1, 1, 1, 1), @(0, 1, 0, 0), @(0, 0, 1, 0), @(0, 0, 0, 0)) { Get-ExitCode @{ FAIL = $a[0] } $a[1] $a[2] $a[3] }
    & $check '退出码：FAIL 优先于资源超时，DEFER 优先于一切，只有工作区不干净也是 1' $true ((@($ec)[0..2] -join ',') -eq '1,4,1')
    & $check '退出码：只有资源超时是 3，全过是 0' $false ((@($ec)[3..4] -join ',') -eq '3,0')
    Test-RinneLocks | ForEach-Object { $cases.Add($_) }
    Invoke-EndToEnd $check
    $failed = @($cases | Where-Object { -not $_.Passed })
    $failed | ForEach-Object { Write-Host "自测未通过：$($_.Label)" }
    $nBad = @($cases | Where-Object Bad).Count
    Write-Host ('gates --self-test：{0}（{1} 项，坏样例 {2}、正例 {3}）' -f ($failed ? '失败' : '通过'), $cases.Count, $nBad, ($cases.Count - $nBad))
    [int]($failed.Count -gt 0 -or -not $nBad -or $nBad -eq $cases.Count)
}

# 端到端：临时仓里放一份本运行器、Tiers 与定档规则、锁名前缀换成自测专用的 Locks，再加夹具模块，起子进程跑，
# 核对 json、退出码与最后一行。运行器本身没有给自测开的参数或环境变量。
function Invoke-EndToEnd($Check) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "rinne-gates-selftest-$PID-$(Get-Random)"
    $repo, $held = (Join-Path $tmp 'repo'), $null
    try {
        $null = New-Item -ItemType Directory -Path (Join-Path $repo 'scripts/lib'), (Join-Path $repo 'scripts/gates.d'), (Join-Path $tmp '_logs')
        Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $repo 'scripts/gates.ps1')
        foreach ($f in 'Tiers.psm1', 'tier-rules.json') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "lib/$f") -Destination (Join-Path $repo "scripts/lib/$f") }
        $locks = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lib/Locks.psm1') -Raw).Replace("`$script:Scope = 'Rinne'", "`$script:Scope = 'Rinne-selftest-$PID'")
        Set-Content -LiteralPath (Join-Path $repo 'scripts/lib/Locks.psm1') -Value $locks
        $quick = ($Rules.gates | Where-Object tier -ceq 'Quick' | ForEach-Object { "'$($_.id)'" }) -join ', '
        Set-Content -LiteralPath (Join-Path $repo 'scripts/gates.d/10-fx.ps1') -Value @"
function Register-RinneGates {
    foreach (`$id in $quick) { @{ Id = `$id; Tier = 'Quick'; Run = { @{ Status = 'PASS' } } } }
    @{ Id = 'fx-build'; Tier = 'Quick'; Lock = 'build'; Run = { param(`$c) @{ Status = 'PASS'; Reason = "jobs=`$(`$c.Jobs)" } } }
    if (`$env:RINNE_FX -eq 'gui') { @{ Id = 'fx-gui'; Tier = 'Quick'; Lock = 'gui'; Run = { @{ Status = 'PASS' } } } }
    if (`$env:RINNE_FX -eq 'py') { @{ Id = 'fx-py'; Tier = 'Quick'; Run = { @{ Status = 'PASS'; Reason = "py=`$(`$env:RINNE_PYTHON)" } } } }
    if (`$env:RINNE_FX -eq 'touch') { @{ Id = 'fx-touch'; Tier = 'Quick'; Run = { param(`$c) Add-Content (Join-Path `$c.Root 'README.md') 'y'; @{ Status = 'PASS' } } } }
}
"@
        $git = { git -C $repo -c user.name=DLsnows -c user.email=noreply@synchain.ca @args *>$null }
        $commit = { param($f) Set-Content -LiteralPath (Join-Path $repo $f) -Value 'x'; & $git add -A; & $git commit -q -m "test: $f" }
        & $git init -q; & $commit 'README.md'; & $git update-ref refs/remotes/origin/feature/v1 HEAD
        $pwsh = (Get-Process -Id $PID).Path
        $run = {
            param([string[]]$Extra = @())
            $json = Join-Path $tmp "r-$(Get-Random).json"
            $out = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $repo 'scripts/gates.ps1') -ResultJson $json -MemWaitMinutes 0 -LockWaitMinutes 0.02 @Extra *>&1)
            [pscustomobject]@{ Exit = $LASTEXITCODE; J = (Get-Content -LiteralPath $json -Raw | ConvertFrom-Json); Last = "$($out[-1])" }
        }
        $gate = { param($r, $id) @($r.J.gates | Where-Object id -ceq $id)[0].status }
        $r = & $run
        & $Check '干净的树、全 PASS：EXIT=0，最后一行是汇总行，json 记下基线' $false ($r.Exit -eq 0 -and -not $r.J.dirty -and $r.J.base -ceq 'origin/feature/v1' -and
            $r.J.baseSha -match '^[0-9a-f]{40}$' -and $r.J.autoTier -ceq 'Quick' -and $r.Last -cmatch '^PASS \d+ / FAIL 0 / SKIP 0 / FLAKY 0  EXIT=0  @[0-9a-f]{7}  Quick  \d+s$')
        $r = & $run @('-Base', 'HEAD')
        & $Check '基线不是 origin/feature/*：FAIL、EXIT=1' $true ($r.Exit -eq 1 -and (& $gate $r 'G0') -eq 'FAIL')
        $m = New-Module -Function @() -ScriptBlock ([scriptblock]::Create($locks))
        $held = & $m { Enter-RinneLock gui -TimeoutSeconds 5 -Quiet }
        $env:RINNE_FX = 'gui'; $r = & $run
        & $Check 'gui 锁被占、等锁超时：资源超时 EXIT=3' $true ($r.Exit -eq 3 -and $r.J.fail -eq 0 -and (& $gate $r 'fx-gui') -eq 'SKIP')
        & $m { param($l) Exit-RinneLock $l } $held; $held = $null
        $env:RINNE_FX = 'touch'; $r = & $run; $env:RINNE_FX = ''
        & $Check 'gate 跑的时候改了工作区：FAIL、dirty=true' $true ($r.Exit -eq 1 -and $r.J.dirty -and (& $gate $r 'G0') -eq 'FAIL')
        & $git checkout -- README.md
        Set-Content -LiteralPath (Join-Path $repo 'stray.txt') -Value 'x'
        $r = & $run
        & $Check '工作区不干净：dirty=true、EXIT=1' $true ($r.Exit -eq 1 -and $r.J.exit -eq 1 -and $r.J.dirty -eq $true)
        Remove-Item -LiteralPath (Join-Path $repo 'stray.txt')
        # RINNE_PYTHON 没设时取 git config rinne.python，交给各 gate
        & $git config rinne.python rinne-selftest-python
        $savedPy, $env:RINNE_PYTHON, $env:RINNE_FX = $env:RINNE_PYTHON, $null, 'py'
        try { $r = & $run } finally { $env:RINNE_PYTHON, $env:RINNE_FX = $savedPy, '' }
        & $git config --unset rinne.python
        & $Check 'RINNE_PYTHON 没设时取 git config rinne.python 交给各 gate' $false ($r.Exit -eq 0 -and @($r.J.gates | Where-Object id -ceq 'fx-py')[0].reason -ceq 'py=rinne-selftest-python')
        $null = New-Item -ItemType Directory -Path (Join-Path $repo 'src')
        & $commit 'src/a.cpp'
        $r = & $run @('-Quick')
        & $Check '改了 src/ 却选 Quick：FAIL、EXIT=1' $true ($r.Exit -eq 1 -and $r.J.tier -ceq 'Quick' -and $r.J.minTier -ceq 'Standard' -and (& $gate $r 'G0') -eq 'FAIL')
        $r = & $run
        & $Check '不给档位时按 diff 跑 Standard，没有 CMakeLists.txt 时 G6–G8 记 SKIP' $false ($r.Exit -eq 0 -and $r.J.tier -ceq 'Standard' -and
            (@('G6', 'G7', 'G8' | ForEach-Object { & $gate $r $_ }) -join ',') -ceq 'SKIP,SKIP,SKIP')
        & $commit 'CMakeLists.txt'
        $r = & $run
        & $Check '有 CMakeLists.txt 却没有模块注册 G6：FAIL' $true ($r.Exit -eq 1 -and (& $gate $r 'G6') -eq 'FAIL')
        & $git update-ref refs/remotes/origin/feature/v1 HEAD; & $git rm -q CMakeLists.txt; & $git commit -q -m 'test: rm'
        $r = & $run
        & $Check '删掉基线上的 CMakeLists.txt：G6 判 FAIL，不记 SKIP' $true ($r.Exit -eq 1 -and $r.J.autoTier -ceq 'Standard' -and (& $gate $r 'G6') -eq 'FAIL')
        Set-Content -LiteralPath (Join-Path $tmp '_logs/USER_TESTING') -Value 'x'
        $r = & $run
        & $Check 'USER_TESTING 标记存在时 Standard 直接 DEFER：EXIT=4' $true ($r.Exit -eq 4 -and $r.J.exit -eq 4 -and $r.Last -cmatch 'EXIT=4  @[0-9a-f]{7}  Standard')
    } catch {
        & $Check "端到端自测抛错：$($_.Exception.Message)" $true $false
    } finally {
        if ($held) { & $m { param($l) Exit-RinneLock $l } $held }
        Remove-Item Env:RINNE_FX -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($SelfTest) { exit @(Invoke-SelfTest)[-1] }

$started = Get-Date
$results = [System.Collections.Generic.List[object]]::new()
$add = { param($R) $results.Add($R); Write-ResultLine $R }
$baseOk = $Base -cmatch '^origin/feature/[\w.-]+(?:/[\w.-]+)*$'
if ($Fetch -and $baseOk) {
    try { Invoke-WithGitLock { git -C $Root fetch --quiet origin ($Base -replace '^origin/', ''); if ($LASTEXITCODE) { throw "git fetch 退出码 $LASTEXITCODE" } } }
    catch { & $add (New-Result 'G0' '定档' 'FAIL' "fetch $Base 失败：$($_.Exception.Message)") }
}
$s0 = Get-RinneTreeState $Root $Rules
$d = $baseOk ? (Get-RinneDiff -Root $Root -Base $Base) : [pscustomobject]@{ Ok = $false; Files = @(); BaseSha = ''; Error = "基线只能是 origin/feature/*，不能是 $Base" }
$asked = if ($Full) { 'Full' } elseif ($Standard) { 'Standard' } elseif ($Quick) { 'Quick' } else { '' }
$t = Resolve-RinneTier -Files $d.Files -CardTier $CardTier -Asked $asked -Rules $Rules
$cardNote = $CardTier ? "，卡片要求 $CardTier" : ''
Write-Host "定档：$($d.Files.Count) 个文件变动，按 diff 是 $($t.Auto)$cardNote，本次跑 $($t.Run)"
if (-not $d.Ok) { & $add (New-Result 'G0' '定档' 'FAIL' $d.Error) }
elseif ($t.Lowered) { & $add (New-Result 'G0' '定档' 'FAIL' "所选 $($t.Run) 低于最低档 $($t.Min)（按 diff 是 $($t.Auto)$cardNote）") }
if ($s0.Ignored.Count) { Write-Host "工作区里有被忽略、构建却会读的文件：$($s0.Ignored -join '、')" }

$common = "$(git -C $Root rev-parse --path-format=absolute --git-common-dir)".Trim()
$markers = @((Join-Path (Split-Path -Parent (Split-Path -Parent $common)) '_logs/USER_TESTING'), $env:RINNE_USER_TESTING_MARKER) | Where-Object { $_ }
$deferred = (Get-RinneTierRank $t.Run $Rules) -ge 1 -and @($markers | Where-Object { Test-Path -LiteralPath $_ }).Count -gt 0
if ($deferred) {
    Write-Host "USER_TESTING 标记存在：用户正在上机测试，$($t.Run) 档暂停（DEFER）"
} else {
    $reg = Test-Registrations @(Import-GateModules (Join-Path $PSScriptRoot 'gates.d'))
    foreach ($p in $reg.Problems) { & $add (New-Result 'G0' '模块注册' 'FAIL' $p) }
    $ctx = [pscustomobject]@{ Root = $Root; Base = $Base; Tier = $t.Run; Sha = $s0.Sha; Files = $d.Files; BuildDir = $BuildDir; JucePath = $JucePath
        Jobs = 2; Setting = ${function:Get-RinneSetting} }
    $rank = Get-RinneTierRank $t.Run $Rules
    foreach ($g in $reg.Valid | Where-Object { (Get-RinneTierRank $_.Tier $Rules) -le $rank }) { & $add (Invoke-Gate $g $ctx) }
    foreach ($m in @(Get-MissingGates $reg.Seen $rank $d.BaseSha)) { & $add $m }
}
$s1 = Get-RinneTreeState $Root $Rules
if ($s1.Sha -cne $s0.Sha -or ($s1.Dirty -and -not $s0.Dirty)) { & $add (New-Result 'G0' '收尾复核' 'FAIL' '跑的过程中 HEAD 或工作区变了，结果不算数') }

$finished, $count = (Get-Date), @{ PASS = 0; FAIL = 0; SKIP = 0; FLAKY = 0 }
foreach ($r in $results) { $count[$r.Status]++ }
$dirty = $s0.Dirty -or $s1.Dirty
$exit = Get-ExitCode $count $dirty (@($results | Where-Object Resource).Count -gt 0) $deferred
if (-not $LogDir) { $LogDir = Join-Path $Root 'build/gates' }
if (-not $ResultJson) { $ResultJson = Join-Path $LogDir "gates-$($s0.Sha).json" }
$null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent ([IO.Path]::GetFullPath($ResultJson)))
[ordered]@{
    sha40 = $s0.Sha; tier = $t.Run; pass = $count.PASS; fail = $count.FAIL; skip = $count.SKIP; flaky = $count.FLAKY; exit = $exit; dirty = $dirty
    started = $started.ToUniversalTime().ToString('o'); finished = $finished.ToUniversalTime().ToString('o')
    base = $Base; baseSha = $d.BaseSha; autoTier = $t.Auto; minTier = $t.Min
    gates = @($results | ForEach-Object { [ordered]@{ id = $_.Id; status = $_.Status; reason = (Format-Redacted $_.Reason); seconds = $_.Seconds } })
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ResultJson -Encoding utf8
$results | ForEach-Object { "=== [$($_.Status)] $($_.Id) $($_.Reason)"; $_.Lines } | Set-Content -LiteralPath ([IO.Path]::ChangeExtension($ResultJson, '.log')) -Encoding utf8
Write-Host "结果：$(Format-Redacted ([IO.Path]::GetFullPath($ResultJson)))"
Write-Output (Format-Summary $count $exit $s0.Sha $t.Run ($finished - $started))
exit $exit
