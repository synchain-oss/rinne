# SPDX-License-Identifier: GPL-3.0-or-later
#
# Standard 档的 G6 构建与 G7 ctest，由 scripts/gates.ps1 加载。两道都以子进程调 scripts/build.ps1，与合并后构建、CI 同一份配方。
#   G6  Lock = build：运行器先等内存、再取一把构建槽，并发折算好放进 $ctx.Jobs；这里把 RINNE_BUILD_SLOT_HELD 设成本进程
#       （持槽的运行器）的 pid 再调 build.ps1，它就不再自己取槽。本机构建用带 D 的段名前缀，与维护者 DAW 里装的正式版互不相见。
#   G7  Lock = ipc：持 ipc-tests 锁跑 ctest。两份 gates 都是 D 前缀，同时跑跨进程测试会撞上同一组真实段名。
#       还没有 tests/CMakeLists.txt 时 build.ps1 打印跳过，这里记 SKIP；G6 没有产出当前 HEAD 的构建时也记 SKIP。
# 构建目录取 $ctx.BuildDir（相对仓根），JUCE 取 $ctx.JucePath，空着时 build.ps1 自己按环境变量与 git config 找。
# build.ps1 把工具的输出写进 <构建目录>/logs/，自己的标准句柄不可继承，这里读它的管道不会被编译器留下的进程拖住；起它的
# 时候运行器自己的标准输出与标准错误也设为不可继承（Invoke-WithoutStdInherit，来自运行器导入的 scripts/lib/StdHandles.psm1），
# 运行器的调用方同样不会被拖住。

function Invoke-BuildScript($Ctx, [string[]]$Arguments, [switch]$SlotHeld) {
    $pwsh = (Get-Process -Id $PID).Path
    $a = @('-BuildDir', $Ctx.BuildDir) + $Arguments
    if ($Ctx.JucePath) { $a += '-JucePath', $Ctx.JucePath }
    $saved = $env:RINNE_BUILD_SLOT_HELD
    if ($SlotHeld) { $env:RINNE_BUILD_SLOT_HELD = "$PID" }
    try {
        $r = Invoke-WithoutStdInherit {
            $l = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $Ctx.Root 'scripts/build.ps1') @a 2>&1 | ForEach-Object { "$_" })
            @{ Lines = $l; Code = $LASTEXITCODE }
        }
    } finally {
        $env:RINNE_BUILD_SLOT_HELD = $saved
    }
    [pscustomobject]@{ Code = $r.Code; Lines = $r.Lines; Last = @($r.Lines | Where-Object { $_ -match '^(失败|完成)' } | Select-Object -Last 1)[0] }
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

