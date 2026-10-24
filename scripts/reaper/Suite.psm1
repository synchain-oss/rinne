# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 烟测的编排：校验配置目录 → 预检 → 把本机构建的插件复制进私有 VST3 目录 → 逐轮跑场景与反例 → 前后核对默认资源目录。
#   Get-ReaperSmokeConfig / Test-ReaperScenarioConfig   读并校验 scenarios.json
#   Get-ReaperPreflight                                 缺什么就 SKIPPED，写明原因（doctor 同用）
#   Install-ReaperPlugins                               只复制本机构建（flavor 与这一段相符）的两个 VST3 到 <CfgDir>\plugins
#   Invoke-ReaperSmoke                                  整套；返回结果对象，Write-ReaperSmokeResult 写 json
# 两段：第一段（默认）测正式的 dev 构建，场景在 scenarios.json 顶层；第二段（-Probe）测探针构建，场景、信号与上限在 probe 一节。
# 第二段每轮另有一个结果目录 <LogDir>\<时刻>-probe-run<k>：probe-logs\<场景>（搬来的日志与比对行）、analyze-<场景>.json（每格的
# 分析器原始输出）、all-logs 与 analyze.json（整轮合在一起再过一次分析器）、docs-listing-before/after.json（文档目录的前后清单）。
# 结局词汇：PASS / FAIL / SKIPPED / INCOMPLETE。INCOMPLETE 不当 PASS。反例的 status 要同时满足两条才是 PASS：观察到的结局等于期望，
# 并且原因对得上（Detail 匹配 scenarios.json 里这条反例的 expectDetail）；没启动、没执行凑出来的同名结局不算（Get-NegativeVerdict）。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Scenarios.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot '../../tests/reaper/Rpp.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'ProbeLogs.psm1') -ErrorAction Stop

$script:Expectable = 'PASS', 'FAIL', 'SKIPPED', 'INCOMPLETE', 'REFUSED'
$script:ProbeKinds = 'probe-play', 'probe-export' # 只能写在 probe 一节；probe 一节也只能写它们
$script:ProbeDirShown = '<documents>\Rinne Probe Logs' # 结果里只写这个，不写文档目录的真实路径（含用户名）

# 用法错误（-Only 写错、没选中任何场景等）：和护栏的 REFUSED 一样不启动 REAPER、不写结果文件，入口按退出码 64 处理。
function Stop-Usage([string]$Reason) { throw [System.InvalidOperationException]::new("USAGE: $Reason") }

function Get-ReaperSmokeConfig([Parameter(Mandatory)][string]$Path) { Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }

# 属性不在返回 $null。按名字取属性对象：空对象（没有任何属性）上读 .Name 在 StrictMode 下会抛错。
function Get-Prop($Object, [string]$Name) { $prop = $null -eq $Object ? $null : $Object.PSObject.Properties[$Name]; $null -eq $prop ? $null : $prop.Value }

# 纯函数：scenarios.json 的结构问题。id 唯一（两段合起来）；kind 认识，且写在它那一段；每种 kind 的必填字段在；反例有认识的 expect，
# 还有一条合法正则的 expectDetail。probe 一节（第二段）另要 group（0..7）、limits 里的几项，场景的 case / variant 要在探针的规则表里。
function Test-ReaperScenarioConfig {
    param([Parameter(Mandatory)]$Config, [string[]]$Kinds = (Get-ReaperScenarioKinds))
    $p = [Collections.Generic.List[string]]::new()
    foreach ($k in 'schema', 'signal', 'limits', 'scenarios', 'negative') { if ($null -eq (Get-Prop $Config $k)) { $p.Add("缺顶层字段 $k") } }
    if ($p.Count) { return , $p.ToArray() }
    $need = @{
        render = 'sr', 'block'; probe = 'sr', 'block'; instances = @(); savereopen = @(); hang = @(); guard = 'guard'
        'probe-play' = 'sr', 'block', 'case', 'variant', 'recordSeconds', 'playSeconds', 'analyzerMustPass'; 'probe-export' = 'sr', 'block', 'case', 'recordSeconds', 'analyzerMustPass'
    }
    $seen = @{}
    $phase1 = @(@($Kinds | Where-Object { $_ -notin $script:ProbeKinds }) + 'guard')
    $phase2 = @($Kinds | Where-Object { $_ -in $script:ProbeKinds })
    $sections = [Collections.Generic.List[object]]::new()
    $sections.Add(@('', $Config, $phase1))
    $probeNode = Get-Prop $Config probe
    if ($null -ne $probeNode) {
        foreach ($k in 'signal', 'limits', 'group', 'scenarios', 'negative') { if ($null -eq (Get-Prop $probeNode $k)) { $p.Add("probe 一节缺字段 $k") } }
        if ($p.Count) { return , $p.ToArray() }
        if ("$($probeNode.group)" -notmatch '^[0-7]$') { $p.Add('probe.group 应为 0..7 的整数（两端同组才会配对）') }
        foreach ($k in 'luaTimeoutSec', 'leadSeconds', 'marginSeconds', 'minBlocks') { if ($null -eq (Get-Prop $probeNode.limits $k)) { $p.Add("probe.limits 缺 $k") } }
        $sections.Add(@('probe.', $probeNode, $phase2))
    }
    foreach ($sec in $sections) {
        $label = $sec[0]; $node = $sec[1]; $allowed = $sec[2]
        foreach ($group in 'scenarios', 'negative') {
            foreach ($s in $node.$group) {
                $id = Get-Prop $s id
                if (-not $id) { $p.Add("$label$group 里有条目没有 id"); continue }
                if ($seen.ContainsKey($id)) { $p.Add("id 重复：$id") }
                $seen[$id] = $true
                $kind = Get-Prop $s kind
                if ($kind -notin ($Kinds + 'guard')) { $p.Add("${id}：不认识的 kind $kind"); continue }
                if ($kind -notin $allowed) { $p.Add(($label ? "${id}：probe 一节只能写 $($phase2 -join ' / ')" : "${id}：$kind 只能写在 probe 一节")); continue }
                foreach ($f in $need[$kind]) { if ($null -eq (Get-Prop $s $f)) { $p.Add("${id}：缺字段 $f") } }
                if ($group -eq 'negative' -and (Get-Prop $s expect) -notin $script:Expectable) { $p.Add("${id}：expect 不是 $($script:Expectable -join '/')") }
                if ($group -eq 'negative') {
                    # 只比结局会被「没启动、没执行」空过（gui 锁等不到时看门狗反例也是 INCOMPLETE），所以每条反例都要写明结局里必须出现的原因
                    $why = [string](Get-Prop $s expectDetail)
                    if (-not $why) { $p.Add("${id}：缺 expectDetail（结局里必须出现的原因，正则）") }
                    else { try { $null = [regex]::new($why) } catch { $p.Add("${id}：expectDetail 不是合法的正则") } }
                }
                if ($group -eq 'scenarios' -and $kind -in 'hang', 'guard') { $p.Add("${id}：$kind 只能做反例") }
                if ($kind -in $script:ProbeKinds) { foreach ($x in (Test-ReaperProbeScenarioFields $s)) { $p.Add("${id}：$x") } }
            }
        }
    }
    , $p.ToArray()
}

