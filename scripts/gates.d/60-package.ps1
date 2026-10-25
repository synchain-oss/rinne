# SPDX-License-Identifier: GPL-3.0-or-later
#
# 打包，由 scripts/gates.ps1 加载：G9-pkg（Full 档）。一条 gate 依次跑下面几件事：
#   1. scripts/package.ps1 -Preflight：只跑不依赖构建产物的判据（版本与 tag 形态、许可双向闭合、
#      NOTICES 点名的声明文件、INSTALL.txt 可渲染、打包清单齐备）。这一步不碰构建目录。
#   2. scripts/package.ps1 -SelfTest：打包脚本自己的判据自测（坏样例与正例）。
#      紧接着是发版判据的两个自测：scripts/check-release-tag.ps1 -SelfTest（tag 门禁，含临时 git 仓里的取数层）与
#      node scripts/check-release-ready.mjs --self-test（发版就绪，含临时仓的端到端）。发版没有演练 tag，
#      这两道门第一次真跑就是真发版，所以每次 Full 档都在这里把它们的判据过一遍。
#   2c. bash 3.2 兼容扫描：git 跟踪的所有 *.sh 里，「$变量」后面紧跟非 ASCII 字符的写法一律判红（macOS 自带的
#      bash 3.2 会把它当成变量名的一部分，set -u 下脚本静默结束、退出码可能是 0）。带坏样例与正例自测。
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

# 跑一条外部命令（发版判据的自测用），返回退出码与输出行。命令的路径与参数都是本文件写死的，cwd 是仓根。
function Invoke-RinneRelSelfTest([string]$Root, [string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    Push-Location -LiteralPath $Root
    try {
        $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" })
        @{ Code = $LASTEXITCODE; Lines = $lines }
    } finally { Pop-Location }
}

# 发版判据的自测清单（纯函数：只拼命令，不执行）。Test-RinneGates 盯着它别被删。
function Get-RinneRelSelfTestSteps([string]$Root, [string]$Pwsh, [string]$Node) {
    @(
        @{ Label = 'check-release-tag.ps1 -SelfTest'; Exe = $Pwsh
            Arguments = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $Root 'scripts/check-release-tag.ps1'), '-SelfTest') }
        @{ Label = 'check-release-ready.mjs --self-test'; Exe = $Node
            Arguments = @((Join-Path $Root 'scripts/check-release-ready.mjs'), '--self-test') }
    )
}

# 逐个跑发版判据的自测，任何一个没过就返回 FAIL 结论（其余不再跑）；全过返回 $null。
# Runner 是 (Root, Exe, Arguments) -> @{ Code; Lines }：gate 里用真的，自测里换成假的。Lines 是调用方的 List，这里往里追加。
function Invoke-RinneRelSelfTests([string]$Root, [object[]]$Steps, [scriptblock]$Runner, $Lines) {
    foreach ($step in $Steps) {
        $rs = & $Runner $Root $step.Exe $step.Arguments
        $Lines.Add("--- $($step.Label)：退出码 $($rs.Code)")
        $Lines.AddRange([string[]]$rs.Lines)
        if ((ConvertTo-RinnePkgStatus $rs.Code) -ne 'PASS') {
            return New-RinnePkgResult (ConvertTo-RinnePkgStatus $rs.Code) "$($step.Label) 没过（退出码 $($rs.Code)）" $Lines.ToArray()
        }
    }
    return $null
}

