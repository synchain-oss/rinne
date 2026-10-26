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
       tests，以及决定二进制内容的 JUCE 钉版文件 .juce-commit / .juce-version，任一有差异就红），
       只差文档不算差异。rc 不受这一条约束。「最后一个 rc」只认 v<X.Y.Z>-rc.<数字> 这种严格形态、
       提交在 origin/prod 上、数字最大的那个，且必须是正式版提交的祖先（或同一个提交）；
       形态不对的野标签（v1.2.3-rc.x、v1.2.3-rc.1-fix）与不在 prod 上的 rc 一律不当 rc 用。
  判据分两层：
    - 纯函数 Test-RinneReleaseGate：输入是 tag、CMake 版本、是否在 prod 上、CHANGELOG 文本、正式版与
      最后一个 rc 的源码是否相同，输出一个 hashtable。
    - 取数层 Invoke-RinneReleaseTagCheck：参数带仓根，自己去仓库里取那几个事实（git rev-parse、
      git merge-base --is-ancestor、git tag --list、git diff --quiet）。-SelfTest 在本进程新建的临时 git 仓里
      造 prod 上与非 prod 上的提交、rc 与正式版两种 tag、改 src 与只改 docs 的差异，把整个取数层走一遍；
      取数层的判据（退出码映射、rc 查找、比对范围）删掉任一条都有用例变红。
  tag 是 CI 里被推送者给的字符串：先过形态判据，再交给 git。
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
# .juce-commit / .juce-version 是决定二进制内容的钉版文件（升级 JUCE 是许可事件），所以也算源码。
# packaging/ 与 scripts/ 不在里面：发正式版允许改打包脚本。
$SourceParityPaths = @('src', 'web', 'cmake', 'CMakeLists.txt', 'third_party', 'tests', '.juce-commit', '.juce-version')

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

<# 在仓库里跑一条 git，返回 @{ Code; Out }。stderr 丢掉：判据只看退出码与标准输出。 #>
function Invoke-RinneTagGit([string]$RepoRoot, [string[]]$GitArgs) {
  $out = & git -C $RepoRoot @GitArgs 2>$null
  return @{ Code = $LASTEXITCODE; Out = (($out | Out-String).Trim()) }
}

<#
  merge-base --is-ancestor 的退出码映射：0 是祖先，1 不是，别的（128：提交不认识、仓库坏了）是取数失败。
  取数失败不能当成「不是祖先」混过去，也不能当成「是祖先」：返回 $null，调用方报错。
#>
function Get-RinneAncestorState([string]$RepoRoot, [string]$Commit, [string]$Ref) {
  $g = Invoke-RinneTagGit $RepoRoot @('merge-base', '--is-ancestor', $Commit, $Ref)
  if ($g.Code -eq 0) { return $true }
  if ($g.Code -eq 1) { return $false }
  return $null
}

<#
  正式版比对用的 rc 标签名：只认 v<核心版本>-rc.<数字> 的严格形态，数字按数值从大到小排（rc.10 在 rc.9 前面）。
  同前缀的野标签（v1.2.3-rc.x、v1.2.3-rc.1-fix）不是 rc，不进结果。纯函数：输入标签名列表。
#>
function Get-RinneRcTags([string[]]$TagNames, [string]$Core) {
  $re = '^v' + [regex]::Escape($Core) + '-rc\.([0-9]+)\z'
  $found = @(foreach ($t in $TagNames) {
      $m = [regex]::Match([string]$t, $re)
      if ($m.Success) { [pscustomobject]@{ Name = [string]$t; N = [System.Numerics.BigInteger]::Parse($m.Groups[1].Value) } }
    })
  return @($found | Sort-Object -Property N -Descending | ForEach-Object { $_.Name })
}

<#
  发版门禁的取数层：在 RepoRoot 里把几个事实取出来，交给纯函数判据。
  返回 hashtable：Ok / Version / Kind / Prerelease / Message / Notes（给人看的附注，例如被忽略的野标签）。
  任何取数失败（tag 解析不到、origin/prod 取不到、git 报错）都是红，不会当成「不在 prod 上」或「源码相同」混过去。
