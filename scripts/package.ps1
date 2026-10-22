#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/release.json
<#
.SYNOPSIS  Rinne 发行打包（Windows x64）：把两个 .vst3 bundle 与合规文件组打成一个 zip，
           另出独立的 .sha256 与 package-summary-win64.md。发布流水线只调用本脚本。
.DESCRIPTION
  发行包的内容与判据：
    ① 名字由版本号算出（-Version，缺省读 CMakeLists.txt 的 project(Rinne VERSION x.y.z)），不写版本号字面量。
    ② 按目录枚举 *.vst3 bundle，断言**恰好 2 个**且名字恰好是 Rinne Input / Rinne Output；
       探针身份的 bundle（Rinne Probe *）一律拒：发行包里不能有探针。
       打包保住 `<name>.vst3/Contents/x86_64-win/<name>` 层级。
    ③ .sha256 是独立文件（sha256sum 格式，`sha256sum -c` 可直接校验），LF 行尾、UTF-8 无 BOM；
       package-summary-win64.md 含 version / tag / 源码 commit / juceCommit / 文件名 / 字节数 / sha256 /
       zip 内逐条目哈希。summary 里不写构建时间：同一份输入重打，字节必须一致。
    ④ INSTALL.txt 由文档生成器渲染（`gen-docs --emit install-txt`），不在这里抄第二份；
       渲染后断言 GPLv3 第 6 条 (d) 的对应源码指引那几行在（tag、源码 commit、JUCE 的版本与 40 位 commit、
       WebView2 SDK 版本、构建说明的路径）。
    ⑤ zip 根目录带 LICENSE（仓库 GPLv3 全文）、THIRD-PARTY-NOTICES.md、LICENSES/ 全部许可证全文、
       third_party/notices/ 全部上游声明原文（与仓库同一相对路径）、INSTALL.txt、SHA256SUMS。
       「随二进制分发」表点名的每个许可证 + 本项目的 GPL-3.0-or-later 都必须在 LICENSES/ 里有全文，
       缺就红；反过来 LICENSES/ 里的每份全文也必须被表点名，多出来就红。
       NOTICES 全文里点名的每个声明文件路径必须是仓库里存在、且在打包范围内的文件。
    ⑥ 打包后重新打开 zip 逐条断言：条目数与待打包文件数相等、每个条目的字节与源文件一致、
       NOTICES 点名的每个声明路径在 zip 里、根条目集合与打包清单相同、没有任何条目带本机路径、
       .sha256 是 LF。
  确定性：条目按序数排序写盘，时间戳统一取 SOURCE_DATE_EPOCH > HEAD 提交时间 > 1980-01-01，
  每个条目的权限位写死，所以同一份输入在同一运行时下重打，zip 的 sha256 不变。
  不同 .NET 运行时（PS 5.1 与 7）的 deflate 实现不同，跨运行时不保证字节一致。
  macOS 包由 scripts/package-macos.sh 打，但合规判据与打包清单**仍只在本脚本**：
  它调 `-Preflight -PreflightOut <文件>`，本脚本把 preflight 核过的结果写成机读清单，sh 只读这份清单，
  不在 sh 里重写第二套解析。不传 -PreflightOut 时行为与输出逐字不变。
.EXAMPLE   pwsh scripts/package.ps1 -Version 0.9.0 -BuildDir build -OutDir dist
.EXAMPLE   pwsh scripts/package.ps1 -Preflight -PreflightOut <清单>
.EXAMPLE   pwsh scripts/package.ps1 -SelfTest
#>
[CmdletBinding()]
param(
  # 缺省 = CMakeLists.txt 的 project(Rinne VERSION x.y.z)。带 -rc.N 时由发布流水线传整串（不含 v）。
  [string]$Version,
  # 缺省 = "v$Version"。对应源码指引与手册链接都钉在这个 tag 上。
  [string]$Tag,
  [string]$BuildDir = 'build',
  [string]$OutDir = 'dist',
  # 缺省 = 仓库 HEAD。只写进 INSTALL.txt / summary 作溯源，不参与判定。
  [string]$SourceCommit,
  # 打包清单：zip 的根条目清单，一行一个。
  [string]$Manifest = 'packaging/manifest-win64.txt',
  # 工具的可执行文件名按平台取；Windows 只有 .exe。
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
  # 带 -Kit 才把 tools/rinne_nulltest 打进包（发行包里没有它）。
  [switch]$Kit,
  # kit 包的名字，形如 Rinne-kit-win64-<kit 版本>-<40 位 commit>。带 -Kit 时给。
  [string]$KitName = '',
  # 构建的运行链接与验证结论，只写进 summary（发布流水线传；本地打包留空）。
  [string]$BuildRun = '',
  [string]$Validation = '',
  # 只跑不依赖构建产物的检查就退出（见下方 Preflight 段）。
  [switch]$Preflight,
  # 只能配合 -Preflight：把 preflight 核过的结果写成机读清单，给 scripts/package-macos.sh 用。
  [string]$PreflightOut,
  # 判据自测：坏样例与正例都跑一遍。
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
. (Join-Path $PSScriptRoot 'lib/release-version.ps1')
Import-Module (Join-Path $PSScriptRoot 'lib/TempDir.psm1') -Force -ErrorAction Stop

$RepoRoot = Split-Path -Parent $PSScriptRoot
$RepoSlug = 'synchain-oss/rinne'
$ExpectedBundles = @('Rinne Input.vst3', 'Rinne Output.vst3')
$KitToolNames = @('rinne_nulltest.exe', 'rinne_nulltest')
# zip 的 DOS 时间只有两秒粒度，且只到 1980 年：取整到偶数秒，早于下限就抬到下限。
$ZipEpochFloor = 315532800
# 本进程经 New-RinneTempDir 建过的临时目录前缀（删除前的纯函数核对用）。
$script:OwnedTempPrefixes = New-Object System.Collections.Generic.List[string]

class RinnePackageError : Exception {
    # 显式写构造器：PowerShell 类默认只生成无参构造器，Exception 的带消息构造器不会被继承。
    RinnePackageError([string]$Message) : base($Message) { }
}

function Stop-RinnePackage([string]$Message) { throw [RinnePackageError]::new($Message) }

# 相对路径按调用者的当前位置解析（绝对路径原样返回）。
function Resolve-RinneFull([string]$p) {
  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p)
}

