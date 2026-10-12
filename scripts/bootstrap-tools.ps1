#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 按钉版文件把本机工具装进 -ToolsDir，或检查本机的 hooks 配置。
#   pwsh scripts/bootstrap-tools.ps1 [-ToolsDir <目录>] [-Tools node,gitleaks,…] [-Python <python>]
#   pwsh scripts/bootstrap-tools.ps1 -CheckHooks
# -ToolsDir 缺省取环境变量 RINNE_TOOLS_DIR，两者都没有就停下。版本取自仓内钉版文件：.node-version、.gitleaks-version、
# .actionlint-version、.prettier-version；clang-format 与 reuse 取 requirements-tools.txt。ninja 还没有钉版文件、
# pluginval 的钉版文件构建骨架入库时才有，两者先用下面清单里的版本，只在 -Tools 点名或钉版文件出现后才装。
# 档案工具：每个包先下载进自己新建的空目录，sha256 与清单逐字一致才解开，原始档案随解开的文件一起放进
# <工具>-<版本>/.rinne-archive/，整个目录最后一步才改名到位。已有同名目录时先核档案的 sha256，再把解开的每个文件与
# 档案里的条目逐个比对，最后核版本；任何一步对不上就停下，不覆盖、不删除。
# Python 工具（clang-format、reuse）：装进按 requirements-tools.txt 内容命名的独立 venv，分两步装，见该文件头注：
# 先按哈希装 reuse 的构建后端 poetry-core，再关掉构建隔离、除 reuse 外只收 wheel，按哈希装其余各项。装失败就删掉这次
# 新建的 venv；装好、核过版本才写完成标记。venv 没法装完再改名（pip 生成的 reuse.exe 等启动器写死了 venv 的绝对路径），
# 所以已有 venv 缺完成标记时说明上次没装完，停下请人移走；有标记的按各包 RECORD 里的 sha256 逐个核对已装文件
# （RECORD 在 venv 里，这一步查的是装好之后有没有被改动或损坏），再核版本。
# 不改 PATH、不写注册表、不全局安装，仓内不写任何本机路径：装完只打印该在主 clone 里执行的 git config（值只进 .git/config）。
# 清单的 sha256 出处：gitleaks、actionlint 是官方 checksums.txt，与 GitHub 发布资产的 digest 一致；ninja 是 GitHub 发布资产的
# digest；node 是 SHASUMS256.txt；prettier 是 npm 包，registry 的 dist.integrity（sha512）与下载的 tgz 一致；pluginval 上游
# 既不发校验文件、发布资产也没有 digest，登记的是首次下载的值（trust on first use）。
# 升级版本时改钉版文件，同时在清单里补上新版本的 sha256，否则这里拒装。目前只登记了 Windows x64 的包。
# -CheckHooks：core.hooksPath 必须指向主 clone 的 scripts/hooks，commit-msg 与 pre-push 都在；个人针文件
# （RINNE_PRIVACY_LOCAL 或 git config rinne.privacyLocal）必须存在。只看文件在不在，不读内容。退出码 0 / 1。
[CmdletBinding()]
param(
    [string]$ToolsDir = $env:RINNE_TOOLS_DIR,
    [string[]]$Tools = @(),
    [string]$Python = 'python',
    [switch]$CheckHooks
)

$ErrorActionPreference = 'Stop'
# 与 gates.ps1 一致：G1 以子进程调 -CheckHooks 并按 UTF-8 读它的输出
try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) } catch { $null = $_ }
$Root = Split-Path -Parent $PSScriptRoot

