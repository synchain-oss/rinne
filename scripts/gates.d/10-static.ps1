# SPDX-License-Identifier: GPL-3.0-or-later
#
# Quick 档的 gate：G1 预检、G1-juce、G2 格式、G3 合规、G4 提交、G5 docs-truth。由 scripts/gates.ps1 加载。
# G2–G5 与 CI 的 format、compliance、branch-gate 三个 workflow 调用同一批命令、同样的参数，判据逐条对应。不同只有：
#   - 个人针读仓外的针表文件（--needles 缺省为 local），CI 读 secret；
#   - 提交区间是 <Base>..HEAD，CI 是 PR 的 base..head；
#   - 工作树的 gitleaks 扫 HEAD 里每个跟踪文件的导出（checkout-index，不受 export-ignore 影响）：CI 的 checkout 里只有
#     入库的文件，本机工作树里还可能有被忽略的构建产物；
#   - PR 标题与正文由 pr-open 在发出前检查，这里没有 PR。
# G3 跑的 --only 清单由 Test-RinneGates 对着 compliance.yml 原文核对，workflow 加了检查而这里没跟上时自测变红。
# 工具先找环境变量，再找 git config（主 clone 的本地配置对它的所有 worktree 生效），最后退回 PATH；键名见 $Tools。
# prettier 配成 prettier.cjs 这类脚本时用钉版 node 运行。版本由 G1 对着钉版文件逐个核对。

$script:Tools = [ordered]@{
    node        = @{ Env = 'RINNE_NODE'; Key = 'rinne.node'; Pin = '.node-version'; Path = 'node'; Args = '--version'; Re = '^v(\S+)' }
    gitleaks    = @{ Env = 'RINNE_GITLEAKS'; Key = 'rinne.gitleaks'; Pin = '.gitleaks-version'; Path = 'gitleaks'; Args = 'version'; Re = '^v?(\d+\.\d+\.\d+)' }
    actionlint  = @{ Env = 'RINNE_ACTIONLINT'; Key = 'rinne.actionlint'; Pin = '.actionlint-version'; Path = 'actionlint'; Args = '-version'; Re = '^v?(\d+\.\d+\.\d+)' }
    prettier    = @{ Env = 'RINNE_PRETTIER'; Key = 'rinne.prettier'; Pin = '.prettier-version'; Path = 'prettier'; Args = '--version'; Re = '^(\d+\.\d+\.\d+)' }
    clangFormat = @{ Env = 'RINNE_CLANG_FORMAT'; Key = 'rinne.clangFormat'; Pin = '.clang-format-version'; Path = 'clang-format'; Args = '--version'; Re = 'clang-format version (\d+\.\d+\.\d+)' }
    reuse       = @{ Env = 'RINNE_REUSE'; Key = 'rinne.reuse'; Pin = 'requirements-tools.txt'; PinRe = '(?m)^reuse(?:\[[^\]]*\])?==(\S+)'; Path = 'reuse'; Args = '--version'; Re = 'reuse, version (\S+)' }
}

function Get-ToolPath($Ctx, [string]$Name) {
    $t = $script:Tools[$Name]
    $p = & $Ctx.Setting $t.Env $t.Key
    $p ? $p : $t.Path
}

# 返回命令数组（可执行文件 + 前置参数）；单元素数组从函数返回时会被拆成字符串，调用方一律按 [string[]] 接。
function Get-ToolCommand($Ctx, [string]$Name) {
    $p = Get-ToolPath $Ctx $Name
    if ($p -match '\.[cm]?js$') { @((Get-ToolPath $Ctx node), $p) } else { @($p) }
}

function Invoke-Native([string]$Label, [string[]]$Command, [string[]]$Arguments, [string]$Cwd) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    Push-Location -LiteralPath $Cwd
    try {
        $exe, $pre = $Command[0], @($Command | Select-Object -Skip 1)
        $lines = @(& $exe @pre @Arguments 2>&1 | ForEach-Object { "$_" -replace '\x1b\[[0-9;]*m', '' })
        $code = $LASTEXITCODE
    } catch {
        $lines, $code = @("$Label：$($_.Exception.Message)"), 127
    } finally {
        Pop-Location
    }
    [pscustomobject]@{ Label = $Label; Code = $code; Lines = $lines }
}