function Get-RinneSha256([string]$path) {
  return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-RinneStreamSha256([System.IO.Stream]$s) {
  $h = [System.Security.Cryptography.SHA256]::Create()
  try { return ([BitConverter]::ToString($h.ComputeHash($s)) -replace '-', '').ToLowerInvariant() }
  finally { $h.Dispose() }
}

# 序数排序：不依赖文件系统枚举顺序，也不依赖 locale。
function Get-RinneOrdinalSort([string[]]$items) {
  $a = [string[]]@($items)
  [Array]::Sort($a, [StringComparer]::Ordinal)
  return $a
}

# ── 判据用的纯函数（吃文本与名字列表，不碰文件系统） ───────────────────────────
#
# 这些纯函数一律把结果**写到管道上**（不return数组，而是让数组自己流出去），调用方一律用 @() 包住：
#   @() 作用在 List[object] 上会抛「Argument types do not match」（PowerShell 7.6），所以内部用 .ToArray()；
#   逗号技巧（return ,$arr）会与调用方的 @() 叠成两层数组（取 .Count 得到 1，取 [0] 得到整个数组），所以不用。
# 取 .Count 时必须写成 @(F).Count：@(F) 恒是数组（空时 0 项、单元素时 1 项）。

# 一个 Markdown 表格：按表头定位列，返回行数组，每行是 @{ Component; Spdx; Notices }。
function ConvertFrom-RinneTable([string[]]$Lines) {
  $at = -1
  for ($i = 0; $i -lt $Lines.Count - 1; $i++) {
    if ($Lines[$i] -match '^\s*\|' -and $Lines[$i + 1] -match '^\s*\|[\s:|-]+\|\s*$') { $at = $i; break }
  }
  if ($at -lt 0) { return }
  $head = @($Lines[$at].Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim().ToLowerInvariant() })
  $ci = -1; $si = -1; $ni = -1
  for ($c = 0; $c -lt $head.Count; $c++) {
    if ($ci -lt 0 -and $head[$c].StartsWith('component')) { $ci = $c }
    if ($si -lt 0 -and $head[$c].StartsWith('spdx')) { $si = $c }
    if ($ni -lt 0 -and $head[$c].StartsWith('notice')) { $ni = $c }
  }
  if ($ci -lt 0 -or $si -lt 0) { return }
  $rows = New-Object System.Collections.Generic.List[object]
  for ($i = $at + 2; $i -lt $Lines.Count; $i++) {
    if ($Lines[$i] -notmatch '^\s*\|') { break }
    $cells = @($Lines[$i].Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
    $spdx = New-Object System.Collections.Generic.List[string]
    if ($si -lt $cells.Count) {
      foreach ($m in [regex]::Matches($cells[$si], '`([^`]+)`')) {
        foreach ($id in ($m.Groups[1].Value -split '[,;\s]+' | Where-Object { $_ })) {
          if (-not $spdx.Contains($id)) { $spdx.Add($id) }
        }
      }
    }
    $notices = New-Object System.Collections.Generic.List[string]
    if ($ni -ge 0 -and $ni -lt $cells.Count) {
      foreach ($m in [regex]::Matches($cells[$ni], '`([^`]+)`')) {
        $p = $m.Groups[1].Value
        if ($p.StartsWith('third_party/notices/')) { $notices.Add($p) }
      }
    }
    $rows.Add([pscustomobject]@{ Component = $cells[$ci]; Spdx = @($spdx); Notices = @($notices) })
  }
  # ToArray 而不是 @()：PowerShell 7.6 上 @() 作用在 List[object] 上会抛「Argument types do not match」。
  $rows.ToArray()
}

# NOTICES 里的两张表：「随二进制分发」表在 shipped-table 小节里，「只用于构建」表在
# "Build, test and CI only" 标题下。
function Get-RinneNoticeRows([string]$NoticesText) {
  $lines = @($NoticesText -split "`r?`n")
  $shipped = New-Object System.Collections.Generic.List[string]
  $inShipped = $false
  foreach ($l in $lines) {
    $t = $l.Trim()
    if ($t -eq '<!-- section:shipped-table -->') { $inShipped = $true; continue }
    if ($t -eq '<!-- /section:shipped-table -->') { $inShipped = $false; continue }
    if ($inShipped) { $shipped.Add($l) }
  }
  $build = New-Object System.Collections.Generic.List[string]
  $inBuild = $false
  foreach ($l in $lines) {
    if ($l -match '^##\s.*\bBuild, test and CI only\b') { $inBuild = $true; continue }
    if ($inBuild -and $l -match '^##\s') { $inBuild = $false }
    if ($inBuild) { $build.Add($l) }
  }
  # 表里的行各自是数组：@() 包住，表头缺失时是空数组而不是 $null。
  $shippedRows = @(ConvertFrom-RinneTable ([string[]]$shipped))
  $buildRows = @(ConvertFrom-RinneTable ([string[]]$build))
  return @{ Shipped = $shippedRows; Build = $buildRows }
}

# NOTICES 全文里点名的声明文件路径（third_party/notices/... 与 LICENSES/...），逐字区分大小写。
function Get-RinneCitedPaths([string]$NoticesText) {
  $out = New-Object System.Collections.Generic.List[string]
  foreach ($m in [regex]::Matches($NoticesText, '(?<![\w./-])((?:third_party/notices|LICENSES)/[A-Za-z0-9._+/-]+)')) {
    $p = $m.Groups[1].Value.TrimEnd('.')
    if ($p.EndsWith('/')) { continue }
    if (-not $out.Contains($p)) { $out.Add($p) }
  }
  $out.ToArray()
}

# 许可的双向闭合 + NOTICES 点名的声明路径。纯函数：吃文本与三个名字列表，返回问题数组。
#   LicenseNames：LICENSES/ 下的文件名（顶层）。
#   NoticeNames：third_party/notices/ 下的相对路径（正斜杠）。
#   PackedPaths：这次打进 zip 的路径（LICENSES/<名> 与 third_party/notices/... ）。
function Get-RinneLicenseProblems([string]$NoticesText, [string[]]$LicenseNames, [string[]]$NoticeNames, [string[]]$PackedPaths) {
  $out = New-Object System.Collections.Generic.List[string]
  $tables = Get-RinneNoticeRows $NoticesText
  if (-not $tables.Shipped.Count) { $out.Add('NOTICES 里没读到「随二进制分发」表的任何一行（标题或表格式变了？）') }
  $referenced = New-Object System.Collections.Generic.List[string]
  $referenced.Add('GPL-3.0-or-later')
  foreach ($r in @($tables.Shipped) + @($tables.Build)) {
    foreach ($id in $r.Spdx) { if (-not $referenced.Contains($id)) { $referenced.Add($id) } }
  }
  # 表里点名的每个标识都要有全文。
  foreach ($id in $referenced) {
    if (-not (@($LicenseNames | Where-Object { $_ -ceq "$id.txt" }).Count)) { $out.Add("NOTICES 用了 $id，LICENSES/ 里没有 $id.txt") }
  }
  # 反方向：LICENSES/ 里的每份全文都要有人点名（多出来是登记错了，不是缺口）。
  foreach ($n in $LicenseNames) {
    $id = [IO.Path]::GetFileNameWithoutExtension($n)
    if (-not $referenced.Contains($id)) { $out.Add("LICENSES/$n 没有被 NOTICES 的任何一行点名（补上那一行，或删掉这份全文）") }
  }
  # NOTICES 点名的声明文件：仓库里要有，且在打包范围内。
  $repo = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::Ordinal)
  foreach ($n in $LicenseNames) { [void]$repo.Add("LICENSES/$n") }
  foreach ($n in $NoticeNames) { [void]$repo.Add("third_party/notices/$n") }
  $packed = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::Ordinal)
  foreach ($p in $PackedPaths) { [void]$packed.Add($p) }
  foreach ($p in @(Get-RinneCitedPaths $NoticesText)) {
    if ($packed.Contains($p)) { continue }
    if ($repo.Contains($p)) { $out.Add("NOTICES 点名了 $p，它不在打包清单里（打包范围是 LICENSES/ 顶层文件与 third_party/notices/ 全部文件；大小写须与实际文件名一致）") }
    else { $out.Add("NOTICES 点名了 $p，仓库里没有这个文件") }
  }
  $out.ToArray()
}

# zip 的根条目集合。根条目 = zip 里的顶层单元：顶层文件就是文件名，目录单元写成「目录/」。
# 目录单元由打包计划给出（两个 bundle 目录、LICENSES/、third_party/notices/），不在这里猜：
# 只取路径第一段的话，third_party/notices/x 会被算成 third_party/，与清单里的 third_party/notices/ 对不上。
# 不属于任何单元、又带斜杠的条目是野条目（多出来的目录），单独报出来。
function Get-RinneRootEntries([string[]]$EntryNames, [string[]]$DirUnits) {
  $roots = New-Object System.Collections.Generic.List[string]
  $stray = New-Object System.Collections.Generic.List[string]
  foreach ($n in $EntryNames) {
    $hit = $null
    foreach ($d in $DirUnits) {
      if ($n.StartsWith($d, [StringComparison]::Ordinal)) { $hit = $d; break }
    }
    if ($hit) { if (-not $roots.Contains($hit)) { $roots.Add($hit) }; continue }
    if ($n.Contains('/')) { if (-not $stray.Contains($n)) { $stray.Add($n) }; continue }
    if (-not $roots.Contains($n)) { $roots.Add($n) }
  }
  return [pscustomobject]@{
    # 属性里放数组不会被摊平（返回值才会），所以这里不用逗号技巧。
    Roots = [string[]]$roots.ToArray()
    Stray = [string[]]$stray.ToArray()
  }
}

# 打包清单的行（去掉空行与注释行）。纯函数。
function Get-RinneManifestLines([string]$Text) {
  $out = New-Object System.Collections.Generic.List[string]
  foreach ($l in ($Text -split "`r?`n")) {
    $t = $l.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $out.Add($t)
  }
  $out.ToArray()
}

# 清单与 zip 根条目的比对。纯函数：返回问题数组（集合相等且数量相等才算通过）。
function Get-RinneManifestProblems([string[]]$ManifestLines, [string[]]$RootEntries) {
  $out = New-Object System.Collections.Generic.List[string]
  $want = New-Object System.Collections.Generic.List[string]
  foreach ($l in $ManifestLines) {
    $t = $l.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    if (-not $want.Contains($t)) { $want.Add($t) }
  }
  if (-not $want.Count) { $out.Add('打包清单是空的'); return $out.ToArray() }
  $have = New-Object System.Collections.Generic.List[string]
  foreach ($r in $RootEntries) { if (-not $have.Contains($r)) { $have.Add($r) } }
  foreach ($w in $want) { if (-not $have.Contains($w)) { $out.Add("zip 里缺清单上的根条目：$w") } }
  foreach ($h in $have) { if (-not $want.Contains($h)) { $out.Add("zip 里出现清单外的根条目：$h") } }
  if ($want.Count -ne $have.Count) { $out.Add("根条目数量不对：清单 $($want.Count) 条，zip $($have.Count) 条") }
  $out.ToArray()
}

# .sha256 文件的格式：`<64 位小写 hex><两个空格><文件名>` 加一个 LF，全文没有 CR，没有 BOM。
function Get-RinneSha256Problems([string]$Text, [string]$ExpectedName) {
  $out = New-Object System.Collections.Generic.List[string]
  if ($Text -match "`r") { $out.Add('.sha256 里有 CR（必须是 LF 行尾）') }
  if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) { $out.Add('.sha256 里有 BOM') }
  if (-not $Text.EndsWith("`n")) { $out.Add('.sha256 没有以 LF 结尾') }
  $lines = @($Text -split "`n" | Where-Object { $_ -ne '' })
  if ($lines.Count -ne 1) { $out.Add(".sha256 应当只有一行，实际 $($lines.Count) 行") }
  else {
    $m = [regex]::Match($lines[0], '^([0-9a-f]{64})  (.+)$')
    if (-not $m.Success) { $out.Add('.sha256 那一行的格式不对（应是 <64 位小写 hex><两个空格><文件名>）') }
    elseif ($ExpectedName -and $m.Groups[2].Value -cne $ExpectedName) { $out.Add(".sha256 里的文件名是 $($m.Groups[2].Value)，应是 $ExpectedName") }
  }
  $out.ToArray()
}

# 本机路径扫描：读字节按 latin-1 解码，找盘符路径与几个要查的字面量（构建目录、仓根、用户目录）。
# 纯函数：吃字节与字面量，返回命中的片段列表。
function Get-RinneLocalPathHits([byte[]]$Bytes, [string[]]$Literals) {
  $text = [Text.Encoding]::GetEncoding(28591).GetString($Bytes)
  $hits = New-Object System.Collections.Generic.List[string]
  foreach ($lit in $Literals) {
    if (-not $lit) { continue }
    foreach ($form in @($lit, ($lit -replace '\\', '/'))) {
      if (-not $form) { continue }
      if ($text.Contains($form)) { $hits.Add($form) }
    }
  }
  foreach ($m in [regex]::Matches($text, '[A-Za-z]:[\\/](?:[A-Za-z0-9_.$()+ -]{2,}[\\/]){1,}[A-Za-z0-9_.$()+ -]{1,}')) {
    $hits.Add($m.Value)
  }
  $hits.ToArray()
}