# 探针场景的取值：case / variant 在探针的规则表里（导出格是 export-offline 与 export-rt 两个 variant）；记录窗口 1..3600 s（探针隐藏参数
# auto_record_s 的范围）；稳态格的播放时长短于记录窗口（停走带之后还要留出 Ping 的时间）。
function Test-ReaperProbeScenarioFields($ProbeScenario) {
    $bad = [Collections.Generic.List[string]]::new()
    $variants = ($ProbeScenario.kind -eq 'probe-export') ? @('export-offline', 'export-rt') : @([string](Get-Prop $ProbeScenario variant))
    foreach ($v in $variants) {
        try { $null = Get-ReaperProbeAutoIndex -Case ([string](Get-Prop $ProbeScenario case)) -Variant $v } catch { $bad.Add("($(Get-Prop $ProbeScenario case), $v) 不在探针的规则表里") }
    }
    $rec = Get-Prop $ProbeScenario recordSeconds
    if ("$rec" -notmatch '^[0-9]{1,4}$' -or [int]$rec -lt 1 -or [int]$rec -gt 3600) { $bad.Add('recordSeconds 应为 1..3600 的整数') }
    elseif ($ProbeScenario.kind -eq 'probe-play' -and [double](Get-Prop $ProbeScenario playSeconds) -ge [int]$rec) { $bad.Add('playSeconds 要短于 recordSeconds（停走带之后的 Ping 也要落在记录窗口里）') }
    $grp = Get-Prop $ProbeScenario group
    $must = @(Get-Prop $ProbeScenario analyzerMustPass)
    if (-not $must.Count -or @($must | Where-Object { "$_" -cnotmatch '^A[1-9]$' }).Count) { $bad.Add('analyzerMustPass 应为非空的 A1..A9 列表（计入这一格结局的分析器项）') }
    if ($null -ne $grp -and "$grp" -notmatch '^(-1|[0-7])$') { $bad.Add('group 应为 -1（不设组，反例用）或 0..7') }
    , $bad.ToArray()
}

# 构建产物里的 VST3 清单（<包>/Contents/Resources/moduleinfo.json）是插件类 ID 的真源。工程模板（Rpp.psm1）里的类 ID 是写死的：
# 两边对不上，链上就会是「效果不可用」的空槽，渲染照样逐位相同，会误判 PASS。所以预检读清单比对，不一致就给出原因（整套 SKIPPED）。
# 包目录本身不在的由调用方另报，这里不重复。
function Test-ReaperPluginClassIds {
    param([string[]]$Bundles = @(), [switch]$Probe)
    $why = [Collections.Generic.List[string]]::new()
    foreach ($e in (Get-ReaperPluginIdentities -Probe:$Probe)) {
        $dir = @($Bundles | Where-Object { (Split-Path -Leaf $_) -ieq $e.Bundle }) | Select-Object -First 1
        if (-not $dir) { $why.Add("构建清单里没有 $($e.Bundle)，没法核对它的类 ID"); continue }
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
        $manifest = Join-Path $dir 'Contents/Resources/moduleinfo.json'
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { $why.Add("$($e.Bundle) 里没有 moduleinfo.json，读不到类 ID"); continue }
        $cid = Get-ReaperAudioClassId -ModuleInfoText ([IO.File]::ReadAllText($manifest))
        if (-not $cid) { $why.Add("读不出 $($e.Bundle) 清单里 Audio Module Class 的类 ID") }
        elseif ($cid -cne $e.ClassId) { $why.Add("$($e.Bundle) 的类 ID 是 $cid，工程模板里写的是 $($e.ClassId)：插件身份变了，先更新 tests/reaper/Rpp.psm1") }
    }
    , $why.ToArray()
}

