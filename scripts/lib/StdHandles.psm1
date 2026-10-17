# SPDX-License-Identifier: GPL-3.0-or-later
#
# 起子进程时不把本进程调用方的管道交给它。Windows 上起子进程（.NET 的 Process.Start、PowerShell 调本地命令）会把本进程
# 所有可继承的句柄都交给子进程，其中就有本进程连着调用方的标准输出与标准错误；子进程以及它留下的常驻进程（编译器的 PDB
# 服务、编译缓存的服务）不退出，调用方就一直读不到 EOF，调用方的超时也就失效。给子进程重定向的是另建的管道，不受影响。
#   Import-Module <仓根>/scripts/lib/StdHandles.psm1
#   Invoke-WithoutStdInherit { <起子进程的代码> }   期间本进程的标准输出与标准错误不可继承，结束后按原样还原；标准输入不动
#   Test-StdHandles                                 自测用例（gates 的自测调用）
# 调用方：maintainer/lib/RinneGh.psm1 的 Invoke-RinneTimed（合并后构建经它起构建脚本）、gates.d/20-build.ps1（G6、G7）、
# gates.d/30-plugin.ps1（G8）。build.ps1 自己的标准句柄整个进程都不可继承，用的是它自带的那一份。

Set-StrictMode -Version Latest

function Initialize-StdHandleType {
    if ('Rinne.StdInherit' -as [type]) { return }
    Add-Type -Namespace Rinne -Name StdInherit -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int nStdHandle);
[DllImport("kernel32.dll")] public static extern bool GetHandleInformation(System.IntPtr hObject, out int lpdwFlags);
[DllImport("kernel32.dll")] public static extern bool SetHandleInformation(System.IntPtr hObject, int dwMask, int dwFlags);
'@
}

<# 标准输出与标准错误此刻是否可继承：@(输出, 错误)，取不到的记 $null。 #>
function Get-StdHandleInherit {
    Initialize-StdHandleType
    foreach ($n in -11, -12) {
        $f = 0
        if ([Rinne.StdInherit]::GetHandleInformation([Rinne.StdInherit]::GetStdHandle($n), [ref]$f)) { [bool]($f -band 1) } else { $null }
    }
}

function Invoke-WithoutStdInherit {
    param([Parameter(Mandatory)][scriptblock]$Script)
    if (-not $IsWindows) { return & $Script }
    Initialize-StdHandleType
    $saved = foreach ($n in -11, -12) {
        $h = [Rinne.StdInherit]::GetStdHandle($n); $f = 0
        if ([Rinne.StdInherit]::GetHandleInformation($h, [ref]$f)) { $null = [Rinne.StdInherit]::SetHandleInformation($h, 1, 0); @{ H = $h; F = $f -band 1 } }
    }
    try { & $Script } finally { foreach ($s in @($saved)) { $null = [Rinne.StdInherit]::SetHandleInformation($s.H, 1, $s.F) } }
}

<#
  自测：子进程的标准输出与标准错误接管道（可继承），在里面量块内与块后的状态；块里抛错也照样还原；
  本来就不可继承的，结束后仍是不可继承（按原样还原，不是一律放开）。另外核对三个调用方都用这个模块、没有自己的一份。
  返回 { Label; Bad; Passed }。
#>
function Test-StdHandles {
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
    $check = { param($Label, $Bad, $Passed) [pscustomobject]@{ Label = $Label; Bad = $Bad; Passed = [bool]$Passed } }
    foreach ($f in 'scripts/maintainer/lib/RinneGh.psm1', 'scripts/gates.d/20-build.ps1', 'scripts/gates.d/30-plugin.ps1') {
        $t = Get-Content -Raw -LiteralPath (Join-Path $root $f)
        & $check "$f 用本模块的 Invoke-WithoutStdInherit，没有自己的一份" $false ($t -match 'Invoke-WithoutStdInherit' -and $t -notmatch 'SetHandleInformation')
    }
    if (-not $IsWindows) { return }
    $probe = "Import-Module '$PSCommandPath'; `$m = Get-Module StdHandles; `$g = { (& `$m { Get-StdHandleInherit }) -join ',' }; " +
        "`$a = & `$g; `$in = Invoke-WithoutStdInherit { & `$g }; `$b = & `$g; " +
        "try { Invoke-WithoutStdInherit { throw 'x' } } catch { `$null = `$_ }; `$c = & `$g; " +
        "`$off = Invoke-WithoutStdInherit { Invoke-WithoutStdInherit { 'x' } | Out-Null; & `$g }; " +
        "[Console]::Error.WriteLine(""before=`$a inside=`$in after=`$b after-throw=`$c nested-inside=`$off"")"
    $psi = [Diagnostics.ProcessStartInfo]::new([Environment]::ProcessPath)
    foreach ($x in '-NoProfile', '-NonInteractive', '-Command', $probe) { $psi.ArgumentList.Add($x) }
    $psi.UseShellExecute, $psi.RedirectStandardOutput, $psi.RedirectStandardError = $false, $true, $true
    $p = [Diagnostics.Process]::Start($psi)
    try {
        $out, $err = $p.StandardOutput.ReadToEndAsync(), $p.StandardError.ReadToEndAsync()
        $done = $out.Wait(30000) -and $err.Wait(30000)
        if (-not $done) { try { $p.Kill($true) } catch [InvalidOperationException] { $null = $_ } }
        $line = $done ? $err.Result.Trim() : ''
    } finally { $p.Dispose() }
    & $check "块里不可继承、块后还原：$line" $false ($line -ceq 'before=True,True inside=False,False after=True,True after-throw=True,True nested-inside=False,False')
}

Export-ModuleMember -Function Invoke-WithoutStdInherit, Test-StdHandles