function Step([string]$Label, [string[]]$Command, [string[]]$Arguments, [string]$Cwd = '') {
    @{ Label = $Label; Command = $Command; Arguments = $Arguments; Cwd = $Cwd }
}

# 依次跑完全部步骤（前面失败也接着跑，一次报全），任何一步非 0 即 FAIL。
function Invoke-Steps($Ctx, [object[]]$Steps) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $failed = @(foreach ($s in $Steps) {
            $r = Invoke-Native $s.Label $s.Command $s.Arguments ($s.Cwd ? $s.Cwd : $Ctx.Root)
            $lines.Add("--- $($s.Label)：退出码 $($r.Code)")
            foreach ($l in $r.Lines) { $lines.Add($l) }
            if ($r.Code -ne 0) { $s.Label }
        })
    @{ Status = $failed.Count ? 'FAIL' : 'PASS'; Reason = $failed.Count ? "失败：$($failed -join '、')" : "$($Steps.Count) 步全过"; Lines = $lines.ToArray() }
}

function Test-ToolVersion($Ctx, [string]$Name) {
    $t = $script:Tools[$Name]
    $pin = Get-Content -LiteralPath (Join-Path $Ctx.Root $t.Pin) -Raw
    $want = $t.PinRe ? [regex]::Match($pin, $t.PinRe).Groups[1].Value : $pin.Trim()
    $r = Invoke-Native $Name (Get-ToolCommand $Ctx $Name) @($t.Args) $Ctx.Root
    $have = [regex]::Match(($r.Lines -join "`n"), "(?m)$($t.Re)").Groups[1].Value
    $where = (& $Ctx.Setting $t.Env $t.Key) ? '' : "（未配置 $($t.Env) 或 git config $($t.Key)，用的是 PATH 上的）"
    [pscustomobject]@{ Ok = $r.Code -eq 0 -and $have -ceq $want; Text = "$Name $($have ? $have : '找不到')，钉版 $want（$($t.Pin)）$where" }
}

function Get-GhLogin {
    $saved = $env:GH_TOKEN
    try {
        $token = "$(gh auth token --user DLsnows 2>$null)".Trim()
        if (-not $token) { return '' }
        $env:GH_TOKEN = $token
        "$(gh api user --jq .login 2>$null)".Trim()
    } catch { '' } finally { $env:GH_TOKEN = $saved }
}

# G1：工作区干净、gh 账号、仓内身份、remote、hooks 与个人针文件、工具版本。逐项列出，任何一项不过即 FAIL。
function Invoke-Preflight($Ctx) {
    $rows = [System.Collections.Generic.List[object]]::new()
    $row = { param($Ok, $Text) $rows.Add([pscustomobject]@{ Ok = [bool]$Ok; Text = $Text }) }
    $cfg = { param($k) "$(git -C $Ctx.Root config --get $k 2>$null)".Trim() }
    $changes = @(git -C $Ctx.Root status --porcelain --untracked-files=normal)
    & $row (-not $changes.Count) ($changes.Count ? "工作区不干净：$($changes.Count) 处改动或未跟踪的文件，先提交或清掉" : '工作区干净')
    $login = Get-GhLogin
    & $row ($login -ceq 'DLsnows') "gh 的 DLsnows 凭据解析为 $($login ? $login : '（取不到，查 gh auth status 与网络）')"
    & $row ((& $cfg user.name) -ceq 'DLsnows' -and (& $cfg user.email) -ceq 'noreply@synchain.ca') '仓内 user.name / user.email 是 DLsnows <noreply@synchain.ca>'
    & $row ("$(git -C $Ctx.Root remote get-url origin 2>$null)" -cmatch '^https://DLsnows@github\.com/') 'origin 的 URL 带 DLsnows@'
    $hooks = Invoke-Native 'hooks' @((Get-Process -Id $PID).Path) @('-NoProfile', '-NonInteractive', '-File', (Join-Path $Ctx.Root 'scripts/bootstrap-tools.ps1'), '-CheckHooks') $Ctx.Root
    & $row ($hooks.Code -eq 0) "hooks：$($hooks.Lines -join '；')"
    foreach ($n in $script:Tools.Keys) { $v = Test-ToolVersion $Ctx $n; & $row $v.Ok $v.Text }
    $bad = @($rows | Where-Object { -not $_.Ok })
    $lines = @($rows | ForEach-Object { "[$($_.Ok ? 'ok' : 'FAIL')] $($_.Text)" })
    @{ Status = $bad.Count ? 'FAIL' : 'PASS'; Reason = $bad.Count ? ($bad.Text -join '；') : "$($rows.Count) 项全过"; Lines = $lines }
}

