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

# 孙进程的命令：沿用 Get-RinneLingerCommand（最多活 Seconds 秒，停止文件出现就退出），开头多写一个启动标记。
# 重新编码时同样补空格凑成 3 的倍数，编码后的串里没有补位的 =。
function Get-MarkedLingerCommand([int]$Seconds, [string]$StopFile, [string]$Started) {
    $plain = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String((Get-RinneLingerCommand -Seconds $Seconds -StopFile $StopFile))).TrimEnd()
    $cmd = "[IO.File]::WriteAllText('$($Started -replace "'", "''")', 'x'); $plain"
    while ($cmd.Length % 3) { $cmd += ' ' }
    [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
}

# 真起进程量一次：一个「中间层」pwsh 的输出接管道（像调用方读 gates 那样），它加载 gates.d 里的一个文件，执行 $Call 调起
# 临时目录里的桩 build.ps1；桩只取真 build.ps1 的函数，留下一个孙进程就退出。孙进程的寿命由本函数用停止文件掌握
# （300 秒的兜底时限只防本进程被杀之后它一直留着）。
# 判据不比较墙钟秒数，只看因果：孙进程已经起来（它自己写启动标记）、还活着的时候，调用方读到 EOF 没有。
#   Launched          孙进程写出了启动标记；没起来就什么也证明不了，不能算通过。
#   EofBeforeRelease  EOF 在本函数写停止文件之前就读到了：EOF 不依赖孙进程退出，即孙进程没有拖住调用方。
# 机器再忙，这两条的真假也不变，变的只是等多久。GraceSeconds 是「等不到就当它被拖住」的上限，不是通过的门槛：
# 孙进程还拿着管道时 EOF 来不了，泄漏必定表现为等满 GraceSeconds、写停止文件放走孙进程之后才读到 EOF，所以放宽这个上限
# 不会漏掉泄漏，只会让泄漏的那次多等几秒。反例（故意不改句柄继承）只需证明被拖住，用较短的 GraceSeconds 少等几秒。
# 返回 Seconds（读到 EOF 用的秒数，只用于显示）、Done（读到 EOF 且中间层跑完）、Launched、EofBeforeRelease，
# 以及临时目录删掉了没有：量完写停止文件，孙进程退出、放开日志，临时目录才删得掉。临时目录经运行器导入的
# scripts/lib/TempDir.psm1 建与删（新建的、本进程的，删之前按真实路径核对）。
# 30-plugin.ps1 的 G8 也在这里量（它没有自己的自测）。中间层像运行器那样先导入 scripts/lib/StdHandles.psm1。
function Measure-CallerEof([string]$Root, [string]$File, [string]$Call, [double]$GraceSeconds = 60) {
    $pwsh = (Get-Process -Id $PID).Path
    $tmp = New-RinneTempDir -Prefix 'rinne-gates-eof-'
    $stop, $started = (Join-Path $tmp 'stop'), (Join-Path $tmp 'started')
    $null = New-Item -ItemType Directory -Path (Join-Path $tmp 'scripts')
    $p, $res, $left = $null, $null, $null
    try {
        Set-Content -LiteralPath (Join-Path $tmp 'scripts/build.ps1') -Value (". '$(Join-Path $Root 'scripts/build.ps1')'; Protect-StdHandles; " +
            "exit (Invoke-Logged 'cmd.exe' @('/c', 'start', '/b', '', '$pwsh', '-NoProfile', '-EncodedCommand', '$(Get-MarkedLingerCommand -Seconds 300 -StopFile $stop -Started $started)') '$tmp\l.log' 60)")
        $middle = "Import-Module '$(Join-Path $Root 'scripts/lib/StdHandles.psm1')'; `$m = New-Module -ScriptBlock ([scriptblock]::Create((Get-Content -Raw -LiteralPath '$(Join-Path $Root "scripts/gates.d/$File")'))); " +
            "`$c = [pscustomobject]@{ Root = '$tmp'; BuildDir = 'b'; JucePath = '' }; `$null = & `$m { param(`$c) $Call } `$c; 'middle-done'"
        $psi = [Diagnostics.ProcessStartInfo]::new($pwsh)
        foreach ($x in '-NoProfile', '-NonInteractive', '-Command', $middle) { $psi.ArgumentList.Add($x) }
        $psi.UseShellExecute, $psi.RedirectStandardOutput, $psi.RedirectStandardError = $false, $true, $true
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = [Diagnostics.Process]::Start($psi)
        $out, $err = $p.StandardOutput.ReadToEndAsync(), $p.StandardError.ReadToEndAsync()
        $tasks = [Threading.Tasks.Task[]]@($out, $err)
        $waitEof = { param([double]$Seconds) try { [Threading.Tasks.Task]::WaitAll($tasks, [int]($Seconds * 1000)) } catch { $false } }
        # 先等孙进程起来（120 秒是兜底上限，机器再忙也远够；起不来就如实记 Launched 为假）；这段时间里不放它走
        $until = [datetime]::UtcNow.AddSeconds(120)
        while (-not [IO.File]::Exists($started) -and [datetime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 50 }
        $launched = [IO.File]::Exists($started)
        # 孙进程还活着：读得到 EOF 就说明没被拖住；等满 GraceSeconds 读不到，才写停止文件放走孙进程，再等 EOF
        $before = [bool](& $waitEof $GraceSeconds)
        $eof = $before
        if (-not $before) {
            Set-Content -LiteralPath $stop -Value 'x'
            $eof = [bool](& $waitEof 60)
        }
        $s = $sw.Elapsed.TotalSeconds
        # 放走之后 60 秒还读不到 EOF 就结束中间层（连同它的子孙）并判没跑完；没读完的 Result 不去碰，免得一直阻塞
        if (-not $eof) { try { $p.Kill($true) } catch [InvalidOperationException] { $null = $_ } }
        $res = [pscustomobject]@{ Seconds = $s; Done = $eof -and $out.Result -match 'middle-done'; Launched = $launched; EofBeforeRelease = $before; Removed = $false }
    } finally {
        if ($p) { $p.Dispose() }
        # 孙进程收到停止信号就退出；等的时间比它的兜底时限短，没写停止文件就删不掉。20 秒只是等它退出的上限
        Set-Content -LiteralPath $stop -Value 'x'
        $left = Remove-RinneTempDir -Path $tmp -WaitSeconds 20
    }
    $res.Removed = -not $left -and -not (Test-Path -LiteralPath $tmp)
    $res
}

# 自测（运行器传入仓根）：build.ps1 的自测，退出码到 gate 结论的映射，以及运行器的管道不被构建留下的进程拖住。
function Test-RinneGates([string]$Root) {
    $pwsh = (Get-Process -Id $PID).Path
    $null = & $pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/build.ps1') -SelfTest 2>&1
    [pscustomobject]@{ Label = 'build.ps1 -SelfTest 通过'; Bad = $false; Passed = $LASTEXITCODE -eq 0 }
    $marked = Get-MarkedLingerCommand -Seconds 9 -StopFile "s'p" -Started "a'b"
    $plainMarked = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($marked))
    [pscustomobject]@{ Label = '孙进程的命令：先写启动标记（路径里的单引号转义），再照常等停止文件，编码后没有补位的 ='; Bad = $false
        Passed = -not $marked.Contains('=') -and $plainMarked.StartsWith("[IO.File]::WriteAllText('a''b', 'x'); ") -and $plainMarked -match 'AddSeconds\(9\)' -and $plainMarked.Contains("Exists('s''p')") }
    # 桩靠 cmd.exe 留下孙进程，句柄继承也只在 Windows 上改：别的平台不量
    if ($IsWindows) {
        # 判据看因果，不比较秒数（见 Measure-CallerEof）：正例要在孙进程还活着时就读到 EOF；反例故意不改句柄继承，孙进程拿着
        # 管道，EOF 必须等放走它之后才来。两边都要求孙进程确实起来过，否则用例形同虚设。
        $fast = Measure-CallerEof $Root '20-build.ps1' 'Invoke-BuildScript $c @()'
        $slow = Measure-CallerEof $Root '20-build.ps1' "@(& '$pwsh' -NoProfile -NonInteractive -File (Join-Path `$c.Root 'scripts/build.ps1') 2>&1)" -GraceSeconds 8
        [pscustomobject]@{ Label = ('G6/G7 调 build.ps1：构建留下的进程不拖住运行器的调用方（孙进程还活着时读到 EOF，{0:N1} 秒）' -f $fast.Seconds); Bad = $false; Passed = $fast.Done -and $fast.Launched -and $fast.EofBeforeRelease }
        [pscustomobject]@{ Label = ('反例：不改句柄继承时，孙进程不放走调用方就读不到 EOF（放走后才读到，{0:N1} 秒）' -f $slow.Seconds); Bad = $true; Passed = $slow.Done -and $slow.Launched -and -not $slow.EofBeforeRelease }
        $plug = Measure-CallerEof $Root '30-plugin.ps1' "Invoke-PluginvalGate `$c '-Validate' 'x'"
        [pscustomobject]@{ Label = ('G8 调 build.ps1：pluginval 留下的进程不拖住运行器的调用方（孙进程还活着时读到 EOF，{0:N1} 秒）' -f $plug.Seconds); Bad = $false; Passed = $plug.Done -and $plug.Launched -and $plug.EofBeforeRelease }
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