$Manifest = [ordered]@{
    node       = @{ Pin = '.node-version'; Key = 'rinne.node'; Entry = 'node-v{0}-win-x64/node.exe'; Args = '--version'
        Url = 'https://nodejs.org/dist/v{0}/node-v{0}-win-x64.zip'
        Sha256 = @{ '22.23.3' = '2b0ff57b049cda1bbcea2240eec20467018713c1efe1f7360c2681859b90ed71' }
    }
    gitleaks   = @{ Pin = '.gitleaks-version'; Key = 'rinne.gitleaks'; Entry = 'gitleaks.exe'; Args = 'version'
        Url = 'https://github.com/gitleaks/gitleaks/releases/download/v{0}/gitleaks_{0}_windows_x64.zip'
        Sha256 = @{ '8.30.1' = 'd29144deff3a68aa93ced33dddf84b7fdc26070add4aa0f4513094c8332afc4e' }
    }
    actionlint = @{ Pin = '.actionlint-version'; Key = 'rinne.actionlint'; Entry = 'actionlint.exe'; Args = '-version'
        Url = 'https://github.com/rhysd/actionlint/releases/download/v{0}/actionlint_{0}_windows_amd64.zip'
        Sha256 = @{ '1.7.12' = '6e7241b51e6817ea6a047693d8e6fed13b31819c9a0dd6c5a726e1592d22f6e9' }
    }
    prettier   = @{ Pin = '.prettier-version'; Key = 'rinne.prettier'; Entry = 'package/bin/prettier.cjs'; Args = '--version'; ViaNode = $true
        Url = 'https://registry.npmjs.org/prettier/-/prettier-{0}.tgz'
        Sha256 = @{ '3.9.6' = '997da95cf2ae81053cafc79ef122a6e8dc12e3f2c619d57eb1f2e19525fb212f' }
    }
    ninja      = @{ Default = '1.13.1'; Key = 'rinne.ninja'; Entry = 'ninja.exe'; Args = '--version'
        Url = 'https://github.com/ninja-build/ninja/releases/download/v{0}/ninja-win.zip'
        Sha256 = @{ '1.13.1' = '26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf' }
    }
    pluginval  = @{ Pin = '.pluginval-version'; Default = '1.0.4'; Key = 'rinne.pluginval'; Entry = 'pluginval.exe'; Args = '--version'
        Url = 'https://github.com/Tracktion/pluginval/releases/download/v{0}/pluginval_Windows.zip'
        Sha256 = @{ '1.0.4' = 'c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab' }
    }
}

# venv 里各包 RECORD 记下的 sha256 与已装文件逐个比对；有不符、缺文件或一条都没核到就以非 0 退出。
$RecordCheck = @'
import base64, hashlib, sys
from importlib import metadata
n = bad = 0
for d in metadata.distributions():
    for f in d.files or []:
        if f.hash is None or f.hash.mode != 'sha256':
            continue
        n += 1
        got = base64.urlsafe_b64encode(hashlib.sha256(d.locate_file(f).read_bytes()).digest()).rstrip(b'=').decode()
        if got != f.hash.value:
            bad += 1
            print('changed:', f)
sys.exit(1 if bad or n == 0 else 0)
'@

function Get-Setting([string]$EnvName, [string]$GitKey) {
    $v = [Environment]::GetEnvironmentVariable($EnvName)
    if ($v) { return $v }
    "$(git -C $Root config --get $GitKey 2>$null)".Trim()
}

