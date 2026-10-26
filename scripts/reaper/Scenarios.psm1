# SPDX-License-Identifier: GPL-3.0-or-later
#
# 烟测场景的执行：造信号、生成工程、起 REAPER 渲染或跑 Lua 驱动、用 rinne_nulltest 判结果。
# 每个场景返回 @{ Status; Detail }：PASS / FAIL / INCOMPLETE。INCOMPLETE 是「没能得出结论」（超时、被杀、卡对话框、
# 没产出文件、锁等不到），任何时候都不当 PASS。判据不通过才是 FAIL。
# 场景种类登记在 $script:Handlers，scenarios.json 里每个条目的 kind 选一个；以后加种类只在这里加一项。
#   render     离线或 1x 渲染，与同参数无插件渲染逐位对拍
#   probe      设备块长 / 采样率读回，两个插件上报延迟为 0
#   instances  反复加减实例，句柄与内存增长有上界
#   savereopen 存盘重开后状态还在，重开的工程再渲染仍逐位相同
#   hang       故意卡住的驱动，只给超时与杀进程两个反例用
# 第二段（探针构建，-Probe 时才跑；场景在 scenarios.json 的 probe 一节）：
#   probe-play    一格稳态：经隐藏参数开记录窗口、实时播放、停走带让探针自动 Ping，正常退出后收日志、读出 Ping 与 D、块长、
#                 配对、丢记录，再交给 analyze-probe.py
#   probe-export  离线渲染与在线渲染（Online Render，REAPER 按实时速度跑、对插件报实时模式）各一次（同一段范围），两份渲染用
#                 rinne_nulltest compare 对拍，比对结果写成 compare-*.csv，和两次的日志一起交给 analyze-probe.py。
#                 REAPER 的 1x Offline 虽然也按实时速度跑，对插件仍报离线模式，不是实时渲染，这里不用它做 export-rt。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot '../lib/Locks.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot '../../tests/reaper/Rpp.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'ProbeLogs.psm1') -ErrorAction Stop

function New-Outcome([string]$Status, [string]$Detail = '') { [pscustomobject]@{ Status = $Status; Detail = $Detail } }

# 跑一个原生程序，输出按 UTF-8 解码（rinne_nulltest 的报告是中文）。
function Invoke-Native {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @())
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    [pscustomobject]@{ Exit = $p.ExitCode; Out = $so.Result; Err = $se.Result }
}

function New-ReaperContext {
    param([Parameter(Mandatory)][string]$ReaperExe, [Parameter(Mandatory)][string]$NullTest, [Parameter(Mandatory)][string]$WorkDir,
          [Parameter(Mandatory)][string]$Driver, [Parameter(Mandatory)]$Config, [double]$LockWaitSec = 1800, [string]$GuiLock = 'gui')
    @{
        Exe = $ReaperExe; NullTest = $NullTest; Work = $WorkDir; Driver = $Driver; Limits = $Config.limits; Signal = $Config.signal
        LockWaitSec = $LockWaitSec; Launches = 0; Signals = @{}; Refs = @{}; Files = [Collections.Generic.List[string]]::new()
        # GuiLock：起 REAPER 时持的锁，看门狗反例事后核对的也是它（同一个名字、同一套锁名前缀）
        GuiLock = $GuiLock; Env = @{}; Launcher = $null # Env：当前场景的 env 字段；Launcher 只给自测注入
    }
}

# 私有 ini 里的音频配置：Dummy 设备（不出声）、采样率、块长；录音结束不弹保存框。
function Set-ReaperAudioConfig([int]$Sr, [int]$Block) {
    $null = Set-ReaperIniValue -Section audioconfig -Key mode -Value 4
    $null = Set-ReaperIniValue -Section audioconfig -Key dummy_srate -Value $Sr
    $null = Set-ReaperIniValue -Section audioconfig -Key dummy_blocksize -Value $Block
    $null = Set-ReaperIniValue -Section REAPER -Key promptendrec -Value 0
}

# 持 gui 锁起一次 REAPER；锁等不到返回 Status = LockBusy（调用方判 INCOMPLETE，绝不无锁硬跑）。
function Invoke-ReaperLaunch {
    param([Parameter(Mandatory)]$Ctx, [Parameter(Mandatory)][hashtable]$HostArgs)
    $lock = Enter-RinneLock $Ctx.GuiLock -TimeoutSeconds $Ctx.LockWaitSec
    if (-not $lock) { return [pscustomobject]@{ Status = 'LockBusy'; ExitCode = $null; Seconds = 0; Modal = $null; Dismissed = @(); Windows = @(); Marks = @{} } }
    try {
        $Ctx.Launches++
        $envMap = @{}
        foreach ($src in @($Ctx.Env, $HostArgs['Environment'])) { if ($src) { foreach ($k in $src.Keys) { $envMap[$k] = $src[$k] } } }
        $extra = @{ Environment = $envMap }
        if ($Ctx.Launcher) { $extra.Launcher = $Ctx.Launcher }
        $rest = @{} + $HostArgs; $null = $rest.Remove('Environment')
        Invoke-ReaperHost -ReaperExe $Ctx.Exe -WorkDir $Ctx.Work @rest @extra
    } finally { Exit-RinneLock $lock }
}

# REAPER 这一趟有没有正常结束；没有就说清楚怎么没的。
function Get-HostFailure($Run) {
    switch ($Run.Status) {
        'LockBusy' { return 'gui 锁等不到' }
        'Timeout' { return "超时（$($Run.Seconds) s）被结束" }
        'Killed' { return "到点被结束（$($Run.Seconds) s）" }
        'Modal' { return "卡在对话框 $($Run.Modal)（见到：$($Run.Windows -join ', ')），被结束" }
    }
    if ($Run.ExitCode -ne 0) { return "退出码 $($Run.ExitCode)" }
    $null
}

function Get-Signal {
    param([Parameter(Mandatory)]$Ctx, [int]$Sr, [int]$Channels, [int]$OffsetSamples = 0)
    $key = "$Sr-$Channels-$OffsetSamples"
    if (-not $Ctx.Signals.ContainsKey($key)) {
        $path = Join-Path $Ctx.Work "sig-$key.wav"
        $a = @('make-signal', '--out', $path, '--sr', "$Sr", '--channels', "$Channels", '--seconds', "$($Ctx.Signal.seconds)", '--kind', $Ctx.Signal.kind)
        if ($OffsetSamples -gt 0) { $a += @('--offset-samples', "$OffsetSamples") }
        $r = Invoke-Native -Exe $Ctx.NullTest -Arguments $a
        if ($r.Exit -ne 0) { throw "造测试信号失败（退出码 $($r.Exit)）：$($r.Err.Trim())" }
        $Ctx.Files.Add($path)
        $Ctx.Signals[$key] = $path
    }
    $Ctx.Signals[$key]
}

