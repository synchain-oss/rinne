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

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot '../lib/Locks.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot '../../tests/reaper/Rpp.psm1') -ErrorAction Stop

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
          [Parameter(Mandatory)][string]$Driver, [Parameter(Mandatory)]$Config, [double]$LockWaitSec = 1800)
    @{
        Exe = $ReaperExe; NullTest = $NullTest; Work = $WorkDir; Driver = $Driver; Limits = $Config.limits; Signal = $Config.signal
        LockWaitSec = $LockWaitSec; Launches = 0; Signals = @{}; Refs = @{}; Files = [Collections.Generic.List[string]]::new()
        Env = @{}; Launcher = $null # Env：当前场景的 env 字段；Launcher 只给自测注入
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
    $lock = Enter-RinneLock gui -TimeoutSeconds $Ctx.LockWaitSec
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
          [double]$Volume = 1.0, [int]$OffsetSamples = 0)
    $projectPath = Join-Path $Ctx.Work "$Name.rpp"
    $wavPath = Join-Path $Ctx.Work "$Name.wav"
    if (Test-Path -LiteralPath $wavPath) { Remove-Item -LiteralPath $wavPath }
    $text = New-ReaperProjectText -SourceWav (Get-Signal $Ctx $Sr $Channels $OffsetSamples) -RenderDir $Ctx.Work -RenderName $Name `
        -SampleRate $Sr -Channels $Channels -OneX:($Mode -eq '1x') -Chain $Chain -Bypass $Bypass -TrackVolume $Volume -ItemSeconds $Ctx.Signal.seconds
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
    if (-not ($marks.Contains('warm') -and $marks.Contains('loop'))) { return New-Outcome INCOMPLETE '没取到预热与循环结束两处的进程计数' }
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
    if (Get-Process -Id $lua.Run.Pid -ErrorAction SilentlyContinue) { return New-Outcome FAIL "进程 $($lua.Run.Pid) 还活着" }
    $lock = $null
    $deadline = (Get-Date).AddSeconds(20)
    while (-not $lock -and (Get-Date) -lt $deadline) {
        $lock = Test-RinneLockFree gui
        if (-not $lock) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $lock) { return New-Outcome FAIL 'gui 锁在进程结束后仍取不到' }
    New-Outcome INCOMPLETE "$($lua.Why)；进程已结束，gui 锁已放开"
}

# 另起一个 pwsh 试取 gui 锁（锁有线程归属，同一进程里取不出「别人持有」的效果）。取到即放，返回是否取到。
function Test-RinneLockFree([string]$Name) {
    $locks = Join-Path $PSScriptRoot '../lib/Locks.psm1'
    $cmd = "Import-Module '$locks'; `$l = Enter-RinneLock $Name -TimeoutSeconds 0 -Quiet; if (`$l) { Exit-RinneLock `$l; exit 0 } else { exit 1 }"
    $r = Invoke-Native -Exe (Get-Process -Id $PID).Path -Arguments @('-NoProfile', '-NonInteractive', '-Command', $cmd)
    $r.Exit -eq 0
}

$script:Handlers = @{
    render = ${function:Invoke-RenderScenario}; probe = ${function:Invoke-ProbeScenario}; instances = ${function:Invoke-InstancesScenario}
    savereopen = ${function:Invoke-SaveReopenScenario}; hang = ${function:Invoke-HangScenario}
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

Export-ModuleMember -Function New-Outcome, Invoke-Native, New-ReaperContext, Get-ReaperScenarioKinds, Invoke-ReaperScenario, Invoke-ReaperLaunch, Get-HostFailure, Get-CompareVerdict