# 真起进程量一次：一个「中间层」pwsh 的输出接管道（像调用方读 gates 那样），它加载 gates.d 里的一个文件，执行 $Call 调起
# 临时目录里的桩 build.ps1；桩只取真 build.ps1 的函数，留下一个最多活 12 秒的孙进程就退出。返回调用方读到中间层输出 EOF 的秒数，
# 以及临时目录删掉了没有：量完写停止文件，孙进程退出、放开日志，临时目录才删得掉。临时目录经运行器导入的
# scripts/lib/TempDir.psm1 建与删（新建的、本进程的，删之前按真实路径核对）。
# 30-plugin.ps1 的 G8 也在这里量（它没有自己的自测）。中间层像运行器那样先导入 scripts/lib/StdHandles.psm1。
function Measure-CallerEof([string]$Root, [string]$File, [string]$Call) {
    $pwsh = (Get-Process -Id $PID).Path
    $tmp = New-RinneTempDir -Prefix 'rinne-gates-eof-'
    $stop = Join-Path $tmp 'stop'
    $null = New-Item -ItemType Directory -Path (Join-Path $tmp 'scripts')
    $p, $res, $left = $null, $null, $null
    try {
        Set-Content -LiteralPath (Join-Path $tmp 'scripts/build.ps1') -Value (". '$(Join-Path $Root 'scripts/build.ps1')'; Protect-StdHandles; " +
            "exit (Invoke-Logged 'cmd.exe' @('/c', 'start', '/b', '', '$pwsh', '-NoProfile', '-EncodedCommand', '$(Get-RinneLingerCommand -Seconds 12 -StopFile $stop)') '$tmp\l.log' 60)")
        $middle = "Import-Module '$(Join-Path $Root 'scripts/lib/StdHandles.psm1')'; `$m = New-Module -ScriptBlock ([scriptblock]::Create((Get-Content -Raw -LiteralPath '$(Join-Path $Root "scripts/gates.d/$File")'))); " +
            "`$c = [pscustomobject]@{ Root = '$tmp'; BuildDir = 'b'; JucePath = '' }; `$null = & `$m { param(`$c) $Call } `$c; 'middle-done'"
        $psi = [Diagnostics.ProcessStartInfo]::new($pwsh)
        foreach ($x in '-NoProfile', '-NonInteractive', '-Command', $middle) { $psi.ArgumentList.Add($x) }
        $psi.UseShellExecute, $psi.RedirectStandardOutput, $psi.RedirectStandardError = $false, $true, $true
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = [Diagnostics.Process]::Start($psi)
        $out, $err = $p.StandardOutput.ReadToEndAsync(), $p.StandardError.ReadToEndAsync()
        # 30 秒还读不到 EOF 就结束中间层（连同它的子孙）并判没跑完；没读完的 Result 不去碰，免得一直阻塞
        $eof = $out.Wait(30000) -and $err.Wait(30000)
        $s = $sw.Elapsed.TotalSeconds
        if (-not $eof) { try { $p.Kill($true) } catch [InvalidOperationException] { $null = $_ } }
        $res = [pscustomobject]@{ Seconds = $s; Done = $eof -and $out.Result -match 'middle-done'; Removed = $false }
    } finally {
        if ($p) { $p.Dispose() }
        # 孙进程收到停止信号就退出；等的时间比它的时限短，没写停止文件就删不掉
        Set-Content -LiteralPath $stop -Value 'x'
        $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 5
    }
    $res.Removed = -not $left -and -not (Test-Path -LiteralPath $tmp)
    $res
}

# 自测（运行器传入仓根）：build.ps1 的自测，退出码到 gate 结论的映射，以及运行器的管道不被构建留下的进程拖住。
function Test-RinneGates([string]$Root) {
    $pwsh = (Get-Process -Id $PID).Path
    $null = & $pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/build.ps1') -SelfTest 2>&1
    [pscustomobject]@{ Label = 'build.ps1 -SelfTest 通过'; Bad = $false; Passed = $LASTEXITCODE -eq 0 }
    # 桩靠 cmd.exe 留下孙进程，句柄继承也只在 Windows 上改：别的平台不量
    if ($IsWindows) {
        $fast = Measure-CallerEof $Root '20-build.ps1' 'Invoke-BuildScript $c @()'
        $slow = Measure-CallerEof $Root '20-build.ps1' "@(& '$pwsh' -NoProfile -NonInteractive -File (Join-Path `$c.Root 'scripts/build.ps1') 2>&1)"
        [pscustomobject]@{ Label = ('G6/G7 调 build.ps1：构建留下的进程不拖住运行器的调用方（{0:N1} 秒读到 EOF）' -f $fast.Seconds); Bad = $false; Passed = $fast.Done -and $fast.Seconds -lt 7 }
        [pscustomobject]@{ Label = ('反例：不改句柄继承时调用方要等孙进程退出（{0:N1} 秒）' -f $slow.Seconds); Bad = $true; Passed = $slow.Done -and $slow.Seconds -ge 10 }
        $plug = Measure-CallerEof $Root '30-plugin.ps1' "Invoke-PluginvalGate `$c '-Validate' 'x'"
        [pscustomobject]@{ Label = ('G8 调 build.ps1：pluginval 留下的进程不拖住运行器的调用方（{0:N1} 秒读到 EOF）' -f $plug.Seconds); Bad = $false; Passed = $plug.Done -and $plug.Seconds -lt 7 }
        [pscustomobject]@{ Label = '三次测量量完都写停止文件，孙进程退出，临时目录都删掉了'; Bad = $false; Passed = $fast.Removed -and $slow.Removed -and $plug.Removed }
    }
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
