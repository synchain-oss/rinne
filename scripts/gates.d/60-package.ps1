# SPDX-License-Identifier: GPL-3.0-or-later
#
# 打包，由 scripts/gates.ps1 加载：G9-pkg（Full 档）。一条 gate 依次跑下面几件事：
#   1. scripts/package.ps1 -Preflight：只跑不依赖构建产物的判据（版本与 tag 形态、许可双向闭合、
#      NOTICES 点名的声明文件、INSTALL.txt 可渲染、打包清单齐备）。这一步不碰构建目录。
#   2. scripts/package.ps1 -SelfTest：打包脚本自己的判据自测（坏样例与正例）。
#      紧接着是发版判据的两个自测：scripts/check-release-tag.ps1 -SelfTest（tag 门禁，含临时 git 仓里的取数层）与
#      node scripts/check-release-ready.mjs --self-test（发版就绪，含临时仓的端到端）。发版没有演练 tag，
#      这两道门第一次真跑就是真发版，所以每次 Full 档都在这里把它们的判据过一遍。
#   2c. bash 3.2 兼容扫描：git 跟踪的所有 *.sh 里，「$变量」后面紧跟非 ASCII 字节（以及 ASCII 控制字节）的写法一律判红（macOS 自带的
#      bash 3.2 会把它当成变量名的一部分，set -u 下脚本静默结束、退出码可能是 0）。带坏样例与正例自测。
#   2d. EXIT trap 的「走完了」标记检查：git 跟踪的所有 *.sh 里，凡是装了 EXIT trap 的，都要带约定写法（RINNE_RAN_TO_END 标记；
#      bash 3.2 下变量展开出错会静默结束脚本，进 trap 的 $? 常常是 0，没有标记就会以 0 退出）。带坏样例与正例自测。
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
# 文件按字节读（Latin-1 一一对应，一个字节一个字符），所以字符类就是字节类。
# 判的是哪些字节：mac 侧 selftest.sh 用 LC_ALL=C 的 [^[:print:][:space:]]，C locale 下 [:print:] 是 0x20-0x7E、[:space:] 是 0x09-0x0D 与 0x20，
# 即命中「不在 0x09-0x0D 与 0x20-0x7E 里」的字节：0x00-0x08、0x0E-0x1F、0x7F 这些 ASCII 控制字节，加上 0x80-0xFF 的全部非 ASCII 字节。
# 这里的 [^\x09-\x0D\x20-\x7E] 是同一个字节集合（下面 Test-RinneGates 里 0x00-0xFF 逐字节的用例钉着；mac 侧有 0x01-0xFF 的同款用例）。
# ASCII 控制字节其实不会被 bash 3.2 并进变量名，两边都从严一并判红：*.sh 里本来就不该有裸的控制字节。
# 不是 mac 上跑的脚本（只在 Linux 上跑，bash 4 以上）登记在这里，整条路径逐字匹配，不是前缀、不是后缀、不是通配；
# 后面的 EXIT trap 标记检查共用这张登记表，tests/sanitizer-canary/run.sh 的两处都改掉之后才可以把它删掉。
$script:RinneBash32Pattern = '\$[A-Za-z_][A-Za-z0-9_]*[^\x09-\x0D\x20-\x7E]'
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

