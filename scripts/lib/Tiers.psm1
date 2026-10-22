# SPDX-License-Identifier: GPL-3.0-or-later
#
# 按 diff 定档。gates.ps1 用它算本次的最低档；合并时 pr-merge 用主 clone 里的同一份模块与 tier-rules.json，按 PR 的
# 文件清单再算一次：gates 的结果 json 是自报的，档位够不够以合并时重算的为准。
#   Import-Module <主 clone>/scripts/lib/Tiers.psm1 -ErrorAction Stop
#   $t = Resolve-RinneTier -Files $files -CardTier Standard   # → { Auto; Min; Run; Lowered }，Min = max(按 diff, 卡片)
#   (Get-RinneTierRank $json.tier) -ge (Get-RinneTierRank $t.Min)
#   $d = Get-RinneDiff -Root <仓根> -Base origin/feature/v1     # → { Ok; Files; BaseSha; Error }
#   $s = Get-RinneTreeState -Root <仓根>                        # → { Sha; Dirty; Ignored }
# 文件清单要同时含改名前后的路径与删掉的路径（GitHub 的 PR 文件接口里是 filename 与 previous_filename）。
# 规则见 tier-rules.json：quick 是白名单，命中它、又没被更高的规则命中的文件才是 Quick；其余文件至少是 default
# （Standard）；rules 只往上抬。一律按不区分大小写的正则匹配仓内相对路径（正斜杠）：Windows 与 mac 的文件系统
# 不区分大小写，Src/ 与 src/ 在那里是同一个目录。

# 自测的临时目录经 TempDir 建与删（只删本进程新建的）
Import-Module (Join-Path $PSScriptRoot 'TempDir.psm1') -ErrorAction Stop
$script:Cache = @{}

function Get-RinneTierRules {
    param([string]$Path = (Join-Path $PSScriptRoot 'tier-rules.json'))
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $script:Cache.ContainsKey($full)) { $script:Cache[$full] = Get-Content -LiteralPath $full -Raw | ConvertFrom-Json }
    $script:Cache[$full]
}

function Get-RinneTierRank {
    param([string]$Tier, $Rules = (Get-RinneTierRules))
    if (-not $Tier) { return -1 }
    $i = [array]::IndexOf([string[]]$Rules.tiers, $Tier)
    if ($i -lt 0) { throw "未知档位 $Tier（只有 $($Rules.tiers -join ' / ')）" }
    $i
}

function Get-RinneFileTier {
    param([Parameter(Mandatory)][string]$Path, $Rules = (Get-RinneTierRules))
    $quick = @($Rules.quick | Where-Object { $Path -match $_ }).Count -gt 0
    $rank = $quick ? 0 : (Get-RinneTierRank $Rules.default $Rules)
    foreach ($r in $Rules.rules) { if ($Path -match $r.re) { $rank = [Math]::Max($rank, (Get-RinneTierRank $r.tier $Rules)) } }
    $Rules.tiers[$rank]
}

function Resolve-RinneTier {
    param([string[]]$Files = @(), [string]$CardTier = '', [string]$Asked = '', $Rules = (Get-RinneTierRules))
    $rank = 0
    foreach ($f in $Files) { $rank = [Math]::Max($rank, (Get-RinneTierRank (Get-RinneFileTier $f $Rules) $Rules)) }
    $min = $Rules.tiers[[Math]::Max($rank, (Get-RinneTierRank $CardTier $Rules))]
    $run = $Asked ? $Asked : $min
    [pscustomobject]@{ Auto = $Rules.tiers[$rank]; Min = $min; Run = $run; Lowered = (Get-RinneTierRank $run $Rules) -lt (Get-RinneTierRank $min $Rules) }
}

# <Base>...HEAD 的文件（以共同祖先为基线；改名拆成删与增，所以两个路径都在；-z 输出，引号与转义不会把路径弄乱）。
function Get-RinneDiff {
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Base)
    $fail = { param($e) [pscustomobject]@{ Ok = $false; Files = @(); BaseSha = ''; Error = $e } }
    $mb = "$(git -C $Root merge-base $Base HEAD 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $mb -notmatch '^[0-9a-f]{40,64}$') { return & $fail "找不到 $Base 与 HEAD 的共同祖先（先 fetch 基线）" }
    $raw = @(git -C $Root -c core.quotepath=false diff --name-only --no-renames -z $mb HEAD 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0) { return & $fail "git diff $Base...HEAD 失败" }
    [pscustomobject]@{ Ok = $true; Files = @($raw -split "`0" | Where-Object { $_ }); BaseSha = $mb; Error = '' }
}