#>
function Invoke-RinneReleaseTagCheck {
  param(
    [Parameter(Mandatory)][string]$RepoRoot,
    [Parameter(Mandatory)][string]$Tag
  )
  $notes = New-Object System.Collections.Generic.List[string]
  $fail = {
    param([string]$Message)
    return @{ Ok = $false; Version = $null; Kind = $null; Prerelease = $false; Message = $Message; Notes = @($notes) }
  }
  $cmakeFile = Join-Path $RepoRoot 'CMakeLists.txt'
  $changelogFile = Join-Path $RepoRoot 'CHANGELOG.md'
  if (-not (Test-Path -LiteralPath $cmakeFile)) { return (& $fail "找不到 $cmakeFile") }
  if (-not (Test-Path -LiteralPath $changelogFile)) { return (& $fail "找不到 $changelogFile") }
  $cmakeVersion = Get-RinneCMakeVersion ([IO.File]::ReadAllText($cmakeFile))
  $changelogText = [IO.File]::ReadAllText($changelogFile)

  # 形态先于一切 git 调用：tag 是推送者给的字符串，不合形态的不交给 git。
  $shape = Test-RinneReleaseTag $Tag $cmakeVersion
  if (-not $shape.Ok) { return (& $fail $shape.Message) }

  # tag 指向的提交（轻量与附注 tag 都由 ^{commit} 解到提交）。用 refs/tags/ 限定，免得与同名分支混淆。
  $g = Invoke-RinneTagGit $RepoRoot @('rev-parse', '--verify', '--quiet', "refs/tags/$Tag^{commit}")
  if ($g.Code -ne 0 -or $g.Out -notmatch '^[0-9a-f]{40}$') { return (& $fail "tag '$Tag' 在仓库里解析不到提交") }
  $commit = $g.Out

  # CMake 与 CHANGELOG 读的是工作树，工作树必须就是被 tag 的那个提交。
  $g = Invoke-RinneTagGit $RepoRoot @('rev-parse', '--verify', '--quiet', 'HEAD^{commit}')
  if ($g.Code -ne 0 -or $g.Out -ne $commit) {
    return (& $fail "检出的 HEAD 不是 tag '$Tag' 指向的提交 $commit：CMakeLists.txt 与 CHANGELOG.md 读的是工作树，要先检出这个 tag")
  }

  # origin/prod 还没取到就报错，不当成「不是祖先」。
  $g = Invoke-RinneTagGit $RepoRoot @('rev-parse', '--verify', '--quiet', 'refs/remotes/origin/prod^{commit}')
  if ($g.Code -ne 0 -or $g.Out -notmatch '^[0-9a-f]{40}$') { return (& $fail '取不到 origin/prod，先把 prod 分支取下来') }
  $prodRef = 'refs/remotes/origin/prod'

  $anc = Get-RinneAncestorState $RepoRoot $commit $prodRef
  if ($null -eq $anc) { return (& $fail "git merge-base --is-ancestor 报错，判不出 $commit 是不是 origin/prod 的祖先") }
  $onProd = $anc

  # 正式版：源码要与最后一个 rc 相同。rc 的 tag 不看这一条。
  $sameSource = $true
  if ($shape.Kind -eq 'final') {
    $g = Invoke-RinneTagGit $RepoRoot @('tag', '--list', "v$($shape.Core)-rc.*")
    if ($g.Code -ne 0) { return (& $fail 'git tag --list 报错，列不出 rc 标签') }
    $names = @($g.Out -split "`r?`n" | Where-Object { $_ })
    $strict = @(Get-RinneRcTags $names $shape.Core)
    foreach ($n in $names) {
      if ($strict -notcontains $n) { $notes.Add("忽略形态不对的标签 '$n'（不是 v$($shape.Core)-rc.<数字>）") }
    }
    $lastRc = $null
    $lastRcCommit = $null
    foreach ($n in $strict) {
      $g = Invoke-RinneTagGit $RepoRoot @('rev-parse', '--verify', '--quiet', "refs/tags/$n^{commit}")
      if ($g.Code -ne 0 -or $g.Out -notmatch '^[0-9a-f]{40}$') { return (& $fail "rc 标签 '$n' 解析不到提交") }
      $rcAnc = Get-RinneAncestorState $RepoRoot $g.Out $prodRef
      if ($null -eq $rcAnc) { return (& $fail "git merge-base --is-ancestor 报错，判不出 '$n' 是不是 origin/prod 的祖先") }
      if (-not $rcAnc) { $notes.Add("忽略不在 origin/prod 上的 rc 标签 '$n'"); continue }
      $lastRc = $n
      $lastRcCommit = $g.Out
      break
    }
    if (-not $lastRc) { return (& $fail "找不到 $($shape.Core) 在 origin/prod 上的最后一个 rc，正式版无从比对源码") }
    $rcIsBefore = Get-RinneAncestorState $RepoRoot $lastRcCommit $commit
    if ($null -eq $rcIsBefore) { return (& $fail "git merge-base --is-ancestor 报错，判不出 '$lastRc' 是不是正式版提交的祖先") }
    if (-not $rcIsBefore) { return (& $fail "最后一个 rc '$lastRc' 不在正式版提交的历史里：正式版要从它往后发") }
    # diff --quiet：0 相同，1 有差异，别的是 git 出错（出错不能当成相同）。
    $d = Invoke-RinneTagGit $RepoRoot (@('diff', '--quiet', $lastRcCommit, $commit, '--') + $SourceParityPaths)
    if ($d.Code -eq 0) { $sameSource = $true }
    elseif ($d.Code -eq 1) { $sameSource = $false }
    else { return (& $fail "git diff 报错（退出码 $($d.Code)），比对不了 '$lastRc' 与正式版的源码") }
    $notes.Add("最后一个 rc 是 '$lastRc'")
  }

  $r = Test-RinneReleaseGate $Tag $cmakeVersion $onProd $changelogText $sameSource
  $r.Notes = @($notes)
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

  # ── 取数层：在本进程新建的临时 git 仓里把整个 Invoke-RinneReleaseTagCheck 走一遍 ──────────────
  # 上面的格子都是喂布尔值的纯函数，管不到真正出错的地方：merge-base 的退出码映射、git tag 的 rc 查找与排序、
  # git diff 的路径范围。这里造真实的提交与 tag，把 origin/prod 用 update-ref 摆好，再调取数层。
  # 红格不只看 Ok：还要看原因（Message 里的关键词），免得它因为别的原因红而放过了本该盯的那条判据。
  Import-Module (Join-Path $PSScriptRoot 'lib/TempDir.psm1') -Force -ErrorAction Stop

  function Invoke-FxGit([string]$Repo, [string[]]$GitArgs) {
    $out = & git -C $Repo -c user.name=fixture -c user.email=fixture@example.invalid -c commit.gpgsign=false -c tag.gpgsign=false -c core.autocrlf=false -c core.safecrlf=false @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "fixture: git $($GitArgs -join ' ') 失败：$(($out | Out-String).Trim())" }
    return (($out | Out-String).Trim())
  }
  function New-FxRepo([string]$Name) {
    $repo = Join-Path $script:fxRoot $Name
    $null = New-Item -ItemType Directory -Path $repo
    $null = Invoke-FxGit $repo @('init', '-q', '-b', 'prod')
    return $repo
  }
  function Add-FxCommit([string]$Repo, [hashtable]$Files) {
    foreach ($k in $Files.Keys) {
      $p = Join-Path $Repo $k
      $null = New-Item -ItemType Directory -Path (Split-Path -Parent $p) -Force
      [IO.File]::WriteAllText($p, $Files[$k], (New-Object System.Text.UTF8Encoding($false)))
    }
    $null = Invoke-FxGit $Repo @('add', '-A')
    $null = Invoke-FxGit $Repo @('commit', '-q', '--allow-empty', '-m', 'fixture')
    return (Invoke-FxGit $Repo @('rev-parse', 'HEAD'))
  }
  function New-FxTag([string]$Repo, [string]$Name, [string]$Commit, [switch]$Annotated) {
    if ($Annotated) { $null = Invoke-FxGit $Repo @('tag', '-a', '-m', 'fixture', $Name, $Commit) }
    else { $null = Invoke-FxGit $Repo @('tag', $Name, $Commit) }
  }
  function Set-FxProd([string]$Repo, [string]$Commit) { $null = Invoke-FxGit $Repo @('update-ref', 'refs/remotes/origin/prod', $Commit) }
  function Get-FxBase([string]$CMakeVersion = '1.2.3', [string]$Changelog = '') {
    if (-not $Changelog) {
      $Changelog = "## [Unreleased]`n`n" + ((1..12 | ForEach-Object { "## [1.2.3-rc.$_]`n`n- rc $_`n" }) -join "`n") + "`n## [1.2.3]`n`n- final`n"
    }
    return @{
      'CMakeLists.txt' = "cmake_minimum_required(VERSION 3.22)`nproject(Rinne VERSION $CMakeVersion LANGUAGES CXX)`n"
      'CHANGELOG.md'   = $Changelog
      'src/core/a.cpp' = "int a() { return 1; }`n"
      'docs/guide.md'  = "guide v1`n"
      '.juce-commit'   = ('a' * 40) + "`n"
      '.juce-version'  = "8.0.8`n"
    }
  }

  # 正式版的源码「会不会判不同」的夹具：在 c1 里改这个文件，正式版就该与 rc.1 不同。键是 $SourceParityPaths 里的路径。
  # 这张表是自测自己写的，不从 $SourceParityPaths 生成：生成的话，清空 $SourceParityPaths 会连带清空这些格子，自测照样全绿。
  $parityTouch = [ordered]@{
    'src'            = @{ 'src/core/a.cpp' = "int a() { return 2; }`n" }
    'web'            = @{ 'web/index.html' = "<p>changed</p>`n" }
    'cmake'          = @{ 'cmake/Mac.cmake' = "# changed`n" }
    'CMakeLists.txt' = @{ 'CMakeLists.txt' = "cmake_minimum_required(VERSION 3.22)`nproject(Rinne VERSION 1.2.3 LANGUAGES CXX)`n# changed`n" }
    'third_party'    = @{ 'third_party/x/y.txt' = "changed`n" }
    'tests'          = @{ 'tests/t.cpp' = "// changed`n" }
    '.juce-commit'   = @{ '.juce-commit' = ('b' * 40) + "`n" }
    '.juce-version'  = @{ '.juce-version' = "8.0.9`n" }
  }
  foreach ($k in $parityTouch.Keys) {
    & $check "比对范围 `$SourceParityPaths 含 '$k'" $true ($SourceParityPaths -contains $k)
  }
  $docsLike = @('docs', 'packaging', 'scripts', 'CHANGELOG.md') | Where-Object { $SourceParityPaths -contains $_ }
  & $check '比对范围不含文档、打包脚本与 scripts（发正式版允许改它们）' $true (@($docsLike).Count -eq 0)

  $tc = [System.Collections.Generic.List[hashtable]]::new()
  # 每格：Name（仓目录名）/ Tag（被检查的 tag）/ Setup（建仓、打 tag、摆 origin/prod）/ WantOk / WantKind / WantMsg（红格的原因关键词）/
  #       KeepHead（Setup 自己摆好 HEAD，运行器不再检出 tag）
  $tc.Add(@{ Name = 'rc-on-prod'; Tag = 'v1.2.3-rc.1'; WantOk = $true; WantKind = 'rc'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0; Set-FxProd $r $c0 } })
  $tc.Add(@{ Name = 'rc-annotated'; Tag = 'v1.2.3-rc.1'; WantOk = $true; WantKind = 'rc'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0 -Annotated; Set-FxProd $r $c0 } })
  # 验收：非 prod 上的 v1.2.3 / rc 必须红。
  $tc.Add(@{ Name = 'rc-off-prod'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = 'origin/prod 的祖先'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); Set-FxProd $r $c0
        $null = Invoke-FxGit $r @('checkout', '-q', '-b', 'side')
        $c1 = Add-FxCommit $r @{ 'docs/guide.md' = "side`n" }; New-FxTag $r 'v1.2.3-rc.1' $c1 } })
  $tc.Add(@{ Name = 'final-off-prod'; Tag = 'v1.2.3'; WantOk = $false; WantMsg = 'origin/prod 的祖先'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0; Set-FxProd $r $c0
        $null = Invoke-FxGit $r @('checkout', '-q', '-b', 'side')
        $c1 = Add-FxCommit $r @{ 'docs/guide.md' = "side`n" }; New-FxTag $r 'v1.2.3' $c1 } })
  # 正式版：rc 在 prod 上、只差文档放行。
  $tc.Add(@{ Name = 'final-docs-only'; Tag = 'v1.2.3'; WantOk = $true; WantKind = 'final'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
        $c1 = Add-FxCommit $r @{ 'docs/guide.md' = "guide v2`n"; 'CHANGELOG.md' = (Get-FxBase)['CHANGELOG.md'] + "`n- doc`n" }
        Set-FxProd $r $c1; New-FxTag $r 'v1.2.3' $c1 } })
  $tc.Add(@{ Name = 'final-packaging-scripts-only'; Tag = 'v1.2.3'; WantOk = $true; WantKind = 'final'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
        $c1 = Add-FxCommit $r @{ 'packaging/m.txt' = "m`n"; 'scripts/p.ps1' = "p`n" }
        Set-FxProd $r $c1; New-FxTag $r 'v1.2.3' $c1 } })
  $tc.Add(@{ Name = 'final-same-commit-as-rc'; Tag = 'v1.2.3'; WantOk = $true; WantKind = 'final'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0; New-FxTag $r 'v1.2.3' $c0 -Annotated; Set-FxProd $r $c0 } })
  # 比对范围里每一条路径：只改它一个，正式版就该判红。
  foreach ($key in $parityTouch.Keys) {
    $files = $parityTouch[$key]
    $tc.Add(@{ Name = "final-differs-$($key -replace '[^A-Za-z0-9]', '_')"; Tag = 'v1.2.3'; WantOk = $false; WantMsg = '源码与最后一个 rc 不一致'
        Setup = {
          param($r)
          $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
          $c1 = Add-FxCommit $r $files
          Set-FxProd $r $c1; New-FxTag $r 'v1.2.3' $c1
        }.GetNewClosure() })
  }
  $tc.Add(@{ Name = 'final-no-rc'; Tag = 'v1.2.3'; WantOk = $false; WantMsg = '最后一个 rc'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); Set-FxProd $r $c0; New-FxTag $r 'v1.2.3' $c0 } })
  # 野 rc：不在 prod 上的 rc.2，源码恰与正式版一致 —— 不能拿它当「最后一个 rc」混过去。
  $tc.Add(@{ Name = 'final-rogue-rc-off-prod'; Tag = 'v1.2.3'; WantOk = $false; WantMsg = '源码与最后一个 rc 不一致'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
        $c1 = Add-FxCommit $r @{ 'src/core/a.cpp' = "int a() { return 2; }`n" }; Set-FxProd $r $c1; New-FxTag $r 'v1.2.3' $c1
        $null = Invoke-FxGit $r @('checkout', '-q', '-b', 'side', $c1)
        $s1 = Add-FxCommit $r @{ 'docs/guide.md' = "rogue`n" }; New-FxTag $r 'v1.2.3-rc.2' $s1 } })
  # 形态不对的野标签：不当 rc 用，也不能让脚本崩。
  $tc.Add(@{ Name = 'final-stray-rc-names'; Tag = 'v1.2.3'; WantOk = $false; WantMsg = '源码与最后一个 rc 不一致'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
        $c1 = Add-FxCommit $r @{ 'src/core/a.cpp' = "int a() { return 2; }`n" }; Set-FxProd $r $c1
        New-FxTag $r 'v1.2.3-rc.x' $c1; New-FxTag $r 'v1.2.3-rc.1-fix' $c1; New-FxTag $r 'v1.2.3' $c1 } })
  $tc.Add(@{ Name = 'final-only-stray-rc'; Tag = 'v1.2.3'; WantOk = $false; WantMsg = '最后一个 rc'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); Set-FxProd $r $c0; New-FxTag $r 'v1.2.3-rc.x' $c0; New-FxTag $r 'v1.2.3' $c0 } })
  # rc 按数值排：rc.10 比 rc.9 新。按字典序排会选中 rc.9，与 rc.10 的源码比对就错了。
  $tc.Add(@{ Name = 'final-rc-numeric-order'; Tag = 'v1.2.3'; WantOk = $true; WantKind = 'final'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.9' $c0
        $c1 = Add-FxCommit $r @{ 'src/core/a.cpp' = "int a() { return 2; }`n" }; New-FxTag $r 'v1.2.3-rc.10' $c1
        Set-FxProd $r $c1; New-FxTag $r 'v1.2.3' $c1 } })
  # 最后一个 rc 在 prod 上，却比正式版的提交还新：正式版不是从它往后发的。
  $tc.Add(@{ Name = 'final-rc-newer-than-final'; Tag = 'v1.2.3'; WantOk = $false; WantMsg = '不在正式版提交的历史里'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
        $c1 = Add-FxCommit $r @{ 'docs/guide.md' = "v2`n" }; New-FxTag $r 'v1.2.3' $c1
        $c2 = Add-FxCommit $r @{ 'docs/guide.md' = "v3`n" }; New-FxTag $r 'v1.2.3-rc.2' $c2; Set-FxProd $r $c2 } })
  # 取数失败不能混过去。
  $tc.Add(@{ Name = 'no-origin-prod'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = '取不到 origin/prod'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0 } })
  $tc.Add(@{ Name = 'tag-missing'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = '解析不到提交'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase); Set-FxProd $r $c0 } })
  $tc.Add(@{ Name = 'head-not-tag'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = '检出的 HEAD 不是'; KeepHead = $true
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); New-FxTag $r 'v1.2.3-rc.1' $c0
        $c1 = Add-FxCommit $r @{ 'docs/guide.md' = "later`n" }; Set-FxProd $r $c1 } })
  # 同名分支不能顶替 tag。
  $tc.Add(@{ Name = 'branch-named-like-tag'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = '解析不到提交'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase); Set-FxProd $r $c0
        $null = Invoke-FxGit $r @('branch', 'v1.2.3-rc.1', $c0) } })
  # 读的是被 tag 的提交里的 CMake / CHANGELOG。
  $tc.Add(@{ Name = 'changelog-section-missing'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = 'CHANGELOG'
      Setup = { param($r)
        $c0 = Add-FxCommit $r (Get-FxBase -Changelog "## [Unreleased]`n`n## [1.2.3]`n`n- final`n"); New-FxTag $r 'v1.2.3-rc.1' $c0; Set-FxProd $r $c0 } })
  $tc.Add(@{ Name = 'cmake-version-differs'; Tag = 'v1.2.3-rc.1'; WantOk = $false; WantMsg = '不一致'
      Setup = { param($r) $c0 = Add-FxCommit $r (Get-FxBase -CMakeVersion '1.2.4'); New-FxTag $r 'v1.2.3-rc.1' $c0; Set-FxProd $r $c0 } })

  $script:fxRoot = New-RinneTempDir -Prefix 'rinne-tagcheck-selftest-'
  try {
    foreach ($c in $tc) {
      $repo = New-FxRepo $c.Name
      & $c.Setup $repo
      if (-not $c.ContainsKey('KeepHead')) {
        # 检出被检查的 tag（detached）。tag 不存在的格子检出不了，留在原地，让取数层自己报「解析不到提交」。
        & git -C $repo checkout -q --detach "refs/tags/$($c.Tag)" 2>$null
        $global:LASTEXITCODE = 0
      }
      $r = Invoke-RinneReleaseTagCheck -RepoRoot $repo -Tag $c.Tag
      $bad = ($r.Ok -ne $c.WantOk)
      if (-not $bad -and $c.WantOk) { $bad = ($r.Kind -ne $c.WantKind) }
      if (-not $bad -and -not $c.WantOk) { $bad = ($r.Message -notmatch [regex]::Escape($c.WantMsg)) }
      & $check ("取数层[{0}] tag={1} -> Ok={2} Kind={3} {4}" -f $c.Name, $c.Tag, $r.Ok, $r.Kind, $r.Message) $false $bad
    }

    # merge-base 报错（提交不认识，退出码 128）既不是「祖先」也不是「不是祖先」：必须是 $null。
    $repo = New-FxRepo 'ancestor-error'
    $c0 = Add-FxCommit $repo (Get-FxBase); Set-FxProd $repo $c0
    & $check 'merge-base 退出码映射：祖先 -> true' $true ((Get-RinneAncestorState $repo $c0 'refs/remotes/origin/prod') -eq $true)
    $c1 = Add-FxCommit $repo @{ 'docs/guide.md' = "x`n" }
    & $check 'merge-base 退出码映射：不是祖先（1）-> false' $true ((Get-RinneAncestorState $repo $c1 'refs/remotes/origin/prod') -eq $false)
    & $check 'merge-base 退出码映射：报错（128）-> $null，不当成 false' $true ($null -eq (Get-RinneAncestorState $repo ('0' * 40) 'refs/remotes/origin/prod'))
  } finally {
    $left = Remove-RinneTempDir -Path $script:fxRoot -WaitSeconds 5
    if ($left) { Write-Host "  [warn] 自测临时目录没删掉：$left" -ForegroundColor Yellow }
  }

  # 野标签的纯函数：只认严格形态，按数值降序。
  $rcNames = @('v1.2.3-rc.2', 'v1.2.3-rc.10', 'v1.2.3-rc.x', 'v1.2.3-rc.1-fix', 'v1.2.3-rc.', 'v1.2.30-rc.99', 'v1.2.3-rc.9')
  $rcSorted = @(Get-RinneRcTags $rcNames '1.2.3')
  & $check 'rc 标签挑选：只留严格形态并按数值降序（rc.10、rc.9、rc.2）' $true (($rcSorted -join ',') -eq 'v1.2.3-rc.10,v1.2.3-rc.9,v1.2.3-rc.2')
  & $check 'rc 标签挑选：没有合格标签时返回空' $true (@(Get-RinneRcTags @('v1.2.3-rc.x') '1.2.3').Count -eq 0)

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
$r = Invoke-RinneReleaseTagCheck -RepoRoot $repoRoot -Tag $Tag
foreach ($n in @($r.Notes)) { Write-Host "check-release-tag: $n" }
if (-not $r.Ok) { Write-Host "check-release-tag: $($r.Message)" -ForegroundColor Red; exit 1 }
Write-Host "check-release-tag: $($r.Message)"
if ($env:GITHUB_OUTPUT) {
  # 显式无 BOM：PS 5.1 的 Out-File -Encoding utf8 会带 BOM。
  $out = "version=$($r.Version)`nkind=$($r.Kind)`nprerelease=$(([string]$r.Prerelease).ToLowerInvariant())`n"
  [IO.File]::AppendAllText($env:GITHUB_OUTPUT, $out, (New-Object System.Text.UTF8Encoding($false)))
}
exit 0
