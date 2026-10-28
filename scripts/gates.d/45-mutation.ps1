# SPDX-License-Identifier: GPL-3.0-or-later
#
# 全仓变异检查，由 scripts/gates.ps1 加载：mutation（Full 档，不持锁）。
# 每个导出了 mutationCheck 的检查，把自己的判据逐条改坏，自测必须因此变红：这一步证明「删掉任何一条判据都有用例变红」不是
# 一句空话。两条命令与 compliance.yml 的 Mutation check 一步逐字相同，Test-RinneGates 对着 workflow 原文核对（同 G3 的做法）：
#   1. run-checks --group compliance --only mutation-coverage：覆盖清单核对。run-checks 的 --mutation-check 只跑导出了
#      mutationCheck 的检查，某项的 mutationCheck 被删掉它就悄悄不跑、整遍照样全绿；清单（checks.d/mutation-coverage.mjs 的
#      REQUIRED）里的每一项都必须还在运行器的名单里。这一项很快，G3 也跑它，所以 Quick 档就认得出「mutationCheck 被删」。
#   2. run-checks --mutation-check：变异检查本体，全仓所有检查，约 1.5 分钟，几乎全是单线程的 node 与 python。
# 两条都跑完再下结论（前面失败也接着跑，一次报全）；变异检查还要读它的汇总行（有、PASS 至少一项、FAIL 与 SKIP 为零）：
# 退出码 0 却一项都没跑不能算过。
# 档位选 Full：整遍约 1.5 分钟（几乎全是单线程的 node 与 python），放进每个 PR 都跑的 Quick 太慢；它也不需要构建，放进 Standard
# 只会让每次编译前白等一遍。Full 与压测、页面冒烟、打包同一类，本文件自己也在 tier-rules.json 里把整个 diff 抬到 Full。
# 每个 PR 的 CI（compliance，required）无论本机跑哪一档都会跑同样的两条命令，所以这里是合并前的提前预警，不是唯一的一道。
# 起命令时给子进程设 CI=true（只给它，起完恢复调用方的值）：probe-analysis 的变异检查在找不到 Python 3 时，本机口径是「跳过」
# 并算通过，CI 口径是判红；gates 要与 CI 同口径，跳过不是通过。
# Node 用钉版：先找环境变量 RINNE_NODE，再找 git config rinne.node，最后退回 PATH；主版本由 run-checks 自己对着 .node-version 断言。

# 与 compliance.yml 的 Mutation check 一步逐字相同的两条命令（node 之后的参数）。
$script:CoverageArgs = @('scripts/run-checks.mjs', '--group', 'compliance', '--only', 'mutation-coverage')
$script:MutationArgs = @('scripts/run-checks.mjs', '--mutation-check')

function Get-MutationNode([string]$Root) {
    $v = [Environment]::GetEnvironmentVariable('RINNE_NODE')
    if (-not $v) { $v = "$(git -C $Root config --get rinne.node 2>$null)".Trim() }
    $v ? $v : 'node'
}

# 跑一条原生命令，返回 Code 与输出行（ANSI 颜色码去掉）。Vars 里的环境变量只给这个子进程，跑完恢复调用方原来的值。
function Invoke-MutationNative([string]$Exe, [string[]]$Arguments, [string]$Cwd, [hashtable]$Vars = @{ CI = 'true' }) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    $saved = @{}
    foreach ($k in $Vars.Keys) {
        $saved[$k] = if (Test-Path -LiteralPath "Env:$k") { (Get-Item -LiteralPath "Env:$k").Value } else { $null }
        Set-Item -LiteralPath "Env:$k" -Value $Vars[$k]
    }
    Push-Location -LiteralPath $Cwd
    try {
        $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" -replace '\x1b\[[0-9;]*m', '' })
        $code = $LASTEXITCODE
    } catch {
        $lines, $code = @("$Exe：$($_.Exception.Message)"), 127
    } finally {
        Pop-Location
        foreach ($k in $saved.Keys) { if ($null -eq $saved[$k]) { Remove-Item -LiteralPath "Env:$k" -ErrorAction Ignore } else { Set-Item -LiteralPath "Env:$k" -Value $saved[$k] } }
    }
    [pscustomobject]@{ Code = $code; Lines = $lines }
}