# ── bash 3.2 兼容扫描 ─────────────────────────────────────────────────────────
# macOS 自带的 /bin/bash 是 3.2。变量名后紧跟非 ASCII 字符（全角标点、中文）时，3.2 在 UTF-8 下把多字节的首字节
# 当成变量名的一部分：set -u 下直接 unbound variable，随后整个脚本静默结束，调用它的 CI 步骤退出码可能是 0。
# Windows 与 Linux 的 bash 上看不出来，所以只能靠扫描：对 git 跟踪的所有 *.sh 逐个扫，命中即红（一律写成 ${name}）。
# mac CI 的 scripts/ci/mac/selftest.sh 扫同一个模式（那边在真 mac 上、用 BSD grep 跑），这里是本机 Full 档的那一份。
# 文件按字节读（Latin-1 一一对应），模式与 `LC_ALL=C grep -P` 的 \$[A-Za-z_][A-Za-z0-9_]*[\x80-\xff] 逐字相同。
# 不是 mac 上跑的脚本（只在 Linux 上跑，bash 4 以上）登记在这里，整条路径逐字匹配，不是前缀、不是后缀、不是通配。
$script:RinneBash32Pattern = '\$[A-Za-z_][A-Za-z0-9_]*[\x80-\xFF]'
$script:RinneBash32Exempt = @('tests/sanitizer-canary/run.sh')

# 登记过的 Linux 专用脚本（区分大小写，整条路径相等）。
function Test-RinneBash32Exempt([string]$RelPath) {
    $script:RinneBash32Exempt -ccontains $RelPath
}

