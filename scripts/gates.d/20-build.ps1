# SPDX-License-Identifier: GPL-3.0-or-later
#
# Standard 档的 G6 构建与 G7 ctest，由 scripts/gates.ps1 加载。两道都以子进程调 scripts/build.ps1，与合并后构建、CI 同一份配方。
#   G6  Lock = build：运行器先等内存、再取一把构建槽，并发折算好放进 $ctx.Jobs；这里把 RINNE_BUILD_SLOT_HELD 设成本进程
#       （持槽的运行器）的 pid 再调 build.ps1，它就不再自己取槽。本机构建用带 D 的段名前缀，与维护者 DAW 里装的正式版互不相见。
#   G7  Lock = ipc：持 ipc-tests 锁跑 ctest。两份 gates 都是 D 前缀，同时跑跨进程测试会撞上同一组真实段名。
#       还没有 tests/CMakeLists.txt 时 build.ps1 打印跳过，这里记 SKIP；G6 没有产出当前 HEAD 的构建时也记 SKIP。
# 构建目录取 $ctx.BuildDir（相对仓根），JUCE 取 $ctx.JucePath，空着时 build.ps1 自己按环境变量与 git config 找。
# build.ps1 把工具的输出写进 <构建目录>/logs/，自己的标准句柄不可继承，这里读它的管道不会被编译器留下的进程拖住。

function Invoke-BuildScript($Ctx, [string[]]$Arguments, [switch]$SlotHeld) {
    $pwsh = (Get-Process -Id $PID).Path
    $a = @('-BuildDir', $Ctx.BuildDir) + $Arguments
    if ($Ctx.JucePath) { $a += '-JucePath', $Ctx.JucePath }
    $saved = $env:RINNE_BUILD_SLOT_HELD
    if ($SlotHeld) { $env:RINNE_BUILD_SLOT_HELD = "$PID" }
    try {
        $lines = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $Ctx.Root 'scripts/build.ps1') @a 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } finally {
        $env:RINNE_BUILD_SLOT_HELD = $saved
    }
    [pscustomobject]@{ Code = $code; Lines = $lines; Last = @($lines | Where-Object { $_ -match '^(失败|完成)' } | Select-Object -Last 1)[0] }
}

function ConvertTo-GateResult($R, [string]$Pass) {
    switch ($R.Code) {
        0 { @{ Status = 'PASS'; Reason = $Pass; Lines = $R.Lines } }
        2 { @{ Status = 'SKIP'; Reason = "没有当前 HEAD 的构建（构建那道没过）：$($R.Last)"; Lines = $R.Lines } }
        3 { @{ Status = 'SKIP'; Reason = "资源超时：$($R.Last)"; Lines = $R.Lines; Resource = $true } }
        default { @{ Status = 'FAIL'; Reason = ($R.Last ? $R.Last : "build.ps1 退出码 $($R.Code)"); Lines = $R.Lines } }
    }
}

function Invoke-BuildGate($Ctx) {
    $r = Invoke-BuildScript $Ctx @('-Jobs', "$($Ctx.Jobs)") -SlotHeld
    ConvertTo-GateResult $r "构建通过、第一方零 warning（-j$($Ctx.Jobs)）"
}

function Invoke-CtestGate($Ctx) {
    $r = Invoke-BuildScript $Ctx @('-NoBuild', '-Test')
    $res = ConvertTo-GateResult $r 'ctest 通过'
    if ($r.Code -eq 0 -and @($r.Lines | Where-Object { $_ -match '^== ctest：' }).Count) { $res = @{ Status = 'SKIP'; Reason = '还没有 tests/CMakeLists.txt'; Lines = $r.Lines } }
    $res
}

# 自测（运行器传入仓根）：build.ps1 的自测，以及退出码到 gate 结论的映射。
function Test-RinneGates([string]$Root) {
    $pwsh = (Get-Process -Id $PID).Path
    $null = & $pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/build.ps1') -SelfTest 2>&1
    [pscustomobject]@{ Label = 'build.ps1 -SelfTest 通过'; Bad = $false; Passed = $LASTEXITCODE -eq 0 }
    $map = { param($c) (ConvertTo-GateResult ([pscustomobject]@{ Code = $c; Lines = @(); Last = '失败：x' }) 'ok').Status }
    [pscustomobject]@{ Label = 'build.ps1 退出 0 记 PASS'; Bad = $false; Passed = (& $map 0) -eq 'PASS' }
    [pscustomobject]@{ Label = 'build.ps1 退出 1 记 FAIL'; Bad = $true; Passed = (& $map 1) -eq 'FAIL' }
    [pscustomobject]@{ Label = 'build.ps1 退出 2（没有当前 HEAD 的构建）记 SKIP，不算通过'; Bad = $true; Passed = (& $map 2) -eq 'SKIP' }
    [pscustomobject]@{ Label = '其他退出码一律 FAIL'; Bad = $true; Passed = (& $map -1) -eq 'FAIL' }
}

function Register-RinneGates {
    @{ Id = 'G6'; Tier = 'Standard'; Name = '构建'; Lock = 'build'; Run = ${function:Invoke-BuildGate} }
    @{ Id = 'G7'; Tier = 'Standard'; Name = 'ctest'; Lock = 'ipc'; Run = ${function:Invoke-CtestGate} }
}