# 逐项打印 [ok] / [FAIL]，返回不过的项数。
function Test-Hooks {
    $rows = [System.Collections.Generic.List[object]]::new()
    $common = "$(git -C $Root rev-parse --path-format=absolute --git-common-dir)".Trim()
    $want = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $common) 'scripts/hooks')).TrimEnd('\', '/')
    $set = "$(git -C $Root config --get core.hooksPath 2>$null)".Trim()
    $have = $set ? [IO.Path]::GetFullPath($set, $Root).TrimEnd('\', '/') : ''
    $same = $IsWindows ? ($have -ieq $want) : ($have -ceq $want)
    $rows.Add(@($same, ($same ? 'core.hooksPath 指向主 clone 的 scripts/hooks' : ($set ? 'core.hooksPath 没有指向主 clone 的 scripts/hooks' : '没有设置 core.hooksPath'))))
    foreach ($h in 'commit-msg', 'pre-push') {
        $ok = Test-Path -LiteralPath (Join-Path $want $h) -PathType Leaf
        $rows.Add(@($ok, ($ok ? "主 clone 有 scripts/hooks/$h" : "主 clone 缺 scripts/hooks/$h")))
    }
    $needles = Get-Setting 'RINNE_PRIVACY_LOCAL' 'rinne.privacyLocal'
    $ok = $needles -and (Test-Path -LiteralPath $needles -PathType Leaf)
    $rows.Add(@($ok, ($ok ? '个人针文件存在' : ($needles ? '配置的个人针文件不存在' : '没有配置个人针文件（RINNE_PRIVACY_LOCAL 或 git config rinne.privacyLocal）'))))
    foreach ($r in $rows) { Write-Host "[$($r[0] ? 'ok' : 'FAIL')] $($r[1])" }
    @($rows | Where-Object { -not $_[0] }).Count
}

function Get-PinVersion($Spec) {
    $pin = $Spec.Pin ? (Join-Path $Root $Spec.Pin) : ''
    if ($pin -and (Test-Path -LiteralPath $pin)) { return (Get-Content -LiteralPath $pin -Raw).Trim() -replace '^v', '' }
    $Spec.Default
}

# 跑一次 --version 之类的命令，输出里要有这个版本号。用 Process 而不是 &：pluginval 是 GUI 子系统的程序，
# & 调它不等退出、也拿不到输出。
function Test-Installed([string]$Exe, [string[]]$Arguments, [string]$Version, [string]$Node) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new($Node ? $Node : $Exe)
    foreach ($a in @($Node ? @($Exe) : @()) + $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    try { $p = [System.Diagnostics.Process]::Start($psi) } catch { return $false }
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    ($p.ExitCode -eq 0) -and ("$($out.Result)$err" -match "(?<![\d.])$([regex]::Escape($Version))(?![\d.])")
}

# 档案（zip 或 tgz）里每个普通文件都要在 Dest 下有一份逐字节相同的（按 sha256 比）。返回对不上或缺失的条目数；
# 档案里一个文件都没有也算 1。
function Test-Unpacked([string]$Archive, [string]$Dest) {
    $entries = [System.Collections.Generic.List[object]]::new()
    $hash = { param($s) [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($s ?? [IO.MemoryStream]::new())) }
    if ($Archive -like '*.zip') {
        $z = [IO.Compression.ZipFile]::OpenRead($Archive)
        try { foreach ($e in $z.Entries | Where-Object Name) { $s = $e.Open(); try { $entries.Add(@($e.FullName, (& $hash $s))) } finally { $s.Dispose() } } }
        finally { $z.Dispose() }
    } else {
        $in = [IO.File]::OpenRead($Archive)
        try {
            $r = [System.Formats.Tar.TarReader]::new([IO.Compression.GZipStream]::new($in, [IO.Compression.CompressionMode]::Decompress))
            while ($e = $r.GetNextEntry()) { if ("$($e.EntryType)" -in 'RegularFile', 'V7RegularFile') { $entries.Add(@($e.Name, (& $hash $e.DataStream))) } }
        } finally { $in.Dispose() }
    }
    if (-not $entries.Count) { return 1 }
    @($entries | Where-Object {
            $p = Join-Path $Dest $_[0]
            -not (Test-Path -LiteralPath $p -PathType Leaf) -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ne $_[1]
        }).Count
}

# 下载、核对、解开一个包；返回入口文件的路径。
function Install-Archive([string]$Name, $Spec, [string]$Node) {
    $ver = Get-PinVersion $Spec
    $sha = $Spec.Sha256[$ver]
    if (-not $sha) { throw "$Name $ver 没有登记 sha256：在本脚本的清单里补上（取自上游发布的校验文件）后再装" }
    $dest = Join-Path $ToolsDir "$Name-$ver"
    $entry = Join-Path $dest ($Spec.Entry -f $ver)
    $via = $Spec.ViaNode ? $Node : ''
    if ($Spec.ViaNode -and -not $Node) { throw "$Name 要用 node 运行：先装 node，或设 RINNE_NODE / git config rinne.node" }
    $url = $Spec.Url -f $ver
    $file = ($url -split '/')[-1]
    if (Test-Path -LiteralPath $dest) {
        $kept = Join-Path $dest ".rinne-archive/$file"
        if (-not (Test-Path -LiteralPath $kept -PathType Leaf)) { throw "$Name-$ver 里没有留下原始档案，核对不了；不覆盖，请先手动移走该目录" }
        if ((Get-FileHash -LiteralPath $kept -Algorithm SHA256).Hash -ne $sha) { throw "$Name-$ver 留下的档案与清单的 sha256 不符；不覆盖，请先手动移走" }
        $changed = Test-Unpacked $kept $dest
        if ($changed) { throw "$Name-$ver 里有 $changed 个文件与档案不一致；不覆盖，请先手动移走" }
        if (-not (Test-Installed $entry @($Spec.Args) $ver $via)) { throw "$Name-$ver 核对不过版本；不覆盖，请先手动移走" }
        Write-Host "[已装] $Name $ver（档案与解开的文件逐个核对过）"
        return $entry
    }
    $work = Join-Path $ToolsDir ".download-$Name-$ver-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
    $null = New-Item -ItemType Directory -Path $work
    try {
        $download = Join-Path $work $file
        Write-Host "[下载] $url"
        Invoke-WebRequest -Uri $url -OutFile $download
        $got = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($got -cne $sha) { throw "$Name $ver 的 sha256 不符：期望 $sha，实际 $got；不解开" }
        $stage = Join-Path $work 'unpacked'
        $null = New-Item -ItemType Directory -Path (Join-Path $stage '.rinne-archive')
        if ($file -like '*.zip') { Expand-Archive -LiteralPath $download -DestinationPath $stage }
        else {
            $in = [IO.File]::OpenRead($download)
            try { [System.Formats.Tar.TarFile]::ExtractToDirectory([IO.Compression.GZipStream]::new($in, [IO.Compression.CompressionMode]::Decompress), $stage, $false) }
            finally { $in.Dispose() }
        }
        Move-Item -LiteralPath $download -Destination (Join-Path $stage ".rinne-archive/$file")
        if (Test-Unpacked (Join-Path $stage ".rinne-archive/$file") $stage) { throw "$Name $ver 解开后与档案对不上" }
        Move-Item -LiteralPath $stage -Destination $dest
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Installed $entry @($Spec.Args) $ver $via)) { throw "$Name $ver 装好后核对不过版本" }
    Write-Host "[装好] $Name $ver"
    $entry
}

