# SPDX-License-Identifier: GPL-3.0-or-later
#
# 打包，由 scripts/gates.ps1 加载：G9-pkg（Full 档）。一条 gate 跑四件事：
#   1. scripts/package.ps1 -Preflight：只跑不依赖构建产物的判据（版本与 tag 形态、许可双向闭合、
#      NOTICES 点名的声明文件、INSTALL.txt 可渲染、打包清单齐备）。这一步不碰构建目录。
#   2. scripts/package.ps1 -SelfTest：打包脚本自己的判据自测（坏样例与正例）。
#   3. 假版本打包：在本进程用 New-RinneTempDir 建的临时目录里造一个假 BuildDir（两个 .vst3，各含
#      Contents/x86_64-win/<名字>），跑 package.ps1 打一个 0.0.0-local 的假包，再把 zip 的根条目与
#      packaging/manifest-win64.txt 逐条比对。
#   4. 两条负例：假 BuildDir 里放一个 "Rinne Probe Input.vst3"、以及用一份多了一项的清单 —— 两者都必须红。
#      这两条不在 package.ps1 的自测里重复做一遍：这里验的是「从外面看这条 gate 真会红」。
#
# 临时目录纪律：只用 scripts/lib/TempDir.psm1 的 New-RinneTempDir / Remove-RinneTempDir（自带真实路径核对）；
# **绝不删参数传入的路径**；Remove-RinneTempDir 删不掉时只记原因不强行删。
#
# 不持构建槽（Lock = none）：这道 gate 不编译，用的是自己造的假 BuildDir。
# 与 G9-web（页面冒烟）不冲突：那道持 gui 锁。

# 造一个假的 bundle 目录：<Dir>/<BundleName>/Contents/x86_64-win/<BundleName>。
# 纯文件操作，不碰仓库、不碰构建目录。返回 bundle 目录的路径。
function New-RinneFakeBundle([string]$Dir, [string]$BundleName = 'Rinne Input.vst3', [string]$Body = 'fake-binary') {
    $b = Join-Path $Dir $BundleName
    $null = New-Item -ItemType Directory -Path (Join-Path $b 'Contents\x86_64-win') -Force
    [IO.File]::WriteAllText((Join-Path $b "Contents\x86_64-win\$BundleName"), $Body, (New-Object System.Text.UTF8Encoding($false)))
    $b
}

# 读一个 zip 的根条目集合（第一段路径；目录单元以 / 结尾）。返回 @{ Roots; Stray }。
# 目录单元与打包脚本一致：两个 bundle 目录、LICENSES/、third_party/notices/。
function Get-RinneGateZipRoots([string]$ZipPath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
    $dirUnits = @('Rinne Input.vst3/', 'Rinne Output.vst3/', 'LICENSES/', 'third_party/notices/')
    $roots = New-Object System.Collections.Generic.List[string]
    $stray = New-Object System.Collections.Generic.List[string]
    $zr = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($e in $zr.Entries) {
            $n = $e.FullName
            $hit = $null
            foreach ($d in $dirUnits) { if ($n.StartsWith($d, [StringComparison]::Ordinal)) { $hit = $d; break } }
            if ($hit) { if (-not $roots.Contains($hit)) { $roots.Add($hit) }; continue }
            if ($n.Contains('/')) { if (-not $stray.Contains($n)) { $stray.Add($n) }; continue }
            if (-not $roots.Contains($n)) { $roots.Add($n) }
        }
    } finally { $zr.Dispose() }
    @{ Roots = [string[]]$roots.ToArray(); Stray = [string[]]$stray.ToArray() }
}

# 读清单文件（去掉空行与注释行）。
function Get-RinneGateManifestLines([string]$Path) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($l in ([IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8) -split "`r?`n")) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $out.Add($t)
    }
    $out.ToArray()
}

# 一道 gate 跑完的结论。
function New-RinnePkgResult([string]$Status, [string]$Reason, [string[]]$Lines = @()) {
    @{ Status = $Status; Reason = $Reason; Lines = @($Lines) }
}

# package.ps1 的退出码到 gate 结论的映射：0 记 PASS，别的记 FAIL。
# 打包判据没有「跳过」这一说：它不依赖构建产物，跑不动就是判据没过。
function ConvertTo-RinnePkgStatus([int]$Code) {
    if ($Code -eq 0) { 'PASS' } else { 'FAIL' }
}

# 跑 package.ps1，返回退出码与输出行。
function Invoke-RinnePkgScript([string]$Root, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    $pwsh = (Get-Process -Id $PID).Path
    $lines = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/package.ps1') @Arguments 2>&1 | ForEach-Object { "$_" })
    @{ Code = $LASTEXITCODE; Lines = $lines }
}