# 写工程并设好音频配置；返回工程路径与渲染输出 wav 的路径。
function New-ReaperRun {
    param([Parameter(Mandatory)]$Ctx, [string]$Name, [int]$Sr, [int]$Block, [string]$Mode, [int]$Channels, [string[]]$Chain, [string[]]$Bypass = @(),
          [double]$Volume = 1.0, [int]$OffsetSamples = 0, [switch]$Probe, [int]$ProbeGroup = -1)
    $probeArgs = $Probe ? @{ Probe = $true; ProbeGroup = $ProbeGroup } : @{}
    $projectPath = Join-Path $Ctx.Work "$Name.rpp"
    $wavPath = Join-Path $Ctx.Work "$Name.wav"
    if (Test-Path -LiteralPath $wavPath) { Remove-Item -LiteralPath $wavPath }
    $text = New-ReaperProjectText -SourceWav (Get-Signal $Ctx $Sr $Channels $OffsetSamples) -RenderDir $Ctx.Work -RenderName $Name `
        -SampleRate $Sr -Channels $Channels -OneX:($Mode -eq '1x') -Online:($Mode -eq 'online') -Chain $Chain -Bypass $Bypass -TrackVolume $Volume -ItemSeconds $Ctx.Signal.seconds @probeArgs
    Write-ReaperText -Path $projectPath -Text $text
    Set-ReaperAudioConfig -Sr $Sr -Block $Block
    $Ctx.Files.Add($projectPath); $Ctx.Files.Add($wavPath)
    @{ Project = $projectPath; Wav = $wavPath }
}

function Invoke-RenderProject($Ctx, [string]$Project, [string]$Wav) {
    $hostRun = Invoke-ReaperLaunch -Ctx $Ctx -HostArgs @{ ReaperArgs = @('-renderproject', "`"$Project`""); TimeoutSec = $Ctx.Limits.renderTimeoutSec }
    $why = Get-HostFailure $hostRun
    if (-not $why -and -not (Test-Path -LiteralPath $Wav)) { $why = '渲染没有产出 wav' }
    [pscustomobject]@{ Run = $hostRun; Why = $why }
}

function Get-RenderReference {
    param([Parameter(Mandatory)]$Ctx, [int]$Sr, [string]$Mode, [int]$Channels)
    $key = "$Sr-$Mode-$Channels"
    if (-not $Ctx.Refs.ContainsKey($key)) {
        $r = New-ReaperRun -Ctx $Ctx -Name "ref-$key" -Sr $Sr -Block 512 -Mode $Mode -Channels $Channels -Chain @()
        $res = Invoke-RenderProject $Ctx $r.Project $r.Wav
        $Ctx.Refs[$key] = @{ Wav = $r.Wav; Why = $res.Why }
    }
    $Ctx.Refs[$key]
}

# rinne_nulltest compare 的退出码与报告 → 结论（纯函数，自测用合成文本逐条验证）：退出码 0、逐位相同、lag 0、前 2 s 残差 -inf
# 四条同时成立才算过，缺一条就是 FAIL。退出码 1（文件或参数错）是 INCOMPLETE：没能比，不是比出了差异。
function Get-CompareVerdict([int]$Exit, [string]$Out, [string]$Err = '') {
    if ($Exit -eq 1) { return New-Outcome INCOMPLETE ('nulltest 报错：' + ($Err.Trim() -replace '\s+', ' ')) }
    $bit = $Out -match '逐位相同：是'
    $lag = [regex]::Match($Out, 'lag）：(-?\d+)')
    $head = [regex]::Match($Out, '开头[^\n]*?峰值 (\S+) dBFS')
    $summary = '逐位相同={0} lag={1} 前2s峰值={2}' -f ($bit ? '是' : '否'), ($lag.Success ? $lag.Groups[1].Value : '?'), ($head.Success ? $head.Groups[1].Value : '?')
    $reason = [regex]::Match($Out, '原因：([^\r\n]+)') # 对拍工具自己给的拒绝理由（例如 A 里没有声音）：没有比对行时靠它说清为什么
    if ($reason.Success) { $summary += '；对拍工具说：' + $reason.Groups[1].Value.Trim() }
    $zeroLag = $lag.Success -and $lag.Groups[1].Value -eq '0'
    $silentHead = $head.Success -and $head.Groups[1].Value -eq '-inf'
    if ($Exit -eq 0 -and $bit -and $zeroLag -and $silentHead) { return New-Outcome PASS $summary }
    New-Outcome FAIL $summary
}

function Compare-RenderWavs($Ctx, [string]$Reference, [string]$Test) {
    $r = Invoke-Native -Exe $Ctx.NullTest -Arguments @('compare', '--a', $Reference, '--b', $Test, '--head-seconds', '2')
    Get-CompareVerdict -Exit $r.Exit -Out $r.Out -Err $r.Err
}