# 临时目录能不能删：纯函数。路径必须在系统临时目录下一层、且名字以本进程建过的某个前缀开头。
# 只做判定，不删任何东西。
function Test-RinneRemovableTempPath([string]$Path, [string[]]$OwnedPrefixes) {
  if (-not $Path) { return $false }
  try { $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/') } catch { return $false }
  try { $root = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') } catch { return $false }
  $cmp = if ($IsWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
  if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, $cmp)) { return $false }
  $rest = $full.Substring($root.Length + 1)
  if ($rest -match '[\\/]') { return $false }
  $leaf = [IO.Path]::GetFileName($full)
  foreach ($p in $OwnedPrefixes) { if ($leaf.StartsWith($p, [StringComparison]::Ordinal)) { return $true } }
  return $false
}

# ── 仓库内的真源 ──────────────────────────────────────────────────────────────

# 装机说明里要写的那几条规则（标了 installTxt 的那些）：中文真源在用户指南，英文译文在 i18n 文件。
# 返回 @{ Lead; Body; En; Zh } 的数组。
function Get-RinneInstallRules([string]$Root) {
  $srcPath = Join-Path $Root 'docs/USER_GUIDE.zh-CN.md'
  $i18nPath = Join-Path $Root 'docs/hard-rules.i18n.json'
  if (-not (Test-Path -LiteralPath $srcPath -PathType Leaf)) { Stop-RinnePackage "缺 $srcPath" }
  if (-not (Test-Path -LiteralPath $i18nPath -PathType Leaf)) { Stop-RinnePackage "缺 $i18nPath" }
  $src = [IO.File]::ReadAllLines($srcPath, [Text.Encoding]::UTF8)
  $i18n = [IO.File]::ReadAllText($i18nPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
  $zh = New-Object System.Collections.Generic.List[object]
  $in = $false
  foreach ($raw in $src) {
    $l = $raw.TrimEnd()
    if (-not $in) { if ($l -eq '## 使用前必读') { $in = $true }; continue }
    if ($l -match '^## ') { break }
    $m = [regex]::Match($l, '^([0-9]+)\. \*\*(.+?)\*\*：(.+)$')
    if ($m.Success) { $zh.Add([pscustomobject]@{ Lead = $m.Groups[2].Value; Body = $m.Groups[3].Value }) }
  }
  if (-not $zh.Count) { Stop-RinnePackage '用户指南里没读到「使用前必读」的编号规则' }
  if (@($i18n.rules).Count -ne $zh.Count) { Stop-RinnePackage "译文有 $(@($i18n.rules).Count) 条规则，中文真源有 $($zh.Count) 条" }
  $out = New-Object System.Collections.Generic.List[object]
  for ($i = 0; $i -lt $zh.Count; $i++) {
    $t = $i18n.rules[$i]
    if ($t.installTxt -ne $true) { continue }
    $out.Add([pscustomobject]@{ Lead = $zh[$i].Lead; Body = $zh[$i].Body; En = "$($t.en.lead): $($t.en.body)"; Zh = "$($zh[$i].Lead)：$($zh[$i].Body)" })
  }
  if (-not $out.Count) { Stop-RinnePackage '译文里没有标了 installTxt 的规则' }
  # ToArray 而不是 @()：PowerShell 7.6 上 @() 作用在 List[object] 上会抛「Argument types do not match」。
  $out.ToArray()
}

# ── 文档生成器（渲染 INSTALL.txt） ────────────────────────────────────────────

function Get-RinneNode([string]$Root) {
  $p = $env:RINNE_NODE
  if (-not $p) { $p = ((& git -C $Root config --get rinne.node 2>$null) | Out-String).Trim() }
  if (-not $p) { $p = 'node' }
  return $p
}

# 跑一条命令，返回退出码与输出。参数按数组传，不经 shell 引号。
function Invoke-RinneCommand([string]$Exe, [string[]]$Arguments, [string]$Cwd) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.WorkingDirectory = $Cwd
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  try {
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(180000)) { try { $p.Kill($true) } catch { $null = $_ }; Stop-RinnePackage "$Exe 180 秒没退出" }
    return [pscustomobject]@{ Code = $p.ExitCode; Out = $out.Result; Err = $err.Result }
  } finally { $p.Dispose() }
}

# 渲染 INSTALL.txt。变量缺一个就抛错（生成器自己也会拒）。
function Invoke-RinneInstallTxt([string]$Root, [hashtable]$Vars, [string]$Platform = 'win') {
  $node = Get-RinneNode $Root
  $want = (Get-Content -LiteralPath (Join-Path $Root '.node-version') -Raw).Trim().Split('.')[0]
  $v = Invoke-RinneCommand $node @('--version') $Root
  $have = [regex]::Match($v.Out, '^v(\d+)').Groups[1].Value
  if ($v.Code -ne 0 -or $have -cne $want) { Stop-RinnePackage "需要 Node $want（.node-version），得到 $($have ? $have : '找不到')" }
  $json = ($Vars | ConvertTo-Json -Compress)
  $r = Invoke-RinneCommand $node @('scripts/gen-docs.mjs', '--emit', 'install-txt', '--platform', $Platform, '--vars', $json) $Root
  if ($r.Code -ne 0) { Stop-RinnePackage "渲染 INSTALL.txt 失败（退出码 $($r.Code)）：$($r.Err.Trim())" }
  return $r.Out
}

# 渲染逐文件来源表（gen-docs --emit provenance）：发行包里那份 NOTICES 的「从哪来」一节由它填。
# 表里逐个文件写上游仓、上游提交、上游路径与改写方式；渲染不出来就抛错（不打包半张表）。
function Invoke-RinneProvenance([string]$Root) {
  $node = Get-RinneNode $Root
  $r = Invoke-RinneCommand $node @('scripts/gen-docs.mjs', '--emit', 'provenance') $Root
  if ($r.Code -ne 0) { Stop-RinnePackage "渲染逐文件来源表失败（退出码 $($r.Code)）：$($r.Err.Trim())" }
  return $r.Out
}

# 把来源表插进 NOTICES 的 scvb-derived 小节（在仓里那份的正文末尾，标记之前）。
# 纯函数：吃 NOTICES 全文与表文本，返回插好的全文。标记不在就不插（判据自己报）。
function Add-RinneProvenanceTable([string]$NoticesText, [string]$Table) {
  $open = '<!-- section:scvb-derived -->'
  $close = '<!-- /section:scvb-derived -->'
  $i = $NoticesText.IndexOf($open, [StringComparison]::Ordinal)
  $j = $NoticesText.IndexOf($close, [StringComparison]::Ordinal)
  if ($i -lt 0 -or $j -lt 0 -or $j -lt $i) { return $NoticesText }
  $head = $NoticesText.Substring(0, $j).TrimEnd()
  $tail = $NoticesText.Substring($j)
  $body = $Table.TrimEnd()
  # 末尾留一个空行，表与小节标记之间不粘连。
  "$head`n`n$body`n`n$tail"
}

# INSTALL.txt 里必须有的对应源码指引那几行（GPLv3 第 6 条 (d)）。纯函数。
function Get-RinneInstallProblems([string]$Text, [hashtable]$Vars) {
  $out = New-Object System.Collections.Generic.List[string]
  # 变量本身先查一遍：缺一个就红（渲染器会拒，但判据自己也要站得住，
  # 免得「WebView2 SDK 」这种「前缀 + 空值」的字样把缺变量糊过去）。
  foreach ($k in 'VERSION', 'TAG', 'COMMIT', 'JUCE_VERSION', 'JUCE_COMMIT', 'WEBVIEW2_VERSION') {
    if (-not $Vars.ContainsKey($k) -or -not $Vars[$k]) { $out.Add("INSTALL.txt 的变量缺一个：$k") }
  }
  $need = @(
    "Rinne  $($Vars['VERSION']): https://github.com/$RepoSlug/tree/$($Vars['TAG'])",
    "(commit $($Vars['COMMIT']))",
    "JUCE   $($Vars['JUCE_VERSION'])",
    "tree/$($Vars['JUCE_COMMIT'])",
    "WebView2 SDK $($Vars['WEBVIEW2_VERSION'])",
    'Build steps: docs/BUILDING.md'
  )
  foreach ($n in $need) { if (-not $Text.Contains($n)) { $out.Add("INSTALL.txt 缺对应源码指引的一行：$n") } }
  if ($Text -match '\{\{|\}\}') { $out.Add('INSTALL.txt 里还有未替换的占位符') }
  $out.ToArray()
}

# ── 输出目录与删除纪律 ────────────────────────────────────────────────────────