# 解释器与版本。没指定就找 py 启动器（它要 -3）。返回 @{ Exe; Lead（解释器名后面先带的参数）; Version; Why }。
function Resolve-ReaperPython([string]$Python = '') {
    $exe = $Python
    if (-not $exe) { $cmd = Get-Command py -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1; $exe = $cmd ? $cmd.Source : '' }
    $out = [pscustomobject]@{ Exe = $exe; Lead = @(); Version = ''; Why = '' }
    if (-not $exe -or -not (Test-Path -LiteralPath $exe -PathType Leaf)) { $out.Why = '找不到 Python 解释器（用 -Python 指定，或装 py 启动器）'; return $out }
    $out.Lead = ((Split-Path -Leaf $exe) -in 'py', 'py.exe') ? @('-3') : @()
    $r = try { Invoke-Native -Exe $exe -Arguments (@($out.Lead) + '--version') } catch { $null }
    $versionText = $r ? ("$($r.Out) $($r.Err)".Trim()) : ''
    if (-not $r -or $r.Exit -ne 0 -or $versionText -notmatch '^Python (3\.[0-9]+\.[0-9]+)') { $out.Why = 'Python 解释器跑不起来，或不是 Python 3'; return $out }
    $out.Version = $Matches[1]
    $out
}

# 缺什么就列出来；有原因就整套 SKIPPED，不启动 REAPER。USER_TESTING 标记在时不再看别的。
# Flavor：dev（第一段）或 probe（第二段）。构建的 flavor 必须与它相同，类 ID 按同一套身份比对：正式与探针的产物不能混用。
# 第二段另查：Python 解释器与版本、analyze-probe.py 在、用户文档目录取得到且探针日志目录列得出（ProbeLogDir 与 Analyzer 只给自测注入）。
function Get-ReaperPreflight {
    param([Parameter(Mandatory)][string]$ReaperExe, [Parameter(Mandatory)][string]$BuildDir, [string]$NullTest = '', [Parameter(Mandatory)][string]$UserTestingMarker,
          [ValidateSet('dev', 'probe')][string]$Flavor = 'dev', [string]$Python = '', [AllowNull()][object]$ProbeLogDir = $null, [string]$Analyzer = '')
    $why = [Collections.Generic.List[string]]::new()
    $out = [ordered]@{
        Reasons = $why; ReaperExe = $ReaperExe; NullTest = $NullTest; Bundles = @(); Sha7 = '0000000'; Flavor = ''; ReaperVersion = ''
        Python = ''; PythonLead = @(); PythonVersion = ''; Analyzer = ''; ProbeLogDir = ''
    }
    if (Test-Path -LiteralPath $UserTestingMarker) { $why.Add('USER_TESTING 标记存在：用户正在上机测试，不启动 REAPER'); return [pscustomobject]$out }
    if (Test-Path -LiteralPath $ReaperExe -PathType Leaf) { $out.ReaperVersion = (Get-Item -LiteralPath $ReaperExe).VersionInfo.ProductVersion } else { $why.Add('找不到 reaper.exe') }
    $info = Join-Path $BuildDir 'rinne-build.json'
    if (Test-Path -LiteralPath $info -PathType Leaf) {
        $b = try { Get-Content -LiteralPath $info -Raw | ConvertFrom-Json } catch { $null }
        if ($null -eq $b -or $b -isnot [pscustomobject]) { $why.Add('rinne-build.json 不是合法的 JSON 对象（重新用 gates 构建）'); $b = [pscustomobject]@{} }
        # sha 要先校验：它会拼进结果文件名，也用来证明测的是哪次构建。缺失、太短或不是十六进制，都不能默默往下走
        $sha = [string](Get-Prop $b sha)
        if ($sha -match '^[0-9a-fA-F]{7,40}$') { $out.Sha7 = $sha.Substring(0, 7).ToLowerInvariant() } else { $why.Add('rinne-build.json 里的 sha 缺失，或不是 7 到 40 位的十六进制（重新用 gates 构建）') }
        # 构建的 flavor 另起名字：PowerShell 的变量名不分大小写，写成 $flavor 就是改了参数 $Flavor（还带着它的 ValidateSet）
        $built = [string](Get-Prop $b flavor)
        $out.Flavor = $built
        if ($Flavor -eq 'dev' -and $built -ne 'dev') { $why.Add("构建的 flavor 是 $built，只测 dev（D 前缀）构建") }
        if ($Flavor -eq 'probe' -and $built -ne 'probe') { $why.Add("构建的 flavor 是 $built，第二段只测探针（P 前缀）构建") }
        $out.Bundles = @((Get-Prop $b bundles) | Where-Object { $_ } | ForEach-Object { [string]$_ })
        if ($out.Bundles.Count -ne 2) { $why.Add("构建清单里的 VST3 应为 2 个，实际 $($out.Bundles.Count) 个") }
        foreach ($d in $out.Bundles) { if (-not (Test-Path -LiteralPath $d -PathType Container)) { $why.Add("构建产物不在：$(Split-Path -Leaf $d)") } }
        foreach ($r in (Test-ReaperPluginClassIds -Bundles $out.Bundles -Probe:($Flavor -eq 'probe'))) { $why.Add($r) }
    } else { $why.Add('构建目录里没有 rinne-build.json（先用 gates 构建）') }
    if (-not $NullTest) { $out.NullTest = Join-Path $BuildDir 'tools-bin/Release/rinne_nulltest.exe' }
    if (-not (Test-Path -LiteralPath $out.NullTest -PathType Leaf)) { $why.Add('找不到 rinne_nulltest.exe') }
    if (@(Get-ReaperProcessIds).Count -gt 0) { $why.Add('本机已有 reaper.exe 在运行（不是本工具启动的）：不并行跑，也无法把默认资源目录的变动归因') }
    if ($Flavor -eq 'probe') {
        $py = Resolve-ReaperPython $Python
        if ($py.Why) { $why.Add($py.Why) } else { $out.Python = $py.Exe; $out.PythonLead = $py.Lead; $out.PythonVersion = $py.Version }
        $out.Analyzer = $Analyzer ? $Analyzer : [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../probe/analyze-probe.py')) # Analyzer 只给自测注入
        if (-not (Test-Path -LiteralPath $out.Analyzer -PathType Leaf)) { $why.Add('找不到 scripts/probe/analyze-probe.py') }
        $out.ProbeLogDir = ($null -ne $ProbeLogDir) ? [string]$ProbeLogDir : (Get-ReaperProbeLogDir) # 自测可以注入空串，造「取不到文档目录」
        if (-not $out.ProbeLogDir) { $why.Add('取不到用户文档目录：不知道探针把日志写在哪，收不了日志') }
        else { try { $null = Get-ReaperProbeLogListing -Dir $out.ProbeLogDir } catch { $why.Add('列不出探针日志目录的清单：前后核对做不了') } }
    }
    [pscustomobject]$out
}

# 复制到 <CfgDir>\plugins，内容相同就不动。返回每个文件的 sha256 前 12 位，写进结果里证明测的就是这次构建。
function Install-ReaperPlugins {
    param([Parameter(Mandatory)][string]$CfgDir, [Parameter(Mandatory)][string[]]$Bundles)
    $null = Get-ReaperCfgArgument
    $dest = Join-Path $CfgDir 'plugins'
    $null = New-Item -ItemType Directory -Force -Path $dest
    # 私有 VST3 目录要排在 vstpath64 的最前（REAPER 自己会补上系统目录）
    $iniText = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes((Join-Path $CfgDir 'reaper.ini')))
    if ($iniText -notmatch ('(?m)^vstpath64=' + [regex]::Escape($dest) + '(;|\r?$)')) { $null = Set-ReaperIniValue -Section REAPER -Key vstpath64 -Value $dest }
    $report = [Collections.Generic.List[object]]::new()
    foreach ($bundle in $Bundles) {
        $root = [IO.Path]::GetFullPath($bundle).TrimEnd('\') + '\'
        foreach ($f in Get-ChildItem -LiteralPath $bundle -Recurse -File) {
            $target = Join-Path $dest (Join-Path (Split-Path -Leaf $bundle) $f.FullName.Substring($root.Length))
            $new = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
            $old = (Test-Path -LiteralPath $target) ? (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash : ''
            if ($new -ne $old) {
                $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)
                Copy-Item -LiteralPath $f.FullName -Destination $target -Force
            }
            if ($f.Extension -eq '.vst3') { $report.Add([ordered]@{ file = $f.Name; sha256 = $new.Substring(0, 12).ToLowerInvariant(); copied = ($new -ne $old) }) }
        }
    }
    , $report.ToArray()
}

# 路径一律打码：结果 json 要能过隐私检查。
function Hide-LocalPaths([string]$Text) { $Text -replace '[A-Za-z]:[\\/][^\s"''；，。]*', '<path>' -replace '\\\\[^\s"'']*', '<path>' }

# 一组结局里最差的。空列表是「什么都没跑」，没有结论，不能当 PASS。
function Get-WorstStatus([string[]]$StatusList) {
    if ($null -eq $StatusList -or $StatusList.Count -eq 0) { return 'INCOMPLETE' }
    foreach ($s in 'FAIL', 'INCOMPLETE') { if ($s -in $StatusList) { return $s } }
    if ($StatusList.Count -gt 0 -and @($StatusList | Where-Object { $_ -ne 'SKIPPED' }).Count -eq 0) { return 'SKIPPED' }
    if ('SKIPPED' -in $StatusList) { return 'INCOMPLETE' }
    'PASS'
}

# 一轮的结局：各场景里最差的；默认资源目录变了、或结束后还留着 reaper.exe、或探针日志目录里别人的文件变了，一律 FAIL（不论场景多绿）。
function Get-RunVerdict {
    param([string[]]$RowStatuses, [bool]$CfgUnchanged, [int]$Residual, [bool]$DocsUnchanged = $true)
    if (-not $CfgUnchanged -or $Residual -gt 0 -or -not $DocsUnchanged) { return 'FAIL' }
    Get-WorstStatus $RowStatuses
}

# 纯函数：这一轮之后还接不接着跑。默认资源目录变了、探针日志目录里别人的文件变了，立刻停手，不再启动 REAPER；
# 分析器拒收真日志（日志与分析器的契约对不上）再跑也是一样，同样停下。返回停下的原因，接着跑返回空串。
function Get-ReaperRoundStop {
    param([bool]$CfgUnchanged, [bool]$DocsUnchanged = $true, [bool]$Rejected = $false)
    if (-not $CfgUnchanged) { return '默认资源目录变了' }
    if (-not $DocsUnchanged) { return '探针日志目录里别人的文件变了' }
    if ($Rejected) { return '分析器拒收真日志' }
    ''
}

# 守卫类反例：在进程内重走真正的入口，看它是否按预期拒绝 / 跳过，并且没有启动过 REAPER。
function Invoke-GuardNegative {
    param([Parameter(Mandatory)][hashtable]$Params, [Parameter(Mandatory)][string]$Guard, [Parameter(Mandatory)][string]$TempRoot)
    $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $made = [Collections.Generic.List[string]]::new()
    try {
        switch ($Guard) {
            'user-testing' {
                $marker = Join-Path $TempRoot "marker-$tag"
                Set-Content -LiteralPath $marker -Value x; $made.Add($marker)
                # $Params 里本来就带着调用方的 UserTestingMarker：克隆后覆盖成这里新建的标记再展开一次，
                # 不指望「显式实参盖过展开出来的同名键」这条绑定规则（与下面另两个守卫的写法一致）
                $p2 = $Params.Clone(); $p2.UserTestingMarker = $marker
                $r = Invoke-ReaperSmoke @p2 -Only '^null-offline-48000-512$' -NoNegative -Repeat 1
                if ($r.launches -ne 0) { return [pscustomobject]@{ Observed = 'FAIL'; Detail = "标记存在却启动了 $($r.launches) 次 REAPER" } }
                return [pscustomobject]@{ Observed = $r.verdict; Detail = "launches=$($r.launches)；$($r.skipReasons -join '；')" }
            }
            { $_ -in 'sentinel-missing', 'default-dir' } {
                $dir = Get-ReaperDefaultCfgDir
                if ($Guard -eq 'sentinel-missing') {
                    $dir = Join-Path $TempRoot "cfg-$tag"
                    $null = New-Item -ItemType Directory -Path $dir
                    $made.Add($dir); Set-Content -LiteralPath (Join-Path $dir 'reaper.ini') -Value '[REAPER]'; $made.Add((Join-Path $dir 'reaper.ini'))
                }
                $want = $Guard -eq 'sentinel-missing' ? '*哨兵*' : '*永不碰*'
                $p2 = $Params.Clone(); $p2.CfgDir = $dir
                try { $null = Invoke-ReaperSmoke @p2 -NoNegative -Repeat 1; return [pscustomobject]@{ Observed = 'FAIL'; Detail = '没有拒绝' } }
                catch {
                    if ($_.Exception.Message -like 'REFUSED:*' -and $_.Exception.Message -like $want) { return [pscustomobject]@{ Observed = 'REFUSED'; Detail = ($_.Exception.Message -replace '\s+', ' ') } }
                    return [pscustomobject]@{ Observed = 'FAIL'; Detail = "拒绝的理由不对：$($_.Exception.Message -replace '\s+', ' ')" }
                }
            }
            default { return [pscustomobject]@{ Observed = 'INCOMPLETE'; Detail = "不认识的 guard $Guard" } }
        }
    } finally {
        # 只删上面刚建的临时文件，再删空目录（不递归）
        foreach ($m in ($made | Sort-Object Length -Descending)) { if (Test-Path -LiteralPath $m) { Remove-Item -LiteralPath $m -ErrorAction SilentlyContinue } }
    }
}

# 纯函数：反例这一行的结局。只比结局不够：有的结局「没启动、没执行」也凑得出来（gui 锁等不到时 REAPER 没有启动，看门狗反例照样是 INCOMPLETE，
# 但看门狗根本没跑过）。所以要同时满足：观察到的结局等于期望，并且 Detail 里有这条反例要求的原因（expectDetail，区分大小写的正则）。
#   结局等于期望、原因对得上                 PASS
#   结局等于期望、原因对不上                 INCOMPLETE：反例没能验证（空过的就是这一种）
#   观察到 INCOMPLETE（没能执行）、期望不是它  INCOMPLETE：同样是没能验证，不判 FAIL
#   别的结局                                 FAIL
function Get-NegativeVerdict {
    param([Parameter(Mandatory)][string]$Expect, [Parameter(Mandatory)][string]$ExpectDetail, [Parameter(Mandatory)][string]$Observed, [string]$Detail = '')
    if ($Observed -ne $Expect) {
        if ($Observed -eq 'INCOMPLETE') { return [pscustomobject]@{ Status = 'INCOMPLETE'; Detail = "反例没能执行，没能验证（期望 $Expect）：$Detail" } }
        return [pscustomobject]@{ Status = 'FAIL'; Detail = $Detail }
    }
    if ($Detail -cnotmatch $ExpectDetail) { return [pscustomobject]@{ Status = 'INCOMPLETE'; Detail = "结局是 $Observed，但原因对不上（要求匹配 $ExpectDetail），反例没能验证：$Detail" } }
    [pscustomobject]@{ Status = 'PASS'; Detail = $Detail }
}

# 跑一个反例并给出结果里的这一行。守卫类在进程内重走真正的入口；其余走和正例相同的场景处理器。
function Invoke-ReaperNegativeRow {
    param([Parameter(Mandatory)]$Context, [Parameter(Mandatory)]$Scenario, [Parameter(Mandatory)][hashtable]$GuardParams, [Parameter(Mandatory)][string]$TempRoot)
    $t0 = [Diagnostics.Stopwatch]::StartNew()
    if ($Scenario.kind -eq 'guard') {
        $g = Invoke-GuardNegative -Params $GuardParams -Guard $Scenario.guard -TempRoot $TempRoot
        $seen = $g.Observed; $note = $g.Detail
    } else {
        $o = Invoke-ReaperScenario -Ctx $Context -Scenario $Scenario
        $seen = $o.Status; $note = $o.Detail
    }
    $v = Get-NegativeVerdict -Expect $Scenario.expect -ExpectDetail $Scenario.expectDetail -Observed $seen -Detail $note
    [ordered]@{
        id = $Scenario.id; kind = $Scenario.kind; negative = $true; expected = $Scenario.expect; observed = $seen; status = $v.Status
        seconds = [math]::Round($t0.Elapsed.TotalSeconds, 1); detail = (Hide-LocalPaths $v.Detail)
    }
}

# 纯函数：按 -Only（匹配场景 id 的正则）与 -NoNegative 选出这次要跑的正例与反例。-Only 不是合法正则、或者什么场景都没选中，
# 都是用法错误（Stop-Usage）：零个场景的一轮没有任何结论，不能落成 PASS。
function Select-ReaperScenarios {
    param([Parameter(Mandatory)]$Config, [string]$Only = '', [switch]$NoNegative)
    if ($Only) { try { $null = [regex]::new($Only) } catch { Stop-Usage "-Only 不是合法的正则：$($_.Exception.Message)" } }
    $positive = @($Config.scenarios | Where-Object { -not $Only -or $_.id -match $Only })
    $negative = $NoNegative ? @() : @($Config.negative | Where-Object { -not $Only -or $_.id -match $Only })
    if ($positive.Count + $negative.Count -eq 0) {
        Stop-Usage ("-Only '{0}' 没有选中任何场景{1}（场景 id 见 tests/reaper/scenarios.json）" -f $Only, ($NoNegative ? '（-NoNegative 时反例不跑）' : ''))
    }
    [pscustomobject]@{ Positive = $positive; Negative = $negative }
}

function Write-ReaperJson([string]$Path, $Data) { [IO.File]::WriteAllText($Path, ($Data | ConvertTo-Json -Depth 12) + "`n", [Text.UTF8Encoding]::new($false)) }

# 第二段一轮的上下文：探针日志在哪、本轮结果放哪、组、分析器。搬走的、自己的、出的错都记在这里，轮末汇总。
function New-ReaperProbeRound($Preflight, $ProbeSection, [string]$ResultDir) {
    @{
        LogDir = $Preflight.ProbeLogDir; RoundDir = $ResultDir; Group = [int]$ProbeSection.group; Python = $Preflight.Python; Analyzer = $Preflight.Analyzer
        Redact = @(@($env:USERNAME, $env:COMPUTERNAME) | Where-Object { $_ }); Own = [Collections.Generic.List[string]]::new(); Moved = 0
        Problems = [Collections.Generic.List[string]]::new(); Analyses = [ordered]@{}; Rejected = $false
    }
}

# 整轮合在一起再过一次分析器：把各格搬来的日志与比对行复制进 all-logs（文件名各不相同：pid 与 tok4 不同），原始输出写 analyze.json。
# 各格的结局只看各自那一次分析；这一次只作记录（几格同键时分析器会按同键重复处理，见 REAPER.md），唯一例外是拒收（退出码 2）。
function Invoke-ReaperProbeWhole($Probe) {
    $cells = Join-Path $Probe.RoundDir 'probe-logs'
    $all = Join-Path $Probe.RoundDir 'all-logs'
    $files = @(if (Test-Path -LiteralPath $cells) { Get-ChildItem -LiteralPath $cells -Recurse -File | Where-Object { $_.Name -like 'probe-*.csv' -or $_.Name -like 'compare-*.csv' } })
    if (-not $files.Count) { return [ordered]@{ exit = $null; summary = '没有日志可分析' } }
    $null = New-Item -ItemType Directory -Force -Path $all
    foreach ($f in $files) {
        $copyName = $f.Name -like 'compare-*' ? "compare-$($f.Directory.Name)-$($f.Name.Substring(8))" : $f.Name
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $all $copyName)
    }
    $a = Invoke-ReaperProbeAnalyzer -Python $Probe.Python -Script $Probe.Analyzer -Dir $all -Redact $Probe.Redact
    [IO.File]::WriteAllText((Join-Path $Probe.RoundDir 'analyze.json'), $a.Out, [Text.UTF8Encoding]::new($false))
    $sum = Get-ReaperAnalyzerSummary $a
    if ($sum.Rejected) { $Probe.Rejected = $true }
    [ordered]@{ exit = $a.Exit; summary = $sum.Text }
}