# 一段字节里的命中：每个命中一行 "<行号>:<内容>"。匹配按 Latin-1 逐字节做；显示的内容按 UTF-8 还原
# （UTF-8 的多字节序列里不会出现 0x0A，两种解码切出来的行一一对应）。
function Get-RinneBash32Hits([byte[]]$Bytes) {
    $raw = [System.Text.Encoding]::Latin1.GetString($Bytes) -split "`n"
    $shown = [System.Text.Encoding]::UTF8.GetString($Bytes) -split "`n"
    $hits = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $raw.Count; $i++) {
        if ($raw[$i] -cmatch $script:RinneBash32Pattern) { $hits.Add("$($i + 1):$($shown[$i].TrimEnd("`r"))") }
    }
    $hits.ToArray()
}

# 一批文件的问题清单（纯函数）。Items 里每项有 Path（仓内相对路径）与 Bytes。登记过的路径跳过。
function Get-RinneBash32Problems([object[]]$Items) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($it in $Items) {
        if (Test-RinneBash32Exempt $it.Path) { continue }
        foreach ($h in (Get-RinneBash32Hits $it.Bytes)) { $out.Add("$($it.Path):$h") }
    }
    $out.ToArray()
}

# git 跟踪的所有 *.sh，连同它们的字节。Root 是仓根（也可以是自测造的临时 git 仓）。
function Get-RinneBash32Items([string]$Root) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    $files = @(& git -c core.quotepath=false -C $Root ls-files -- '*.sh' 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { throw "git ls-files 失败（退出码 $LASTEXITCODE）：$($files -join ' ')" }
    foreach ($f in $files) {
        if (-not $f) { continue }
        [pscustomobject]@{ Path = $f; Bytes = [IO.File]::ReadAllBytes((Join-Path $Root $f)) }
    }
}

# gate 里的一步：扫完有命中就返回 FAIL 结论，干净返回 $null。Lines 是调用方的 List，这里往里追加。
function Invoke-RinneBash32Scan([string]$Root, $Lines) {
    $items = @(Get-RinneBash32Items $Root)
    $problems = @(Get-RinneBash32Problems $items)
    $Lines.Add("--- bash 3.2 兼容扫描：git 跟踪的 *.sh 共 $($items.Count) 个，命中 $($problems.Count) 处")
    if ($problems.Count -eq 0) { return $null }
    foreach ($p in $problems) { $Lines.Add($p) }
    New-RinnePkgResult 'FAIL' "有 $($problems.Count) 处「`$变量」后面紧跟非 ASCII 字符（macOS 自带的 bash 3.2 会把它当成变量名的一部分；改成 `${name}）" $Lines.ToArray()
}

# Node 用钉版：先找环境变量 RINNE_NODE，再找 git config rinne.node，最后退回 PATH（与 50-web.ps1 同一套口径）。
function Get-RinnePkgNode($Ctx) {
    $p = & $Ctx.Setting 'RINNE_NODE' 'rinne.node'
    $p ? $p : 'node'
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

    # 2b. 发版判据的自测：tag 门禁与发版就绪。任何一个红都直接红，不往下打假包。
    $relSteps = Get-RinneRelSelfTestSteps $root (Get-Process -Id $PID).Path (Get-RinnePkgNode $Ctx)
    $relFail = Invoke-RinneRelSelfTests $root $relSteps ${function:Invoke-RinneRelSelfTest} $lines
    if ($relFail) { return $relFail }

    # 2c. bash 3.2 兼容扫描：命中即红，不往下打假包。
    $b32Fail = Invoke-RinneBash32Scan $root $lines
    if ($b32Fail) { return $b32Fail }

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
        # rinne-delete-ok: 自测在本进程 New-RinneTempDir 建的临时目录里造的假 bundle；路径由自测拼出，不是参数
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

    # 发版判据的自测清单：tag 门禁与发版就绪的自测必须在里面（它们没有演练 tag 可以兜底）。
    $relSteps = @(Get-RinneRelSelfTestSteps 'R' 'pwsh' 'node')
    [pscustomobject]@{ Label = '发版判据自测清单含 check-release-tag.ps1 -SelfTest'; Bad = $true
        Passed = (@($relSteps | Where-Object { $_.Arguments -contains '-SelfTest' -and ($_.Arguments -join ' ') -match 'check-release-tag\.ps1' }).Count -eq 1) }
    [pscustomobject]@{ Label = '发版判据自测清单含 check-release-ready.mjs --self-test'; Bad = $true
        Passed = (@($relSteps | Where-Object { $_.Arguments -contains '--self-test' -and ($_.Arguments -join ' ') -match 'check-release-ready\.mjs' }).Count -eq 1) }

    # 逐个跑自测的编排：全过返回 $null 且每个都跑了；有一个红就返回 FAIL 并且停在那里。
    $relCalls = New-Object System.Collections.Generic.List[string]
    $relLines = New-Object System.Collections.Generic.List[string]
    $okRunner = { param($r, $e, $a) $relCalls.Add($e); @{ Code = 0; Lines = @('ok') } }
    $relOk = Invoke-RinneRelSelfTests 'R' $relSteps $okRunner $relLines
    [pscustomobject]@{ Label = '发版判据自测编排：全部通过时返回空，且清单里每个自测都跑了'; Bad = $false
        Passed = ($null -eq $relOk) -and ($relCalls.Count -eq $relSteps.Count) -and ($relLines.Count -ge 2 * $relSteps.Count) }
    $relCalls.Clear(); $relLines.Clear()
    $failSecond = { param($r, $e, $a) $relCalls.Add($e); @{ Code = ($relCalls.Count -ge 2 ? 1 : 0); Lines = @() } }
    $relBad = Invoke-RinneRelSelfTests 'R' $relSteps $failSecond $relLines
    [pscustomobject]@{ Label = '发版判据自测编排：第二个自测红时整道 gate 记 FAIL，原因点名它'; Bad = $true
        Passed = ($relBad -and $relBad.Status -eq 'FAIL' -and $relBad.Reason -like "*$($relSteps[1].Label)*") }
    [pscustomobject]@{ Label = '发版判据自测编排：红了就停，后面的自测不再跑'; Bad = $true
        Passed = ($relCalls.Count -eq 2) }

    # bash 3.2 兼容扫描：坏样例要被报出来，正例不许误报；登记表只认整条路径；真走 git ls-files 的路径也过一遍。
    # 样例的字节在这里用 [char] 拼出来：这个文件自己不含「变量名后紧跟非 ASCII」的写法。
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $colon = [string][char]0xFF1A; $rparen = [string][char]0xFF09; $han = [string][char]0x4E2D
    $b32 = { param([string]$Text) @(Get-RinneBash32Hits $utf8.GetBytes($Text)).Count }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：$name 后紧跟全角冒号要报出来'; Bad = $true; Passed = (& $b32 ('echo "$name' + $colon + 'x"')) -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：单字母变量后紧跟全角括号要报出来'; Bad = $true; Passed = (& $b32 ('echo "($k' + $rparen)) -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：下划线变量名后紧跟非 ASCII 要报出来'; Bad = $true; Passed = (& $b32 ('echo "$_' + $colon + '"')) -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：变量后紧跟汉字要报出来'; Bad = $true; Passed = (& $b32 ('echo "$name' + $han + '"')) -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：不在引号里的 $name 后紧跟非 ASCII 要报出来'; Bad = $true; Passed = (& $b32 ('x=$name' + $colon)) -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：命中在第 3 行时报的行号是 3'; Bad = $true
        Passed = ((@(Get-RinneBash32Hits $utf8.GetBytes("echo ok`necho ok`ndie `"$han`$name$colon`"`n"))[0]) -like '3:*') }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：${name} 后紧跟非 ASCII 不误报'; Bad = $false; Passed = (& $b32 ('echo "${name}' + $colon + 'x"')) -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：特殊参数 $1 $# $? $@ 后紧跟非 ASCII 不误报'; Bad = $false; Passed = (& $b32 ('echo "$1' + $colon + '" "$#' + $colon + '" "$?' + $colon + '" "$@' + $colon + '"')) -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：${x:-汉字} 的默认值里有汉字不误报'; Bad = $false; Passed = (& $b32 ('echo "${have:-' + $han + '}"')) -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：变量后面先有空格不误报'; Bad = $false; Passed = (& $b32 ('echo "$name x' + $colon + '"')) -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：变量在引号里、非 ASCII 在引号外不误报'; Bad = $false; Passed = (& $b32 ('echo "$name"' + $colon)) -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：行尾是 CRLF 不误报'; Bad = $false; Passed = (& $b32 "echo `"`$name`"`r`n") -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：纯中文行不误报'; Bad = $false; Passed = (& $b32 ('echo ' + $han + $colon)) -eq 0 }
    # 接线：gate 主体里真的调了这一步（删掉那一行调用，别的用例都还绿，只有这一条会红），而且在假版本打包之前。
    $gateSrc = (${function:Invoke-RinnePackageGate}).ToString()
    [pscustomobject]@{ Label = 'bash 3.2 扫描：gate 主体里调用了它，且在假版本打包之前'; Bad = $true
        Passed = ($gateSrc.IndexOf('Invoke-RinneBash32Scan') -ge 0 -and $gateSrc.IndexOf('Invoke-RinneBash32Scan') -lt $gateSrc.IndexOf('# 3. 假版本打包')) }
    $badItem = { param([string]$P) [pscustomobject]@{ Path = $P; Bytes = $utf8.GetBytes('echo "$name' + $colon + '"') } }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：登记过的 Linux 专用脚本放行（整条路径相等）'; Bad = $false
        Passed = @(Get-RinneBash32Problems @(& $badItem 'tests/sanitizer-canary/run.sh')).Count -eq 0 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：登记表不是后缀匹配'; Bad = $true
        Passed = @(Get-RinneBash32Problems @(& $badItem 'scripts/tests/sanitizer-canary/run.sh')).Count -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：登记表不是前缀匹配'; Bad = $true
        Passed = @(Get-RinneBash32Problems @(& $badItem 'tests/sanitizer-canary/run.sh.bak')).Count -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：登记表区分大小写'; Bad = $true
        Passed = @(Get-RinneBash32Problems @(& $badItem 'Tests/sanitizer-canary/run.sh')).Count -eq 1 }
    [pscustomobject]@{ Label = 'bash 3.2 扫描：打包脚本不在登记表里'; Bad = $true
        Passed = @(Get-RinneBash32Problems @(& $badItem 'scripts/package-macos.sh')).Count -eq 1 }
    # 整仓：临时 git 仓里放好文件 → 绿；再放一个坏文件 → 红并点名；登记路径里的坏写法放行；未跟踪的 .sh 与非 .sh 不扫。
    $b32tmp = New-RinneTempDir -Prefix 'rinne-gate-b32-selftest-'
    try {
        $null = & git -C $b32tmp init -q 2>&1
        $null = & git -C $b32tmp config core.autocrlf false 2>&1
        $null = New-Item -ItemType Directory -Path (Join-Path $b32tmp 'scripts'), (Join-Path $b32tmp 'tests/sanitizer-canary'), (Join-Path $b32tmp 'other') -Force
        $wr = { param($rel, $text) [IO.File]::WriteAllBytes((Join-Path $b32tmp $rel), $utf8.GetBytes($text)); $null = & git -C $b32tmp add -- $rel 2>&1 }
        & $wr 'scripts/good.sh' ('echo "${name}' + $colon + 'x"' + "`n")
        $scanClean = New-Object System.Collections.Generic.List[string]
        [pscustomobject]@{ Label = 'bash 3.2 整仓扫描：只有好文件时通过'; Bad = $false; Passed = ($null -eq (Invoke-RinneBash32Scan $b32tmp $scanClean)) }
        & $wr 'scripts/bad.sh' ('echo "$name' + $colon + 'x"' + "`n")
        $scanLines = New-Object System.Collections.Generic.List[string]
        $scanRes = Invoke-RinneBash32Scan $b32tmp $scanLines
        [pscustomobject]@{ Label = 'bash 3.2 整仓扫描：混进一个坏文件就红，并点名文件与行号'; Bad = $true
            Passed = ($scanRes -and $scanRes.Status -eq 'FAIL' -and ($scanLines -contains ('scripts/bad.sh:1:echo "$name' + $colon + 'x"'))) }
        $null = & git -C $b32tmp rm -q -f --cached -- scripts/bad.sh 2>&1
        & $wr 'tests/sanitizer-canary/run.sh' ('echo "$name' + $colon + 'x"' + "`n")
        [pscustomobject]@{ Label = 'bash 3.2 整仓扫描：登记过的路径里有坏写法也放行'; Bad = $false
            Passed = ($null -eq (Invoke-RinneBash32Scan $b32tmp (New-Object System.Collections.Generic.List[string]))) }
        & $wr 'other/run.sh' ('echo "$name' + $colon + 'x"' + "`n")
        [pscustomobject]@{ Label = 'bash 3.2 整仓扫描：同名文件在别的目录里不放行'; Bad = $true
            Passed = ($null -ne (Invoke-RinneBash32Scan $b32tmp (New-Object System.Collections.Generic.List[string]))) }
        $null = & git -C $b32tmp rm -q -f --cached -- other/run.sh 2>&1
        [IO.File]::WriteAllBytes((Join-Path $b32tmp 'other/untracked.sh'), $utf8.GetBytes('echo "$name' + $colon + 'x"' + "`n"))
        & $wr 'other/notes.txt' ('echo "$name' + $colon + 'x"' + "`n")
        [pscustomobject]@{ Label = 'bash 3.2 整仓扫描：未跟踪的 .sh 与非 .sh 文件不扫'; Bad = $false
            Passed = ($null -eq (Invoke-RinneBash32Scan $b32tmp (New-Object System.Collections.Generic.List[string]))) }
    } finally {
        $b32left = Remove-RinneTempDir -Path $b32tmp -WaitSeconds 5
        [pscustomobject]@{ Label = 'bash 3.2 整仓扫描自测建的临时目录删掉了'; Bad = $true; Passed = ($null -eq $b32left) }
    }

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