# run-checks 的最后一行 `run-checks mutation-check：PASS n / FAIL n / SKIP n` 折成三个数；读不到时返回 $null。
function Get-MutationSummary([string[]]$Lines) {
    $last = @($Lines | Where-Object { $_ -cmatch '^run-checks mutation-check：' } | Select-Object -Last 1)[0]
    $m = [regex]::Match("$last", '^run-checks mutation-check：PASS (\d+) / FAIL (\d+) / SKIP (\d+)$')
    if (-not $m.Success) { return $null }
    [pscustomobject]@{ Pass = [int]$m.Groups[1].Value; Fail = [int]$m.Groups[2].Value; Skip = [int]$m.Groups[3].Value }
}

# 汇总行读得出、至少跑了一项、没有失败、没有跳过才算过（变异检查模式里运行器不会出 SKIP，出了就是不对）。
function Test-MutationSummary($Summary) {
    [bool]($Summary -and $Summary.Pass -ge 1 -and $Summary.Fail -eq 0 -and $Summary.Skip -eq 0)
}

# 两条命令的结果（各有 Code 与 Lines）折成 gate 的结论：任何一条退出码非 0 即 FAIL；变异检查退出码 0 还要汇总行过关。
function Resolve-MutationResult($Cov, $Mut) {
    $sum = Get-MutationSummary $Mut.Lines
    $lines = @("--- 覆盖清单：退出码 $($Cov.Code)") + $Cov.Lines + @("--- 变异检查：退出码 $($Mut.Code)") + $Mut.Lines
    $failed = @()
    if ($Cov.Code -ne 0) { $failed += '覆盖清单' }
    if ($Mut.Code -ne 0) { $failed += '变异检查' }
    elseif (-not (Test-MutationSummary $sum)) { $failed += '变异检查（汇总行不对：没有、一项都没跑或有跳过）' }
    if ($failed.Count) { return @{ Status = 'FAIL'; Reason = "失败：$($failed -join '、')"; Lines = $lines } }
    @{ Status = 'PASS'; Reason = "覆盖清单核对过，变异检查 $($sum.Pass) 项全过"; Lines = $lines }
}

function Invoke-MutationGate($Ctx) {
    $node = Get-MutationNode $Ctx.Root
    $cov = Invoke-MutationNative $node $script:CoverageArgs $Ctx.Root
    $mut = Invoke-MutationNative $node $script:MutationArgs $Ctx.Root
    Resolve-MutationResult $cov $mut
}

# workflow 里名字以 $Name 开头的那一步（从它的 `- name:` 行到下一步之前）的原文；找不到返回 $null。
function Get-WorkflowStep([string]$Yaml, [string]$Name) {
    $m = [regex]::Match($Yaml, "(?ms)^ {6}- name: $([regex]::Escape($Name))[^\n]*\n(?<body>.*?)(?=^ {6}- |\z)")
    if ($m.Success) { $m.Groups['body'].Value } else { $null }
}

# 这一步里有没有 `node <参数>` 这一整行：行首只能是空白，不带别的参数，被注释掉的不算。
function Test-StepCommand([string]$Step, [string[]]$Arguments) {
    $line = 'node ' + ($Arguments -join ' ')
    [regex]::IsMatch("$Step", "(?m)^[ \t]*$([regex]::Escape($line))[ \t]*\r?$")
}

# compliance.yml 的 Mutation check 一步：两条命令都在，并且这一步不带 if 也不带 continue-on-error（带了它红了也不拦）。
function Test-WorkflowMutationStep([string]$Yaml) {
    $step = Get-WorkflowStep $Yaml 'Mutation check'
    [bool]((Test-StepCommand $step $script:CoverageArgs) -and (Test-StepCommand $step $script:MutationArgs) -and
        -not [regex]::IsMatch($step, '(?m)^\s+(?:if|continue-on-error)\s*:'))
}