function Invoke-ReaperSmoke {
    param(
        [string]$CfgDir = '', [Parameter(Mandatory)][string]$ReaperExe, [Parameter(Mandatory)][string]$BuildDir, [string]$NullTest = '',
        [Parameter(Mandatory)][string]$LogDir, [Parameter(Mandatory)][string]$UserTestingMarker, [Parameter(Mandatory)][string]$ScenarioFile,
        [string]$Only = '', [int]$Repeat = 1, [switch]$NoNegative, [switch]$Probe, [string]$Python = ''
    )
    $cfg = Initialize-ReaperCfg $CfgDir # 不满足就抛 REFUSED，什么都还没动
    $scenarioConfig = Get-ReaperSmokeConfig $ScenarioFile
    $bad = Test-ReaperScenarioConfig $scenarioConfig
    if ($bad.Count) { throw "scenarios.json 有问题：$($bad -join '；')" }
    if ($Repeat -lt 1) { Stop-Usage '-Repeat 至少是 1' }
    $section = $scenarioConfig
    if ($Probe) {
        $section = Get-Prop $scenarioConfig probe
        if ($null -eq $section) { Stop-Usage 'scenarios.json 里没有 probe 一节，-Probe 没有场景可跑' }
    }
    $picked = Select-ReaperScenarios -Config $section -Only $Only -NoNegative:$NoNegative # 选不出场景就是用法错误：什么都还没启动
    $pre = Get-ReaperPreflight -ReaperExe $ReaperExe -BuildDir $BuildDir -NullTest $NullTest -UserTestingMarker $UserTestingMarker -Flavor ($Probe ? 'probe' : 'dev') -Python $Python
    $result = [ordered]@{
        schema = 1; phase = ($Probe ? 2 : 1); startedUtc = [DateTime]::UtcNow.ToString('o'); build = [ordered]@{ sha7 = $pre.Sha7; flavor = $pre.Flavor }
        reaper = $pre.ReaperVersion; verdict = 'SKIPPED'; launches = 0; skipReasons = @($pre.Reasons | ForEach-Object { Hide-LocalPaths $_ })
        plugins = @(); runs = @()
    }
    if ($Probe) { $result.python = $pre.PythonVersion }
    if ($pre.Reasons.Count) { return [pscustomobject]$result }

    $result.plugins = Install-ReaperPlugins -CfgDir $cfg.Dir -Bundles $pre.Bundles
    $stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
    $positive = $picked.Positive
    $negative = $picked.Negative
    $driver = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../tests/reaper/lua/driver.lua'))
    $launches = 0
    for ($k = 1; $k -le $Repeat; $k++) {
        $work = Join-Path $LogDir "work/$stamp-run$k"
        $null = New-Item -ItemType Directory -Force -Path $work
        $before = Get-ReaperDefaultCfgSnapshot
        $ctx = New-ReaperContext -ReaperExe $ReaperExe -NullTest $pre.NullTest -WorkDir $work -Driver $driver -Config $section
        if ($Probe) {
            $roundDir = Join-Path $LogDir "$stamp-probe-run$k"
            $null = New-Item -ItemType Directory -Force -Path $roundDir
            $ctx.Probe = New-ReaperProbeRound $pre $section $roundDir
            $docsBefore = Get-ReaperProbeLogListing -Dir $pre.ProbeLogDir # 列不出就抛错：看不见不能当作没变
            Write-ReaperJson (Join-Path $roundDir 'docs-listing-before.json') $docsBefore
        }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $rows = [Collections.Generic.List[object]]::new()
        foreach ($s in $positive) {
            $o = Invoke-ReaperScenario -Ctx $ctx -Scenario $s
            $row = [ordered]@{ id = $s.id; kind = $s.kind; status = $o.Status; seconds = $o.Seconds; detail = (Hide-LocalPaths $o.Detail) }
            if ($o.PSObject.Properties.Name -contains 'Facts') { $row.facts = $o.Facts }
            $rows.Add($row)
            Write-Host ("  [{0}] {1}  {2} s  {3}" -f $o.Status, $s.id, $o.Seconds, (Hide-LocalPaths $o.Detail))
        }
        $guardSet = @{ CfgDir = $CfgDir; ReaperExe = $ReaperExe; BuildDir = $BuildDir; NullTest = $NullTest; LogDir = $LogDir; UserTestingMarker = $UserTestingMarker; ScenarioFile = $ScenarioFile }
        foreach ($s in $negative) {
            $row = Invoke-ReaperNegativeRow -Context $ctx -Scenario $s -GuardParams $guardSet -TempRoot $work
            $rows.Add($row)
            Write-Host ("  [{0}] {1}  期望 {2}，观察到 {3}  {4}" -f $row.status, $row.id, $row.expected, $row.observed, $row.detail)
        }
        $launches += $ctx.Launches
        $docsSame = $true
        if ($Probe) {
            $whole = Invoke-ReaperProbeWhole $ctx.Probe
            $docsAfter = Get-ReaperProbeLogListing -Dir $pre.ProbeLogDir
            Write-ReaperJson (Join-Path $roundDir 'docs-listing-after.json') $docsAfter
            $docs = Compare-ReaperProbeLogListing -Before $docsBefore -After $docsAfter -Own $ctx.Probe.Own.ToArray()
            $docsSame = $docs.Same
        }
        # 全绿就删掉这一轮的临时文件（逐个删，不递归）；有红的留着供查
        if (@($rows | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) {
            foreach ($f in $ctx.Files) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue } }
        }
        if (-not (Get-ChildItem -LiteralPath $work -Force)) { Remove-Item -LiteralPath $work -ErrorAction SilentlyContinue }
        $after = Get-ReaperDefaultCfgSnapshot
        $cmp = Compare-ReaperDefaultCfgSnapshot -Before $before -After $after
        $strayCount = @(Get-ReaperProcessIds).Count
        $statuses = @($rows | ForEach-Object { $_.status })
        $verdict = Get-RunVerdict -RowStatuses $statuses -CfgUnchanged $cmp.Same -Residual $strayCount -DocsUnchanged $docsSame
        $runRow = [ordered]@{
            index = $k; seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); verdict = $verdict; launches = $ctx.Launches
            counts = [ordered]@{ PASS = @($statuses | Where-Object { $_ -eq 'PASS' }).Count; FAIL = @($statuses | Where-Object { $_ -eq 'FAIL' }).Count; INCOMPLETE = @($statuses | Where-Object { $_ -eq 'INCOMPLETE' }).Count; SKIPPED = @($statuses | Where-Object { $_ -eq 'SKIPPED' }).Count }
            defaultCfgDir = [ordered]@{ unchanged = $cmp.Same; entries = $cmp.Count; changed = @($cmp.Changed | Select-Object -First 8) }
            reaperProcessesLeft = $strayCount
            scenarios = $rows.ToArray()
        }
        if ($Probe) {
            $runRow.probe = [ordered]@{
                roundDir = (Split-Path -Leaf $roundDir); logDir = $script:ProbeDirShown; moved = $ctx.Probe.Moved; own = @($ctx.Probe.Own)
                problems = @($ctx.Probe.Problems | ForEach-Object { Hide-LocalPaths $_ })
                docsListing = [ordered]@{ unchanged = $docs.Same; entriesBefore = $docs.Count; changed = @($docs.Changed | Select-Object -First 8) }
                analyzer = [ordered]@{ cells = [ordered]@{}; whole = $whole; rejected = $ctx.Probe.Rejected }
            }
            foreach ($id in $ctx.Probe.Analyses.Keys) { $runRow.probe.analyzer.cells[$id] = $ctx.Probe.Analyses[$id].exit }
        }
        $result.runs += $runRow
        Write-Host ("第 {0}/{1} 轮：{2}，{3} s，REAPER 启动 {4} 次，默认资源目录{5}" -f $k, $Repeat, $verdict, $result.runs[-1].seconds, $ctx.Launches, ($cmp.Same ? '未变' : "变了：$($cmp.Changed -join ', ')"))
        if ($Probe) {
            Write-Host ("  探针日志：搬走 {0} 份，文档目录除本轮自己的文件外{1}；整轮分析 {2}" -f $ctx.Probe.Moved, ($docs.Same ? '未变' : "变了：$($docs.Changed -join ', ')"), $whole.summary)
        }
        $stop = Get-ReaperRoundStop -CfgUnchanged $cmp.Same -DocsUnchanged $docsSame -Rejected ($Probe -and $ctx.Probe.Rejected)
        if ($stop) { $runRow.stoppedBecause = $stop; Write-Host "  停止后续轮次：$stop"; break }
    }
    $result.launches = $launches
    $result.verdict = Get-WorstStatus @($result.runs | ForEach-Object { $_.verdict })
    [pscustomobject]$result
}

