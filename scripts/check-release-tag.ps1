#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/release.json
<#
.SYNOPSIS  发版 tag 门禁：形态、版本、来源分支、CHANGELOG 与正式版的源码一致性。
.DESCRIPTION
  发布流水线的第一道门。tag 要同时满足下面五条，任何一条不成立都不往下走：
    ① 形态：只接受 vX.Y.Z 与 vX.Y.Z-rc.N。beta、alpha、演练用的 v0.0.0-test、没有前导 v 的、
       尾随空格或换行的、多行的，一律拒。
    ② 版本：X.Y.Z 必须等于 CMakeLists.txt 的 project(Rinne VERSION x.y.z)。
    ③ 来源：tag 指向的提交必须是 origin/prod 的祖先。tag 只打在 prod 上。
    ④ CHANGELOG：要有与本次版本对应的小节（`## [1.2.3]` 或 `## [1.2.3-rc.1]`）。
    ⑤ 源码：正式版的源码必须与最后一个 rc 相同（src、web、cmake、CMakeLists.txt、third_party、
       tests 任一有差异就红），只差文档不算差异。rc 不受这一条约束。
  判据写成纯函数：输入是 tag、CMake 版本、是否在 prod 上、CHANGELOG 文本、正式版与最后一个 rc 的源码
  是否相同，输出是一个 hashtable。自测用夹具喂这些输入，不联网、不碰真实仓库。
  真跑时那三个事实由脚本自己去仓库里取：git rev-parse、git merge-base --is-ancestor、git diff --quiet。