# .juce-commit 是 JUCE 版本的唯一真源；JUCE 路径只经参数、环境变量或 git config 传入，不进仓库。
function Invoke-JuceCheck($Ctx) {
    $pin = Join-Path $Ctx.Root '.juce-commit'
    if (-not (Test-Path -LiteralPath $pin)) { return @{ Status = 'SKIP'; Reason = '仓内还没有 .juce-commit（构建骨架入库时才有），不查 JUCE' } }
    $want = (Get-Content -LiteralPath $pin -Raw).Trim()
    $dir = $Ctx.JucePath ? $Ctx.JucePath : (& $Ctx.Setting 'RINNE_JUCE_PATH' 'rinne.jucePath')
    if (-not $dir) { return @{ Status = 'FAIL'; Reason = '没有 JUCE 路径：传 -JucePath，或设 RINNE_JUCE_PATH / git config rinne.jucePath' } }
    $have = "$(git -C $dir rev-parse HEAD 2>$null)".Trim()
    $short = { param($s) $s ? ($s -replace '^(.{7}).*$', '$1') : '（读不到）' }
    if ($have -ceq $want) { @{ Status = 'PASS'; Reason = "JUCE 的 HEAD 与 .juce-commit 一致（$(& $short $want)）" } }
    else { @{ Status = 'FAIL'; Reason = "JUCE 的 HEAD 是 $(& $short $have)，.juce-commit 要求 $(& $short $want)" } }
}

# 与 CI 同一份清单：入库的 C/C++/Objective-C++ 源，不含 third_party/。
function Get-CxxFiles([string]$Root) {
    @(git -C $Root -c core.quotepath=false ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm' | Where-Object { $_ -cnotmatch '^third_party/' })
}

function Test-CxxTree($Ctx, [string]$Root) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'src') -PathType Container)) { return @{ Status = 'SKIP'; Reason = '尚无 C++ 源（没有 src/）' } }
    # 函数返回的单元素数组会被 PowerShell 展开成字符串，再按下标取就成了取字符：必须重新包成数组
    $files = @(Get-CxxFiles $Root)
    if (-not $files.Count) { return @{ Status = 'FAIL'; Reason = 'src/ 存在，却没有列出任何 C++ 源文件' } }
    $cf = Get-ToolCommand $Ctx clangFormat
    $steps = for ($i = 0; $i -lt $files.Count; $i += 100) {
        Step "clang-format（第 $($i + 1) 个起）" $cf (@('--dry-run', '--Werror', '--style=file') + $files[$i..([Math]::Min($i + 99, $files.Count - 1))]) $Root
    }
    Invoke-Steps $Ctx @($steps)
}

function Invoke-ClangFormat($Ctx) {
    # 自测与 CI 相同：有 src/ 但清单为空的夹具必须判红，免得清单写坏后静默放行
    $fx = New-RinneTempDir -Prefix 'rinne-gates-cxx-'
    try {
        $null = New-Item -ItemType Directory -Path (Join-Path $fx 'src')
        Set-Content -LiteralPath (Join-Path $fx 'src/README.md') -Value 'x'
        git -C $fx init -q *>$null
        git -C $fx add -A *>$null
        if ((Test-CxxTree $Ctx $fx).Status -ne 'FAIL') { return @{ Status = 'FAIL'; Reason = '自测失败：src/ 下没有 C++ 源的夹具没有判红' } }
        # 恰好一个 C++ 文件：单元素数组被展开成字符串时，clang-format 会被调用在一个字符的路径上而失败
        Set-Content -LiteralPath (Join-Path $fx 'src/one.h') -Value '#pragma once'
        git -C $fx add -A *>$null
        $one = Test-CxxTree $Ctx $fx
        if ($one.Status -ne 'PASS') { return @{ Status = 'FAIL'; Reason = "自测失败：恰好一个 C++ 文件的夹具没有通过（$($one.Status)：$($one.Reason)）" } }
    } finally { $left = Remove-RinneTempDir -Path $fx -WaitSeconds 5; if ($left) { Write-Warning "临时目录没清掉：$left" } }
    Test-CxxTree $Ctx $Ctx.Root
}

