# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 烟测的编排：校验配置目录 → 预检 → 把本机构建的插件复制进私有 VST3 目录 → 逐轮跑场景与反例 → 前后核对默认资源目录。
#   Get-ReaperSmokeConfig / Test-ReaperScenarioConfig   读并校验 scenarios.json
#   Get-ReaperPreflight                                 缺什么就 SKIPPED，写明原因（doctor 同用）
#   Install-ReaperPlugins                               只复制本机构建（flavor 为 dev）的两个 VST3 到 <CfgDir>\plugins
#   Invoke-ReaperSmoke                                  整套；返回结果对象，Write-ReaperSmokeResult 写 json
# 结局词汇：PASS / FAIL / SKIPPED / INCOMPLETE。INCOMPLETE 不当 PASS。反例的 status 是「观察到的结局等于期望」才是 PASS。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Guard.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Host.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Scenarios.psm1') -ErrorAction Stop

$script:Expectable = 'PASS', 'FAIL', 'SKIPPED', 'INCOMPLETE', 'REFUSED'

function Get-ReaperSmokeConfig([Parameter(Mandatory)][string]$Path) { Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }

function Get-Prop($Object, [string]$Name) { $Object.PSObject.Properties.Name -contains $Name ? $Object.$Name : $null }

# 纯函数：scenarios.json 的结构问题。id 唯一；kind 认识；每种 kind 的必填字段在；反例有认识的 expect。
function Test-ReaperScenarioConfig {
    param([Parameter(Mandatory)]$Config, [string[]]$Kinds = (Get-ReaperScenarioKinds))
    $p = [Collections.Generic.List[string]]::new()
    foreach ($k in 'schema', 'signal', 'limits', 'scenarios', 'negative') { if ($null -eq (Get-Prop $Config $k)) { $p.Add("缺顶层字段 $k") } }
    if ($p.Count) { return , $p.ToArray() }
    $need = @{ render = 'sr', 'block'; probe = 'sr', 'block'; instances = @(); savereopen = @(); hang = @(); guard = 'guard' }
    $seen = @{}
    foreach ($group in 'scenarios', 'negative') {
        foreach ($s in $Config.$group) {
            $id = Get-Prop $s id
            if (-not $id) { $p.Add("$group 里有条目没有 id"); continue }
            if ($seen.ContainsKey($id)) { $p.Add("id 重复：$id") }
            $seen[$id] = $true
            $kind = Get-Prop $s kind
            if ($kind -notin ($Kinds + 'guard')) { $p.Add("${id}：不认识的 kind $kind"); continue }
            foreach ($f in $need[$kind]) { if ($null -eq (Get-Prop $s $f)) { $p.Add("${id}：缺字段 $f") } }
            if ($group -eq 'negative' -and (Get-Prop $s expect) -notin $script:Expectable) { $p.Add("${id}：expect 不是 $($script:Expectable -join '/')") }
            if ($group -eq 'scenarios' -and $kind -in 'hang', 'guard') { $p.Add("${id}：$kind 只能做反例") }
        }
    }
    , $p.ToArray()
}