.EXAMPLE   pwsh scripts/check-release-tag.ps1 -SelfTest
.EXAMPLE   pwsh scripts/check-release-tag.ps1 -Tag v0.9.0-rc.1
#>
[CmdletBinding()]
param(
  # 被检查的 tag 名（含前导 v）。
  [string]$Tag,
  # 判据自测：坏样例与正例都跑一遍。
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { $null = $_ }
. (Join-Path $PSScriptRoot 'lib/release-version.ps1')

# 正式版与最后一个 rc 之间要逐字相同的路径。文档不在里面：发正式版允许改文档。
$SourceParityPaths = @('src', 'web', 'cmake', 'CMakeLists.txt', 'third_party', 'tests')

<# CHANGELOG 里有没有与版本对应的小节（`## [1.2.3]` 或 `## [1.2.3-rc.1]`）。 #>
function Test-RinneChangelogSection([string]$ChangelogText, [string]$Version) {
  if (-not $ChangelogText -or -not $Version) { return $false }
  $pattern = '(?m)^##\s+\[' + [regex]::Escape($Version) + '\]\s*$'
  return [regex]::IsMatch($ChangelogText, $pattern)
}

<#
  发版门禁的判据：纯函数，什么都不读、什么都不写。
  Tag                 被检查的 tag 名
  CMakeVersion        CMakeLists.txt 里的版本
  OnProd              tag 指向的提交是不是 origin/prod 的祖先
  ChangelogText       CHANGELOG.md 的全文
  SameSourceAsLastRc  正式版的源码与最后一个 rc 是否相同（只有正式版才看这一条）
  返回 hashtable：Ok / Version / Kind / Prerelease / Message。
#>
function Test-RinneReleaseGate {
  param(
    [string]$Tag,
    [string]$CMakeVersion,
    [bool]$OnProd,
    [string]$ChangelogText,
    [bool]$SameSourceAsLastRc
  )
  $r = @{ Ok = $false; Version = $null; Kind = $null; Prerelease = $false; Message = '' }
  $shape = Test-RinneReleaseTag $Tag $CMakeVersion
  if (-not $shape.Ok) { $r.Message = $shape.Message; return $r }
  $r.Version = $shape.Version
  $r.Kind = $shape.Kind
  $r.Prerelease = $shape.Prerelease
  if (-not $OnProd) {
    $r.Message = "tag '$Tag' 指向的提交不是 origin/prod 的祖先：tag 只打在 prod 上"
    return $r
  }
  if (-not (Test-RinneChangelogSection $ChangelogText $r.Version)) {
    $r.Message = "CHANGELOG.md 里没有与 $($r.Version) 对应的小节（要一行 `## [$($r.Version)]`）"
    return $r
  }
  if ($r.Kind -eq 'final' -and -not $SameSourceAsLastRc) {
    $r.Message = "正式版的源码与最后一个 rc 不一致（$($SourceParityPaths -join '、') 任一有差异就红，只差文档不算）"
    return $r
  }
  $r.Ok = $true
  $r.Message = "tag '$Tag' 通过（$($r.Kind)；在 prod 上；CHANGELOG 有对应小节）"
  return $r
}

if ($SelfTest) {
  $script:selfTestTotal = 0
  $script:selfTestFail = 0

  # 一格自测的比较：Want 与 Got 不同就红。写成恒真的比较就等于没有自测。
  function Test-RinneExpect([bool]$Want, [bool]$Got) { return ($Got -eq $Want) }

  $check = {
    param([string]$Label, [bool]$Want, [bool]$Got)
    $script:selfTestTotal++
    if (Test-RinneExpect $Want $Got) { Write-Host "  [ok] $Label" }
    else {
      Write-Host "  [FAIL] $Label（期望 $Want，实际 $Got）" -ForegroundColor Red
      $script:selfTestFail++
    }
  }

  # 这一格盯住自测自己：比较函数必须能判红。没有它，上面那些格写错或写成都恒真都会「全过」。
  & $check '自测的比较函数本身可判红（期望 false、实际 true 必须判红）' $false (Test-RinneExpect $false $true)
  & $check '自测的比较函数本身可判绿（期望 true、实际 true 必须判绿）' $true (Test-RinneExpect $true $true)

  # CMake 版本解析：cmake_minimum_required 在前、project 在后、中间隔着别的行；被注释掉的 project 不算。
  $cmakeText = "cmake_minimum_required(VERSION 3.22)`n`nset(CMAKE_CXX_STANDARD 17)`n# project(Rinne VERSION 9.9.9)`nproject(Rinne VERSION 1.2.3)`n"
  $parsed = Get-RinneCMakeVersion $cmakeText
  & $check 'CMake 版本解析：跳过 cmake_minimum_required 与被注释掉的 project()' $true ($parsed -eq '1.2.3')
  $none = Get-RinneCMakeVersion "cmake_minimum_required(VERSION 3.22)`nproject(Other VERSION 1.0.0)`n"
  & $check '非 Rinne 的 project() 不认' $true ($null -eq $none)

  # CHANGELOG 三种形态：有本次版本的小节、没有 rc 的小节、什么小节都没有。
  # 夹具覆盖到本组用到的每一个版本号（1.2.3、1.2.3-rc.1、1.2.3-rc.12、0.9.0）：
  # 小节行是整行匹配（`^## \[版本\]$`），所以 1.2.3-rc.12 那行不算 1.2.3-rc.1 的小节。
  $changelog = @(
    '## [Unreleased]', '',
    '## [1.2.3]', '', '- 正式版', '',
    '## [1.2.3-rc.1]', '', '- 第一个候选版', '',
    '## [1.2.3-rc.12]', '', '- 第十二个候选版', '',
    '## [0.9.0]', '', '- 第一个版本'
  ) -join "`n"
  $changelogNoRc = @('## [Unreleased]', '', '## [1.2.3]', '', '- 正式版', '', '## [0.9.0]', '', '- 第一个版本') -join "`n"
  $changelogNone = "## [Unreleased]`n`n- 还没有切节`n"

  # 每一格是一个具名的 hashtable，不用按下标取：下标对错了，表格自己会看得见。
  #   Tag / CMake / OnProd / Changelog（文本）/ SameSource（源码是否与最后一个 rc 相同）
  #   WantOk / WantKind / WantVersion / WantPrerelease
  # 放行的格才比对 Kind / Version / Prerelease：Kind 决定草稿 Release 勾不勾 pre-release，Version 决定产物名。
  # 这一组至少 20 例（验收的硬指标），按「形态 / 版本 / 来源 / CHANGELOG / 源码」五组铺开。
  $cases = @(
    # ── 形态：只认 vX.Y.Z 与 vX.Y.Z-rc.N（放行 3 格） ──
    @{ Tag = 'v1.2.3'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $true; WantKind = 'final'; WantVersion = '1.2.3'; WantPrerelease = $false }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $true; WantKind = 'rc'; WantVersion = '1.2.3-rc.1'; WantPrerelease = $true }
    @{ Tag = 'v1.2.3-rc.12'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $true; WantKind = 'rc'; WantVersion = '1.2.3-rc.12'; WantPrerelease = $true }
    # ── 形态：其余全部拒（12 格） ──
    @{ Tag = 'v1.2.3-rc'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-rc.1.2'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-RC.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-beta.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-alpha.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v0.0.0-test'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v0.0.0-test.2'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = '1.2.3'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2'; CMake = '1.2'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3 '; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = "v1.2.3`n"; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = "v1.2.3`nv1.2.4"; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = ''; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    # ── 版本：X.Y.Z 必须等于 CMake 的版本（1 格放行、2 格拒） ──
    @{ Tag = 'v1.2.4'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.4'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v0.9.0'; CMake = '0.9.0'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $true; WantKind = 'final'; WantVersion = '0.9.0'; WantPrerelease = $false }
    # ── 来源：不在 prod 上就红（2 格） ──
    @{ Tag = 'v1.2.3'; CMake = '1.2.3'; OnProd = $false; Changelog = $changelog; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.3'; OnProd = $false; Changelog = $changelog; SameSource = $true; WantOk = $false }
    # ── CHANGELOG：缺对应小节就红（3 格） ──
    @{ Tag = 'v1.2.3'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelogNone; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelogNone; SameSource = $true; WantOk = $false }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelogNoRc; SameSource = $true; WantOk = $false }
    # ── 源码：正式版必须与最后一个 rc 相同；rc 不看这一条（1 格红、2 格放行） ──
    @{ Tag = 'v1.2.3'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $false; WantOk = $false }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $false; WantOk = $true; WantKind = 'rc'; WantVersion = '1.2.3-rc.1'; WantPrerelease = $true }
    @{ Tag = 'v1.2.3-rc.1'; CMake = '1.2.3'; OnProd = $true; Changelog = $changelog; SameSource = $true; WantOk = $true; WantKind = 'rc'; WantVersion = '1.2.3-rc.1'; WantPrerelease = $true }
  )
  foreach ($c in $cases) {
    $r = Test-RinneReleaseGate $c.Tag $c.CMake $c.OnProd $c.Changelog $c.SameSource
    # 期望与实际分开算：把同一个表达式同时当期望与实际，就等于恒真，自测永远绿。
    $kindOk = $true; $versionOk = $true; $prereleaseOk = $true
    if ($c.WantOk) {
      $kindOk = ($r.Kind -eq $c.WantKind)
      $versionOk = ($r.Version -eq $c.WantVersion)
      $prereleaseOk = ($r.Prerelease -eq $c.WantPrerelease)
    }
    $bad = ($r.Ok -ne $c.WantOk) -or (-not $kindOk) -or (-not $versionOk) -or (-not $prereleaseOk)
    # 期望是「这一格不该失败」，实际是「它到底失败没有」：两者不是同一个表达式。
    & $check ("tag='{0}' cmake={1} 在prod={2} 源码相同={3} -> Ok={4} Kind={5} Version={6} Prerelease={7}" -f $c.Tag, $c.CMake, $c.OnProd, $c.SameSource, $r.Ok, $r.Kind, $r.Version, $r.Prerelease) $false $bad
  }

  # CHANGELOG 的小节行必须整行只有版本号：`## [1.2.3] 别的字` 不算
  $r = Test-RinneReleaseGate 'v1.2.3' '1.2.3' $true "## [1.2.3] 附带说明`n" $true
  & $check 'CHANGELOG 的小节行后面有多余文字时判红' $false $r.Ok

  # 形态那一组至少 20 例，是验收的硬指标；少一格就说明表被改小了。
  & $check ("形态与门禁的夹具至少 20 例（现在 $($cases.Count) 例）") $true ($cases.Count -ge 20)
  # 每一组都要有放行的格与判红的格：只有红格的自测证明不了「该放的真的放了」。
  & $check '夹具里有放行的格' $true (@($cases | Where-Object { $_.WantOk }).Count -ge 3)
  & $check '夹具里有判红的格' $true (@($cases | Where-Object { -not $_.WantOk }).Count -ge 10)

  $total = $script:selfTestTotal
  if ($script:selfTestFail -gt 0) {
    Write-Host "check-release-tag self-test: $total 格里有 $($script:selfTestFail) 格失败" -ForegroundColor Red
    exit 1
  }
  Write-Host "check-release-tag self-test: 全部 $total 格通过" -ForegroundColor Green
  exit 0
}