function Invoke-RinnePackageGate($Ctx) {
    $root = $Ctx.Root
    $lines = New-Object System.Collections.Generic.List[string]
    $pkg = Join-Path $root 'scripts/package.ps1'
    if (-not (Test-Path -LiteralPath $pkg -PathType Leaf)) {
        return New-RinnePkgResult 'FAIL' "仓里缺 scripts/package.ps1" @()
    }

    # 1. preflight：只跑不依赖构建产物的判据。
    $pf = Invoke-RinnePkgScript $root @('-Preflight')
    $lines.Add("--- package.ps1 -Preflight：退出码 $($pf.Code)")
    $lines.AddRange([string[]]$pf.Lines)
    if ((ConvertTo-RinnePkgStatus $pf.Code) -ne 'PASS') {
        return New-RinnePkgResult (ConvertTo-RinnePkgStatus $pf.Code) "package.ps1 -Preflight 没过（退出码 $($pf.Code)）" $lines.ToArray()
    }

    # 2. 打包脚本自己的判据自测。
    $st = Invoke-RinnePkgScript $root @('-SelfTest')
    $lines.Add("--- package.ps1 -SelfTest：退出码 $($st.Code)")
    $lines.AddRange([string[]]$st.Lines)
    if ((ConvertTo-RinnePkgStatus $st.Code) -ne 'PASS') {
        return New-RinnePkgResult (ConvertTo-RinnePkgStatus $st.Code) "package.ps1 -SelfTest 没过（退出码 $($st.Code)）" $lines.ToArray()
    }

    # 3. 假版本打包 + 清单比对。0.0.0-local 只是本地假版本号：不涉及任何 tag，也不写进任何仓内文件。
    $tmp = New-RinneTempDir -Prefix 'rinne-gate-pkg-'
    try {
        $fakeBuild = Join-Path $tmp 'build'
        $null = New-Item -ItemType Directory -Path $fakeBuild -Force
        $null = New-RinneFakeBundle $fakeBuild 'Rinne Input.vst3' 'input-binary'
        $null = New-RinneFakeBundle $fakeBuild 'Rinne Output.vst3' 'output-binary'
        $fakeCommit = '0123456789abcdef0123456789abcdef01234567'
        $outDir = Join-Path $tmp 'out'
        $pkgArgs = @('-Version', '0.0.0-local', '-Tag', 'v0.0.0-local', '-BuildDir', $fakeBuild, '-OutDir', $outDir, '-SourceCommit', $fakeCommit)
        $pk = Invoke-RinnePkgScript $root $pkgArgs
        $lines.Add("--- 假版本打包：退出码 $($pk.Code)")
        $lines.AddRange([string[]]$pk.Lines)
        if ($pk.Code -ne 0) {
            return New-RinnePkgResult 'FAIL' "假版本打包没过（退出码 $($pk.Code)）" $lines.ToArray()
        }
        $zip = Join-Path $outDir 'Rinne-v0.0.0-local-win64.zip'
        if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) {
            return New-RinnePkgResult 'FAIL' "假版本打包没出 zip：$zip" $lines.ToArray()
        }

        # zip 的根条目与 packaging/manifest-win64.txt 逐条比对（集合相等 + 数量相等）。
        $manifestPath = Join-Path $root 'packaging/manifest-win64.txt'
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            return New-RinnePkgResult 'FAIL' "仓里缺 packaging/manifest-win64.txt" $lines.ToArray()
        }
        $want = @(Get-RinneGateManifestLines $manifestPath)
        $roots = Get-RinneGateZipRoots $zip
        $lines.Add("zip 根条目：$($roots.Roots -join '、')")
        $diff = New-Object System.Collections.Generic.List[string]
        foreach ($w in $want) { if ($roots.Roots -notcontains $w) { $diff.Add("zip 里缺清单上的根条目：$w") } }
        foreach ($h in $roots.Roots) { if ($want -notcontains $h) { $diff.Add("zip 里出现清单外的根条目：$h") } }
        if ($want.Count -ne $roots.Roots.Count) { $diff.Add("根条目数量不对：清单 $($want.Count) 条，zip $($roots.Roots.Count) 条") }
        if ($roots.Stray.Count) { $diff.Add("zip 里出现清单外的目录条目：$($roots.Stray[0])") }
        if ($diff.Count) {
            $diff | ForEach-Object { $lines.Add("清单比对：$_") }
            return New-RinnePkgResult 'FAIL' "假包的根条目与 packaging/manifest-win64.txt 不一致" $lines.ToArray()
        }

        # 4a. 负例：假 BuildDir 里混进探针 → 必须红。
        $null = New-RinneFakeBundle $fakeBuild 'Rinne Probe Input.vst3' 'probe-binary'
        $probe = Invoke-RinnePkgScript $root (@('-Version', '0.0.0-local', '-Tag', 'v0.0.0-local', '-BuildDir', $fakeBuild,
                '-OutDir', (Join-Path $tmp 'out-probe'), '-SourceCommit', $fakeCommit))
        $lines.Add("--- 负例：BuildDir 里有 Rinne Probe Input.vst3，退出码 $($probe.Code)")
        if ($probe.Code -eq 0) {
            return New-RinnePkgResult 'FAIL' '假 BuildDir 里有 Rinne Probe Input.vst3 时打包仍然成功（判据没生效）' $lines.ToArray()
        }
        Remove-Item -LiteralPath (Join-Path $fakeBuild 'Rinne Probe Input.vst3') -Recurse -Force

        # 4b. 负例：清单多一项 → 必须红。清单文件放在本进程建的临时目录里，不写进仓内。
        # 清单路径是绝对路径：Join-Path 会把绝对子路径拼坏（变成 C:\repo\C:\temp\…），所以直接用绝对路径。
        $badManifest = [IO.Path]::GetFullPath((Join-Path $tmp 'manifest-extra.txt'))
        [IO.File]::WriteAllText($badManifest, (($want + 'EXTRA/') -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
        $bad = Invoke-RinnePkgScript $root (@('-Version', '0.0.0-local', '-Tag', 'v0.0.0-local', '-BuildDir', $fakeBuild,
                '-OutDir', [IO.Path]::GetFullPath((Join-Path $tmp 'out-manifest')), '-SourceCommit', $fakeCommit, '-Manifest', $badManifest))
        $lines.Add("--- 负例：打包清单多一项，退出码 $($bad.Code)")
        if ($bad.Code -eq 0) {
            return New-RinnePkgResult 'FAIL' '打包清单多一项时打包仍然成功（判据没生效）' $lines.ToArray()
        }

        New-RinnePkgResult 'PASS' "preflight、自测、假版本打包（根条目 $($roots.Roots.Count) 条与清单相同）与两条负例都过" $lines.ToArray()
    } finally {
        # 只删本进程建过的临时目录：走模块的删除（它自己按真实路径核对）。删不掉只记原因。
        $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 5
        if ($left) { Write-Host "G9-pkg：临时目录没删掉（$left）" -ForegroundColor Yellow }
    }
}

