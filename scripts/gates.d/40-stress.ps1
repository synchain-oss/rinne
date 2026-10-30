# SPDX-License-Identifier: GPL-3.0-or-later
#
# Full 档的压测 gate，由 scripts/gates.ps1 加载：stress（Lock = build）。
# 以子进程调 scripts/stress.ps1（本机与手动长跑共用同一份判据），参数固定带 -Mutations：
#   1. 跑 G6 建好的 rinne_stress，环境变量 RINNE_STRESS_SCALE 放大到 -Scale（缺省 15，约 2 分钟；ctest 里 G7 跑的是规模 1，约 9 秒）。
#   2. 另建 no_intent 与 no_read_fence 两个 core-only 变异构建，各跑一遍压测：no_intent 必须变红；no_read_fence 在强内存序的 x86
#      上抓不到，压测自己以「证据不成立」结束，这里认，并在结果行里写明待 arm64 的 dispatch 确认。变异构建要编译，
#      所以持构建槽（Lock = build，并发由运行器折算好放进 $ctx.Jobs），本机约再加 3 分钟。
#   3. tsan.supp 的理由检查（每一条抑制前面要有「# 理由：」）。
# 规模可由环境变量 RINNE_STRESS_GATE_SCALE 或 git config rinne.stressGateScale 改成 1..600 的整数；写错了退回缺省并在日志里说明。
# 构建目录取 $ctx.BuildDir（相对仓根）。G6 没有产出当前 HEAD 的构建时，stress.ps1 退出码 2，这里记 SKIP（同 G7 的口径），
# 不会拿旧构建的结果凑数。
# 起 stress.ps1 时运行器自己的标准输出与标准错误设为不可继承（Invoke-WithoutStdInherit，来自运行器导入的 scripts/lib/StdHandles.psm1），
# 编译器留下的常驻进程拖不住运行器的调用方。

$script:StressGateDefaultScale = 15

# 规模设置：缺省 15；1..600 的整数照用，别的退回缺省并带一句说明。返回 @{ Scale; Note }。
function Get-StressGateScale([string]$Text) {
    if (-not $Text) { return @{ Scale = $script:StressGateDefaultScale; Note = '' } }
    if ($Text -match '^\d{1,3}$' -and [int]$Text -ge 1 -and [int]$Text -le 600) { return @{ Scale = [int]$Text; Note = '' } }
    @{ Scale = $script:StressGateDefaultScale; Note = "压测规模设置 '$Text' 不是 1..600 的整数，退回缺省 $($script:StressGateDefaultScale)" }
}

# 传给 stress.ps1 的参数。-Mutations 不能少：少了它，gate 就只剩一遍放大的压测，变异构建悄悄不跑。
function Get-StressArguments($Ctx, [int]$Scale) {
    @('-BuildDir', $Ctx.BuildDir, '-Scale', "$Scale", '-Mutations', '-Jobs', "$($Ctx.Jobs)")
}

# stress.ps1 的退出码折成 gate 的结论。$Last 是它最后一行「完成：…」或「失败：…」。
function ConvertTo-StressGateResult([int]$Code, [string]$Last, [string[]]$Lines) {
    switch ($Code) {
        0 { @{ Status = 'PASS'; Reason = ($Last ? ($Last -replace '^完成：', '') : '压测与变异构建通过'); Lines = $Lines } }
        2 { @{ Status = 'SKIP'; Reason = "没有当前 HEAD 的压测构建（构建那道没过）：$Last"; Lines = $Lines } }
        default { @{ Status = 'FAIL'; Reason = ($Last ? ($Last -replace '^失败：', '') : "stress.ps1 退出码 $Code"); Lines = $Lines } }
    }
}

function Invoke-StressScript($Ctx, [string[]]$Arguments) {
    $pwsh = (Get-Process -Id $PID).Path
    $r = Invoke-WithoutStdInherit {
        $l = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $Ctx.Root 'scripts/stress.ps1') @Arguments 2>&1 | ForEach-Object { "$_" })
        @{ Lines = $l; Code = $LASTEXITCODE }
    }
    [pscustomobject]@{ Code = $r.Code; Lines = $r.Lines; Last = @($r.Lines | Where-Object { $_ -match '^(失败|完成)：' } | Select-Object -Last 1)[0] }
}

function Invoke-StressGate($Ctx) {
    $s = Get-StressGateScale (& $Ctx.Setting 'RINNE_STRESS_GATE_SCALE' 'rinne.stressGateScale')
    $r = Invoke-StressScript $Ctx (Get-StressArguments $Ctx $s.Scale)
    $lines = @($s.Note ? $s.Note : @()) + $r.Lines
    ConvertTo-StressGateResult $r.Code $r.Last $lines
}