if (-not $Tag) { Write-Host 'check-release-tag: 缺 -Tag' -ForegroundColor Red; exit 1 }

$repoRoot = Split-Path -Parent $PSScriptRoot
$cmakeFile = Join-Path $repoRoot 'CMakeLists.txt'
$changelogFile = Join-Path $repoRoot 'CHANGELOG.md'
if (-not (Test-Path -LiteralPath $cmakeFile)) { Write-Host "check-release-tag: 找不到 $cmakeFile" -ForegroundColor Red; exit 1 }
if (-not (Test-Path -LiteralPath $changelogFile)) { Write-Host "check-release-tag: 找不到 $changelogFile" -ForegroundColor Red; exit 1 }
$cmakeVersion = Get-RinneCMakeVersion ([IO.File]::ReadAllText($cmakeFile))
$changelogText = [IO.File]::ReadAllText($changelogFile)

# tag 指向的提交。解析不出来就是 tag 不在仓库里。
$commit = (& git -C $repoRoot rev-parse --verify "$Tag^{commit}" 2>$null | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or -not $commit) {
  Write-Host "check-release-tag: tag '$Tag' 在仓库里解析不到提交" -ForegroundColor Red
  exit 1
}

# prod 祖先：tag 只打在 prod 上。origin/prod 还没取到就报错，不当成「不是祖先」。
& git -C $repoRoot rev-parse --verify --quiet 'origin/prod' *> $null
if ($LASTEXITCODE -ne 0) {
  Write-Host 'check-release-tag: 取不到 origin/prod，先把 prod 分支取下来' -ForegroundColor Red
  exit 1
}
& git -C $repoRoot merge-base --is-ancestor $commit 'origin/prod'
$onProd = ($LASTEXITCODE -eq 0)

