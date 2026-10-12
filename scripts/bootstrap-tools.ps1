#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 按钉版文件把本机工具装进 -ToolsDir，或检查本机的 hooks 配置。
#   pwsh scripts/bootstrap-tools.ps1 [-ToolsDir <目录>] [-Tools node,gitleaks,…] [-Python <python>]
#   pwsh scripts/bootstrap-tools.ps1 -CheckHooks
# -ToolsDir 缺省取环境变量 RINNE_TOOLS_DIR，两者都没有就停下。版本取自仓内钉版文件：.node-version、.gitleaks-version、
# .actionlint-version、.prettier-version；clang-format 与 reuse 取 requirements-tools.txt。ninja 还没有钉版文件、
# pluginval 的钉版文件构建骨架入库时才有，两者先用下面清单里的版本，只在 -Tools 点名或钉版文件出现后才装。
# 每个包先下载进自己新建的空目录，sha256 与清单逐字一致才解开，解开后整个目录改名为 <工具>-<版本>；已有同名目录时
# 只核对版本，不覆盖、不删除。Python 工具装进按 requirements-tools.txt 内容命名的独立 venv，由 pip --require-hashes
# 逐个核对哈希。不改 PATH、不写注册表、不全局安装，仓内不写任何本机路径：装完只打印该在主 clone 里执行的
# git config（值只进 .git/config）。
# 清单的 sha256：gitleaks、actionlint 取官方 checksums.txt，node 取 SHASUMS256.txt，prettier 的 npm 包与 registry 的
# dist.integrity（sha512）对拍过；ninja 与 pluginval 上游不发校验文件，登记的是两次独立下载一致的值。
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
    foreach ($h in 'commit-msg', 'pre-push') { $rows.Add(@((Test-Path -LiteralPath (Join-Path $want $h) -PathType Leaf), "主 clone 的 scripts/hooks/$h 存在")) }
    $needles = Get-Setting 'RINNE_PRIVACY_LOCAL' 'rinne.privacyLocal'
    $rows.Add(@(($needles -and (Test-Path -LiteralPath $needles -PathType Leaf)), ($needles ? '个人针文件存在' : '没有配置个人针文件（RINNE_PRIVACY_LOCAL 或 git config rinne.privacyLocal）')))
    foreach ($r in $rows) { Write-Host "[$($r[0] ? 'ok' : 'FAIL')] $($r[1])" }
    @($rows | Where-Object { -not $_[0] }).Count
}

function Get-PinVersion($Spec) {
    $pin = $Spec.Pin ? (Join-Path $Root $Spec.Pin) : ''
    if ($pin -and (Test-Path -LiteralPath $pin)) { return (Get-Content -LiteralPath $pin -Raw).Trim() -replace '^v', '' }
    $Spec.Default
}

function Test-Installed([string]$Exe, [string[]]$Arguments, [string]$Version, [string]$Node) {
    $out = if ($Node) { & $Node $Exe @Arguments 2>&1 } else { & $Exe @Arguments 2>&1 }
    ($LASTEXITCODE -eq 0) -and ("$out" -match "(?<![\d.])$([regex]::Escape($Version))(?![\d.])")
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
    if (Test-Path -LiteralPath $dest) {
        if (-not (Test-Installed $entry @($Spec.Args) $ver $via)) { throw "$Name-$ver 目录已存在但核对不过版本；不覆盖，请先手动移走" }
        Write-Host "[已装] $Name $ver"
        return $entry
    }
    $url = $Spec.Url -f $ver
    $work = Join-Path $ToolsDir ".download-$Name-$ver-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
    $null = New-Item -ItemType Directory -Path $work
    try {
        $file = Join-Path $work ($url -split '/')[-1]
        Write-Host "[下载] $url"
        Invoke-WebRequest -Uri $url -OutFile $file
        $got = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($got -cne $sha) { throw "$Name $ver 的 sha256 不符：期望 $sha，实际 $got；不解开" }
        $stage = Join-Path $work 'unpacked'
        $null = New-Item -ItemType Directory -Path $stage
        if ($file -like '*.zip') { Expand-Archive -LiteralPath $file -DestinationPath $stage }
        else {
            $tar = Join-Path $work 'pkg.tar'
            $in, $out = [IO.File]::OpenRead($file), [IO.File]::Create($tar)
            try { $gz = [IO.Compression.GZipStream]::new($in, [IO.Compression.CompressionMode]::Decompress); $gz.CopyTo($out); $gz.Dispose() }
            finally { $in.Dispose(); $out.Dispose() }
            [System.Formats.Tar.TarFile]::ExtractToDirectory($tar, $stage, $false)
        }
        Move-Item -LiteralPath $stage -Destination $dest
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Installed $entry @($Spec.Args) $ver $via)) { throw "$Name $ver 装好后核对不过版本" }
    Write-Host "[装好] $Name $ver"
    $entry
}

# clang-format 与 reuse：独立 venv，pip 按 requirements-tools.txt 的哈希逐个核对。返回 @{ 键 = 路径 }。
function Install-PyTools {
    $req = Join-Path $Root 'requirements-tools.txt'
    $tag = (Get-FileHash -LiteralPath $req -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
    $dest = Join-Path $ToolsDir "pytools-$tag"
    $bin = Join-Path $dest ($IsWindows ? 'Scripts' : 'bin')
    $text = Get-Content -LiteralPath $req -Raw
    $want = @{ 'clang-format' = [regex]::Match($text, '(?m)^clang-format==(\S+)').Groups[1].Value; reuse = [regex]::Match($text, '(?m)^reuse(?:\[[^\]]*\])?==(\S+)').Groups[1].Value }
    if (-not (Test-Path -LiteralPath $dest)) {
        & $Python -m venv $dest
        if ($LASTEXITCODE -ne 0) { throw "用 $Python 建 venv 失败" }
        & (Join-Path $bin 'python') -m pip install --disable-pip-version-check -q --require-hashes -r $req
        if ($LASTEXITCODE -ne 0) { throw 'pip 安装失败：哈希不符，或 requirements-tools.txt 没有这个 Python 版本的 wheel' }
    }
    foreach ($n in $want.Keys) {
        if (-not (Test-Installed (Join-Path $bin $n) @('--version') $want[$n] '')) { throw "pytools-$tag 里的 $n 核对不过版本 $($want[$n])；不覆盖，请先手动移走该目录" }
    }
    Write-Host "[就绪] clang-format $($want['clang-format'])、reuse $($want.reuse)"
    [ordered]@{ 'rinne.clangFormat' = (Join-Path $bin 'clang-format'); 'rinne.reuse' = (Join-Path $bin 'reuse') }
}

if ($CheckHooks) {
    $bad = Test-Hooks
    exit [int]($bad -gt 0)
}

if (-not $ToolsDir) { throw '需要 -ToolsDir 或环境变量 RINNE_TOOLS_DIR' }
if (-not $IsWindows) { throw '清单里目前只登记了 Windows x64 的包' }
$all = @($Manifest.Keys) + 'pytools'
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