# HEAD 与工作区。改动、未跟踪的文件，以及 ignoredInputs 里被忽略却会被构建读到的文件，都算不干净；git 失败也算。
function Get-RinneTreeState {
    param([Parameter(Mandatory)][string]$Root, $Rules = (Get-RinneTierRules))
    $sha = "$(git -C $Root rev-parse HEAD 2>$null)".Trim(); $ok = $LASTEXITCODE -eq 0
    $status = @(git -C $Root status --porcelain --untracked-files=normal 2>$null); $ok = $ok -and $LASTEXITCODE -eq 0
    $ignored = @($Rules.ignoredInputs | Where-Object { Test-Path -LiteralPath (Join-Path $Root $_) })
    [pscustomobject]@{ Sha = $sha; Dirty = (-not $ok) -or $status.Count -gt 0 -or $ignored.Count -gt 0; Ignored = $ignored }
}

# 自测的临时目录清不掉时要把原因报出来，不能无声丢掉（与 10-static.ps1、new-worktree.ps1、bootstrap-tools.ps1 同一口径）。
# 返回清不掉的原因（空串表示清掉了）。
function Remove-RinneTiersTempDir {
    param([Parameter(Mandatory)][string]$Path, [double]$WaitSeconds = 5)
    $left = Remove-RinneTempDir -Path $Path -WaitSeconds $WaitSeconds
    if ($left) { Write-Warning "临时目录没清掉：$left" }
    if ($left) { $left } else { '' }
}

# 自测：tier-rules.json 的 cases 逐条核对；quick 与 rules 的每条正则都至少要被一个用例命中；工作区状态在临时仓里核对。
# 返回 { Label; Bad; Passed }。
function Test-RinneTierRules {
    param($Rules = (Get-RinneTierRules))
    foreach ($c in $Rules.cases) {
        $t = Resolve-RinneTier -Files @($c.files) -CardTier "$($c.card)" -Asked "$($c.ask)" -Rules $Rules
        $label = "定档 [$(@($c.files) -join ', ')]$($c.card ? " 卡片 $($c.card)" : '')$($c.ask ? " 选 $($c.ask)" : '') → $($c.want)"
        [pscustomobject]@{ Label = $label; Bad = [bool]$c.fail; Passed = ($t.Min -ceq $c.want) -and ($t.Lowered -eq [bool]$c.fail) }
    }
    $all = @($Rules.cases | ForEach-Object { $_.files })
    foreach ($re in @($Rules.quick) + @($Rules.rules | ForEach-Object re)) {
        [pscustomobject]@{ Label = "规则 /$re/ 至少有一个用例命中"; Bad = $false; Passed = @($all | Where-Object { $_ -match $re }).Count -gt 0 }
    }
    $loose = @($Rules.gates | Where-Object { $_.tier -cne 'Quick' -and -not ($_.by -and $_.requires) })
    [pscustomobject]@{ Label = '目录里非 Quick 的 gate 都写了 by 与 requires'; Bad = $false; Passed = -not $loose.Count }
    # 清临时目录：清不掉要有提示（不能无声丢掉原因）。造一个不是本进程建的目录：核对不过，删不掉
    $probe = New-RinneTempDir -Prefix 'rinne-tiers-warn-'
    $foreign = Join-Path $probe 'foreign'
    $null = New-Item -ItemType Directory -Path $foreign
    $warned = $null
    $left = Remove-RinneTiersTempDir -Path $foreign -WarningVariable warned -WarningAction SilentlyContinue
    [pscustomobject]@{ Label = '清不掉临时目录时给出提示'; Bad = $true; Passed = @($warned).Count -gt 0 -and @($warned)[0] -match '临时目录没清掉' -and $left -match '不删' }
    $null = Remove-RinneTempDir -Path $probe -WaitSeconds 5
    $tmp = New-RinneTempDir -Prefix 'rinne-tiers-selftest-'
    try {
        $ignoredFile = @($Rules.ignoredInputs)[0]
        Set-Content -LiteralPath (Join-Path $tmp '.gitignore') -Value $ignoredFile
        git -C $tmp init -q *>$null; git -C $tmp add -A *>$null
        git -C $tmp -c user.name=t -c user.email=t@example.invalid commit -q -m t *>$null
        $clean = (Get-RinneTreeState $tmp $Rules).Dirty
        Set-Content -LiteralPath (Join-Path $tmp $ignoredFile) -Value '{}'
        $state = Get-RinneTreeState $tmp $Rules
        [pscustomobject]@{ Label = '干净的仓不算脏'; Bad = $false; Passed = $clean -eq $false }
        [pscustomobject]@{ Label = "被忽略的 $ignoredFile 存在也算工作区不干净"; Bad = $true; Passed = $state.Dirty -and @($state.Ignored) -contains $ignoredFile }
    } finally { $null = Remove-RinneTiersTempDir -Path $tmp -WaitSeconds 5 }
}

Export-ModuleMember -Function Get-RinneTierRules, Get-RinneTierRank, Get-RinneFileTier, Resolve-RinneTier, Get-RinneDiff,
    Get-RinneTreeState, Test-RinneTierRules, Remove-RinneTiersTempDir