# 正式版：源码要与最后一个 rc 相同。rc 的 tag 不看这一条。
$sameSource = $true
$kind = 'final'
if ($Tag -match '^v[0-9]+\.[0-9]+\.[0-9]+-rc\.[0-9]+$') {
  $kind = 'rc'
} else {
  $core = ($Tag -replace '^v', '') -replace '-rc\.[0-9]+$', ''
  $rcTags = @(& git -C $repoRoot tag --list "v$core-rc.*" 2>$null)
  $lastRc = @($rcTags | Sort-Object { [int](($_ -replace '^.*-rc\.', '')) } | Select-Object -Last 1)
  if ($lastRc.Count -eq 0) {
    Write-Host "check-release-tag: 找不到 $core 的最后一个 rc，正式版无从比对源码" -ForegroundColor Red
    exit 1
  }
  & git -C $repoRoot diff --quiet "$($lastRc[0])..$Tag" -- @SourceParityPaths
  $sameSource = ($LASTEXITCODE -eq 0)
}

$r = Test-RinneReleaseGate $Tag $cmakeVersion $onProd $changelogText $sameSource
if (-not $r.Ok) { Write-Host "check-release-tag: $($r.Message)" -ForegroundColor Red; exit 1 }
Write-Host "check-release-tag: $($r.Message)"
if ($env:GITHUB_OUTPUT) {
  # 显式无 BOM：PS 5.1 的 Out-File -Encoding utf8 会带 BOM。
  $out = "version=$($r.Version)`nkind=$($r.Kind)`nprerelease=$(([string]$r.Prerelease).ToLowerInvariant())`n"
  [IO.File]::AppendAllText($env:GITHUB_OUTPUT, $out, (New-Object System.Text.UTF8Encoding($false)))
}
exit 0