function Write-ReaperSmokeResult {
    param([Parameter(Mandatory)]$Data, [Parameter(Mandatory)][string]$LogDir)
    $null = New-Item -ItemType Directory -Force -Path $LogDir
    $tail = ($Data.PSObject.Properties.Name -contains 'phase' -and $Data.phase -eq 2) ? '-probe' : ''
    $jsonPath = Join-Path $LogDir ('{0}-{1}{2}.json' -f [DateTime]::Now.ToString('yyyyMMdd-HHmmss'), $Data.build.sha7, $tail)
    [IO.File]::WriteAllText($jsonPath, ($Data | ConvertTo-Json -Depth 12) + "`n", [Text.UTF8Encoding]::new($false))
    $jsonPath
}

Export-ModuleMember -Function Get-ReaperSmokeConfig, Test-ReaperScenarioConfig, Test-ReaperPluginClassIds, Get-ReaperPreflight, Install-ReaperPlugins, Hide-LocalPaths, Select-ReaperScenarios,
    Resolve-ReaperPython, Test-ReaperProbeScenarioFields, Get-ReaperRoundStop,
    Get-WorstStatus, Get-RunVerdict, Get-NegativeVerdict, Invoke-GuardNegative, Invoke-ReaperNegativeRow, Invoke-ReaperSmoke, Write-ReaperSmokeResult