# ── EXIT trap 的「走完了」标记检查 ─────────────────────────────────────────────
# macOS 自带的 bash 3.2 在 set -u 下遇到变量展开出错会直接结束整个脚本；脚本装了 EXIT trap 时，进入 trap 的 $? 常常是 0
# （出事前最后一条命令的状态），于是脚本半路死了却以 0 退出，调用它的 CI 步骤是绿的。对策是「走完了」标记：主流程正常结束处
# （以及每个合法的提前 exit 0 之前）置位；EXIT trap 里退出码为 0 而标记没置位，就改成非 0 并打印 ::error::。
# 约定写法固定，git 跟踪的 *.sh 里凡是装了 EXIT trap（trap … EXIT，或末尾写 0）的，都要同时带下面五样，按行扫，注释行与空行不算：
#   RINNE_RAN_TO_END=0                                      装 trap 之前初始化（整个文件里有这一行）
#   RINNE_RAN_TO_END=1                                      主流程正常结束处置位（整个文件里有这一行）
# 下面三样只认 EXIT trap 的处理函数的函数体里（trap 后面直接写函数名，不写带引号的命令；函数定义写成「名字() {」独占一行，
# 右花括号独占一行、与定义行同缩进）：
#   rc=$?                                                   函数体的第一条命令（可以带 local）：一进来先取退出码；行首与 rc 之间
#                                                           只许空白与 local，所以 foo_rc=$?、self_test_rc=$? 不算；只认第一条，
#                                                           是因为前面再有别的命令，$? 就已经被冲掉了
#   [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]         退出码为 0、标记却没置位（前后要有分隔符，写在字符串里的不算；
#                                                           因为 rc=$? 必是函数体的第一条，判据读到的 rc 一定是取过的）
#   脚本没有走完就退出了                                      报错文字
# 所以「foo_rc=$?」「别处的 || rc=$?」「写在别的函数里的判据」都不算数；处理程序是带引号的命令、找不到函数定义、
# 函数写在一行里、右花括号对不上缩进，一律判红：宁可多报，约定写法就这一种。
# mac CI 的 scripts/ci/mac/selftest.sh 扫同一套写法（外加一个用 /bin/bash 跑的动态用例），这里是本机 Full 档的那一份；
# 两侧的报错文字也一字不差。
# 文件同样按 Latin-1 逐字节读；空白类写成显式的 ASCII 字符类（不用 \s：.NET 的 \s 会把 0x85、0xA0 这两个字节也算空白）。
# RETURN、INT、TERM 的 trap 不是 EXIT trap，不在此列。字符串里写出「trap … EXIT」也会被当成装了 trap：宁可多报。
# 登记表与上面的 bash 3.2 兼容扫描共用（Test-RinneBash32Exempt）。
$script:RinneExitTrapPattern = '(^|[^A-Za-z0-9_])trap[ \t\r\f\v].*[ \t\r\f\v](EXIT|0)([ \t\r\f\v;})]|$)'
# trap 后面直接是函数名（可带 --），函数名之后只有信号名，信号表里有 EXIT 或 0；函数名在命名组 name 里
$script:RinneExitHandlerPattern = '(^|[^A-Za-z0-9_])trap[ \t\r\f\v]+(--[ \t\r\f\v]+)?(?<name>[A-Za-z_][A-Za-z0-9_]*)[ \t\r\f\v]+([A-Za-z0-9_]+[ \t\r\f\v]+)*(EXIT|0)([ \t\r\f\v;})]|$)'
$script:RinneExitInitPattern = '(^|[ \t\r\f\v;{(])RINNE_RAN_TO_END=0([ \t\r\f\v;})]|$)'
$script:RinneExitFinPattern = '(^|[ \t\r\f\v;{(])RINNE_RAN_TO_END=1([ \t\r\f\v;})]|$)'
$script:RinneExitRcFirstPattern = '^[ \t\r\f\v]*(local[ \t\r\f\v]+)?rc=\$\?([ \t\r\f\v;]|$)'
$script:RinneExitGuardPattern = '(^|[ \t\r\f\v;{(])\[ "\$rc" -eq 0 \] && \[ "\$RINNE_RAN_TO_END" != 1 \]([ \t\r\f\v;})]|$)'
$script:RinneExitSkipPattern = '^[ \t\r\f\v]*(#|$)'
# 报错文字是中文：文件按 Latin-1 读，所以拿它的 UTF-8 字节按 Latin-1 还原成同样的字符串再比
$script:RinneExitText = [System.Text.Encoding]::Latin1.GetString([System.Text.Encoding]::UTF8.GetBytes('脚本没有走完就退出了'))