# 缺什么就列出来；有原因就整套 SKIPPED，不启动 REAPER。USER_TESTING 标记在时不再看别的。
function Get-ReaperPreflight {
    param([Parameter(Mandatory)][string]$ReaperExe, [Parameter(Mandatory)][string]$BuildDir, [string]$NullTest = '', [Parameter(Mandatory)][string]$UserTestingMarker)
    $why = [Collections.Generic.List[string]]::new()
    $out = [ordered]@{ Reasons = $why; ReaperExe = $ReaperExe; NullTest = $NullTest; Bundles = @(); Sha7 = '0000000'; Flavor = ''; ReaperVersion = '' }
    if (Test-Path -LiteralPath $UserTestingMarker) { $why.Add('USER_TESTING 标记存在：用户正在上机测试，不启动 REAPER'); return [pscustomobject]$out }
    if (Test-Path -LiteralPath $ReaperExe -PathType Leaf) { $out.ReaperVersion = (Get-Item -LiteralPath $ReaperExe).VersionInfo.ProductVersion } else { $why.Add('找不到 reaper.exe') }
    $info = Join-Path $BuildDir 'rinne-build.json'
    if (Test-Path -LiteralPath $info -PathType Leaf) {
        $b = Get-Content -LiteralPath $info -Raw | ConvertFrom-Json
        $out.Sha7 = ([string]$b.sha).Substring(0, 7); $out.Flavor = [string]$b.flavor
        if ($b.flavor -ne 'dev') { $why.Add("构建的 flavor 是 $($b.flavor)，只测 dev（D 前缀）构建") }
        $out.Bundles = @($b.bundles | ForEach-Object { [string]$_ })
        if ($out.Bundles.Count -ne 2) { $why.Add("构建清单里的 VST3 应为 2 个，实际 $($out.Bundles.Count) 个") }
        foreach ($d in $out.Bundles) { if (-not (Test-Path -LiteralPath $d -PathType Container)) { $why.Add("构建产物不在：$(Split-Path -Leaf $d)") } }
    } else { $why.Add('构建目录里没有 rinne-build.json（先用 gates 构建）') }
    if (-not $NullTest) { $out.NullTest = Join-Path $BuildDir 'tools-bin/Release/rinne_nulltest.exe' }
    if (-not (Test-Path -LiteralPath $out.NullTest -PathType Leaf)) { $why.Add('找不到 rinne_nulltest.exe') }
    if (@(Get-ReaperProcessIds).Count -gt 0) { $why.Add('本机已有 reaper.exe 在运行（不是本工具启动的）：不并行跑，也无法把默认资源目录的变动归因') }
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
    if ($iniText -notmatch ('(?m)^vstpath64=' + [regex]::Escape($dest) + '(;|?$)')) { $null = Set-ReaperIniValue -Section REAPER -Key vstpath64 -Value $dest }
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

function Get-WorstStatus([string[]]$StatusList) {
    foreach ($s in 'FAIL', 'INCOMPLETE') { if ($s -in $StatusList) { return $s } }
    if ($StatusList.Count -gt 0 -and @($StatusList | Where-Object { $_ -ne 'SKIPPED' }).Count -eq 0) { return 'SKIPPED' }
    if ('SKIPPED' -in $StatusList) { return 'INCOMPLETE' }
    'PASS'
}

# 一轮的结局：各场景里最差的；默认资源目录变了、或结束后还留着 reaper.exe，一律 FAIL（不论场景多绿）。
function Get-RunVerdict {
    param([string[]]$RowStatuses, [bool]$CfgUnchanged, [int]$Residual)
    if (-not $CfgUnchanged -or $Residual -gt 0) { return 'FAIL' }
    Get-WorstStatus $RowStatuses
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
                $r = Invoke-ReaperSmoke @Params -UserTestingMarker $marker -Only '^null-offline-48000-512$' -NoNegative -Repeat 1
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

function Invoke-ReaperSmoke {
    param(
        [string]$CfgDir = '', [Parameter(Mandatory)][string]$ReaperExe, [Parameter(Mandatory)][string]$BuildDir, [string]$NullTest = '',
        [Parameter(Mandatory)][string]$LogDir, [Parameter(Mandatory)][string]$UserTestingMarker, [Parameter(Mandatory)][string]$ScenarioFile,
        [string]$Only = '', [int]$Repeat = 1, [switch]$NoNegative
    )
    $cfg = Initialize-ReaperCfg $CfgDir # 不满足就抛 REFUSED，什么都还没动
    $scenarioConfig = Get-ReaperSmokeConfig $ScenarioFile
    $bad = Test-ReaperScenarioConfig $scenarioConfig
    if ($bad.Count) { throw "scenarios.json 有问题：$($bad -join '；')" }
    $pre = Get-ReaperPreflight -ReaperExe $ReaperExe -BuildDir $BuildDir -NullTest $NullTest -UserTestingMarker $UserTestingMarker
    $result = [ordered]@{
        schema = 1; startedUtc = [DateTime]::UtcNow.ToString('o'); build = [ordered]@{ sha7 = $pre.Sha7; flavor = $pre.Flavor }
        reaper = $pre.ReaperVersion; verdict = 'SKIPPED'; launches = 0; skipReasons = @($pre.Reasons | ForEach-Object { Hide-LocalPaths $_ })
        plugins = @(); runs = @()
    }
    if ($pre.Reasons.Count) { return [pscustomobject]$result }

    $result.plugins = Install-ReaperPlugins -CfgDir $cfg.Dir -Bundles $pre.Bundles
    $stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
    $positive = @($scenarioConfig.scenarios | Where-Object { -not $Only -or $_.id -match $Only })
    $negative = $NoNegative ? @() : @($scenarioConfig.negative | Where-Object { -not $Only -or $_.id -match $Only })
    $driver = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../tests/reaper/lua/driver.lua'))
    $launches = 0
    for ($k = 1; $k -le $Repeat; $k++) {
        $work = Join-Path $LogDir "work/$stamp-run$k"
        $null = New-Item -ItemType Directory -Force -Path $work
        $before = Get-ReaperDefaultCfgSnapshot
        $ctx = New-ReaperContext -ReaperExe $ReaperExe -NullTest $pre.NullTest -WorkDir $work -Driver $driver -Config $scenarioConfig
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $rows = [Collections.Generic.List[object]]::new()
        foreach ($s in $positive) {
            $o = Invoke-ReaperScenario -Ctx $ctx -Scenario $s
            $rows.Add([ordered]@{ id = $s.id; kind = $s.kind; status = $o.Status; seconds = $o.Seconds; detail = (Hide-LocalPaths $o.Detail) })
            Write-Host ("  [{0}] {1}  {2} s  {3}" -f $o.Status, $s.id, $o.Seconds, (Hide-LocalPaths $o.Detail))
        }
        foreach ($s in $negative) {
            $t0 = [Diagnostics.Stopwatch]::StartNew()
            if ($s.kind -eq 'guard') {
                $p = @{ CfgDir = $CfgDir; ReaperExe = $ReaperExe; BuildDir = $BuildDir; NullTest = $NullTest; LogDir = $LogDir; UserTestingMarker = $UserTestingMarker; ScenarioFile = $ScenarioFile }
                $g = Invoke-GuardNegative -Params $p -Guard $s.guard -TempRoot $work
                $observed = $g.Observed; $detail = $g.Detail
            } else {
                $o = Invoke-ReaperScenario -Ctx $ctx -Scenario $s
                $observed = $o.Status; $detail = $o.Detail
            }
            $status = $observed -eq $s.expect ? 'PASS' : 'FAIL'
            $rows.Add([ordered]@{ id = $s.id; kind = $s.kind; negative = $true; expected = $s.expect; observed = $observed; status = $status; seconds = [math]::Round($t0.Elapsed.TotalSeconds, 1); detail = (Hide-LocalPaths $detail) })
            Write-Host ("  [{0}] {1}  期望 {2}，观察到 {3}  {4}" -f $status, $s.id, $s.expect, $observed, (Hide-LocalPaths $detail))
        }
        $launches += $ctx.Launches
        # 全绿就删掉这一轮的临时文件（逐个删，不递归）；有红的留着供查
        if (@($rows | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) {
            foreach ($f in $ctx.Files) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue } }
        }
        if (-not (Get-ChildItem -LiteralPath $work -Force)) { Remove-Item -LiteralPath $work -ErrorAction SilentlyContinue }
        $after = Get-ReaperDefaultCfgSnapshot
        $cmp = Compare-ReaperDefaultCfgSnapshot -Before $before -After $after
        $strayCount = @(Get-ReaperProcessIds).Count
        $statuses = @($rows | ForEach-Object { $_.status })
        $verdict = Get-RunVerdict -RowStatuses $statuses -CfgUnchanged $cmp.Same -Residual $strayCount
        $result.runs += [ordered]@{
            index = $k; seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); verdict = $verdict; launches = $ctx.Launches
            counts = [ordered]@{ PASS = @($statuses | Where-Object { $_ -eq 'PASS' }).Count; FAIL = @($statuses | Where-Object { $_ -eq 'FAIL' }).Count; INCOMPLETE = @($statuses | Where-Object { $_ -eq 'INCOMPLETE' }).Count; SKIPPED = @($statuses | Where-Object { $_ -eq 'SKIPPED' }).Count }
            defaultCfgDir = [ordered]@{ unchanged = $cmp.Same; entries = $cmp.Count; changed = @($cmp.Changed | Select-Object -First 8) }
            reaperProcessesLeft = $strayCount
            scenarios = $rows.ToArray()
        }
        Write-Host ("第 {0}/{1} 轮：{2}，{3} s，REAPER 启动 {4} 次，默认资源目录{5}" -f $k, $Repeat, $verdict, $result.runs[-1].seconds, $ctx.Launches, ($cmp.Same ? '未变' : "变了：$($cmp.Changed -join ', ')"))
        if (-not $cmp.Same) { break } # 默认资源目录变了：立刻停手，不再启动 REAPER
    }
    $result.launches = $launches
    $result.verdict = Get-WorstStatus @($result.runs | ForEach-Object { $_.verdict })
    [pscustomobject]$result
}

function Write-ReaperSmokeResult {
    param([Parameter(Mandatory)]$Data, [Parameter(Mandatory)][string]$LogDir)
    $null = New-Item -ItemType Directory -Force -Path $LogDir
    $jsonPath = Join-Path $LogDir ('{0}-{1}.json' -f [DateTime]::Now.ToString('yyyyMMdd-HHmmss'), $Data.build.sha7)
    [IO.File]::WriteAllText($jsonPath, ($Data | ConvertTo-Json -Depth 10) + "`n", [Text.UTF8Encoding]::new($false))
    $jsonPath
}

Export-ModuleMember -Function Get-ReaperSmokeConfig, Test-ReaperScenarioConfig, Get-ReaperPreflight, Install-ReaperPlugins, Hide-LocalPaths,
    Get-WorstStatus, Get-RunVerdict, Invoke-ReaperSmoke, Write-ReaperSmokeResult