# -OutDir 只写不删。它必须不是盘根、不是仓根、不是仓根的上级目录（工作区根、wt\ 目录都在其中）、
# 不是用户目录、也不是任何仓库目录（有 .git 的目录）。
function Assert-RinneSafeOutDir([string]$OutDirFull) {
  $full = [IO.Path]::GetFullPath($OutDirFull).TrimEnd('\', '/')
  $cmp = if ($IsWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
  $root = [IO.Path]::GetPathRoot($full)
  if ($full -eq $root.TrimEnd('\', '/')) { Stop-RinnePackage "输出目录不能是盘根：$full" }
  $repo = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/')
  if ($full.Equals($repo, $cmp)) { Stop-RinnePackage "输出目录不能是仓根：$full" }
  # 仓根的每一级上级目录都不能当输出目录（工作区根、wt\ 目录都在其中）。
  $up = $repo
  while ($up) {
    $parent = [IO.Path]::GetDirectoryName($up)
    if (-not $parent -or $parent -eq $up) { break }
    $up = $parent.TrimEnd('\', '/')
    if ($up -eq $root.TrimEnd('\', '/')) { break }
    if ($full.Equals($up, $cmp)) { Stop-RinnePackage "输出目录不能是仓根的上级目录：$full" }
  }
  # 变量名不能叫 $home：PowerShell 的变量名不区分大小写，$home 与只读的 $HOME 是同一个变量。
  if ($HOME) {
    $homeDir = [IO.Path]::GetFullPath($HOME).TrimEnd('\', '/')
    if ($full.Equals($homeDir, $cmp)) { Stop-RinnePackage "输出目录不能是用户目录：$full" }
  }
  if (Test-Path -LiteralPath (Join-Path $full '.git')) { Stop-RinnePackage "输出目录不能是仓库目录：$full" }
  return $full
}

# 删一个输出目录里的旧文件：先断言它的真实路径落在 -OutDir 的真实路径前缀之内，再按单文件删。
# 绝不递归删，绝不删参数传入的目录。
function Remove-RinneOutFile([string]$OutDirFull, [string]$Name) {
  $dir = Assert-RinneSafeOutDir $OutDirFull
  $target = [IO.Path]::GetFullPath((Join-Path $dir $Name))
  $cmp = if ($IsWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
  if (-not $target.StartsWith($dir + [IO.Path]::DirectorySeparatorChar, $cmp)) { Stop-RinnePackage "要删的文件不在输出目录里：$target" }
  if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target -Force }
}

# ── 打包与 preflight ──────────────────────────────────────────────────────────

function Invoke-RinnePackage {
  param(
    [string]$Version = '', [string]$Tag = '', [string]$BuildDir = 'build', [string]$OutDir = 'dist', [string]$SourceCommit = '',
    [string]$Manifest = 'packaging/manifest-win64.txt', [string]$Config = 'Release', [switch]$Kit, [string]$KitName = '',
    [string]$BuildRun = '', [string]$Validation = '', [switch]$Preflight, [string]$PreflightOut = ''
  )

  # ── 版本与 tag ────────────────────────────────────────────────────────────
  $cmakeText = [IO.File]::ReadAllText((Join-Path $RepoRoot 'CMakeLists.txt'))
  $cmakeVersion = Get-RinneCMakeVersion $cmakeText
  if (-not $cmakeVersion) { Stop-RinnePackage 'CMakeLists.txt 里找不到 project(Rinne ... VERSION x.y.z)' }
  if (-not $Version) { $Version = $cmakeVersion }
  # 版本串会进文件名与 URL：只放行 semver 形态，挡住路径分隔符与空白。
  if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?\z') {
    Stop-RinnePackage "版本号形态不对：'$Version'（应为 x.y.z 或 x.y.z-<预发布标识>，不带前导 v）"
  }
  if (-not $Tag) { $Tag = "v$Version" }
  if ($Tag -ne "v$Version") { Stop-RinnePackage "tag '$Tag' 与版本 '$Version' 不对应（应为 v$Version）" }
  $coreVersion = ($Version -split '-', 2)[0]
  if ($coreVersion -ne $cmakeVersion) {
    # 不判红：tag 与 CMake 的一致性由 check-release-tag.ps1 管（它对形态有更严的判据）；
    # 本地拿假版本号试打包也走到这里。只显形。
    Write-Host "[WARN] 版本 $Version 与 CMakeLists.txt 的 $cmakeVersion 不一致（本地试打包 / 测试 tag 属预期）" -ForegroundColor Yellow
  }

  if (-not $SourceCommit) {
    $SourceCommit = ''
    try { $SourceCommit = ((& git -C $RepoRoot rev-parse HEAD 2>$null) | Out-String).Trim() } catch { $SourceCommit = '' }
  }
  if ($SourceCommit -notmatch '^[0-9a-f]{40}$') { Stop-RinnePackage "源码 commit 不是 40 位小写十六进制：'$SourceCommit'" }

  $juceVersion = (Get-Content -LiteralPath (Join-Path $RepoRoot '.juce-version') -Raw).Trim()
  $juceCommit = (Get-Content -LiteralPath (Join-Path $RepoRoot '.juce-commit') -Raw).Trim()
  if ($juceCommit -notmatch '^[0-9a-f]{40}$') { Stop-RinnePackage '.juce-commit 不是 40 位小写十六进制' }
  $depsText = [IO.File]::ReadAllText((Join-Path $RepoRoot 'cmake/RinneDeps.cmake'))
  $wv2 = [regex]::Match($depsText, '(?m)^\s*set\(WEBVIEW2_VERSION "([^"]+)"\)')
  if (-not $wv2.Success) { Stop-RinnePackage 'cmake/RinneDeps.cmake 里没有 WEBVIEW2_VERSION' }
  $webview2Version = $wv2.Groups[1].Value

  # 条目时间戳：SOURCE_DATE_EPOCH > HEAD 提交时间 > 1980-01-01（zip 的 DOS 时间下限）。
  # 取整到偶数秒：zip 的时间只有两秒粒度。
  $epoch = $null
  if ($env:SOURCE_DATE_EPOCH -match '^[0-9]+$') { $epoch = [long]$env:SOURCE_DATE_EPOCH }
  if ($null -eq $epoch) {
    try {
      $ct = ((& git -C $RepoRoot log -1 --format=%ct 2>$null) | Out-String).Trim()
      if ($ct -match '^[0-9]+$') { $epoch = [long]$ct }
    } catch { $epoch = $null }
  }
  if ($null -eq $epoch) { $epoch = $ZipEpochFloor }
  if ($epoch -lt $ZipEpochFloor) { $epoch = $ZipEpochFloor }
  $epoch = $epoch - ($epoch % 2)
  # 用 UTC 构造：zip 的 DOS 时间存的就是这一刻的墙上时间，与本机时区无关，所以字节可复现。
  $entryTime = [DateTimeOffset]::FromUnixTimeSeconds($epoch).ToUniversalTime()

  if ($Kit -and -not $KitName) { Stop-RinnePackage '-Kit 必须同时给 -KitName' }
  $zipName = if ($Kit) { "$KitName.zip" } else { "Rinne-v$Version-win64.zip" }

  # ── 合规文件 ──────────────────────────────────────────────────────────────
  $licenseDir = Join-Path $RepoRoot 'LICENSES'
  if (-not (Test-Path -LiteralPath $licenseDir -PathType Container)) { Stop-RinnePackage '仓库缺 LICENSES/ 目录' }
  $licenseFiles = @(Get-ChildItem -LiteralPath $licenseDir -File)
  foreach ($f in @('LICENSE', 'THIRD-PARTY-NOTICES.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $f) -PathType Leaf)) { Stop-RinnePackage "仓库根缺 $f" }
  }
  $noticesDir = Join-Path $RepoRoot 'third_party/notices'
  if (-not (Test-Path -LiteralPath $noticesDir -PathType Container)) { Stop-RinnePackage '仓库缺 third_party/notices/ 目录' }
  $noticesRoot = (Get-Item -LiteralPath $noticesDir).FullName.TrimEnd('\', '/')
  $noticeFiles = @{}
  $noticeNames = New-Object System.Collections.Generic.List[string]
  foreach ($f in @(Get-ChildItem -LiteralPath $noticesRoot -Recurse -File)) {
    $rel = $f.FullName.Substring($noticesRoot.Length).TrimStart('\', '/').Replace('\', '/')
    $noticeFiles[('third_party/notices/' + $rel)] = $f.FullName
    $noticeNames.Add($rel)
  }
  if ($noticeFiles.Count -eq 0) { Stop-RinnePackage 'third_party/notices/ 下没有任何文件' }
  $noticesText = [IO.File]::ReadAllText((Join-Path $RepoRoot 'THIRD-PARTY-NOTICES.md'), [Text.Encoding]::UTF8)

  # 打包范围内的路径（LICENSES/ 顶层文件与 third_party/notices/ 全部文件）。
  $packedPaths = New-Object System.Collections.Generic.List[string]
  foreach ($f in $licenseFiles) { $packedPaths.Add(('LICENSES/' + $f.Name)) }
  foreach ($k in $noticeFiles.Keys) { $packedPaths.Add($k) }

  # 许可闭合的判据跑在**仓里那份** NOTICES 上（逐字的那份是判据的真源）。
  $licenseProblems = @(Get-RinneLicenseProblems $noticesText @($licenseFiles | ForEach-Object { $_.Name }) @($noticeNames) @($packedPaths))
  if ($licenseProblems.Count) {
    $licenseProblems | ForEach-Object { Write-Host "package.ps1: $_" -ForegroundColor Red }
    Stop-RinnePackage "许可闭合判据没过（$($licenseProblems.Count) 条）"
  }

  # 打包清单（zip 的根条目清单）必须存在且有内容。
  # 清单路径：相对仓根解析（绝对路径原样用）。Join-Path 会把绝对子路径拼坏，所以分两种情况。
  $manifestPath = if ([IO.Path]::IsPathRooted($Manifest)) { [IO.Path]::GetFullPath($Manifest) } else { Join-Path $RepoRoot $Manifest }
  if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { Stop-RinnePackage "仓库缺 $Manifest" }
  $manifestLines = @(Get-RinneManifestLines ([IO.File]::ReadAllText($manifestPath, [Text.Encoding]::UTF8)))
  if (-not $manifestLines.Count) { Stop-RinnePackage "$Manifest 是空的" }

  # ── INSTALL.txt 的渲染（preflight 与正式打包都要） ────────────────────────
  $installVars = @{
    VERSION = $Version; TAG = $Tag; COMMIT = $SourceCommit
    JUCE_VERSION = $juceVersion; JUCE_COMMIT = $juceCommit; WEBVIEW2_VERSION = $webview2Version
    ZIP = $zipName
  }
  try { $installText = Invoke-RinneInstallTxt $RepoRoot $installVars 'win' }
  catch { Stop-RinnePackage $_.Exception.Message }
  $installProblems = @(Get-RinneInstallProblems $installText $installVars)
  if ($installProblems.Count) {
    $installProblems | ForEach-Object { Write-Host "package.ps1: $_" -ForegroundColor Red }
    Stop-RinnePackage 'INSTALL.txt 的判据没过'
  }

  # ── -Preflight：只跑不依赖构建产物的检查 ──────────────────────────────────
  # 逐文件来源表要能渲染出来（preflight 与正式打包都要，渲染不出来就红）。
  # 先渲染一次：preflight 与正式打包共用这一份，两条路渲染出来的表一样。
  try { $provenanceTable = Invoke-RinneProvenance $RepoRoot }
  catch { Stop-RinnePackage $_.Exception.Message }
  if (-not $provenanceTable.Trim()) { Stop-RinnePackage '逐文件来源表渲染出来是空的' }
  if ($Preflight) {
    if ($PreflightOut) {
      $outFull = Resolve-RinneFull $PreflightOut
      $outParent = Split-Path -Parent $outFull
      if (-not (Test-Path -LiteralPath $outParent -PathType Container)) { Stop-RinnePackage "-PreflightOut 的目录不存在：$outParent" }
      $lines = New-Object System.Collections.Generic.List[string]
      $addKv = { param([string]$k, [string]$v) $lines.Add("$k`t$v") }
      & $addKv 'format' 'rinne-package-preflight/1'
      & $addKv 'version' $Version
      & $addKv 'tag' $Tag
      & $addKv 'cmakeVersion' $cmakeVersion
      & $addKv 'sourceCommit' $SourceCommit
      & $addKv 'sourceUrl' "https://github.com/$RepoSlug/tree/$Tag"
      # 对应源码指引那几行（GPLv3 第 6 条 (d)）：逐行一行，同名键按出现顺序累积。
      foreach ($l in @($installText -split "`r?`n")) {
        if ($l -match '^\s+(Rinne\s|JUCE\s|WebView2 SDK|Build steps|Corresponding Source)') { & $addKv 'sourceLine' $l.Trim() }
      }
      & $addKv 'juceVersion' $juceVersion
      & $addKv 'juceCommit' $juceCommit
      & $addKv 'webview2Version' $webview2Version
      & $addKv 'zipName' $zipName
      & $addKv 'shaName' "$zipName.sha256"
      foreach ($p in (Get-RinneOrdinalSort ([string[]]$packedPaths))) { & $addKv 'license' $p }
      foreach ($p in (Get-RinneOrdinalSort ([string[]]$noticeFiles.Keys))) { & $addKv 'notice' $p }
      foreach ($p in (Get-RinneOrdinalSort ([string[]](@(Get-RinneCitedPaths $noticesText))))) { & $addKv 'cited' $p }
      foreach ($r in @(Get-RinneInstallRules $RepoRoot)) { & $addKv 'ruleEn' $r.En }
      foreach ($r in @(Get-RinneInstallRules $RepoRoot)) { & $addKv 'ruleZh' $r.Zh }
      foreach ($p in $manifestLines) { & $addKv 'manifest' $p }
      foreach ($l in $lines) {
        $i = $l.IndexOf("`t")
        if ($i -lt 0) { Stop-RinnePackage "preflight 清单有一行没有 TAB 分隔：$l" }
        $v = $l.Substring($i + 1)
        if ($v -match "[`t`r`n]") { Stop-RinnePackage "preflight 清单的值里有 TAB / 换行：$v" }
      }
      [IO.File]::WriteAllText($outFull, (($lines.ToArray() -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-Host "package.ps1: preflight OK（版本 $Version，tag $Tag，许可闭合已核，LICENSES/ $($licenseFiles.Count) 份，NOTICES 点名的声明文件 $(@(Get-RinneCitedPaths $noticesText).Count) 个已核，INSTALL.txt 可渲染，清单 $($manifestLines.Count) 条）"
    return @{ Preflight = $true }
  }

  # ── 枚举 bundle ───────────────────────────────────────────────────────────
  $buildFull = Resolve-RinneFull $BuildDir
  $outFull = Assert-RinneSafeOutDir (Resolve-RinneFull $OutDir)
  if (-not (Test-Path -LiteralPath $buildFull -PathType Container)) { Stop-RinnePackage "BuildDir 不存在：$buildFull" }
  $bundles = @(Get-ChildItem -LiteralPath $buildFull -Recurse -Directory -Filter '*.vst3')
  foreach ($b in $bundles) {
    if ($b.Name -match '(?i)probe') { Stop-RinnePackage "发行包里不能有探针：找到 $($b.Name)" }
  }
  if ($bundles.Count -ne 2) {
    $bundles | ForEach-Object { Write-Host "  found: $($_.FullName)" }
    Stop-RinnePackage "期望恰好 2 个 .vst3 bundle 目录（Rinne Input / Output），实际 $($bundles.Count) 个。BuildDir 里混着多套构建（如 Debug + Release）时请指到单一配置的目录。"
  }
  $names = Get-RinneOrdinalSort ([string[]]@($bundles | ForEach-Object { $_.Name }))
  $expectedSorted = Get-RinneOrdinalSort ([string[]]@($ExpectedBundles))
  if (($names -join '|') -cne ($expectedSorted -join '|')) {
    Stop-RinnePackage ("bundle 名字不对：实际 [{0}]，期望 [{1}]" -f ($names -join ', '), ($ExpectedBundles -join ', '))
  }
  foreach ($b in $bundles) {
    $dll = Join-Path $b.FullName (Join-Path 'Contents\x86_64-win' $b.Name)
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { Stop-RinnePackage "bundle 不完整，缺 $dll" }
  }

  # kit 的工具：只有 -Kit 才打进去。发行包里没有它。
  $kitTool = $null
  if ($Kit) {
    foreach ($name in $KitToolNames) {
      foreach ($cand in @((Join-Path $buildFull "tools/$name"), (Join-Path $buildFull "tools-bin/$Config/$name"))) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $kitTool = $cand; break }
      }
      if ($kitTool) { break }
    }
    if (-not $kitTool) { Stop-RinnePackage "带 -Kit 但构建目录里没有 rinne_nulltest（找过 tools\ 与 tools-bin\$Config\）" }
  }

  # ── 组装条目清单：zip 内路径 -> 源文件 ────────────────────────────────────
  $stage = New-RinneTempDir -Prefix 'rinne-package-'
  $script:OwnedTempPrefixes.Add('rinne-package-')
  try {
    $installPath = Join-Path $stage 'INSTALL.txt'
    [IO.File]::WriteAllText($installPath, $installText, (New-Object System.Text.UTF8Encoding($false)))

    # 包里那份 NOTICES：仓里那份加上渲染出来的逐文件来源表（渲染不出来就红，不打包半张表）。
    # 来源表在上面已经渲染过（preflight 与正式打包共用同一份）。
    $packagedNotices = Add-RinneProvenanceTable $noticesText $provenanceTable
    if ($packagedNotices -ceq $noticesText) {
      Stop-RinnePackage 'NOTICES 里没有 scvb-derived 小节，逐文件来源表插不进去'
    }

    $entries = @{}
    foreach ($b in $bundles) {
      $root = $b.FullName.TrimEnd('\', '/')
      foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File)) {
        $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
        $entries[($b.Name + '/' + $rel)] = $f.FullName
      }
    }
    $entries['LICENSE'] = (Join-Path $RepoRoot 'LICENSE')
    # 包里那份 NOTICES 带渲染出来的逐文件来源表：仓里那份是判据的真源，包里那份给用户看「从哪来」。
    $noticesPath = Join-Path $stage 'THIRD-PARTY-NOTICES.md'
    [IO.File]::WriteAllText($noticesPath, $packagedNotices, (New-Object System.Text.UTF8Encoding($false)))
    $entries['THIRD-PARTY-NOTICES.md'] = $noticesPath
    foreach ($f in $licenseFiles) { $entries[('LICENSES/' + $f.Name)] = $f.FullName }
    foreach ($k in $noticeFiles.Keys) { $entries[$k] = $noticeFiles[$k] }
    $entries['INSTALL.txt'] = $installPath
    if ($kitTool) { $entries["tools/$([IO.Path]::GetFileName($kitTool))"] = $kitTool }

    # SHA256SUMS：包里除它自己以外每个文件的哈希（sha256sum 格式，LF）。
    $sums = New-Object System.Collections.Generic.List[string]
    foreach ($k in (Get-RinneOrdinalSort ([string[]]$entries.Keys))) { $sums.Add(('{0}  {1}' -f (Get-RinneSha256 $entries[$k]), $k)) }
    $sumsPath = Join-Path $stage 'SHA256SUMS'
    [IO.File]::WriteAllText($sumsPath, (($sums.ToArray() -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    $entries['SHA256SUMS'] = $sumsPath
    $keys = Get-RinneOrdinalSort ([string[]]$entries.Keys)

    # ── 写 zip ───────────────────────────────────────────────────────────────
    if (-not (Test-Path -LiteralPath $outFull)) { $null = New-Item -ItemType Directory -Path $outFull -Force }
    $zipPath = Join-Path $outFull $zipName
    Remove-RinneOutFile $outFull $zipName
    $fs = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew)
    try {
      $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
      try {
        foreach ($k in $keys) {
          $e = $zip.CreateEntry($k, [System.IO.Compression.CompressionLevel]::Optimal)
          $e.LastWriteTime = $entryTime
          # 权限位写死：普通文件 0644 的位模式，不带任何平台相关的附加位。
          $e.ExternalAttributes = 32
          $dst = $e.Open()
          try {
            $src = [IO.File]::OpenRead($entries[$k])
            try { $src.CopyTo($dst) } finally { $src.Dispose() }
          } finally { $dst.Dispose() }
        }
      } finally { $zip.Dispose() }
    } finally { $fs.Dispose() }

    # ── 打包后断言：重新打开 zip 逐条比对 ─────────────────────────────────────
    $zr = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
      $inZip = @{}
      $zipNames = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
      $wantTime = [DateTimeOffset]::FromUnixTimeSeconds($epoch).UtcDateTime
      foreach ($e in $zr.Entries) {
        if ($inZip.ContainsKey($e.FullName)) { Stop-RinnePackage "zip 内条目重复：$($e.FullName)" }
        [void]$zipNames.Add($e.FullName)
        $s = $e.Open()
        try { $inZip[$e.FullName] = Get-RinneStreamSha256 $s } finally { $s.Dispose() }
        if ($e.LastWriteTime.DateTime -ne $wantTime) { Stop-RinnePackage "zip 内条目 $($e.FullName) 的时间戳不是固定的 $wantTime" }
      }
      if ($inZip.Count -ne $keys.Count) { Stop-RinnePackage "zip 条目数 $($inZip.Count) != 待打包文件数 $($keys.Count)" }
      foreach ($k in $keys) {
        if (-not $inZip.ContainsKey($k)) { Stop-RinnePackage "zip 内缺条目：$k" }
        if ($inZip[$k] -cne (Get-RinneSha256 $entries[$k])) { Stop-RinnePackage "zip 内条目与源文件字节不一致：$k" }
      }
      # NOTICES 点名的每个声明文件路径，在用户实际拿到的 zip 里逐个核对。
      foreach ($p in @(Get-RinneCitedPaths $noticesText)) {
        if (-not $zipNames.Contains($p)) { Stop-RinnePackage "THIRD-PARTY-NOTICES.md 点名的 $p 不在 zip 里（大小写须一致）" }
      }
      # 根条目集合与打包清单比对（集合相等 + 数量相等）。kit 包另有一套清单，不在这里比。
      if (-not $Kit) {
        $dirUnits = @('Rinne Input.vst3/', 'Rinne Output.vst3/', 'LICENSES/', 'third_party/notices/')
        if ($kitTool) { $dirUnits += 'tools/' }
        $roots = Get-RinneRootEntries ([string[]]$inZip.Keys) $dirUnits
        if ($roots.Stray.Count) { Stop-RinnePackage "zip 里出现清单外的目录条目：$($roots.Stray[0])" }
        $manifestProblems = @(Get-RinneManifestProblems $manifestLines $roots.Roots)
        if ($manifestProblems.Count) {
          $manifestProblems | ForEach-Object { Write-Host "package.ps1: $_" -ForegroundColor Red }
          Stop-RinnePackage 'zip 的根条目与打包清单不一致'
        }
      }
      # 探针不进包。
      foreach ($n in $zipNames) { if ($n -match '(?i)probe') { Stop-RinnePackage "zip 里出现了探针条目：$n" } }
      # 插件二进制里不许有本机路径。
      $literals = @($buildFull, $RepoRoot, $HOME) | Where-Object { $_ }
      foreach ($k in $keys) {
        if ($k -notlike '*.vst3/*') { continue }
        $bytes = [IO.File]::ReadAllBytes($entries[$k])
        $hits = @(Get-RinneLocalPathHits $bytes $literals)
        if ($hits.Count) { Stop-RinnePackage "包里的 $k 含有本机路径：$($hits[0])" }
      }
    } finally { $zr.Dispose() }

    # ── .sha256 与 summary ──────────────────────────────────────────────────
    $sha = Get-RinneSha256 $zipPath
    $size = (Get-Item -LiteralPath $zipPath).Length
    $shaPath = "$zipPath.sha256"
    Remove-RinneOutFile $outFull "$zipName.sha256"
    $shaText = "$sha  $zipName`n"
    [IO.File]::WriteAllText($shaPath, $shaText, (New-Object System.Text.UTF8Encoding($false)))
    $shaProblems = @(Get-RinneSha256Problems $shaText $zipName)
    if ($shaProblems.Count) {
      $shaProblems | ForEach-Object { Write-Host "package.ps1: $_" -ForegroundColor Red }
      Stop-RinnePackage '.sha256 的格式不对'
    }

    $summary = New-Object System.Collections.Generic.List[string]
    $summary.AddRange([string[]]@(
      '# Rinne package summary (Windows x64)',
      '',
      '| key | value |',
      '| --- | --- |',
      "| version | $Version |",
      "| tag | $Tag |",
      "| sourceCommit | $SourceCommit |",
      "| cmakeVersion | $cmakeVersion |",
      "| juceVersion | $juceVersion |",
      "| juceCommit | $juceCommit |",
      "| webview2Version | $webview2Version |",
      "| zipFileName | $zipName |",
      "| sizeBytes | $size |",
      "| sha256 | $sha |",
      "| bundles | $($ExpectedBundles -join ', ') |",
      "| kit | $(if ($Kit) { 'yes (tools/rinne_nulltest included)' } else { 'no' }) |",
      "| licenseTexts | $($licenseFiles.Count) files under LICENSES/ |",
      "| thirdPartyNotices | $($noticeFiles.Count) files under third_party/notices/ |",
      "| citedNoticePaths | $((@(Get-RinneCitedPaths $noticesText)) -join ', ') |",
      "| buildRun | $(if ($BuildRun) { $BuildRun } else { 'n/a' }) |",
      "| validation | $(if ($Validation) { $Validation } else { 'n/a' }) |",
      '',
      "Corresponding source: https://github.com/$RepoSlug/tree/$Tag",
      '',
      '## zip contents',
      '',
      '| entry | sha256 |',
      '| --- | --- |'
    ))
    foreach ($k in $keys) { $summary.Add(("| ``{0}`` | {1} |" -f $k, $inZip[$k])) }
    # 发行包与 kit 包的 summary 不同名：kit 包与发行包可能落在同一个输出目录里，
    # 同名会互相覆盖（发行包的 summary 是 Release 资产的清单，不能被 kit 包顶掉）。
    $summaryName = if ($Kit) { "package-summary-$KitName.md" } else { 'package-summary-win64.md' }
    $summaryPath = Join-Path $outFull $summaryName
    Remove-RinneOutFile $outFull $summaryName
    [IO.File]::WriteAllText($summaryPath, (($summary.ToArray() -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

    if ($env:GITHUB_OUTPUT) {
      # 显式无 BOM：PS 5.1 的 Out-File -Encoding utf8 会带 BOM。
      [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "zip=$zipPath`nsha256=$sha`n", (New-Object System.Text.UTF8Encoding($false)))
    }

    Write-Host 'package.ps1: OK'
    Write-Host "  zip      $zipPath ($size bytes, $($keys.Count) entries)"
    Write-Host "  sha256   $sha"
    Write-Host "  summary  $summaryPath"
    return @{ ZipPath = $zipPath; Sha256 = $sha; SummaryPath = $summaryPath; Entries = $keys; Epoch = $epoch }
  } finally {
    # 只删本进程建过的临时目录：先过纯函数核对，再走模块的删除（它自己再核一遍真实路径）。
    if (Test-RinneRemovableTempPath $stage @($script:OwnedTempPrefixes)) {
      $left = Remove-RinneTempDir -Path $stage -WaitSeconds 5
      if ($left) { Write-Host "package.ps1: 临时目录没删掉（$left）" -ForegroundColor Yellow }
    } else {
      Write-Host "package.ps1: 临时目录不是本进程建的，不删：$stage" -ForegroundColor Yellow
    }
  }
}

# ── 判据自测 ──────────────────────────────────────────────────────────────────

function Invoke-RinneSelfTest {
  $cases = New-Object System.Collections.Generic.List[object]
  $check = { param($Label, $Bad, $Passed) $cases.Add([pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed }) }

  # 1. 版本解析：只认 project(Rinne VERSION x.y.z)。
  $cmakeGood = "cmake_minimum_required(VERSION 3.22)`nproject(Rinne VERSION 0.9.0)`n"
  & $check '版本解析：取 project(Rinne ... VERSION x.y.z) 那一段' $false ((Get-RinneCMakeVersion $cmakeGood) -ceq '0.9.0')
  & $check '版本解析：只有 cmake_minimum_required 时取不到' $true ($null -eq (Get-RinneCMakeVersion "cmake_minimum_required(VERSION 3.22)`n"))
  & $check '版本解析：被注释掉的 project(...) 不算' $true ($null -eq (Get-RinneCMakeVersion "#project(Rinne VERSION 0.9.0)`n"))
  & $check '版本解析：别的项目名不算' $true ($null -eq (Get-RinneCMakeVersion "project(Scvb VERSION 1.2.3)`n"))
  & $check '版本解析：project 里的 VERSION 不在行首锚定之外也能取到' $false ((Get-RinneCMakeVersion "project(Rinne VERSION 1.2.3)`n") -ceq '1.2.3')

  # 2. tag 形态。
  $tagGood = { param($t, $c) (Test-RinneReleaseTag $t $c).Ok }
  & $check 'tag 形态：v1.2.3 合法' $false (& $tagGood 'v1.2.3' '1.2.3')
  & $check 'tag 形态：v1.2.3-rc.1 合法' $false (& $tagGood 'v1.2.3-rc.1' '1.2.3')
  & $check 'tag 形态：v1.2.3-rc.12 合法' $false (& $tagGood 'v1.2.3-rc.12' '1.2.3')
  & $check 'tag 形态：v1.2.3-rc 拒（少了 N）' $true (-not (& $tagGood 'v1.2.3-rc' '1.2.3'))
  & $check 'tag 形态：v1.2.3-rc.1.2 拒' $true (-not (& $tagGood 'v1.2.3-rc.1.2' '1.2.3'))
  & $check 'tag 形态：v1.2.3-RC.1 拒（大小写）' $true (-not (& $tagGood 'v1.2.3-RC.1' '1.2.3'))
  & $check 'tag 形态：v1.2.3-beta.1 拒' $true (-not (& $tagGood 'v1.2.3-beta.1' '1.2.3'))
  & $check 'tag 形态：v1.2.3-alpha.1 拒' $true (-not (& $tagGood 'v1.2.3-alpha.1' '1.2.3'))
  & $check 'tag 形态：v0.0.0-test 拒（不做演练 tag）' $true (-not (& $tagGood 'v0.0.0-test' '0.0.0'))
  & $check 'tag 形态：1.2.3 拒（没有 v）' $true (-not (& $tagGood '1.2.3' '1.2.3'))
  & $check 'tag 形态：v1.2 拒' $true (-not (& $tagGood 'v1.2' '1.2'))
  & $check 'tag 形态：v1.2.3 后跟一个空格，拒' $true (-not (& $tagGood 'v1.2.3 ' '1.2.3'))
  & $check 'tag 形态：v1.2.3 后跟一个换行，拒' $true (-not (& $tagGood "v1.2.3`n" '1.2.3'))
  & $check 'tag 形态：两行拼在一起，拒' $true (-not (& $tagGood "v1.2.3`nv1.2.4" '1.2.3'))
  & $check 'tag 形态：空串，拒' $true (-not (& $tagGood '' '1.2.3'))
  & $check 'tag 形态：版本与 CMake 不一致，拒' $true (-not (& $tagGood 'v1.2.3' '1.2.4'))
  & $check 'tag 形态：rc 的 x.y.z 与 CMake 不一致，拒' $true (-not (& $tagGood 'v1.2.3-rc.1' '1.2.4'))
  & $check 'tag 形态：rc 是预发布' $false ((Test-RinneReleaseTag 'v1.2.3-rc.1' '1.2.3').Prerelease)

  # 3. 许可的双向闭合。
  $noticesSample = @"
# Notices
<!-- section:shipped-table -->
| Component | Version | SPDX | Ships in | Notice texts |
| --- | --- | --- | --- | --- |
| JUCE | 8 | ``AGPL-3.0-only`` | both | ``third_party/notices/juce.LICENSE.md`` |
<!-- /section:shipped-table -->
## 2. Build, test and CI only
| Component | Version | SPDX | Used for | Notice texts |
| --- | --- | --- | --- | --- |
| Catch2 | 3 | ``BSL-1.0`` | tests | ``third_party/notices/catch2.LICENSE.txt`` |
"@
  $licNames = @('AGPL-3.0-only.txt', 'BSL-1.0.txt', 'GPL-3.0-or-later.txt')
  $noticeRel = @('juce.LICENSE.md', 'catch2.LICENSE.txt')
  $packedOk = @('LICENSES/AGPL-3.0-only.txt', 'LICENSES/BSL-1.0.txt', 'LICENSES/GPL-3.0-or-later.txt', 'third_party/notices/juce.LICENSE.md', 'third_party/notices/catch2.LICENSE.txt')
  & $check '许可闭合：表里点名的都有全文，点名的声明都在打包清单里' $false (0 -eq @(Get-RinneLicenseProblems $noticesSample $licNames $noticeRel $packedOk).Count)
  & $check '许可闭合：LICENSES/ 里有没人点名的全文，红' $true (@(Get-RinneLicenseProblems $noticesSample @($licNames + 'MIT.txt') $noticeRel $packedOk).Count -gt 0)
  & $check '许可闭合：NOTICES 点名的声明不在打包清单里，红' $true (@(Get-RinneLicenseProblems $noticesSample $licNames $noticeRel @($packedOk | Where-Object { $_ -notlike '*catch2*' })).Count -gt 0)
  & $check '许可闭合：表里点名的标识缺全文，红' $true (@(Get-RinneLicenseProblems $noticesSample @('AGPL-3.0-only.txt') $noticeRel $packedOk).Count -gt 0)

  # 4. 打包清单的比对。
  $manifestOk = @('Rinne Input.vst3/', 'Rinne Output.vst3/', 'INSTALL.txt', 'LICENSE', 'LICENSES/', 'SHA256SUMS', 'THIRD-PARTY-NOTICES.md', 'third_party/notices/')
  & $check '清单比对：一致时没有问题' $false (0 -eq @(Get-RinneManifestProblems $manifestOk $manifestOk).Count)
  & $check '清单比对：manifest 多一项，红' $true (@(Get-RinneManifestProblems @($manifestOk + 'EXTRA/') $manifestOk).Count -gt 0)
  & $check '清单比对：zip 里多一项，红' $true (@(Get-RinneManifestProblems $manifestOk @($manifestOk + 'EXTRA/')).Count -gt 0)

  # 4b. 根条目的推导：目录单元由打包计划给出，不靠切第一段。
  $dirUnits = @('Rinne Input.vst3/', 'Rinne Output.vst3/', 'LICENSES/', 'third_party/notices/')
  $entriesReal = @('Rinne Input.vst3/Contents/x86_64-win/Rinne Input.vst3', 'Rinne Output.vst3/Contents/x86_64-win/Rinne Output.vst3',
    'INSTALL.txt', 'LICENSE', 'LICENSES/GPL-3.0-or-later.txt', 'SHA256SUMS', 'THIRD-PARTY-NOTICES.md',
    'third_party/notices/juce.LICENSE.md')
  $rootsReal = Get-RinneRootEntries $entriesReal $dirUnits
  # 比集合不比顺序：根条目按写入顺序出现，清单按期望顺序列出，两者顺序本就不同。
  $rootsSorted = @(Get-RinneOrdinalSort $rootsReal.Roots) -join ','
  $manifestSorted = @(Get-RinneOrdinalSort $manifestOk) -join ','
  & $check '根条目：目录单元按打包计划归并（third_party/notices/ 不被算成 third_party/）' $false ($rootsSorted -ceq $manifestSorted)
  & $check '根条目：没有野条目' $false ($rootsReal.Stray.Count -eq 0)
  $rootsStray = Get-RinneRootEntries @($entriesReal + 'EXTRA/x.txt') $dirUnits
  & $check '根条目：清单外的目录条目被报出来' $true ($rootsStray.Stray.Count -eq 1)

  # 4c. 逐文件来源表插进 NOTICES 的 scvb-derived 小节。
  $noticesSample2 = @"
# Third-Party Notices

## 4. Code derived from other Synchain projects

<!-- section:scvb-derived -->

Some files are adapted from synchain-oss/scvb.

<!-- /section:scvb-derived -->

## 5. Trademarks

VST is a registered trademark.
"@
  $tableSample = @"
| 本仓文件 | 上游 | 上游路径 | 改写方式 |
| --- | --- | --- | --- |
| ``scripts/package.ps1`` | synchain-oss/scvb@0123456789ab (dev) | ``scripts/package.ps1`` | forked |
"@
  $merged = Add-RinneProvenanceTable $noticesSample2 $tableSample
  & $check '来源表：表插在 scvb-derived 小节的正文末尾、标记之前' $false (
    $merged.Contains('| `scripts/package.ps1` |') -and
    $merged.IndexOf('| `scripts/package.ps1` |') -lt $merged.IndexOf('<!-- /section:scvb-derived -->') -and
    $merged.Contains('## 5. Trademarks'))
  & $check '来源表：仓里那份 NOTICES 没被改动（插的是副本）' $false (-not $noticesSample2.Contains('scripts/package.ps1'))
  $noSection = Add-RinneProvenanceTable "# 没有小节`n" $tableSample
  & $check '来源表：没有 scvb-derived 小节时原样返回（判据自己报红）' $false ($noSection -ceq "# 没有小节`n")

  # 5. .sha256 的格式。
  & $check '.sha256：<64 位小写 hex><两个空格><文件名> 加 LF，没有问题' $false (0 -eq @(Get-RinneSha256Problems ("$('a' * 64)  Rinne-v1.2.3-win64.zip`n") 'Rinne-v1.2.3-win64.zip').Count)
  & $check '.sha256：出现 CRLF，红' $true (@(Get-RinneSha256Problems ("$('a' * 64)  Rinne-v1.2.3-win64.zip`r`n") 'Rinne-v1.2.3-win64.zip').Count -gt 0)
  & $check '.sha256：哈希是大写，红' $true (@(Get-RinneSha256Problems ("$('A' * 64)  Rinne-v1.2.3-win64.zip`n") 'Rinne-v1.2.3-win64.zip').Count -gt 0)
  & $check '.sha256：文件名对不上，红' $true (@(Get-RinneSha256Problems ("$('a' * 64)  other.zip`n") 'Rinne-v1.2.3-win64.zip').Count -gt 0)

  # 6. 本机路径扫描。
  # 夹具里的路径不带用户目录段：仓里不许出现真实的用户目录（隐私规则会判红），
  # 而带 <user> 这种占位又会打断路径扫描自己的正则。盘符路径本身就够判红了。
  $clean = [Text.Encoding]::UTF8.GetBytes('nothing to see here')
  $dirty = [Text.Encoding]::UTF8.GetBytes('built at C:\work\rinne\build\x.obj')
  & $check '路径扫描：干净的文件没有命中' $false (0 -eq @(Get-RinneLocalPathHits $clean @('C:\work\rinne')).Count)
  & $check '路径扫描：命中盘符路径，红' $true (@(Get-RinneLocalPathHits $dirty @('C:\work\rinne')).Count -gt 0)
  & $check '路径扫描：命中给定的字面量，红' $true (@(Get-RinneLocalPathHits ([Text.Encoding]::UTF8.GetBytes('built at D:\ci\rinne')) @('D:\ci\rinne')).Count -gt 0)

  # 7. INSTALL.txt 的判据。
  # ZIP 也是渲染变量：docs/INSTALL.md 的 install:win 块里有 {{ZIP}}（校验与解压命令）。
  $installVars = @{ VERSION = '1.2.3'; TAG = 'v1.2.3'; COMMIT = ('a' * 40); JUCE_VERSION = '8.0.8'; JUCE_COMMIT = ('b' * 40); WEBVIEW2_VERSION = '1.0.2957.106'; ZIP = 'Rinne-v1.2.3-win64.zip' }
  $installSample = @"
Rinne 1.2.3 - Windows x64

Corresponding Source for this build (GPLv3 section 6 (d)):
  Rinne  1.2.3: https://github.com/synchain-oss/rinne/tree/v1.2.3  (commit $($installVars['COMMIT']))
  JUCE   8.0.8 (bundles the VST3 SDK and AudioUnitSDK): https://github.com/juce-framework/JUCE/tree/$($installVars['JUCE_COMMIT'])
  WebView2 SDK 1.0.2957.106 (Windows only; NuGet package Microsoft.Web.WebView2)
  Build steps: docs/BUILDING.md at the tag above
"@
  & $check 'INSTALL.txt：该有的一行都在，没有问题' $false (0 -eq @(Get-RinneInstallProblems $installSample $installVars).Count)
  $missing = $installVars.Clone(); $missing.Remove('WEBVIEW2_VERSION')
  & $check 'INSTALL.txt：变量缺一个，红' $true (@(Get-RinneInstallProblems $installSample $missing).Count -gt 0)
  & $check 'INSTALL.txt：正文少一行，红' $true (@(Get-RinneInstallProblems ($installSample -replace 'Build steps: docs/BUILDING\.md', 'Build steps: elsewhere') $installVars).Count -gt 0)
  # 渲染器自己也要拒缺变量的：走真的 gen-docs 渲染（假 vars），缺一个必须抛错。
  $renderMissing = $installVars.Clone(); $renderMissing.Remove('WEBVIEW2_VERSION')
  $renderRed = $false
  try { $null = Invoke-RinneInstallTxt $RepoRoot $renderMissing 'win' } catch { $renderRed = $true }
  & $check 'INSTALL.txt：渲染器缺变量时必须红' $true $renderRed
  $renderOk = $false
  try { $rendered = Invoke-RinneInstallTxt $RepoRoot $installVars 'win'; $renderOk = (0 -eq @(Get-RinneInstallProblems $rendered $installVars).Count) } catch { $renderOk = $false }
  & $check 'INSTALL.txt：渲染器用齐变量时给出合规的正文' $false $renderOk

  # 8. 临时目录的删除纪律：只做纯函数断言，不对真实路径发起删除。
  $owned = Test-RinneRemovableTempPath (Join-Path ([IO.Path]::GetTempPath()) 'rinne-package-0123456789abcdef') @('rinne-package-')
  & $check '删除纪律：本进程建的临时目录可以删（纯函数断言）' $false $owned
  $foreign = Test-RinneRemovableTempPath (Join-Path ([IO.Path]::GetTempPath()) 'someone-else-0123456789abcdef') @('rinne-package-')
  & $check '删除纪律：不是本进程建的目录不删（纯函数断言）' $true (-not $foreign)
  $outside = Test-RinneRemovableTempPath (Join-Path ([IO.Path]::GetTempPath()) 'sub\rinne-package-0123456789abcdef') @('rinne-package-')
  & $check '删除纪律：不在临时目录下一层的路径不删（纯函数断言）' $true (-not $outside)

  # 9. 真打包：假 BuildDir，两次打包字节一致、改一个字节就不同、探针必须红、清单多一项必须红。
  $tmp = New-RinneTempDir -Prefix 'rinne-package-selftest-'
  $script:OwnedTempPrefixes.Add('rinne-package-selftest-')
  try {
    $fakeBuild = Join-Path $tmp 'build'
    $null = New-Item -ItemType Directory -Path $fakeBuild -Force
    function New-RinneFakeBundle([string]$Dir, [string]$Name, [string]$Body) {
      $b = Join-Path $Dir $Name
      $null = New-Item -ItemType Directory -Path (Join-Path $b 'Contents\x86_64-win') -Force
      [IO.File]::WriteAllText((Join-Path $b "Contents\x86_64-win\$Name"), $Body, (New-Object System.Text.UTF8Encoding($false)))
    }
    New-RinneFakeBundle $fakeBuild 'Rinne Input.vst3' 'input-binary'
    New-RinneFakeBundle $fakeBuild 'Rinne Output.vst3' 'output-binary'
    $outA = Join-Path $tmp 'out-a'; $outB = Join-Path $tmp 'out-b'; $outC = Join-Path $tmp 'out-c'
    $fakeCommit = ('0123456789abcdef0123456789abcdef01234567')

    $savedEpoch = $env:SOURCE_DATE_EPOCH
    $env:SOURCE_DATE_EPOCH = '1700000000'
    try {
      $r1 = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir $outA -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt'
      $r2 = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir $outB -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt'
      & $check '确定性：同一份输入打两次，sha256 相同' $false ($r1.Sha256 -ceq $r2.Sha256)
      & $check '确定性：时间戳固定在 SOURCE_DATE_EPOCH 取整后的值' $false ($r1.Epoch -eq 1700000000)

      # 改一个字节：sha256 必须不同。
      [IO.File]::WriteAllText((Join-Path $fakeBuild 'Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3'), 'input-binary-changed', (New-Object System.Text.UTF8Encoding($false)))
      $r3 = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir $outC -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt'
      & $check '确定性：改一个字节，sha256 不同' $true ($r1.Sha256 -cne $r3.Sha256)
      [IO.File]::WriteAllText((Join-Path $fakeBuild 'Rinne Input.vst3\Contents\x86_64-win\Rinne Input.vst3'), 'input-binary', (New-Object System.Text.UTF8Encoding($false)))

      # 探针必须红。
      New-RinneFakeBundle $fakeBuild 'Rinne Probe Input.vst3' 'probe-binary'
      $probeRed = $false
      try { $null = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir (Join-Path $tmp 'out-probe') -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt' }
      catch { $probeRed = $true }
      & $check '探针：BuildDir 里有 Rinne Probe Input.vst3 时打包必须红' $true $probeRed
      $null = Remove-Item -LiteralPath (Join-Path $fakeBuild 'Rinne Probe Input.vst3') -Recurse -Force
    } finally { $env:SOURCE_DATE_EPOCH = $savedEpoch }

    # 清单多一项必须红：用一份多了一行的清单文件（放在本进程建的临时目录里，不写进仓内）。
    $badManifest = Join-Path $tmp 'manifest-extra.txt'
    $manifestOk = @(Get-RinneManifestLines ([IO.File]::ReadAllText((Join-Path $RepoRoot 'packaging/manifest-win64.txt'), [Text.Encoding]::UTF8)))
    [IO.File]::WriteAllText($badManifest, (($manifestOk + 'EXTRA/') -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
    $manifestRed = $false
    try { $null = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir (Join-Path $tmp 'out-manifest') -SourceCommit $fakeCommit -Manifest $badManifest }
    catch { $manifestRed = $true }
    & $check '清单：打包清单多一项时打包必须红' $true $manifestRed

    # 清单少一项同样要红（清单是集合相等，不是子集）。
    $shortManifest = Join-Path $tmp 'manifest-short.txt'
    [IO.File]::WriteAllText($shortManifest, (($manifestOk | Where-Object { $_ -ne 'LICENSE' }) -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
    $shortRed = $false
    try { $null = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir (Join-Path $tmp 'out-manifest-short') -SourceCommit $fakeCommit -Manifest $shortManifest }
    catch { $shortRed = $true }
    & $check '清单：打包清单少一项时打包必须红' $true $shortRed

    # 清单逐字相同则必须绿（正例）：走真的清单文件，不带 -Kit。
    $manifestGreen = $false
    try {
      $ok = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir (Join-Path $tmp 'out-manifest-ok') -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt'
      $manifestGreen = @($ok.Entries).Count -gt 0
    } catch { $manifestGreen = $false }
    & $check '清单：清单与实际根条目一致时打包必须绿' $false $manifestGreen

    # 真的 zip 里出现 Rinne Probe（改名混进 bundle 目录）：必须红。
    $probeName = Join-Path $fakeBuild 'Rinne Probe.vst3'
    New-RinneFakeBundle $fakeBuild 'Rinne Probe.vst3' 'probe-binary'
    $probeInZip = $false
    try { $null = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir (Join-Path $tmp 'out-probe2') -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt' }
    catch { $probeInZip = $true }
    & $check '探针：BuildDir 里有 Rinne Probe.vst3 时打包必须红' $true $probeInZip
    $null = Remove-Item -LiteralPath $probeName -Recurse -Force

    # 本机路径进包：必须红。
    $dirtyBuild = Join-Path $tmp 'build-dirty'
    $null = New-Item -ItemType Directory -Path $dirtyBuild -Force
    New-RinneFakeBundle $dirtyBuild 'Rinne Input.vst3' 'built at C:\work\rinne\build\obj.obj'
    New-RinneFakeBundle $dirtyBuild 'Rinne Output.vst3' 'output-binary'
    $dirtyRed = $false
    try { $null = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $dirtyBuild -OutDir (Join-Path $tmp 'out-dirty') -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt' }
    catch { $dirtyRed = $true }
    & $check '路径扫描：二进制里有盘符路径时打包必须红' $true $dirtyRed

    # kit 才放工具。
    $kitBuild = Join-Path $tmp 'build-kit'
    $null = New-Item -ItemType Directory -Path (Join-Path $kitBuild 'tools-bin\Release') -Force
    New-RinneFakeBundle $kitBuild 'Rinne Input.vst3' 'input-binary'
    New-RinneFakeBundle $kitBuild 'Rinne Output.vst3' 'output-binary'
    [IO.File]::WriteAllText((Join-Path $kitBuild 'tools-bin\Release\rinne_nulltest.exe'), 'tool-binary', (New-Object System.Text.UTF8Encoding($false)))
    $kitOut = Join-Path $tmp 'out-kit'
    $kitRes = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $kitBuild -OutDir $kitOut -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt' -Kit -KitName 'Rinne-kit-win64-0.0.0-local-0123456789abcdef0123456789abcdef01234567'
    & $check 'kit：带 -Kit 时包里才有 tools/rinne_nulltest' $false (@($kitRes.Entries | Where-Object { $_ -like 'tools/*' }).Count -eq 1)
    $noKitRes = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $kitBuild -OutDir (Join-Path $tmp 'out-nokit') -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt'
    & $check 'kit：不带 -Kit 时包里没有工具' $false (@($noKitRes.Entries | Where-Object { $_ -like 'tools/*' }).Count -eq 0)
    # kit 包与发行包的 summary 不同名：同一个输出目录里，发行包的 summary 不能被 kit 包顶掉。
    & $check 'kit：kit 包的 summary 与发行包的 summary 不同名' $false (
        ($kitRes.SummaryPath -ne $noKitRes.SummaryPath) -and
        ([IO.Path]::GetFileName($kitRes.SummaryPath) -like 'package-summary-Rinne-kit-*') -and
        ([IO.Path]::GetFileName($noKitRes.SummaryPath) -ceq 'package-summary-win64.md'))
    # 发行包的 summary 仍叫 package-summary-win64.md（Release 资产清单按这个名字取）。
    & $check 'kit：发行包的 summary 叫 package-summary-win64.md' $false ([IO.Path]::GetFileName($noKitRes.SummaryPath) -ceq 'package-summary-win64.md')

    # preflight 清单的格式。
    $pf = Join-Path $tmp 'preflight.tsv'
    $null = Invoke-RinnePackage -Version '0.0.0-local' -Tag 'v0.0.0-local' -BuildDir $fakeBuild -OutDir $outA -SourceCommit $fakeCommit -Manifest 'packaging/manifest-win64.txt' -Preflight -PreflightOut $pf
    $pfText = [IO.File]::ReadAllText($pf)
    $pfLines = @($pfText -split "`n" | Where-Object { $_ -ne '' })
    $pfFirst = $pfLines[0]
    $pfKeys = @($pfLines | ForEach-Object { ($_ -split "`t", 2)[0] })
    & $check 'preflight：首行是 format<TAB>rinne-package-preflight/1' $false ($pfFirst -ceq "format`trinne-package-preflight/1")
    & $check 'preflight：全文没有 CR' $false (-not ($pfText -match "`r"))
    & $check 'preflight：值里没有 TAB' $false (-not (@($pfLines | Where-Object { @($_ -split "`t").Count -ne 2 }).Count))
    $pfWant = @('version', 'tag', 'manifest', 'license', 'notice', 'cited', 'sourceLine')
    & $check 'preflight：清单里有 version / tag / manifest / license / notice / cited / sourceLine' $false (@($pfWant | Where-Object { $pfKeys -notcontains $_ }).Count -eq 0)
  } finally {
    if (Test-RinneRemovableTempPath $tmp @($script:OwnedTempPrefixes)) {
      $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 5
      if ($left) { Write-Host "package.ps1 -SelfTest：临时目录没删掉（$left）" -ForegroundColor Yellow }
    }
  }

  $failed = @($cases | Where-Object { -not $_.Passed })
  foreach ($f in $failed) { Write-Host "package.ps1 -SelfTest 未通过：$($f.Label)" -ForegroundColor Red }
  $nBad = @($cases | Where-Object Bad).Count
  Write-Host ("package.ps1 -SelfTest：{0}（{1} 项，坏样例 {2}、正例 {3}）" -f $(if ($failed.Count) { '失败' } else { '通过' }), $cases.Count, $nBad, ($cases.Count - $nBad))
  if ($failed.Count) { exit 1 }
  exit 0
}

# ── 入口 ──────────────────────────────────────────────────────────────────────

if ($SelfTest) { Invoke-RinneSelfTest }

if ($PreflightOut -and -not $Preflight) { Stop-RinnePackage '-PreflightOut 只能与 -Preflight 一起用' }

$null = Invoke-RinnePackage -Version $Version -Tag $Tag -BuildDir $BuildDir -OutDir $OutDir -SourceCommit $SourceCommit `
  -Manifest $Manifest -Config $Config -Kit:$Kit -KitName $KitName -BuildRun $BuildRun -Validation $Validation `
  -Preflight:$Preflight -PreflightOut $PreflightOut
exit 0