function Invoke-LuaStep {
    param([Parameter(Mandatory)]$Ctx, [string]$Name, [string]$Step, [hashtable]$Job = @{}, [hashtable]$Watch = @{}, [double]$TimeoutSec = 0, [double]$KillAfterSec = 0)
    $jobFile = Join-Path $Ctx.Work "$Name.job.lua"
    $resultFile = Join-Path $Ctx.Work "$Name.result"
    foreach ($f in @($resultFile) + @($Watch.Values)) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f } }
    $Ctx.Files.Add($jobFile); $Ctx.Files.Add($resultFile); foreach ($f in $Watch.Values) { $Ctx.Files.Add($f) }
    Write-ReaperText -Path $jobFile -Text (ConvertTo-ReaperLuaTable (@{ step = $Step; result = $resultFile } + $Job))
    $hostRun = Invoke-ReaperLaunch -Ctx $Ctx -HostArgs @{
        ReaperArgs = @("`"$($Ctx.Driver)`""); Environment = @{ RINNE_REAPER_JOB = $jobFile }; WatchFiles = $Watch
        TimeoutSec = ($TimeoutSec -gt 0 ? $TimeoutSec : $Ctx.Limits.luaTimeoutSec); KillAfterSec = $KillAfterSec
    }
    $report = @{}
    if (Test-Path -LiteralPath $resultFile) {
        foreach ($line in [IO.File]::ReadAllLines($resultFile)) { $i = $line.IndexOf('='); if ($i -gt 0) { $report[$line.Substring(0, $i)] = $line.Substring($i + 1) } }
    }
    $why = Get-HostFailure $hostRun
    if (-not $why -and $report['done'] -ne '1') { $why = 'Lua 驱动没有写出结果' }
    if (-not $why -and $report.ContainsKey('error')) { $why = "Lua 驱动报错：$($report['error'])" }
    [pscustomobject]@{ Run = $hostRun; Result = $report; Why = $why }
}

function Get-Field($Scenario, [string]$Name, $Default) { $Scenario.PSObject.Properties.Name -contains $Name ? $Scenario.$Name : $Default }

# -------- 场景种类 --------

function Invoke-RenderScenario($Ctx, $S) {
    $rate = [int]$S.sr; $renderMode = Get-Field $S mode offline; $mono = [bool](Get-Field $S mono $false)
    $mut = Get-Field $S mutate $null
    $ch = $mono ? 1 : 2
    $ref = Get-RenderReference $Ctx $rate $renderMode $ch
    if ($ref.Why) { return New-Outcome INCOMPLETE "参照渲染没成：$($ref.Why)" }
    $offset = ($mut -and ($mut.PSObject.Properties.Name -contains 'sourceOffsetSamples')) ? [int]$mut.sourceOffsetSamples : 0
    $vol = ($mut -and ($mut.PSObject.Properties.Name -contains 'trackVolume')) ? [double]$mut.trackVolume : 1.0
    $r = New-ReaperRun -Ctx $Ctx -Name $S.id -Sr $rate -Block ([int]$S.block) -Mode $renderMode -Channels $ch -Chain @(Get-Field $S chain @('input', 'output')) `
        -Bypass @(Get-Field $S bypass @()) -Volume $vol -OffsetSamples $offset
    $res = Invoke-RenderProject $Ctx $r.Project $r.Wav
    if ($res.Why) { return New-Outcome INCOMPLETE $res.Why }
    Compare-RenderWavs $Ctx $ref.Wav $r.Wav
}

function Test-FxReport([hashtable]$Facts, [string]$Prefix, [string[]]$Bypassed = @()) {
    $problems = [Collections.Generic.List[string]]::new()
    if ($Facts["${Prefix}fx_count"] -ne '2') { $problems.Add("${Prefix}插件数 $($Facts["${Prefix}fx_count"])，应为 2") }
    foreach ($pair in @(@(0, 'Rinne Input', 'input'), @(1, 'Rinne Output', 'output'))) {
        $i = $pair[0]
        if ($Facts["${Prefix}fx${i}_name"] -notlike "*$($pair[1])*") { $problems.Add("${Prefix}第 $i 个不是 $($pair[1])") }
        foreach ($k in 'pdc', 'chain_pdc_actual') { if ($Facts["${Prefix}fx${i}_$k"] -ne '0') { $problems.Add("${Prefix}fx$i $k=$($Facts["${Prefix}fx${i}_$k"])，应为 0") } }
        $want = ($pair[2] -in $Bypassed) ? 'true' : 'false'
        if ($Facts["${Prefix}fx${i}_bypassed"] -ne $want) { $problems.Add("${Prefix}fx$i 旁路=$($Facts["${Prefix}fx${i}_bypassed"])，应为 $want") }
    }
    , $problems.ToArray()
}

function Invoke-ProbeScenario($Ctx, $S) {
    $rate = [int]$S.sr; $blockSize = [int]$S.block
    $r = New-ReaperRun -Ctx $Ctx -Name $S.id -Sr $rate -Block $blockSize -Mode offline -Channels 2 -Chain @('input', 'output')
    $lua = Invoke-LuaStep -Ctx $Ctx -Name $S.id -Step probe -Job @{ project = $r.Project }
    if ($lua.Why) { return New-Outcome INCOMPLETE $lua.Why }
    $bad = [Collections.Generic.List[string]]::new()
    if ([int]$lua.Result['dev_bsize'] -ne $blockSize) { $bad.Add("块长读回 $($lua.Result['dev_bsize'])，应为 $blockSize") }
    if ([int]$lua.Result['dev_srate'] -ne $rate) { $bad.Add("采样率读回 $($lua.Result['dev_srate'])，应为 $rate") }
    foreach ($p in (Test-FxReport $lua.Result '')) { $bad.Add($p) }
    if ($bad.Count) { return New-Outcome FAIL ($bad -join '；') }
    New-Outcome PASS "块长 $blockSize、采样率 $rate 读回一致，两个插件 pdc 与 chain_pdc_actual 均为 0"
}

function Invoke-InstancesScenario($Ctx, $S) {
    $lim = $Ctx.Limits.instances
    $r = New-ReaperRun -Ctx $Ctx -Name $S.id -Sr 48000 -Block 512 -Mode offline -Channels 2 -Chain @('input', 'output')
    $watchFiles = @{ warm = (Join-Path $Ctx.Work "$($S.id).warm"); loop = (Join-Path $Ctx.Work "$($S.id).loop") }
    $lua = Invoke-LuaStep -Ctx $Ctx -Name $S.id -Step instances -Watch $watchFiles -Job @{
        project = $r.Project; cycles = [int]$lim.cycles; warmup = [int]$lim.warmup; play = [bool]$lim.play; warmMark = $watchFiles.warm; loopMark = $watchFiles.loop
    }
    if ($lua.Why) { return New-Outcome INCOMPLETE $lua.Why }
    if ($lua.Result['cycles_done'] -ne "$($lim.cycles)") { return New-Outcome FAIL "只做完 $($lua.Result['cycles_done']) / $($lim.cycles) 轮" }
    $marks = $lua.Run.Marks
    $markWhy = Get-ReaperMarkProblem -Samples $marks -LostAt @($lua.Run.SampleLost) -Need 'warm', 'loop'
    if ($markWhy) { return New-Outcome INCOMPLETE $markWhy }
    $dh = $marks['loop'].Handles - $marks['warm'].Handles
    $dm = [math]::Round($marks['loop'].PrivateMB - $marks['warm'].PrivateMB, 1)
    $summary = "$($lim.cycles) 轮（预热 $($lim.warmup)）：句柄 $($dh -ge 0 ? '+' : '')$dh，私有内存 $($dm -ge 0 ? '+' : '')$dm MB，轨道剩 $($lua.Result['tracks_left'])"
    if ($dh -gt $lim.maxHandleGrowth -or $dm -gt $lim.maxPrivateGrowthMB) { return New-Outcome FAIL "增长超过上界（句柄 $($lim.maxHandleGrowth)、内存 $($lim.maxPrivateGrowthMB) MB）：$summary" }
    New-Outcome PASS $summary
}

function Invoke-SaveReopenScenario($Ctx, $S) {
    $rate = 48000
    $bypassList = @(Get-Field $S bypass @())
    $ref = Get-RenderReference $Ctx $rate offline 2
    if ($ref.Why) { return New-Outcome INCOMPLETE "参照渲染没成：$($ref.Why)" }
    $r = New-ReaperRun -Ctx $Ctx -Name $S.id -Sr $rate -Block 512 -Mode offline -Channels 2 -Chain @('input', 'output') -Bypass $bypassList
    $saved = Join-Path $Ctx.Work "$($S.id)-saved.rpp"
    if (Test-Path -LiteralPath $saved) { Remove-Item -LiteralPath $saved }
    $Ctx.Files.Add($saved)
    $lua = Invoke-LuaStep -Ctx $Ctx -Name $S.id -Step savereopen -Job @{ project = $r.Project; saveAs = $saved }
    if ($lua.Why) { return New-Outcome INCOMPLETE $lua.Why }
    $bad = [Collections.Generic.List[string]]::new()
    foreach ($p in (Test-FxReport $lua.Result 'before_' $bypassList)) { $bad.Add($p) }
    foreach ($p in (Test-FxReport $lua.Result 'after_' $bypassList)) { $bad.Add($p) }
    if ($lua.Result['saved'] -ne 'true') { $bad.Add('没有存出新工程文件') }
    if ($bad.Count) { return New-Outcome FAIL ($bad -join '；') }
    if (Test-Path -LiteralPath $r.Wav) { Remove-Item -LiteralPath $r.Wav }
    $res = Invoke-RenderProject $Ctx $saved $r.Wav
    if ($res.Why) { return New-Outcome INCOMPLETE "重开后渲染没成：$($res.Why)" }
    $cmp = Compare-RenderWavs $Ctx $ref.Wav $r.Wav
    New-Outcome $cmp.Status "存盘重开后状态一致；重开渲染 $($cmp.Detail)"
}

# 故意卡住的驱动：超时 / 被杀都应是 INCOMPLETE，并且进程已经没了、gui 锁已经放开。
# 返回 INCOMPLETE 表示「看门狗按预期处理了」；锁没放开则返回 FAIL，别的结局也返回 FAIL。
function Invoke-HangScenario($Ctx, $S) {
    $r = New-ReaperRun -Ctx $Ctx -Name $S.id -Sr 48000 -Block 512 -Mode offline -Channels 2 -Chain @('input', 'output')
    $lua = Invoke-LuaStep -Ctx $Ctx -Name $S.id -Step hang -Job @{ project = $r.Project } -TimeoutSec ([double](Get-Field $S timeoutSec 0)) -KillAfterSec ([double](Get-Field $S killAfterSec 0))
    if (-not $lua.Why) { return New-Outcome PASS '本该卡住的驱动正常结束了（反例没有生效）' }
    # gui 锁没等到时 REAPER 根本没启动，Run 里没有进程号可查；原因照实写，别落成「场景抛错」
    if ($lua.Run.Status -eq 'LockBusy') { return New-Outcome INCOMPLETE "$($lua.Why)；REAPER 没有启动，看门狗没能验证" }
    if (Get-Process -Id $lua.Run.Pid -ErrorAction SilentlyContinue) { return New-Outcome FAIL "进程 $($lua.Run.Pid) 还活着" }
    $lock = $null
    $deadline = (Get-Date).AddSeconds(20)
    while (-not $lock -and (Get-Date) -lt $deadline) {
        $lock = Test-RinneLockFree $Ctx.GuiLock
        if (-not $lock) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $lock) { return New-Outcome FAIL 'gui 锁在进程结束后仍取不到' }
    New-Outcome INCOMPLETE "$($lua.Why)；进程已结束，gui 锁已放开"
}

# 锁的完整互斥体名：在本进程里按当前的锁名前缀解析（自测换了前缀，这里跟着换），子进程拿到的就是这个确切的名字。
function Get-ReaperLockMutexName([string]$Name) { Get-RinneLockName $Name }

# 另起一个 pwsh 试取这把锁（锁有线程归属，同一进程里取不出「别人持有」的效果）。取到即放，返回是否取到。
# 子进程不重新导入锁模块：重新导入会回到默认的锁名前缀，自测查到的就成了正式的那把（别人在跑 GUI 任务时误判）。
function Test-RinneLockFree([string]$Name) {
    $full = Get-ReaperLockMutexName $Name
    $cmd = "`$m = [Threading.Mutex]::new(`$false, '$full'); try { `$got = `$m.WaitOne(0) } catch [Threading.AbandonedMutexException] { `$got = `$true }; if (`$got) { `$m.ReleaseMutex(); exit 0 } else { exit 1 }"
    $r = Invoke-Native -Exe (Get-Process -Id $PID).Path -Arguments @('-NoProfile', '-NonInteractive', '-Command', $cmd)
    $r.Exit -eq 0
}

# -------- 第二段：探针 --------
# Ctx.Probe 只在 -Probe 的一轮里有：@{ LogDir（文档目录下的 Rinne Probe Logs）; RoundDir（本轮结果目录）; Group（探针 state 的组）;
#   Python; Analyzer（analyze-probe.py）; Redact（交给分析器 --redact 的词）; Own（本轮自己的日志名）; Moved（搬走的个数）;
#   Problems（收日志出的错）; Analyses（每格分析器的原始输出）; Rejected（分析器拒收过真日志） }

# 每个 REAPER 进程退出后立即收它自己的日志（只按这次启动的 pid 与启动时刻认）。锁等不到（REAPER 根本没启动）时没什么可收。
function Save-ProbeLogs($Ctx, $Run, [string]$CellDir) {
    if ($Run.Status -eq 'LockBusy' -or -not ($Run.PSObject.Properties.Name -contains 'Pid') -or -not $Run.Pid) { return $null }
    # 退出时刻是这次启动的进程退出的那一刻（Invoke-ReaperHost 记下）；没有这个字段的 Run（自测里手造的）不给，收日志取现在
    $exited = ($Run.PSObject.Properties.Name -contains 'ExitedUtc') ? @{ ExitedUtc = $Run.ExitedUtc } : @{}
    $m = Move-ReaperProbeLogs -LogDir $Ctx.Probe.LogDir -Pids @([int]$Run.Pid) -SinceUtc $Run.StartedUtc -DestDir $CellDir @exited
    foreach ($logName in $m.Own) { $Ctx.Probe.Own.Add($logName) }
    foreach ($why in $m.Problems) { $Ctx.Probe.Problems.Add($why) }
    foreach ($note in $m.Notes) { $Ctx.Probe.Notes.Add($note) }
    $Ctx.Probe.Moved += $m.Moved.Count
    $m
}

function Get-SortedText($Values) { (@($Values) | ForEach-Object { "$_" }) -join '/' }

# 纯函数：一次 REAPER 启动留下的两份日志（Read-ReaperProbeLog 的结果）对不对。Expect：Pid、Case、Variant、Sr、Block、MinBlocks、
# NeedPing（要有 Ping，且 Ping 求出的延迟恰为块记录里的 D；稳态格与导出格的两次都要）、PlayingNonRt（播放中的块的 nonrt 应恰为这个值：
# 宿主对插件报实时是 0、离线是 1；给了才查）、Driver（Lua 驱动记下的退出时刻，原样写进事实）。
# 每格都查：两端各一份、恰好一个测试格、正常收尾、块数够、没丢记录、每块的块长不超过它自己声明的最大块长（逐块比 n 与 n_decl）、配对、播放中的块长恰为设定的块长、
# D 恰为探针默认的 100 ms（工程里的探针 state 写的就是它；不是它说明 state 模板与探针对不上）。
# 返回 @{ Bad（要判 FAIL 的原因）; Facts（读出来的事实，写进结果） }。
function Test-ReaperProbeCell {
    param([object[]]$Logs = @(), [Parameter(Mandatory)][hashtable]$Expect)
    $bad = [Collections.Generic.List[string]]::new()
    $cellFacts = [ordered]@{ pid = $Expect.Pid }
    $mine = @($Logs | Where-Object { $_.Header -and $_.Header.Pid -eq $Expect.Pid })
    $ins = @($mine | Where-Object { $_.Header.Role -ceq 'I' })
    $outs = @($mine | Where-Object { $_.Header.Role -ceq 'O' })
    if ($ins.Count -ne 1 -or $outs.Count -ne 1) {
        $bad.Add("这次启动的日志应为 In、Out 各一份，实际 In $($ins.Count)、Out $($outs.Count)")
        return [pscustomobject]@{ Bad = $bad.ToArray(); Facts = $cellFacts }
    }
    foreach ($pair in @(@('In', $ins[0]), @('Out', $outs[0]))) {
        $tag = $pair[0]; $lg = $pair[1]
        $cells = @($lg.Cases | ForEach-Object { "$($_.Case)/$($_.Variant)/buf=$($_.Buf)/guard=$($_.Guard)/sr=$($_.Sr)" })
        $hit = @($lg.Cases | Where-Object { $_.Case -ceq $Expect.Case -and $_.Variant -ceq $Expect.Variant })
        if ($lg.Cases.Count -ne 1 -or $hit.Count -ne 1) { $bad.Add("$tag 的测试格是 [$($cells -join '; ')]，应恰好一格 $($Expect.Case)/$($Expect.Variant)") }
        elseif ($hit[0].Sr -ne $Expect.Sr) { $bad.Add("$tag 测试格的采样率是 $($hit[0].Sr)，应为 $($Expect.Sr)") }
        if (-not $lg.Complete) { $bad.Add("$tag 日志没有正常收尾（没有 Z 行，或末行没写完）") }
        if ($lg.Blocks -lt $Expect.MinBlocks) { $bad.Add("$tag 只有 $($lg.Blocks) 块记录，至少要 $($Expect.MinBlocks) 块") }
        if ($lg.Dropped -gt 0 -or $lg.SeqGaps -gt 0) { $bad.Add("$tag 丢了记录（丢弃计数 $($lg.Dropped)、块序号缺口 $($lg.SeqGaps)）") }
        $decl = @($lg.NDecl)
        # 块长上限逐块比：每个 B 行带自己的 n 与 n_decl（一份日志里 prepare 可以有多次，n_decl 不止一个值），比最小值会冤枉后一段，比最大值会放过前一段
        if ($lg.OverBlocks -gt 0) { $bad.Add("$tag 有 $($lg.OverBlocks) 块的块长超过它自己声明的最大块长（第一例：$($lg.OverExample)；全部声明值 $(Get-SortedText $decl)）") }
        if ($Expect.Pid -notin @($lg.PairedPids)) { $bad.Add("$tag 没有配对（没有对端是本进程的 paired 事件）") }
        if ($Expect.ContainsKey('PlayingNonRt')) {
            $seen = @($lg.PlayingNonRt)
            if ($seen.Count -ne 1 -or $seen[0] -ne $Expect.PlayingNonRt) {
                $bad.Add("$tag 播放中的块的 nonrt 是 [$(Get-SortedText $seen)]，应恰为 $($Expect.PlayingNonRt)（宿主应按$(($Expect.PlayingNonRt -eq 1) ? '离线' : '实时')模式渲染或播放）")
            }
        }
        if ($lg.Unread -gt 0) { $bad.Add("$tag 有 $($lg.Unread) 行读不懂（格式交给分析器判）") }
        $cellFacts[$tag] = [ordered]@{
            cell = ($cells -join '; '); blocks = $lg.Blocks; playingBlocks = $lg.PlayingBlocks; playingSeconds = [math]::Round($lg.PlayingBlocks * [double]$Expect.Block / [double]$Expect.Sr, 2); n = (Get-SortedText $lg.NValues); nDecl = (Get-SortedText $decl)
            maxN = $lg.MaxN; overBlocks = $lg.OverBlocks; nonrt = (Get-SortedText $lg.NonRt); playingNonrt = (Get-SortedText $lg.PlayingNonRt); dropped = $lg.Dropped; seqGaps = $lg.SeqGaps; paired = ($Expect.Pid -in @($lg.PairedPids))
            complete = $lg.Complete; prepare = (@($lg.Prepares) -join ' ')
        }
    }
    # Ping 只在 Input 一端；D 取 Input 块记录里的 d（探针自己记下的当前生效环路延迟）
    $dList = @($ins[0].DValues)
    $pings = @($ins[0].Pings)
    if ($Expect.ContainsKey('Driver')) { $cellFacts.driver = $Expect.Driver }
    $cellFacts.d = Get-SortedText $dList
    $cellFacts.ping = Get-SortedText $pings
    $cellFacts.dNominal = [long][Math]::Round(0.1 * $Expect.Sr) # 探针默认的 D 是 100 ms
    $playingN = @(@($ins[0].NValues) + @($outs[0].NValues) | Sort-Object -Unique)
    $cellFacts.blockMatches = ($playingN.Count -eq 1 -and $playingN[0] -eq $Expect.Block)
    if ($playingN.Count -and -not $cellFacts.blockMatches) { $bad.Add("播放中的块长是 $(Get-SortedText $playingN)，应恰为设定的 $($Expect.Block)") }
    if ($dList.Count -eq 1 -and $dList[0] -ne $cellFacts.dNominal) { $bad.Add("D 是 $($dList[0]) 样本，应为探针默认的 100 ms（$($cellFacts.dNominal) 样本）") }
    if ($Expect.NeedPing) {
        if ($dList.Count -ne 1) { $bad.Add("In 端块记录里的 D 应恰好一个值，实际 [$(Get-SortedText $dList)]") }
        if ($pings.Count -eq 0) { $bad.Add('没有 Ping 结果（停走带后探针应自动 Ping 一次）') }
        elseif ($dList.Count -eq 1 -and @($pings | Where-Object { $_ -ne $dList[0] }).Count) { $bad.Add("Ping 求出的延迟是 $(Get-SortedText $pings) 样本，D 是 $($dList[0]) 样本") }
        if ($ins[0].PlayingBlocks -lt $Expect.MinBlocks -or $outs[0].PlayingBlocks -lt $Expect.MinBlocks) { $bad.Add("播放中的块不足 $($Expect.MinBlocks)（In $($ins[0].PlayingBlocks)、Out $($outs[0].PlayingBlocks)）") }
    }
    [pscustomobject]@{ Bad = $bad.ToArray(); Facts = $cellFacts }
}

# 跑 analyze-probe.py <目录> --json，按退出码归类：0 / 1 / 3 读得出 JSON；2 是输入不合格式（拒收），4 是分析器内部错误。
function Invoke-ReaperProbeAnalyzer {
    param([Parameter(Mandatory)][string]$Python, [Parameter(Mandatory)][string]$Script, [Parameter(Mandatory)][string]$Dir, [string[]]$Redact = @())
    $lead = ((Split-Path -Leaf $Python) -in 'py', 'py.exe') ? @('-3') : @()
    $argv = @($lead) + @('-I', $Script, $Dir, '--json')
    foreach ($w in $Redact) { if ($w) { $argv += @('--redact', $w) } }
    $r = Invoke-Native -Exe $Python -Arguments $argv
    $parsed = $null
    $parseError = ''
    if ($r.Exit -in 0, 1, 3) {
        try { $parsed = $r.Out | ConvertFrom-Json -ErrorAction Stop } catch { $parseError = '分析器的输出不是 JSON' }
    }
    [pscustomobject]@{ Exit = $r.Exit; Json = $parsed; Out = $r.Out; Err = ($r.Err.Trim() -replace '\s+', ' '); ParseError = $parseError }
}

# 纯函数：分析器结果 → 一行摘要与这一格要看的结论。分析器的判据按 Cubase 的清单设计，REAPER 下多数项是数据不全；
# 只有 MustPass 里的项（scenarios.json 里每格的 analyzerMustPass）计入这一格的结局：其中 FAIL 记进 Fails，没到 PASS 的
# （数据不全、没有这一项）记进 Short。别的项照录在 Text 里，不计入（为什么不作数见 REAPER.md）。Exit 2 是拒收（输入不合格式）。
function Get-ReaperAnalyzerSummary($Analysis, [string[]]$MustPass = @()) {
    $none = [pscustomobject]@{ Rejected = $false; Fails = @(); Short = @($MustPass); Text = '' }
    if ($Analysis.Exit -eq 2) { $none.Rejected = $true; $none.Text = "分析器拒收（退出码 2）：$($Analysis.Err)"; return $none }
    if (-not $Analysis.Json) { $none.Text = "分析器没给出结论（退出码 $($Analysis.Exit)）$($Analysis.ParseError)"; return $none }
    $items = @($Analysis.Json.items)
    $verdictOf = @{}
    foreach ($it in $items) { $verdictOf[[string]$it.id] = [string]$it.verdict }
    $fails = @($MustPass | Where-Object { $verdictOf[$_] -ceq 'FAIL' })
    $short = @($MustPass | Where-Object { $verdictOf[$_] -cne 'PASS' -and $verdictOf[$_] -cne 'FAIL' })
    $record = -not $MustPass.Count # 没有要计入的项：整轮合起来那一次，只作记录
    $line = ($items | ForEach-Object { "$($_.id)=$($_.verdict)" + (($record -or $_.id -in $MustPass) ? '' : '*') }) -join ' '
    $legend = $record ? '只作记录' : '* 为不计入本格的项'
    [pscustomobject]@{ Rejected = $false; Fails = $fails; Short = $short; Text = "分析器（退出码 $($Analysis.Exit)，$legend）$line F1=$($Analysis.Json.f1.ok ? '满足' : '未满足')" }
}

function Invoke-ProbeCellAnalysis($Ctx, [string]$Id, [string]$CellDir, [string[]]$MustPass) {
    $a = Invoke-ReaperProbeAnalyzer -Python $Ctx.Probe.Python -Script $Ctx.Probe.Analyzer -Dir $CellDir -Redact $Ctx.Probe.Redact
    $raw = Join-Path $Ctx.Probe.RoundDir "analyze-$Id.json"
    [IO.File]::WriteAllText($raw, $a.Out, [Text.UTF8Encoding]::new($false))
    $Ctx.Probe.Analyses[$Id] = [ordered]@{ exit = $a.Exit; result = $a.Json; stderr = $a.Err }
    $sum = Get-ReaperAnalyzerSummary $a -MustPass $MustPass
    if ($sum.Rejected) { $Ctx.Probe.Rejected = $true }
    $sum
}

function Read-ProbeCellLogs([string]$CellDir) {
    if (-not (Test-Path -LiteralPath $CellDir -PathType Container)) { return @() }
    foreach ($f in Get-ChildItem -LiteralPath $CellDir -File -Filter 'probe-*.csv') { Read-ReaperProbeLog -Text ([IO.File]::ReadAllText($f.FullName)) }
}

function Format-ProbeFacts($Facts) {
    $ping = $Facts.ping ? "Ping $($Facts.ping) 样本 / D $($Facts.d) 样本" : "没有 Ping（D $($Facts.d) 样本）"
    "pid $($Facts.pid)：$ping；块长 n=$($Facts.In.n)（n_decl $($Facts.In.nDecl)）；配对 In=$($Facts.In.paired) Out=$($Facts.Out.paired)；" +
    "块 In $($Facts.In.blocks)/Out $($Facts.Out.blocks)（播放中 $($Facts.In.playingBlocks)/$($Facts.Out.playingBlocks) 块，约 $($Facts.In.playingSeconds) s）；丢记录 $($Facts.In.dropped + $Facts.Out.dropped)；nonrt $($Facts.In.nonrt)（播放中 $($Facts.In.playingNonrt)）" +
    ($Facts.Contains('driver') ? "；驱动 $(($Facts.driver.Keys | ForEach-Object { "$_=$($Facts.driver[$_])" }) -join ' ')" : '')
}

# 收完日志、读出事实、过分析器之后给这一格下结论：收日志出错 FAIL；REAPER 这一趟没正常结束 INCOMPLETE；分析器拒收 INCOMPLETE
# （并让整套停下：日志与分析器的契约对不上）；自己的判据、或 MustPass 里的分析器项有 FAIL 就 FAIL；MustPass 里有项没到 PASS
# 是 INCOMPLETE（本该有数据的一项分析器没能下结论）；否则 PASS。
function Complete-ProbeCell($Ctx, [string]$Id, [string]$CellDir, $Moves, [string[]]$Whys, [hashtable[]]$Expects, [string[]]$MustPass, [string[]]$Notes = @(), [string[]]$ExtraBad = @()) {
    $collectBad = @($Moves | Where-Object { $_ } | ForEach-Object { $_.Problems })
    if ($collectBad.Count) { return New-Outcome FAIL "收日志出错：$($collectBad -join '；')" }
    $hostWhy = @($Whys | Where-Object { $_ })
    if ($hostWhy.Count) { return New-Outcome INCOMPLETE ($hostWhy -join '；') }
    $cellLogs = @(Read-ProbeCellLogs $CellDir)
    $bad = [Collections.Generic.List[string]]::new()
    foreach ($b in $ExtraBad) { $bad.Add($b) }
    $factsAll = [Collections.Generic.List[object]]::new()
    $parts = [Collections.Generic.List[string]]::new()
    foreach ($e in $Expects) {
        $t = Test-ReaperProbeCell -Logs $cellLogs -Expect $e
        foreach ($b in $t.Bad) { $bad.Add("$($e.Variant)：$b") }
        $one = [ordered]@{ variant = $e.Variant }
        foreach ($k in $t.Facts.Keys) { $one[$k] = $t.Facts[$k] }
        $factsAll.Add($one)
        if ($t.Facts.Contains('In')) { $parts.Add("$($e.Variant) " + (Format-ProbeFacts $t.Facts)) }
    }
    $sum = Invoke-ProbeCellAnalysis $Ctx $Id $CellDir $MustPass
    $cellDetail = (@($parts) + @($Notes) + @($sum.Text)) -join '；'
    $o = if ($sum.Rejected) { New-Outcome INCOMPLETE "分析器拒收真日志，日志与分析器的契约对不上：$($sum.Text)" }
    elseif ($bad.Count) { New-Outcome FAIL (($bad -join '；') + '；' + $cellDetail) }
    elseif ($sum.Fails.Count) { New-Outcome FAIL "分析器判出不通过（$($sum.Fails -join '、')）；$cellDetail" }
    elseif ($sum.Short.Count) { New-Outcome INCOMPLETE "分析器在本该有数据的项上没下结论（$($sum.Short -join '、')）；$cellDetail" }
    else { New-Outcome PASS $cellDetail }
    $o | Add-Member -NotePropertyName Facts -NotePropertyValue ([ordered]@{ cells = $factsAll.ToArray(); analyzer = $sum.Text }) -PassThru
}

function Get-ProbeLuaJob($Ctx, $S, [string]$Variant, [string]$Project) {
    $idx = Get-ReaperProbeAutoIndex -Case $S.case -Variant $Variant
    @{
        project = $Project; autoCase = [int]$idx.Case; autoVariant = [int]$idx.Variant; caseMax = [int]$idx.CaseMax; variantMax = [int]$idx.VariantMax
        recordSeconds = [int]$S.recordSeconds; leadSeconds = [double]$Ctx.Limits.leadSeconds; marginSeconds = [double]$Ctx.Limits.marginSeconds
        afterRenderSeconds = [double]$Ctx.Limits.afterRenderSeconds # 驱动在渲染返回后至少再等的秒数；场景表校验稳态格的停走带后余量用的是同一个字段
    }
}

# 探针 state 里的组：场景可以自己给（反例用 -1 造「没有配对」），不给就用 probe 一节的 group。
function Get-ProbeGroup($Ctx, $S) { [int](Get-Field $S group $Ctx.Probe.Group) }

function Invoke-ProbePlayScenario($Ctx, $S) {
    $rate = [int]$S.sr; $blockSize = [int]$S.block
    $r = New-ReaperRun -Ctx $Ctx -Name $S.id -Sr $rate -Block $blockSize -Mode offline -Channels 2 -Chain @('input', 'output') -Probe -ProbeGroup (Get-ProbeGroup $Ctx $S)
    $luaJob = Get-ProbeLuaJob $Ctx $S $S.variant $r.Project
    $luaJob.playSeconds = [double]$S.playSeconds
    $lua = Invoke-LuaStep -Ctx $Ctx -Name $S.id -Step probeplay -Job $luaJob
    $cell = Join-Path $Ctx.Probe.RoundDir "probe-logs/$($S.id)"
    $moved = Save-ProbeLogs $Ctx $lua.Run $cell
    # 实时播放：播放中的块 REAPER 都该按实时模式调用（nonrt 为 0）
    $playExpect = @{
        Pid = (Get-RunPid $lua.Run); Case = $S.case; Variant = $S.variant; Sr = $rate; Block = $blockSize; MinBlocks = [int]$Ctx.Limits.minBlocks; NeedPing = $true
        PlayingNonRt = 0; Driver = (Get-DriverFacts $lua.Result 'stopped_after', 'stop_to_exit_s')
    }
    Complete-ProbeCell -Ctx $Ctx -Id $S.id -CellDir $cell -Moves @($moved) -Whys @($lua.Why) -Expects @($playExpect) -MustPass @($S.analyzerMustPass)
}

# Lua 驱动写在结果文件里的几项时间（秒、退出由谁定的），原样记进这一格的事实；驱动没写出来的项不记
function Get-DriverFacts($Result, [string[]]$Keys) {
    $d = [ordered]@{}
    foreach ($k in $Keys) { if ($Result.ContainsKey($k)) { $d[$k] = $Result[$k] } }
    $d
}

# 纯函数：rinne_nulltest compare 的报告 → compare-*.csv 的一行（case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples）。
# 读不全、或逐位相同与残差自相矛盾（分析器会判格式错）就返回 $null，由调用方判 INCOMPLETE。
$script:CompareHead = 'case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples' # 与 scripts/probe/analyze-probe.py 的 COMPARE_HEAD 相同（自测比对）
function Get-ReaperCompareHead { $script:CompareHead }
function ConvertTo-ReaperCompareRow([string]$Case, [string]$Variant, [string]$Out) {
    $bit = $Out -match '逐位相同：是' ? 1 : ($Out -match '逐位相同：否' ? 0 : -1)
    $lag = [regex]::Match($Out, 'lag）：(-?\d+)')
    $whole = [regex]::Match($Out, '整体残差[^\n]*?峰值 (-inf|-?\d+(?:\.\d+)?) dBFS')
    $head = [regex]::Match($Out, '开头[^\n]*?峰值 (-inf|-?\d+(?:\.\d+)?) dBFS')
    if ($bit -lt 0 -or -not ($lag.Success -and $whole.Success -and $head.Success)) { return $null }
    if (($bit -eq 1) -ne ($whole.Groups[1].Value -ceq '-inf')) { return $null }
    '{0},{1},{2},{3},{4},{5}' -f $Case, $Variant, $bit, $whole.Groups[1].Value, $head.Groups[1].Value, $lag.Groups[1].Value
}

function Invoke-ProbeExportScenario($Ctx, $S) {
    $rate = [int]$S.sr; $blockSize = [int]$S.block
    $cell = Join-Path $Ctx.Probe.RoundDir "probe-logs/$($S.id)"
    $legMoves = [Collections.Generic.List[object]]::new()
    $legWhys = [Collections.Generic.List[string]]::new()
    $cellExpects = [Collections.Generic.List[hashtable]]::new()
    $wavs = @{}
    # 两次渲染：离线（全速）与在线（Online Render：按实时速度跑，REAPER 对插件报实时模式）。播放中的块的 nonrt 分别应恰为 1 与 0。
    # 两次都要有渲染结束后的自动 Ping，且恰为 D：Ping 没出来不能放过去（驱动在渲染返回之后至少再等 afterRenderSeconds 才退出）。
    foreach ($leg in @(@('offline', 'export-offline', 1), @('online', 'export-rt', 0))) {
        $legMode = $leg[0]; $legVariant = $leg[1]; $legNonRt = $leg[2]
        $r = New-ReaperRun -Ctx $Ctx -Name "$($S.id)-$legMode" -Sr $rate -Block $blockSize -Mode $legMode -Channels 2 -Chain @('input', 'output') -Probe -ProbeGroup (Get-ProbeGroup $Ctx $S)
        $luaJob = Get-ProbeLuaJob $Ctx $S $legVariant $r.Project
        $luaJob.wav = $r.Wav
        $lua = Invoke-LuaStep -Ctx $Ctx -Name "$($S.id)-$legMode" -Step probeexport -Job $luaJob
        $legMoves.Add((Save-ProbeLogs $Ctx $lua.Run $cell))
        $why = $lua.Why
        if (-not $why -and -not (Test-Path -LiteralPath $r.Wav)) { $why = "$legMode 渲染没有产出 wav" }
        if ($why) { $legWhys.Add("${legMode}：$why") }
        $wavs[$legMode] = $r.Wav
        $cellExpects.Add(@{
            Pid = (Get-RunPid $lua.Run); Case = $S.case; Variant = $legVariant; Sr = $rate; Block = $blockSize; MinBlocks = [int]$Ctx.Limits.minBlocks; NeedPing = $true
            PlayingNonRt = $legNonRt; Driver = (Get-DriverFacts $lua.Result 'render_seconds', 'exit_bound', 'render_end_to_exit_s')
        })
        if ($why) { break }
    }
    $extra = @(); $compareBad = @()
    if (-not $legWhys.Count) {
        $cmp = Invoke-Native -Exe $Ctx.NullTest -Arguments @('compare', '--a', $wavs['offline'], '--b', $wavs['online'], '--head-seconds', '2')
        $verdict = Get-CompareVerdict -Exit $cmp.Exit -Out $cmp.Out -Err $cmp.Err
        $row = ConvertTo-ReaperCompareRow -Case $S.case -Variant 'export-offline' -Out $cmp.Out
        if ($verdict.Status -eq 'INCOMPLETE') { $legWhys.Add("离线与在线的对拍没能比：$($verdict.Detail)") }
        elseif (-not $row) { $compareBad = @("离线对在线对拍不通过，报告里读不出比对行：$($verdict.Detail)") } # 采样率或声道不同、没有声音等
        else {
            $null = New-Item -ItemType Directory -Force -Path $cell
            [IO.File]::WriteAllText((Join-Path $cell 'compare-reaper.csv'), "$($script:CompareHead)`n$row`n", [Text.ASCIIEncoding]::new())
            if ($verdict.Status -eq 'PASS') { $extra = @("离线对在线：$($verdict.Detail)") } else { $compareBad = @("离线对在线对拍不通过：$($verdict.Detail)") }
        }
    }
    Complete-ProbeCell -Ctx $Ctx -Id $S.id -CellDir $cell -Moves $legMoves.ToArray() -Whys $legWhys.ToArray() -Expects $cellExpects.ToArray() -MustPass @($S.analyzerMustPass) -Notes $extra -ExtraBad $compareBad
}

# 这次启动的进程号；没启动（gui 锁等不到）时 Run 里没有这个属性，记 0，原因由 Whys 照实写，不落成「场景抛错」
function Get-RunPid($Run) { $p = $Run.PSObject.Properties['Pid']; $p ? [int]$p.Value : 0 }

$script:Handlers = @{
    render = ${function:Invoke-RenderScenario}; probe = ${function:Invoke-ProbeScenario}; instances = ${function:Invoke-InstancesScenario}
    savereopen = ${function:Invoke-SaveReopenScenario}; hang = ${function:Invoke-HangScenario}
    'probe-play' = ${function:Invoke-ProbePlayScenario}; 'probe-export' = ${function:Invoke-ProbeExportScenario}
}

function Get-ReaperScenarioKinds { , @($script:Handlers.Keys | Sort-Object) }

function Invoke-ReaperScenario {
    param([Parameter(Mandatory)]$Ctx, [Parameter(Mandatory)]$Scenario)
    $handler = $script:Handlers[$Scenario.kind]
    if (-not $handler) { return New-Outcome INCOMPLETE "没有 $($Scenario.kind) 这种场景" }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $Ctx.Env = @{}
    $envField = Get-Field $Scenario env $null
    if ($envField) { foreach ($p in $envField.PSObject.Properties) { $Ctx.Env[$p.Name] = ([string]$p.Value).Replace('{work}', $Ctx.Work) } }
    try { $o = & $handler $Ctx $Scenario } catch { $o = New-Outcome INCOMPLETE "场景抛错：$($_.Exception.Message -replace '\s+', ' ')" }
    $o | Add-Member -NotePropertyName Seconds -NotePropertyValue ([math]::Round($sw.Elapsed.TotalSeconds, 1)) -PassThru
}

Export-ModuleMember -Function New-Outcome, Invoke-Native, New-ReaperContext, Get-ReaperScenarioKinds, Invoke-ReaperScenario, Invoke-ReaperLaunch, Get-HostFailure, Get-CompareVerdict,
    Get-ReaperLockMutexName, Test-RinneLockFree, Test-ReaperProbeCell, Get-ReaperAnalyzerSummary,
    ConvertTo-ReaperCompareRow, Get-ReaperCompareHead, Invoke-ReaperProbeAnalyzer