# 自测（运行器传入仓根）：判据自测的退出码到 gate 结论的映射，以及清单比对的纯函数。
function Test-RinneGates([string]$Root) {
    # 退出码到结论的映射：0 记 PASS，别的记 FAIL（打包判据没有「跳过」这一说：它不依赖构建产物）。
    $map = { param($c) ConvertTo-RinnePkgStatus $c }
    [pscustomobject]@{ Label = 'package.ps1 退出 0 记 PASS'; Bad = $false; Passed = (& $map 0) -eq 'PASS' }
    [pscustomobject]@{ Label = 'package.ps1 退出非 0 记 FAIL'; Bad = $true; Passed = (& $map 1) -eq 'FAIL' }

    # 清单比对的纯函数：一致时没有差异，多一项与少一项都要被报出来。
    $ok = @('Rinne Input.vst3/', 'Rinne Output.vst3/', 'INSTALL.txt', 'LICENSE', 'LICENSES/', 'SHA256SUMS', 'THIRD-PARTY-NOTICES.md', 'third_party/notices/')
    $diff = { param($want, $have) $d = New-Object System.Collections.Generic.List[string]
        foreach ($w in $want) { if ($have -notcontains $w) { $d.Add("缺 $w") } }
        foreach ($h in $have) { if ($want -notcontains $h) { $d.Add("多 $h") } }
        if ($want.Count -ne $have.Count) { $d.Add('数量不对') }
        $d.ToArray() }
    [pscustomobject]@{ Label = '清单比对：一致时没有差异'; Bad = $false; Passed = (@(& $diff $ok $ok)).Count -eq 0 }
    [pscustomobject]@{ Label = '清单比对：清单多一项要报出来'; Bad = $true; Passed = (@(& $diff @($ok + 'EXTRA/') $ok)).Count -gt 0 }
    [pscustomobject]@{ Label = '清单比对：清单少一项要报出来'; Bad = $true; Passed = (@(& $diff @($ok | Where-Object { $_ -ne 'LICENSE' }) $ok)).Count -gt 0 }

    # 假 BuildDir 的形状：两个 bundle，各含 Contents/x86_64-win/<名字>。
    $tmp = New-RinneTempDir -Prefix 'rinne-gate-pkg-selftest-'
    try {
        $b = New-RinneFakeBundle $tmp 'Rinne Input.vst3' 'x'
        $ok2 = (Test-Path -LiteralPath (Join-Path $b 'Contents\x86_64-win\Rinne Input.vst3') -PathType Leaf)
        [pscustomobject]@{ Label = '假 BuildDir：bundle 里有 Contents/x86_64-win/<名字>'; Bad = $false; Passed = $ok2 }
        $bundles = @(Get-ChildItem -LiteralPath $tmp -Recurse -Directory -Filter '*.vst3')
        [pscustomobject]@{ Label = '假 BuildDir：按 *.vst3 找得到那个 bundle'; Bad = $false; Passed = ($bundles.Count -eq 1) }
    } finally {
        $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 5
        [pscustomobject]@{ Label = '自测建的临时目录删掉了'; Bad = $true; Passed = ($null -eq $left) }
    }
}

function Register-RinneGates {
    @{ Id = 'G9-pkg'; Tier = 'Full'; Name = '打包'; Lock = 'none'; Run = ${function:Invoke-RinnePackageGate} }
}