# 自测（运行器传入仓根）：汇总行的判据、workflow 里那一步、注册与目录一致、CI=true 只给子进程，以及找不到 Python 3 时判红。
function Test-RinneGates([string]$Root) {
    $sum = { param($p, $f, $s) Get-MutationSummary @('PASS  x（docs）', "run-checks mutation-check：PASS $p / FAIL $f / SKIP $s") }
    [pscustomobject]@{ Label = '变异检查的汇总行：PASS 至少一项、FAIL 与 SKIP 为零算过'; Bad = $false; Passed = Test-MutationSummary (& $sum 8 0 0) }
    [pscustomobject]@{ Label = '变异检查的汇总行：有失败不算过'; Bad = $true; Passed = -not (Test-MutationSummary (& $sum 7 1 0)) }
    [pscustomobject]@{ Label = '变异检查的汇总行：一项都没跑（退出码 0 也不算过）'; Bad = $true; Passed = -not (Test-MutationSummary (& $sum 0 0 0)) }
    [pscustomobject]@{ Label = '变异检查的汇总行：有跳过不算过'; Bad = $true; Passed = -not (Test-MutationSummary (& $sum 8 0 1)) }
    [pscustomobject]@{ Label = '变异检查的汇总行：没有汇总行不算过'; Bad = $true; Passed = -not (Test-MutationSummary (Get-MutationSummary @('PASS  x（docs）'))) }
    [pscustomobject]@{ Label = '变异检查的汇总行：缺 SKIP 一栏的残行不算过'; Bad = $true; Passed = -not (Test-MutationSummary (Get-MutationSummary @('run-checks mutation-check：PASS 8 / FAIL 0'))) }

    $ran = { param($code, $p, $f, $s) [pscustomobject]@{ Code = $code; Lines = @("run-checks mutation-check：PASS $p / FAIL $f / SKIP $s") } }
    $covOk, $mutOk = [pscustomobject]@{ Code = 0; Lines = @() }, (& $ran 0 8 0 0)
    [pscustomobject]@{ Label = 'gate 的结论：覆盖清单与变异检查都过记 PASS'; Bad = $false; Passed = (Resolve-MutationResult $covOk $mutOk).Status -ceq 'PASS' }
    [pscustomobject]@{ Label = 'gate 的结论：覆盖清单没过记 FAIL（mutationCheck 被删时变异检查本体还是绿的）'; Bad = $true
        Passed = (Resolve-MutationResult ([pscustomobject]@{ Code = 1; Lines = @() }) $mutOk).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = 'gate 的结论：变异检查退出码非 0 记 FAIL（有存活的变异，汇总行里 FAIL 也不是零）'; Bad = $true
        Passed = (Resolve-MutationResult $covOk (& $ran 1 7 1 0)).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = 'gate 的结论：变异检查退出码非 0 记 FAIL，即使汇总行看着是好的'; Bad = $true
        Passed = (Resolve-MutationResult $covOk (& $ran 1 8 0 0)).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = 'gate 的结论：变异检查退出码 0 却一项都没跑记 FAIL'; Bad = $true
        Passed = (Resolve-MutationResult $covOk (& $ran 0 0 0 0)).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = 'gate 的结论：变异检查退出码 0 却没有汇总行记 FAIL'; Bad = $true
        Passed = (Resolve-MutationResult $covOk ([pscustomobject]@{ Code = 0; Lines = @('PASS  x（docs）') })).Status -ceq 'FAIL' }

    $yaml = Get-Content -LiteralPath (Join-Path $Root '.github/workflows/compliance.yml') -Raw
    $cmd = 'node ' + ($script:MutationArgs -join ' ')
    $cov = 'node ' + ($script:CoverageArgs -join ' ')
    $swap = { param($from, $to) $y = $yaml.Replace($from, $to); if ($y -ceq $yaml) { $null } else { $y } }
    $variants = [ordered]@{
        '变异检查那一行没了'                  = & $swap "          $cmd`n" ''
        '变异检查那一行被注释掉'              = & $swap "          $cmd`n" "          # $cmd`n"
        '变异检查那一行只跑一部分（带了别的参数）' = & $swap "          $cmd`n" "          $cmd --only privacy`n"
        '覆盖清单核对那一行没了'              = & $swap "          $cov`n" ''
        '这一步加了 if'                       = & $swap '      - name: Mutation check (all checks)' "      - name: Mutation check (all checks)`n        if: false"
        '这一步加了 continue-on-error'        = & $swap '      - name: Mutation check (all checks)' "      - name: Mutation check (all checks)`n        continue-on-error: true"
        '这一步整个没了'                      = & $swap '- name: Mutation check (all checks)' '- name: Something else'
    }
    [pscustomobject]@{ Label = 'compliance.yml 有 Mutation check 一步：覆盖清单核对与变异检查两行都在，没有 if / continue-on-error'; Bad = $false; Passed = Test-WorkflowMutationStep $yaml }
    foreach ($k in $variants.Keys) {
        # 夹具没改成（原文里找不到要换的那段）时 $null：用例直接判红，不能空洞地通过
        [pscustomobject]@{ Label = "反例：compliance.yml 的 Mutation check 一步，$k"; Bad = $true; Passed = $null -ne $variants[$k] -and -not (Test-WorkflowMutationStep $variants[$k]) }
    }

    $json = Get-Content -LiteralPath (Join-Path $Root 'scripts/lib/tier-rules.json') -Raw | ConvertFrom-Json
    $reg = @(Register-RinneGates)[0]
    $row = @($json.gates | Where-Object id -ceq $reg.Id)[0]
    [pscustomobject]@{ Label = 'tier-rules.json 的 gates 目录里有 mutation 这一行，档位与本模块注册的一致'; Bad = $false
        Passed = [bool]($row -and $row.tier -ceq $reg.Tier -and $reg.Tier -ceq 'Full') }
    [pscustomobject]@{ Label = 'tier-rules.json 里 mutation 这一行的 requires 指向本文件（PR 删掉本文件不能换来 SKIP）'; Bad = $false
        Passed = [bool]($row -and @($row.requires) -ccontains 'scripts/gates.d/45-mutation.ps1') }

    # 真起进程：CI=true 只给子进程，起完恢复；PATH 上找不到 Python 3 时 probe-analysis 的变异检查判红（本机口径是跳过算过）
    try {
        $node = Get-MutationNode $Root
        $exe = (Get-Command $node -ErrorAction Stop).Source
        $before = $env:CI
        $r = Invoke-MutationNative $exe @('-e', 'process.stdout.write(String(process.env.CI))') $Root
        [pscustomobject]@{ Label = 'gate 起检查时给子进程设 CI=true，起完恢复调用方原来的值'; Bad = $false
            Passed = ($r.Code -eq 0 -and ($r.Lines -join '') -ceq 'true' -and $env:CI -ceq $before) }
        $noPy = Invoke-MutationNative $exe (@($script:MutationArgs) + '--only', 'probe-analysis') $Root @{ CI = 'true'; PATH = (Split-Path -Parent $exe) }
        [pscustomobject]@{ Label = '反例：PATH 上没有 Python 3 时，probe-analysis 的变异检查在 gate 里判红，不当成跳过'; Bad = $true
            Passed = ($noPy.Code -ne 0 -and ($noPy.Lines -join "`n") -match '找不到 Python 3') }
    } catch {
        [pscustomobject]@{ Label = "mutation 的真起进程用例抛错：$($_.Exception.Message)"; Bad = $true; Passed = $false }
    }
}

function Register-RinneGates {
    @{ Id = 'mutation'; Tier = 'Full'; Name = '变异检查'; Run = ${function:Invoke-MutationGate} }
}