# 一个文件（字节）的检查：返回 @{ HasTrap; Missing }。没装 EXIT trap 时 Missing 为空；装了就逐条列出缺的东西。
# 整个文件读成行（去掉注释行与空行），先扫出 EXIT trap 的处理函数名，再在每个处理函数的函数体（定义行的下一行起，
# 到与定义行同缩进的右花括号前）里核三样。输出的文字与顺序同 selftest.sh 的 exit_guard_scan_file。
function Get-RinneExitGuardInfo([byte[]]$Bytes) {
    $code = @([System.Text.Encoding]::Latin1.GetString($Bytes) -split "`n" | Where-Object { $_ -cnotmatch $script:RinneExitSkipPattern })
    $hasTrap = $false
    foreach ($l in $code) { if ($l -cmatch $script:RinneExitTrapPattern) { $hasTrap = $true; break } }
    $missing = New-Object System.Collections.Generic.List[string]
    if ($hasTrap) {
        $initOk = $false; $finOk = $false; $unres = $false
        $names = New-Object System.Collections.Generic.List[string]
        foreach ($l in $code) {
            if ($l -cmatch $script:RinneExitInitPattern) { $initOk = $true }
            if ($l -cmatch $script:RinneExitFinPattern) { $finOk = $true }
            if ($l -cmatch $script:RinneExitTrapPattern) {
                if ($l -cmatch $script:RinneExitHandlerPattern) {
                    $nm = $Matches['name']
                    if (-not $names.Contains($nm)) { $names.Add($nm) }
                } else { $unres = $true }
            }
        }
        # 装了 EXIT trap 却一个处理函数名也没认出来：按处理程序不是函数名处理（不会发生，防的是两处正则漂移）
        if ($names.Count -eq 0) { $unres = $true }
        $nodef = New-Object System.Collections.Generic.List[string]
        $noend = New-Object System.Collections.Generic.List[string]
        $rcBad = $false; $guardBad = $false; $textBad = $false
        foreach ($name in $names) {
            $defRe = '^(?<indent>[ \t\r\f\v]*)(function[ \t\r\f\v]+' + $name + '([ \t\r\f\v]*\(\))?|' + $name + '[ \t\r\f\v]*\(\))[ \t\r\f\v]*\{[ \t\r\f\v]*$'
            $found = $false
            for ($i = 0; $i -lt $code.Count; $i++) {
                if (-not ($code[$i] -cmatch $defRe)) { continue }
                $found = $true
                $endRe = '^' + $Matches['indent'] + '\}([ \t\r\f\v;]|$)'
                $end = -1
                for ($j = $i + 1; $j -lt $code.Count; $j++) {
                    if ($code[$j] -cmatch $endRe) { $end = $j; break }
                }
                if ($end -lt 0) { $noend.Add($name); continue }
                if (($i + 1) -ge $end -or -not ($code[$i + 1] -cmatch $script:RinneExitRcFirstPattern)) { $rcBad = $true }
                $g = $false; $t = $false
                for ($j = $i + 1; $j -lt $end; $j++) {
                    if ($code[$j] -cmatch $script:RinneExitGuardPattern) { $g = $true }
                    if ($code[$j].Contains($script:RinneExitText)) { $t = $true }
                }
                if (-not $g) { $guardBad = $true }
                if (-not $t) { $textBad = $true }
            }
            if (-not $found) { $nodef.Add($name) }
        }
        if (-not $initOk) { $missing.Add('缺 RINNE_RAN_TO_END=0（装 trap 之前初始化）') }
        if (-not $finOk) { $missing.Add('缺 RINNE_RAN_TO_END=1（主流程正常结束处置位）') }
        if ($unres) { $missing.Add('EXIT trap 的处理程序不是函数名（要写成「trap 函数名 EXIT」，不写带引号的命令）') }
        foreach ($n in $nodef) { $missing.Add("找不到 EXIT trap 处理函数 ${n} 的定义（要写成「${n}() {」独占一行）") }
        foreach ($n in $noend) { $missing.Add("EXIT trap 处理函数 ${n} 找不到函数体的结尾（右花括号要独占一行、与定义行同缩进）") }
        if ($rcBad) { $missing.Add('缺 rc=$?（EXIT trap 处理函数的第一条命令要先取退出码）') }
        if ($guardBad) { $missing.Add('缺 [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]（EXIT trap 处理函数里：退出码为 0、标记却没置位）') }
        if ($textBad) { $missing.Add('缺报错文字「脚本没有走完就退出了」（要写在 EXIT trap 处理函数里）') }
    }
    @{ HasTrap = $hasTrap; Missing = [string[]]$missing.ToArray() }
}

# 一批文件的问题清单（纯函数）。Items 里每项有 Path 与 Bytes。登记过的路径跳过。每条 "<路径>: <缺什么>"；Count 是装了 EXIT trap 的个数。
function Get-RinneExitGuardProblems([object[]]$Items) {
    $out = New-Object System.Collections.Generic.List[string]
    $withTrap = 0
    foreach ($it in $Items) {
        if (Test-RinneBash32Exempt $it.Path) { continue }
        $info = Get-RinneExitGuardInfo $it.Bytes
        if ($info.HasTrap) { $withTrap++ }
        foreach ($m in $info.Missing) { $out.Add("$($it.Path): $m") }
    }
    @{ Problems = [string[]]$out.ToArray(); WithTrap = $withTrap }
}