# 自测（运行器传入仓根）：规模设置、参数里不能少 -Mutations、退出码折成结论、stress.ps1 自己的自测、注册与目录一致。
function Test-RinneGates([string]$Root) {
    $scale = { param($t) Get-StressGateScale $t }
    [pscustomobject]@{ Label = '压测规模：没设置用缺省 15'; Bad = $false; Passed = (& $scale '').Scale -eq 15 -and -not (& $scale '').Note }
    [pscustomobject]@{ Label = '压测规模：1..600 的整数照用'; Bad = $false; Passed = (& $scale '40').Scale -eq 40 -and (& $scale '600').Scale -eq 600 -and (& $scale '1').Scale -eq 1 }
    foreach ($bad in '0', '601', 'abc', '-3', '1.5', '10x', '99999999999999999999') {
        [pscustomobject]@{ Label = "压测规模：'$bad' 不合法，退回缺省并留下说明"; Bad = $true; Passed = (& $scale $bad).Scale -eq 15 -and (& $scale $bad).Note -match '退回缺省' }
    }
    $ctx = [pscustomobject]@{ BuildDir = 'build'; Jobs = 2 }
    $args1 = Get-StressArguments $ctx 15
    [pscustomobject]@{ Label = '传给 stress.ps1 的参数带 -Mutations（少了它变异构建悄悄不跑）'; Bad = $true; Passed = $args1 -contains '-Mutations' }
    [pscustomobject]@{ Label = '传给 stress.ps1 的参数带构建目录、规模与并发'; Bad = $false
        Passed = ($args1 -join ' ') -ceq '-BuildDir build -Scale 15 -Mutations -Jobs 2' }
    [pscustomobject]@{ Label = '构建目录、规模与并发随参数变化'; Bad = $false
        Passed = (Get-StressArguments ([pscustomobject]@{ BuildDir = 'b'; Jobs = 3 }) 7) -join ' ' -ceq '-BuildDir b -Scale 7 -Mutations -Jobs 3' }

    $lines = @('x')
    [pscustomobject]@{ Label = 'gate 的结论：退出码 0 记 PASS，理由取最后一行「完成：」之后的文字'; Bad = $false
        Passed = ((ConvertTo-StressGateResult 0 '完成：压测通过（共 5 秒）' $lines).Status -ceq 'PASS') -and ((ConvertTo-StressGateResult 0 '完成：压测通过（共 5 秒）' $lines).Reason -ceq '压测通过（共 5 秒）') }
    [pscustomobject]@{ Label = 'gate 的结论：退出码 1 记 FAIL'; Bad = $true; Passed = (ConvertTo-StressGateResult 1 '失败：压测规模 15：退出码 3' $lines).Status -ceq 'FAIL' }
    [pscustomobject]@{ Label = 'gate 的结论：退出码 2（没有当前 HEAD 的构建）记 SKIP，不拿旧构建凑数'; Bad = $true; Passed = (ConvertTo-StressGateResult 2 '' $lines).Status -ceq 'SKIP' }
    [pscustomobject]@{ Label = 'gate 的结论：别的退出码（脚本抛错、被杀）记 FAIL，不记 SKIP'; Bad = $true
        Passed = ((ConvertTo-StressGateResult 255 '' $lines).Status -ceq 'FAIL') -and ((ConvertTo-StressGateResult -1 '' $lines).Status -ceq 'FAIL') }

    $pwsh = (Get-Process -Id $PID).Path
    $null = & $pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/stress.ps1') -SelfTest 2>&1
    [pscustomobject]@{ Label = 'stress.ps1 -SelfTest 通过'; Bad = $false; Passed = $LASTEXITCODE -eq 0 }
    $null = & $pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/stress.ps1') -Lint 2>&1
    [pscustomobject]@{ Label = 'tests/tsan.supp 的每一条抑制都有理由（stress.ps1 -Lint）'; Bad = $false; Passed = $LASTEXITCODE -eq 0 }

    $json = Get-Content -LiteralPath (Join-Path $Root 'scripts/lib/tier-rules.json') -Raw | ConvertFrom-Json
    $reg = @(Register-RinneGates)[0]
    $row = @($json.gates | Where-Object id -ceq $reg.Id)[0]
    [pscustomobject]@{ Label = 'tier-rules.json 的 gates 目录里有 stress 这一行，档位与本模块注册的一致，并且是 Full'; Bad = $false
        Passed = [bool]($row -and $row.tier -ceq $reg.Tier -and $reg.Tier -ceq 'Full') }
    [pscustomobject]@{ Label = 'tier-rules.json 里 stress 这一行的 requires 指向本文件（PR 删掉本文件不能换来 SKIP）'; Bad = $false
        Passed = [bool]($row -and @($row.requires) -ccontains 'scripts/gates.d/40-stress.ps1') }
    [pscustomobject]@{ Label = 'stress 持构建槽（变异构建要编译；并发由运行器折算）'; Bad = $false; Passed = $reg.Lock -ceq 'build' }
}

function Register-RinneGates {
    @{ Id = 'stress'; Tier = 'Full'; Name = '压测'; Lock = 'build'; Run = ${function:Invoke-StressGate} }
}