# requirements-tools.txt 里 poetry-core 那一条（版本行加上以 \ 续接的哈希行）。
function Get-BackendRequirement([string]$Req) {
    $lines = @(Get-Content -LiteralPath $Req)
    $i = [array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match '^poetry-core==' })
    if ($i -lt 0) { throw 'requirements-tools.txt 里没有 poetry-core（reuse 的构建后端）' }
    $out = @($lines[$i])
    while ($lines[$i].TrimEnd().EndsWith('\') -and $i + 1 -lt $lines.Count) { $i++; $out += $lines[$i] }
    $out
}

# clang-format 与 reuse：独立 venv，两步按哈希装（见文件头）。返回 @{ 键 = 路径 }。
function Install-PyTools {
    $req = Join-Path $Root 'requirements-tools.txt'
    $tag = (Get-FileHash -LiteralPath $req -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
    $dest = Join-Path $ToolsDir "pytools-$tag"
    $bin = Join-Path $dest ($IsWindows ? 'Scripts' : 'bin')
    $ext = $IsWindows ? '.exe' : ''
    $py, $stamp = (Join-Path $bin "python$ext"), (Join-Path $dest '.rinne-complete')
    $text = Get-Content -LiteralPath $req -Raw
    $want = @{ 'clang-format' = [regex]::Match($text, '(?m)^clang-format==(\S+)').Groups[1].Value; reuse = [regex]::Match($text, '(?m)^reuse(?:\[[^\]]*\])?==(\S+)').Groups[1].Value }
    $versions = { foreach ($n in $want.Keys) { if (-not (Test-Installed (Join-Path $bin "$n$ext") @('--version') $want[$n] '')) { "$n 核对不过版本 $($want[$n])" } } }
    $pip = '--disable-pip-version-check', '-q', '--require-hashes'
    if (Test-Path -LiteralPath $dest) {
        if (-not (Test-Path -LiteralPath $stamp)) { throw "pytools-$tag 上次没装完；请先手动移走该目录" }
        $changed = @(& $py -I -c $RecordCheck 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "pytools-$tag 里已装的文件与安装记录不一致（$($changed -join '；')）；请先手动移走该目录" }
        $bad = @(& $versions)
        if ($bad.Count) { throw "pytools-$tag 里的 $($bad -join '、')；请先手动移走该目录" }
        Write-Host "[已装] clang-format $($want['clang-format'])、reuse $($want.reuse)（已装文件按 RECORD 核对过）"
    } else {
        try {
            & $Python -m venv $dest
            if ($LASTEXITCODE -ne 0) { throw "用 $Python 建 venv 失败" }
            $backend = Join-Path $dest 'build-backend.txt'
            Set-Content -LiteralPath $backend -Value (Get-BackendRequirement $req)
            & $py -m pip install @pip --no-deps --only-binary=:all: -r $backend
            if ($LASTEXITCODE -ne 0) { throw '按哈希装构建后端 poetry-core 失败' }
            & $py -m pip install @pip --no-build-isolation --only-binary=:all: --no-binary=reuse -r $req
            if ($LASTEXITCODE -ne 0) { throw 'pip 安装失败：哈希不符，或 requirements-tools.txt 没有这个 Python 版本的 wheel' }
            $bad = @(& $versions)
            if ($bad.Count) { throw "装好后 $($bad -join '、')" }
            Set-Content -LiteralPath $stamp -Value $tag
        } catch {
            Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction SilentlyContinue
            throw
        }
        Write-Host "[装好] clang-format $($want['clang-format'])、reuse $($want.reuse)"
    }
    [ordered]@{ 'rinne.clangFormat' = (Join-Path $bin "clang-format$ext"); 'rinne.reuse' = (Join-Path $bin "reuse$ext") }
}

if ($CheckHooks) {
    $bad = Test-Hooks
    exit [int]($bad -gt 0)
}

if (-not $ToolsDir) { throw '需要 -ToolsDir 或环境变量 RINNE_TOOLS_DIR' }
if (-not $IsWindows) { throw '清单里目前只登记了 Windows x64 的包' }
$all = @($Manifest.Keys) + 'pytools'
# pwsh -File 把 a,b,c 当成一个字符串传进来
$Tools = @($Tools | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if (-not $Tools.Count) { $Tools = @($all | Where-Object { $_ -notin 'ninja', 'pluginval' -or ($Manifest[$_].Pin -and (Test-Path -LiteralPath (Join-Path $Root $Manifest[$_].Pin))) }) }
$unknown = @($Tools | Where-Object { $_ -notin $all })
if ($unknown.Count) { throw "不认识的工具：$($unknown -join '、')（可选 $($all -join '、')）" }
$null = New-Item -ItemType Directory -Force -Path $ToolsDir
$config = [ordered]@{}
foreach ($name in $Tools | Sort-Object { [array]::IndexOf($all, $_) }) {
    if ($name -eq 'pytools') { (Install-PyTools).GetEnumerator() | ForEach-Object { $config[$_.Key] = $_.Value }; continue }
    $node = $config['rinne.node'] ?? (Get-Setting 'RINNE_NODE' 'rinne.node')
    $config[$Manifest[$name].Key] = Install-Archive $name $Manifest[$name] $node
}
Write-Host ''
Write-Host '在主 clone 里执行（值是本机路径，只写进 .git/config，不进仓库）：'
foreach ($k in $config.Keys) { Write-Host ("  git config {0} `"{1}`"" -f $k, ($config[$k] -replace '\\', '/')) }