function Invoke-Prettier($Ctx) {
    Invoke-Steps $Ctx @(Step 'prettier --check .' (Get-ToolCommand $Ctx prettier) @('--check', '.'))
}

# 把 HEAD 里每个跟踪文件导出到一个新建的临时目录，返回导出的目录。用临时索引加 checkout-index，不用 git archive：
# 仓内任何一层 .gitattributes 写个 export-ignore 就能让 archive 漏掉文件。
function Export-Head([string]$Root, [string]$Dir) {
    $tree = Join-Path $Dir 'tree'
    $null = New-Item -ItemType Directory -Path $tree
    $saved = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = Join-Path $Dir 'index'
        git -C $Root read-tree HEAD
        if ($LASTEXITCODE -ne 0) { throw 'git read-tree HEAD 失败' }
        git -C $Root checkout-index --all --force "--prefix=$($tree -replace '\\', '/')/"
        if ($LASTEXITCODE -ne 0) { throw 'git checkout-index 失败' }
    } finally { $env:GIT_INDEX_FILE = $saved }
    $tree
}

# 与 compliance.yml 里 run-checks 的 --only 一一对应，顺序也相同；Test-RinneGates 拿 workflow 原文核对。
$script:ComplianceOnly = 'spdx', 'reuse', 'licenses-closure', 'modules-closure', 'offline', 'recursive-delete', 'privacy'

function Invoke-Compliance($Ctx) {
    $node, $gl, $range = (Get-ToolCommand $Ctx node), (Get-ToolCommand $Ctx gitleaks), "$($Ctx.Base)..HEAD"
    $rc = 'scripts/run-checks.mjs'
    $tmp = New-RinneTempDir -Prefix 'rinne-gates-tree-'
    try {
        $tree = Export-Head $Ctx.Root $tmp
        Invoke-Steps $Ctx @(
            (Step '自测（compliance 组）' $node @($rc, '--group', 'compliance', '--self-test'))
            foreach ($only in $script:ComplianceOnly) { Step $only $node @($rc, '--group', 'compliance', '--only', $only) }
            (Step 'gitleaks 工作树' $gl @('dir', '.', '--no-banner', '--redact') $tree)
            (Step 'gitleaks 提交' $gl @('git', '.', '--no-banner', '--redact', "--log-opts=$range --no-ext-diff --no-textconv"))
            (Step '隐私 提交' $node @('scripts/check-privacy.mjs', '--commits', '--no-messages', $range))
        )
    } finally { $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 5; if ($left) { Write-Warning "临时目录没清掉：$left" } }
}

# workflow 里 run-checks 的 compliance 组用到的 --only，按出现顺序。
function Get-WorkflowOnly([string]$Yaml) {
    @([regex]::Matches($Yaml, 'run-checks\.mjs --group compliance --only ([\w-]+)') | ForEach-Object { $_.Groups[1].Value })
}