# gate 里的一步：扫完有问题就返回 FAIL 结论，干净返回 $null。Lines 是调用方的 List，这里往里追加。
function Invoke-RinneExitGuardScan([string]$Root, $Lines) {
    $items = @(Get-RinneBash32Items $Root)
    $res = Get-RinneExitGuardProblems $items
    $Lines.Add("--- EXIT trap 标记检查：git 跟踪的 *.sh 共 $($items.Count) 个，装了 EXIT trap 的 $($res.WithTrap) 个（登记过的除外），缺约定写法的 $($res.Problems.Count) 处")
    if ($res.Problems.Count -eq 0) { return $null }
    foreach ($p in $res.Problems) { $Lines.Add($p) }
    New-RinnePkgResult 'FAIL' "有 $($res.Problems.Count) 处装了 EXIT trap 的 *.sh 缺「走完了」标记的约定写法（macOS 自带的 bash 3.2 下变量展开出错会静默结束脚本，退出码可能是 0；写法见 60-package.ps1 与 scripts/ci/mac/selftest.sh 里这一节的注释）" $Lines.ToArray()
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

    # 2d. EXIT trap 的「走完了」标记检查：装了 EXIT trap 的 *.sh 缺约定写法即红，不往下打假包。
    $egFail = Invoke-RinneExitGuardScan $root $lines
    if ($egFail) { return $egFail }

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
    # 控制字节：变量后紧跟只含 ASCII 控制字节的样例，两边（mac 侧的 [^[:print:][:space:]] 与这里的字节类）都要命中。
    $ctl = { param([int]$Byte) @(Get-RinneBash32Hits ([byte[]]($utf8.GetBytes('echo "$name') + [byte[]]@($Byte) + $utf8.GetBytes('x"')))).Count }
    foreach ($c in @(@(0x01, 'SOH'), @(0x08, '退格，空白类之前的最后一个控制字节'), @(0x0E, '空白类之后的第一个控制字节'), @(0x1B, 'ESC'), @(0x1F, '0x1F'), @(0x7F, 'DEL'), @(0x00, 'NUL'))) {
        [pscustomobject]@{ Label = ('bash 3.2 扫描：变量后紧跟 ASCII 控制字节 0x{0:X2}（{1}）要报出来' -f $c[0], $c[1]); Bad = $true; Passed = (& $ctl $c[0]) -eq 1 }
    }
    foreach ($c in @(@(0x09, 'TAB'), @(0x0B, 'VT'), @(0x0C, 'FF'), @(0x0D, 'CR'), @(0x20, '空格'), @(0x7E, '~，可打印 ASCII 的最后一个'))) {
        [pscustomobject]@{ Label = ('bash 3.2 扫描：变量后紧跟 0x{0:X2}（{1}）不误报' -f $c[0], $c[1]); Bad = $false; Passed = (& $ctl $c[0]) -eq 0 }
    }
    # 逐字节：「变量名 + 一个字节」，0x00-0xFF 里恰好不在 0x09-0x0D 与 0x20-0x7E 的字节命中（和 mac 侧 [^[:print:][:space:]] 的字节集合相同）
    $sweepWrong = New-Object System.Collections.Generic.List[string]
    for ($bv = 0; $bv -le 255; $bv++) {
        $want = -not ((($bv -ge 9) -and ($bv -le 13)) -or (($bv -ge 32) -and ($bv -le 126)))
        $hit = (& $ctl $bv) -ge 1
        if ($hit -ne $want) { $sweepWrong.Add('0x{0:X2}' -f $bv) }
    }
    [pscustomobject]@{ Label = "bash 3.2 扫描：逐字节 0x00-0xFF，恰好不在 0x09-0x0D、0x20-0x7E 的字节命中（不符的字节：$(if ($sweepWrong.Count) { $sweepWrong -join ' ' } else { '无' })）"
        Bad = $true; Passed = ($sweepWrong.Count -eq 0) }
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

    # EXIT trap 的「走完了」标记检查：少一样就红并点名，写法只是长得像不算，没装 EXIT trap 的不管；整仓也真走 git ls-files 过一遍。
    # rc=$?、判据、报错文字三样只认 EXIT trap 处理函数的函数体：别处的、长得像的（foo_rc=$?）、不在第一条命令位置的都不算。
    $egI = 'RINNE_RAN_TO_END=0'; $egF = 'RINNE_RAN_TO_END=1'
    $egR = '  local rc=$?'
    $egG = '  if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then'
    $egT = '    echo "::error::脚本没有走完就退出了"; exit 1'
    $egFull = @('#!/bin/bash', 'set -euo pipefail', $egI, 'on_exit() {', $egR, $egG, $egT, '  fi', '}', 'trap on_exit EXIT', 'work', $egF)
    $egMissing = { param([string[]]$Lines, [string]$Sep = "`n") [string[]]@((Get-RinneExitGuardInfo $utf8.GetBytes(($Lines -join $Sep) + $Sep)).Missing) }
    $egJoin = { param([string[]]$Lines) (@(& $egMissing $Lines)) -join '|' }
    $egMsg = @{
        Init = '缺 RINNE_RAN_TO_END=0（装 trap 之前初始化）'
        Fin = '缺 RINNE_RAN_TO_END=1（主流程正常结束处置位）'
        Unres = 'EXIT trap 的处理程序不是函数名（要写成「trap 函数名 EXIT」，不写带引号的命令）'
        Rc = '缺 rc=$?（EXIT trap 处理函数的第一条命令要先取退出码）'
        Guard = '缺 [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]（EXIT trap 处理函数里：退出码为 0、标记却没置位）'
        Text = '缺报错文字「脚本没有走完就退出了」（要写在 EXIT trap 处理函数里）'
    }
    $egNoDef = { param([string]$N) "找不到 EXIT trap 处理函数 ${N} 的定义（要写成「${N}() {」独占一行）" }
    $egNoEnd = { param([string]$N) "EXIT trap 处理函数 ${N} 找不到函数体的结尾（右花括号要独占一行、与定义行同缩进）" }
    # 坏样例：整个文件算出来的缺项（用 | 连起来）必须恰好等于期望；好样例：一项都不缺
    $egBad = { param([string]$Label, [string[]]$Lines, [string]$Expect) [pscustomobject]@{ Label = "EXIT trap 标记检查：$Label"; Bad = $true; Passed = ((& $egJoin $Lines) -ceq $Expect) } }
    $egOk = { param([string]$Label, [string[]]$Lines) [pscustomobject]@{ Label = "EXIT trap 标记检查：$Label"; Bad = $false; Passed = (@(& $egMissing $Lines).Count -eq 0) } }
    $egWithout = { param([string]$Line) [string[]]@($egFull | Where-Object { $_ -cne $Line }) }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：五样齐全不误报'; Bad = $false; Passed = (@(& $egMissing $egFull).Count -eq 0) }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：行尾是 CRLF 的齐全写法不误报'; Bad = $false; Passed = (@(& $egMissing $egFull "`r`n").Count -eq 0) }
    & $egBad '没有初始化 RINNE_RAN_TO_END=0 要点名' (& $egWithout $egI) $egMsg.Init
    & $egBad '从不置位 RINNE_RAN_TO_END=1 要点名' (& $egWithout $egF) $egMsg.Fin
    & $egBad 'trap 函数里没有先取 rc=$? 要点名' (& $egWithout $egR) $egMsg.Rc
    & $egBad '没有「退出码为 0、标记没置位」的判据要点名' (& $egWithout $egG) $egMsg.Guard
    & $egBad '没有报错文字要点名' (& $egWithout $egT) $egMsg.Text
    $egCommented = [string[]]@($egFull | ForEach-Object { if ($_ -ceq 'trap on_exit EXIT') { $_ } else { "# $_" } })
    & $egBad '标记只写在注释里不算（只剩 trap 一行：缺初始化、缺置位、找不到处理函数的定义）' $egCommented ((@($egMsg.Init, $egMsg.Fin, (& $egNoDef 'on_exit'))) -join '|')
    $egNoFn = (@($egMsg.Init, $egMsg.Fin, $egMsg.Unres)) -join '|'
    & $egBad 'if 的一行分支里装 EXIT trap 认得出来（引号命令不是函数名）' @('#!/bin/bash', "if true; then trap 'rm -f x' EXIT; fi") $egNoFn
    & $egBad 'trap 末尾写 0（等同 EXIT）认得出来' @("trap 'rm -f x' 0") $egNoFn
    & $egBad 'trap 装在函数里（缩进）认得出来' @('f() {', "    trap 'rm -f x' EXIT", '}') $egNoFn
    & $egBad '信号列表里带 EXIT 认得出来' @("trap 'rm -f x' INT EXIT") $egNoFn
    $egFnLines = @('on_exit() {', $egR, $egG, $egT, '  fi', '}')
    & $egBad '变量名带前缀的同名写法不算初始化' (@('PKG_RINNE_RAN_TO_END=0') + $egFnLines + @($egF, 'trap on_exit EXIT')) $egMsg.Init
    & $egBad 'RINNE_RAN_TO_END=10 不算置位 1' (@($egI, 'RINNE_RAN_TO_END=10') + $egFnLines + @('trap on_exit EXIT')) $egMsg.Fin
    # rc=$? 的判据：处理函数的第一条命令，行首与 rc 之间只许空白与 local
    foreach ($rcn in @('foo_rc', 'self_test_rc', 'main_rc', 'xrc')) {
        & $egBad "处理函数里只有 ${rcn}=`$?，没有 rc=`$?" @($egI, 'on_exit() {', ('  local ' + $rcn + '=$?'), $egG, $egT, '  fi', '}', 'trap on_exit EXIT', $egF) $egMsg.Rc
    }
    & $egBad 'rc=$? 写在别的函数与别的行里，处理函数里没有' @($egI, 'other() {', '  rc=$?', '  return "$rc"', '}', 'on_exit() {', $egG, $egT, '  fi', '}', 'trap on_exit EXIT', 'work || rc=$?', 'main "$@" || main_rc=$?', 'self_test; self_rc=$?', $egF) $egMsg.Rc
    & $egBad 'rc=$? 不是处理函数的第一条命令' @($egI, 'on_exit() {', '  cleanup_temp "$tmp"', $egR, $egG, $egT, '  fi', '}', 'trap on_exit EXIT', $egF) $egMsg.Rc
    & $egBad '处理函数是空的' @($egI, 'on_exit() {', '}', 'trap on_exit EXIT', $egF) ((@($egMsg.Rc, $egMsg.Guard, $egMsg.Text)) -join '|')
    & $egBad '「退出码为 0、标记没置位」的判据写在别的函数里' @($egI, 'helper() {', $egG, '  fi', '}', 'on_exit() {', $egR, $egT, '}', 'trap on_exit EXIT', $egF) $egMsg.Guard
    & $egBad '报错文字写在别的函数里' @($egI, 'helper() {', $egT, '}', 'on_exit() {', $egR, $egG, '  fi', '}', 'trap on_exit EXIT', $egF) $egMsg.Text
    & $egBad '判据只是写在字符串里（前后没有分隔符）' @($egI, 'on_exit() {', $egR, "  echo '[ `"`$rc`" -eq 0 ] && [ `"`$RINNE_RAN_TO_END`" != 1 ]'", $egT, '}', 'trap on_exit EXIT', $egF) $egMsg.Guard
    & $egBad '处理程序是带引号的命令（不是函数名）' (@($egI) + $egFnLines + @('trap "on_exit" EXIT', $egF)) $egMsg.Unres
    & $egBad 'EXIT trap 的处理函数找不到定义' (@($egI) + $egFnLines + @('trap other_exit EXIT', $egF)) (& $egNoDef 'other_exit')
    & $egBad '处理函数写在一行里（不认）' @($egI, 'on_exit() { local rc=$?; if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then echo "::error::脚本没有走完就退出了"; exit 1; fi; }', 'trap on_exit EXIT', $egF) (& $egNoDef 'on_exit')
    & $egBad '右花括号对不上定义行的缩进' @($egI, 'on_exit() {', $egR, $egG, $egT, '  fi', '  }', 'trap on_exit EXIT', $egF) (& $egNoEnd 'on_exit')
    & $egBad '两个处理函数，第二个缺判据' @($egI, 'h1() {', $egR, $egG, $egT, '  fi', '}', 'h2() {', $egR, $egT, '}', 'trap h1 EXIT', 'trap h2 EXIT', $egF) $egMsg.Guard
    & $egBad 'rc=$? 后面紧跟别的字符不算' @($egI, 'on_exit() {', ('  local rc' + '=$?x'), $egG, $egT, '  fi', '}', 'trap on_exit EXIT', $egF) $egMsg.Rc
    & $egBad '判据后面紧跟引号（写在字符串里）不算' @($egI, 'on_exit() {', $egR, '  echo "x [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]"', $egT, '}', 'trap on_exit EXIT', $egF) $egMsg.Guard
    & $egBad '判据前面紧跟别的字符（粘在别的词后面）不算' @($egI, 'on_exit() {', $egR, '  if x[ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then', $egT, '  fi', '}', 'trap on_exit EXIT', $egF) $egMsg.Guard
    & $egBad '函数处理程序之外还有一个带引号的 EXIT trap（后装的会盖掉前一个）' (@($egI) + $egFnLines + @('trap on_exit EXIT', "trap 'rm -f x' EXIT", $egF)) $egMsg.Unres
    & $egOk '处理函数里行首的 rc=$?（不带 local）' @($egI, 'on_exit() {', 'rc=$?', $egG, $egT, '  fi', '}', 'trap on_exit EXIT', $egF)
    & $egOk '函数体里有注释与空行，rc=$? 后面跟注释' @($egI, 'on_exit() {', '', '  # 先取退出码，别在前面放别的命令', '  local rc=$? # 退出码', $egG, $egT, '  fi', '}', 'trap on_exit EXIT', $egF)
    & $egOk 'function 关键字的写法、信号列表里带别的信号' @($egI, 'function on_exit {', $egR, $egG, $egT, '  fi', '}', 'trap on_exit INT EXIT', $egF)
    & $egOk 'trap -- 函数名 EXIT，定义写成「名字 () {」' @($egI, 'on_exit () {', $egR, $egG, $egT, '  fi', '}', 'trap -- on_exit EXIT', $egF)
    & $egOk '两个处理函数各自齐全' @($egI, 'h1() {', $egR, $egG, $egT, '  fi', '}', 'h2() {', $egR, $egG, $egT, '  fi', '}', 'trap h1 EXIT', 'trap h2 EXIT', $egF)
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：只有 RETURN trap 不误报'; Bad = $false; Passed = (@(& $egMissing @("trap 'rm -rf x' RETURN")).Count -eq 0) }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：只有 INT、TERM trap 不误报'; Bad = $false; Passed = (@(& $egMissing @("trap 'exit 130' INT", "trap 'exit 143' TERM")).Count -eq 0) }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：注释里提到 trap … EXIT 不误报'; Bad = $false; Passed = (@(& $egMissing @("# trap 'rm -f x' EXIT")).Count -eq 0) }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：引号里的 0 不当成 EXIT'; Bad = $false; Passed = (@(& $egMissing @("trap 'echo 0' INT")).Count -eq 0) }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：空文件不误报'; Bad = $false; Passed = (@(Get-RinneExitGuardInfo ([byte[]]@())).Missing.Count -eq 0) }
    $egIndented = [string[]]@($egFull | ForEach-Object { if ($_ -ceq $egF) { '    --self-test) RINNE_RAN_TO_END=1; exit 0 ;;' } else { "    $_" } })
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：整体缩进、置位写在 case 分支里不误报'; Bad = $false; Passed = (@(& $egMissing $egIndented).Count -eq 0) }
    # 接线：gate 主体里真的调了这一步，而且在假版本打包之前
    $gateSrc2 = (${function:Invoke-RinnePackageGate}).ToString()
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：gate 主体里调用了它，且在假版本打包之前'; Bad = $true
        Passed = ($gateSrc2.IndexOf('Invoke-RinneExitGuardScan') -ge 0 -and $gateSrc2.IndexOf('Invoke-RinneExitGuardScan') -lt $gateSrc2.IndexOf('# 3. 假版本打包')) }
    # 登记表与 bash 3.2 扫描共用：整条路径相等
    $egItem = { param([string]$P, [string[]]$Lines = @('#!/bin/bash', "trap 'rm -f x' EXIT")) [pscustomobject]@{ Path = $P; Bytes = $utf8.GetBytes(($Lines -join "`n") + "`n") } }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：登记过的 Linux 专用脚本放行（整条路径相等）'; Bad = $false
        Passed = (Get-RinneExitGuardProblems @(& $egItem 'tests/sanitizer-canary/run.sh')).Problems.Count -eq 0 }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：登记表不是后缀匹配'; Bad = $true
        Passed = (Get-RinneExitGuardProblems @(& $egItem 'scripts/tests/sanitizer-canary/run.sh')).Problems.Count -eq 3 }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：登记表区分大小写'; Bad = $true
        Passed = (Get-RinneExitGuardProblems @(& $egItem 'Tests/sanitizer-canary/run.sh')).Problems.Count -eq 3 }
    [pscustomobject]@{ Label = 'EXIT trap 标记检查：打包脚本不在登记表里'; Bad = $true
        Passed = (Get-RinneExitGuardProblems @(& $egItem 'scripts/package-macos.sh')).Problems.Count -eq 3 }
    # 整仓：临时 git 仓里放好文件 → 绿并数得出个数；再放坏文件 → 红并点名；登记路径里放行；未跟踪的 .sh 与非 .sh 不扫。
    $egtmp = New-RinneTempDir -Prefix 'rinne-gate-eg-selftest-'
    try {
        $null = & git -C $egtmp init -q 2>&1
        $null = & git -C $egtmp config core.autocrlf false 2>&1
        $null = New-Item -ItemType Directory -Path (Join-Path $egtmp 'scripts'), (Join-Path $egtmp 'tests/sanitizer-canary'), (Join-Path $egtmp 'other') -Force
        $egw = { param($rel, [string[]]$Lines) [IO.File]::WriteAllBytes((Join-Path $egtmp $rel), $utf8.GetBytes(($Lines -join "`n") + "`n")); $null = & git -C $egtmp add -- $rel 2>&1 }
        $badTrap = @('#!/bin/bash', "trap 'rm -f x' EXIT")
        & $egw 'scripts/good.sh' $egFull
        $egLines = New-Object System.Collections.Generic.List[string]
        [pscustomobject]@{ Label = 'EXIT trap 整仓扫描：只有好文件时通过，并数出装了 EXIT trap 的 1 个'; Bad = $false
            Passed = ($null -eq (Invoke-RinneExitGuardScan $egtmp $egLines)) -and (($egLines -join "`n") -like '*装了 EXIT trap 的 1 个*') }
        & $egw 'scripts/bad.sh' $badTrap
        $egLines2 = New-Object System.Collections.Generic.List[string]
        $egRes = Invoke-RinneExitGuardScan $egtmp $egLines2
        [pscustomobject]@{ Label = 'EXIT trap 整仓扫描：混进一个没标记的脚本就红，并点名文件与缺的东西'; Bad = $true
            Passed = ($egRes -and $egRes.Status -eq 'FAIL' -and ($egLines2 -contains ('scripts/bad.sh: ' + $egMsg.Init))) }
        $null = & git -C $egtmp rm -q -f --cached -- scripts/bad.sh 2>&1
        & $egw 'tests/sanitizer-canary/run.sh' $badTrap
        [pscustomobject]@{ Label = 'EXIT trap 整仓扫描：登记过的路径里没标记也放行'; Bad = $false
            Passed = ($null -eq (Invoke-RinneExitGuardScan $egtmp (New-Object System.Collections.Generic.List[string]))) }
        & $egw 'other/run.sh' $badTrap
        [pscustomobject]@{ Label = 'EXIT trap 整仓扫描：同名文件在别的目录里不放行'; Bad = $true
            Passed = ($null -ne (Invoke-RinneExitGuardScan $egtmp (New-Object System.Collections.Generic.List[string]))) }
        $null = & git -C $egtmp rm -q -f --cached -- other/run.sh 2>&1
        [IO.File]::WriteAllBytes((Join-Path $egtmp 'other/untracked.sh'), $utf8.GetBytes(($badTrap -join "`n") + "`n"))
        & $egw 'other/notes.txt' $badTrap
        [pscustomobject]@{ Label = 'EXIT trap 整仓扫描：未跟踪的 .sh 与非 .sh 文件不扫'; Bad = $false
            Passed = ($null -eq (Invoke-RinneExitGuardScan $egtmp (New-Object System.Collections.Generic.List[string]))) }
    } finally {
        $egleft = Remove-RinneTempDir -Path $egtmp -WaitSeconds 5
        [pscustomobject]@{ Label = 'EXIT trap 整仓扫描自测建的临时目录删掉了'; Bad = $true; Passed = ($null -eq $egleft) }
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
