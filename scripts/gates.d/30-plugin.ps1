# SPDX-License-Identifier: GPL-3.0-or-later
#
# pluginval，由 scripts/gates.ps1 加载：G8（Standard，无 GUI）与 G8-gui（Full，带 GUI，持 gui 锁）。两道都以子进程调
# scripts/build.ps1 -NoBuild，验的是 G6 刚构建出的两个插件；版本先对 .pluginval-version，strictness 5，日志在 <构建目录>/logs/。
# pluginval 是 GUI 子系统的程序，PowerShell 的 & 直接调它既不等它退出也收不到输出：build.ps1 按文件重定向起进程、等它退出。
# 验证宿主下插件不开共享内存段，所以无 GUI 的这道不持锁，与维护者开着的 DAW、并行的 gates 互不干扰。
# G6 没有产出当前 HEAD 的构建时记 SKIP。

function Invoke-PluginvalGate($Ctx, [string]$Mode, [string]$Pass) {
    $pwsh = (Get-Process -Id $PID).Path
    $lines = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $Ctx.Root 'scripts/build.ps1') -BuildDir $Ctx.BuildDir -NoBuild $Mode 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $last = @($lines | Where-Object { $_ -match '^(失败|完成)' } | Select-Object -Last 1)[0]
    switch ($code) {
        0 { @{ Status = 'PASS'; Reason = $Pass; Lines = $lines } }
        2 { @{ Status = 'SKIP'; Reason = "没有当前 HEAD 的构建（构建那道没过）：$last"; Lines = $lines } }
        default { @{ Status = 'FAIL'; Reason = ($last ? $last : "build.ps1 退出码 $code"); Lines = $lines } }
    }
}

function Invoke-HeadlessGate($Ctx) { Invoke-PluginvalGate $Ctx '-Validate' '两个插件过 pluginval（strictness 5，无 GUI）' }

function Invoke-GuiGate($Ctx) { Invoke-PluginvalGate $Ctx '-ValidateGui' '两个插件过 pluginval（strictness 5，带 GUI）' }

function Register-RinneGates {
    @{ Id = 'G8'; Tier = 'Standard'; Name = 'pluginval'; Run = ${function:Invoke-HeadlessGate} }
    @{ Id = 'G8-gui'; Tier = 'Full'; Name = 'pluginval（GUI）'; Lock = 'gui'; Run = ${function:Invoke-GuiGate} }
}
