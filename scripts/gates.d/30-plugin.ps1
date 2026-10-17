# SPDX-License-Identifier: GPL-3.0-or-later
#
# pluginval，由 scripts/gates.ps1 加载：G8（Standard，无 GUI）与 G8-gui（Full，带 GUI，持 gui 锁）。两道都以子进程调
# scripts/build.ps1 -NoBuild，验的是 G6 刚构建出的两个插件；版本先对 .pluginval-version，strictness 5，日志在 <构建目录>/logs/。
# pluginval 是 GUI 子系统的程序，PowerShell 的 & 直接调它既不等它退出也收不到输出：build.ps1 按文件重定向起进程、等它退出。
# 验证宿主下插件不开共享内存段，所以无 GUI 的这道不持锁，与维护者开着的 DAW、并行的 gates 互不干扰。
# G6 没有产出当前 HEAD 的构建时记 SKIP。起 build.ps1 的时候运行器自己的标准输出与标准错误设为不可继承，pluginval 与它
# 留下的进程不会拖住运行器的调用方。

# 跑一段代码，期间本进程的标准输出与标准错误不可继承，结束后按原样还原。Windows 上起子进程会把所有可继承的句柄都交给它，
# 其中就有本进程连着调用方的那两条管道（给子进程重定向的是另建的管道，不受影响）。标准输入不动。
# maintainer/lib/RinneGh.psm1 与 20-build.ps1 里有同样的一份。
function Invoke-WithoutStdInherit([scriptblock]$Script) {
    if (-not $IsWindows) { return & $Script }
    if (-not ('Rinne.StdInherit' -as [type])) {
        Add-Type -Namespace Rinne -Name StdInherit -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int nStdHandle);
[DllImport("kernel32.dll")] public static extern bool GetHandleInformation(System.IntPtr hObject, out int lpdwFlags);
[DllImport("kernel32.dll")] public static extern bool SetHandleInformation(System.IntPtr hObject, int dwMask, int dwFlags);
'@
    }
    # 局部变量带 std 前缀：$Script 在这里的子作用域里跑，按动态作用域也看得见它们，不要遮住调用方的同名变量
    $stdSaved = foreach ($stdN in -11, -12) {
        $stdH = [Rinne.StdInherit]::GetStdHandle($stdN); $stdF = 0
        if ([Rinne.StdInherit]::GetHandleInformation($stdH, [ref]$stdF)) { $null = [Rinne.StdInherit]::SetHandleInformation($stdH, 1, 0); @{ H = $stdH; F = $stdF -band 1 } }
    }
    try { & $Script } finally { foreach ($stdS in @($stdSaved)) { $null = [Rinne.StdInherit]::SetHandleInformation($stdS.H, 1, $stdS.F) } }
}

function Invoke-PluginvalGate($Ctx, [string]$Mode, [string]$Pass) {
    $pwsh = (Get-Process -Id $PID).Path
    $r = Invoke-WithoutStdInherit {
        $l = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $Ctx.Root 'scripts/build.ps1') -BuildDir $Ctx.BuildDir -NoBuild $Mode 2>&1 | ForEach-Object { "$_" })
        @{ Lines = $l; Code = $LASTEXITCODE }
    }
    $lines, $code = $r.Lines, $r.Code
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