# 自测（运行器传入仓根）：G3 的 --only 清单与 compliance.yml 一致；workflow 多一项或少一项都要被认出来。
function Test-RinneGates([string]$Root) {
    $yml = Get-Content -LiteralPath (Join-Path $Root '.github/workflows/compliance.yml') -Raw
    $same = { param($a, $b) (@($a) -join ',') -ceq (@($b) -join ',') }
    $real = Get-WorkflowOnly $yml
    $extra = Get-WorkflowOnly ($yml + "`n          node scripts/run-checks.mjs --group compliance --only newcheck")
    [pscustomobject]@{ Label = 'G3 的 --only 清单与 compliance.yml 逐项一致'; Bad = $false; Passed = $real.Count -gt 0 -and (& $same $real $script:ComplianceOnly) }
    [pscustomobject]@{ Label = 'compliance.yml 新增一项检查时 G3 对不上'; Bad = $true; Passed = -not (& $same $extra $script:ComplianceOnly) }
    [pscustomobject]@{ Label = 'G3 少跑一项时对不上'; Bad = $true; Passed = -not (& $same $real @($script:ComplianceOnly | Select-Object -SkipLast 1)) }
    [pscustomobject]@{ Label = 'G4 子支线上要查分支名'; Bad = $false; Passed = $null -eq (Get-BranchCheckSkip 'feat/gate-followups' 'origin/feature/v1') }
    [pscustomobject]@{ Label = 'G4 HEAD 就是 base 分支时跳过分支名检查并写明原因'; Bad = $true; Passed = (Get-BranchCheckSkip 'feature/v1' 'origin/feature/v1') -match 'base 分支 feature/v1' }
    [pscustomobject]@{ Label = 'G4 游离 HEAD 仍跳过分支名检查'; Bad = $true; Passed = (Get-BranchCheckSkip '' 'origin/feature/v1') -match '游离' }
    [pscustomobject]@{ Label = 'G4 名字只是以 base 分支名开头的分支仍要查'; Bad = $false; Passed = $null -eq (Get-BranchCheckSkip 'feature/v1-x' 'origin/feature/v1') }
}

# 分支名检查的去留：HEAD 游离、或 HEAD 就是 base 分支本身（在主 clone 的 feature/v1 上跑 gates）时不查——
# 长期分支的名字不必是 feat/<slug>。返回 $null 表示要查，否则返回跳过的原因。
function Get-BranchCheckSkip([string]$Branch, [string]$Base) {
    $baseName = $Base -replace '^[^/]+/', ''
    if (-not $Branch) { return 'HEAD 是游离的，没有查分支名' }
    if ($Branch -ceq $baseName) { return "HEAD 就是 base 分支 $baseName，没有查分支名" }
    $null
}

function Invoke-Commits($Ctx) {
    $node, $cc, $range = (Get-ToolCommand $Ctx node), 'scripts/check-commits.mjs', "$($Ctx.Base)..HEAD"
    $branch = "$(git -C $Ctx.Root symbolic-ref --quiet --short HEAD 2>$null)".Trim()
    $skip = Get-BranchCheckSkip $branch $Ctx.Base
    $steps = @(Step '自测' $node @($cc, '--self-test'))
    if (-not $skip) { $steps += Step '分支名' $node @($cc, 'branch', '--head', $branch, '--base', ($Ctx.Base -replace '^[^/]+/', '')) }
    $steps += Step '提交' $node @($cc, 'commits', '--range', $range, '--tier', 'same')
    $steps += Step '冻结契约' $node @($cc, 'contract-guard', '--base', $Ctx.Base, '--head', 'HEAD')
    $r = Invoke-Steps $Ctx $steps
    if ($skip) { $r.Reason += "；$skip" }
    $r
}

function Invoke-DocsTruth($Ctx) {
    $node = Get-ToolCommand $Ctx node
    Invoke-Steps $Ctx @(
        (Step '自测（docs 组）' $node @('scripts/run-checks.mjs', '--group', 'docs', '--self-test'))
        (Step 'docs-truth' $node @('scripts/run-checks.mjs', '--group', 'docs'))
    )
}

function Register-RinneGates {
    @{ Id = 'G1'; Tier = 'Quick'; Name = '预检'; Run = ${function:Invoke-Preflight} }
    @{ Id = 'G1-juce'; Tier = 'Quick'; Name = 'JUCE 提交'; Run = ${function:Invoke-JuceCheck} }
    @{ Id = 'G2-cxx'; Tier = 'Quick'; Name = 'clang-format'; Run = ${function:Invoke-ClangFormat} }
    @{ Id = 'G2-prettier'; Tier = 'Quick'; Name = 'prettier'; Run = ${function:Invoke-Prettier} }
    @{ Id = 'G3'; Tier = 'Quick'; Name = 'compliance'; Run = ${function:Invoke-Compliance} }
    @{ Id = 'G4'; Tier = 'Quick'; Name = 'commits'; Run = ${function:Invoke-Commits} }
    @{ Id = 'G5'; Tier = 'Quick'; Name = 'docs-truth'; Run = ${function:Invoke-DocsTruth} }
}
