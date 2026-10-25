# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 工程文本与 Lua 任务文件的生成器。工程是手写的最小 .rpp：一条轨道、一个测试信号 item、按顺序排好的插件。
# 插件块里的状态是 REAPER 自己存出的默认状态（缺了它 REAPER 报「效果不可用」）。块头里的类标识、显示名和默认状态都是写死的
# 值：插件身份或 state 头变了要在这里手动更新。类标识在预检里与构建产物的 VST3 清单（moduleinfo.json）比对，对不上整套 SKIPPED。
#   New-ReaperProjectText   工程文本
#   ConvertTo-ReaperLuaTable 把哈希表写成 Lua 的 return { … }，Lua 驱动用 dofile 读
#   Write-ReaperText        无 BOM 的 UTF-8 写文件
#   Get-ReaperPluginIdentities / Get-ReaperAudioClassId   工程模板里写的插件包名与类 ID；从 moduleinfo.json 文本取 Audio Module Class 的类 ID
#   Get-ReaperProbeStateLines  探针插件的 VST 块状态：REAPER 自己存出的默认状态里把探针 state 的组字节换成指定的组
#   Get-ReaperProbeAutoIndex   隐藏参数 auto_case / auto_variant 的编号（按探针规则表的首现顺序）与两个参数的上限
# 正式（dev）与探针是两套插件身份：两套块头、两套默认状态，工程按 -Probe 选一套，预检按构建的 flavor 选一套比对类 ID，不能混用。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 两个插件的 VST 块头（名字是 REAPER 扫描后给的显示名）与默认状态。
$script:Plugins = @{
    input  = @{
        Head  = '<VST "VST3: Rinne Input (Synchain)" "Rinne Input.vst3" 0 "" 1769505170{ABCDEF019182FAEB536E6368526E6E69} ""'
        State = 'koF4ae5e7f4CAAAAAQAAAAAAAAACAAAAAAAAAAIAAAABAAAAAAAAAAIAAAAAAAAATAAAAAEAAAD//wAA'
    }
    output = @{
        Head  = '<VST "VST3: Rinne Output (Synchain)" "Rinne Output.vst3" 0 "" 1668839456{ABCDEF019182FAEB536E6368526E6E6F} ""'
        State = 'IHh4Y+5e7f4CAAAAAQAAAAAAAAACAAAAAAAAAAIAAAABAAAAAAAAAAIAAAAAAAAATAAAAAEAAAD//wAA'
    }
}
$script:StateTail = @(
    'PAAAAAEAAAAAAAAAAAAAAEpVQ0VQcml2YXRlRGF0YQABAUJ5cGFzcwABAQMAHQAAAAAAAABKVUNFUHJpdmF0ZURhdGEAAAAAAAAAAA==',
    'AAAAAAAA'
)

# 探针构建的两个插件（PLUGIN_CODE 不同，类 ID、包名、显示名都不同）。块头与状态取自 REAPER 自己存出的工程：第一行是 REAPER 的
# 插件头（含后面状态的总长），其后是状态（长度 4 字节、一个 1、JUCE 包装的组件状态、8 个 0），最后一行与正式版相同。组件状态的开头
# 就是探针 state：'RNST'、abi 0、role、1 块、'PRBE'、16，然后是组（0xFF 未设）、3 字节 0、feedback、D（ms）、ceiling（dB）各一个 f32。
# 这里存的是组未设、feedback 0、D 100 ms、ceiling -18 dB 的默认值；Get-ReaperProbeStateLines 只换组字节，别的一个字节都不动。
$script:ProbePlugins = @{
    input  = @{
        Head   = '<VST "VST3: Rinne Probe Input (Synchain)" "Rinne Probe Input.vst3" 0 "" 2103380264{ABCDEF019182FAEB536E6368526E7069} ""'
        Header = 'KAlffe5e7f4CAAAAAQAAAAAAAAACAAAAAAAAAAIAAAABAAAAAAAAAAIAAAAAAAAAdAAAAAEAAAD//wAA'
        State  = 'ZAAAAAEAAABSTlNUAAAAAAEAAAABAAAAUFJCRRAAAAD/AAAAAAAAAAAAyEIAAJDBAAAAAAAAAABKVUNFUHJpdmF0ZURhdGEAAQFCeXBhc3MAAQEDAB0AAAAAAAAASlVDRVByaXZhdGVEYXRhAAAAAAAAAAA='
        Role   = 1
    }
    output = @{
        Head   = '<VST "VST3: Rinne Probe Output (Synchain)" "Rinne Probe Output.vst3" 0 "" 56562330{ABCDEF019182FAEB536E6368526E706F} ""'
        Header = 'mhJfA+5e7f4CAAAAAQAAAAAAAAACAAAAAAAAAAIAAAABAAAAAAAAAAIAAAAAAAAAdAAAAAEAAAD//wAA'
        State  = 'ZAAAAAEAAABSTlNUAAAAAAIAAAABAAAAUFJCRRAAAAD/AAAAAAAAAAAAyEIAAJDBAAAAAAAAAABKVUNFUHJpdmF0ZURhdGEAAQFCeXBhc3MAAQEDAB0AAAAAAAAASlVDRVByaXZhdGVEYXRhAAAAAAAAAAA='
        Role   = 2
    }
}
$script:ProbeStateAt = 8 # 状态里探针 state 的起点（长度 4 字节与那个 1 之后）

# 探针插件块里的状态行：组（-1 未设，0..7 即 A..H）写进探针 state 的组字节，其余原样；按 REAPER 的写法每行 128 个 base64 字符。
# 模板对不上探针 state 的布局（魔数、abi、role、块头、feedback 不是 0）就抛错，不凑合着写。
function Get-ReaperProbeStateLines {
    param([Parameter(Mandatory)][ValidateSet('input', 'output')][string]$Role, [ValidateRange(-1, 7)][int]$Group = -1)
    $p = $script:ProbePlugins[$Role]
    $bytes = [Convert]::FromBase64String($p.State)
    $at = $script:ProbeStateAt
    $u32 = { param($o) [BitConverter]::ToUInt32($bytes, $at + $o) }
    $magicOk = [Text.Encoding]::ASCII.GetString($bytes, $at, 4) -ceq 'RNST' -and [Text.Encoding]::ASCII.GetString($bytes, $at + 16, 4) -ceq 'PRBE'
    if (-not $magicOk -or (& $u32 4) -ne 0 -or (& $u32 8) -ne $p.Role -or (& $u32 12) -ne 1 -or (& $u32 20) -ne 16) {
        throw "探针 $Role 的状态模板不是 abi 0 的探针 state，先按 REAPER 存出的工程更新 tests/reaper/Rpp.psm1"
    }
    if ([BitConverter]::ToSingle($bytes, $at + 28) -ne 0.0) { throw "探针 $Role 的状态模板里 feedback 不是 0：烟测一律用探针默认的 0" }
    $bytes[$at + 24] = ($Group -lt 0) ? [byte]0xFF : [byte]$Group
    $text = [Convert]::ToBase64String($bytes)
    $lines = for ($i = 0; $i -lt $text.Length; $i += 128) { $text.Substring($i, [Math]::Min(128, $text.Length - $i)) }
    @($p.Header) + @($lines) + @($script:StateTail[1])
}

# 探针的 case / variant 规则表，与 src/core/diag/ProbeLog.cpp 里的 kCaseKeys 逐字相同（自测比对）。隐藏参数按它的首现顺序编号：
# auto_case 的 k（1 起）是第 k 个 case；auto_variant 的 0 是 -，k（1 起）是第 k 个不是 - 的 variant。两个参数的上限就是两种的个数。
$script:ProbeCaseKeys = ' CB-01:steady CB-02:toggle CB-03:guard-both CB-03:guard-in CB-03:guard-out' +
    ' CB-04:dop CB-04:export-offline CB-04:export-rt CB-04:freeze CB-04:rip' +
    ' CB-05:bypass-in CB-05:bypass-out CB-05:deactivate CB-05:disable-track CB-06:smart' +
    ' CB-07:dup CB-07:fxchain CB-07:preset CB-07:rename CB-07:reopen CB-07:undo' +
    ' CB-08:misplaced-order CB-08:misplaced-post CB-08:misplaced-track CB-09:mono CB-10:bufchange' +
    ' CB-11:constrain CB-12:twoproj LU-01:- LU-02:au-au LU-02:au-vst3 LU-02:vst3-vst3' +
    ' LU-03:steady LU-03:toggle LU-04:bounce LU-04:export-offline LU-04:export-rt LU-04:freeze' +
    ' LU-05:bypass-in LU-05:bypass-out LU-05:deactivate LU-06:rename LU-07:coreaudio LU-08:- LU-09:reopen '

function Get-ReaperProbeCaseKeys { $script:ProbeCaseKeys }

function Get-ReaperProbeAutoIndex {
    param([Parameter(Mandatory)][string]$Case, [Parameter(Mandatory)][string]$Variant, [string]$Keys = $script:ProbeCaseKeys)
    $cases = [Collections.Generic.List[string]]::new()
    $variants = [Collections.Generic.List[string]]::new()
    $pairs = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($token in @($Keys -split ' ' | Where-Object { $_ })) {
        $pair = $token -split ':', 2
        if (-not $cases.Contains($pair[0])) { $cases.Add($pair[0]) }
        if ($pair[1] -cne '-' -and -not $variants.Contains($pair[1])) { $variants.Add($pair[1]) }
        $null = $pairs.Add($token)
    }
    if (-not $pairs.Contains("${Case}:$Variant")) { throw "($Case, $Variant) 不在探针的规则表里" }
    [pscustomobject]@{
        Case = $cases.IndexOf($Case) + 1; Variant = ($Variant -ceq '-' ? 0 : $variants.IndexOf($Variant) + 1)
        CaseMax = $cases.Count; VariantMax = $variants.Count
    }
}

# 工程模板里写的两个插件：@{ Role; Bundle（VST3 包名）; ClassId（32 位大写十六进制） }，从上面的块头里取，不另存一份。-Probe 给探针那一套。
function Get-ReaperPluginIdentities {
    param([switch]$Probe)
    $table = $Probe ? $script:ProbePlugins : $script:Plugins
    foreach ($role in 'input', 'output') {
        $head = $table[$role].Head
        [pscustomobject]@{
            Role = $role
            Bundle = [regex]::Match($head, '"([^"]+\.vst3)"').Groups[1].Value
            ClassId = [regex]::Match($head, '\{([0-9A-Fa-f]{32})\}').Groups[1].Value.ToUpperInvariant()
        }
    }
}

# 纯函数：构建产物的 moduleinfo.json 文本里 Audio Module Class 那一条的类 ID（32 位大写十六进制），读不到返回 $null。
# 这个文件允许尾逗号，不是严格 JSON，所以不用 JSON 解析器；顺序不对或格式变了就读不到，由调用方判整套 SKIPPED，不会放过。
function Get-ReaperAudioClassId {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$ModuleInfoText)
    $m = [regex]::Match($ModuleInfoText, '"CID"\s*:\s*"([0-9A-Fa-f]{32})"\s*,\s*"Category"\s*:\s*"Audio Module Class"')
    $m.Success ? $m.Groups[1].Value.ToUpperInvariant() : $null
}

function New-ReaperProjectText {
    param(
        [Parameter(Mandatory)][string]$SourceWav,
        [Parameter(Mandatory)][string]$RenderDir,
        [Parameter(Mandatory)][string]$RenderName,
        [int]$SampleRate = 48000,
        [int]$Channels = 2,
        [switch]$OneX, # RENDER_1X 1：1x Offline，按实时速度跑，REAPER 对插件仍报离线模式（不是实时渲染）
        [switch]$Online, # RENDER_1X 2：Online Render，按实时速度跑，REAPER 对插件报实时模式（真实时渲染）
        [string[]]$Chain = @('input', 'output'),
        [string[]]$Bypass = @(),
        [double]$TrackVolume = 1.0,
        [double]$ItemSeconds = 4.0,
        [switch]$Probe, # 链上用探针构建的两个插件
        [int]$ProbeGroup = -1 # 探针 state 里的组：-1 未设，0..7 即 A..H；两端同组才会配对
    )
    foreach ($c in $Chain) { if (-not $script:Plugins.ContainsKey($c)) { throw "不认识的链节点：$c" } }
    if ($OneX -and $Online) { throw '-OneX 与 -Online 只能选一个' }
    $renderSpeed = $Online ? 2 : ($OneX ? 1 : 0)
    $fx = [Collections.Generic.List[string]]::new()
    foreach ($c in $Chain) {
        if ($c -in $Bypass) { $fx.Add('      BYPASS 1 0 0') }
        if ($Probe) {
            $fx.Add('      ' + $script:ProbePlugins[$c].Head)
            foreach ($t in (Get-ReaperProbeStateLines -Role $c -Group $ProbeGroup)) { $fx.Add('        ' + $t) }
        } else {
            $fx.Add('      ' + $script:Plugins[$c].Head)
            $fx.Add('        ' + $script:Plugins[$c].State)
            foreach ($t in $script:StateTail) { $fx.Add('        ' + $t) }
        }
        $fx.Add('      >')
    }
    $chainText = $fx.Count -gt 0 ? ("    <FXCHAIN`n" + ($fx -join "`n") + "`n    >`n") : ''
    $vol = $TrackVolume.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    $len = $ItemSeconds.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    @"
<REAPER_PROJECT 0.1 "7.82/win64" 1792399371 0
  RENDER_FILE "$RenderDir"
  RENDER_PATTERN "$RenderName"
  RENDER_FMT 0 $Channels $SampleRate
  RENDER_1X $renderSpeed
  RENDER_RANGE 1 0 0 0 1000
  RENDER_RESAMPLE 3 0 1
  RENDER_ADDTOPROJ 0
  RENDER_DITHER 0
  SAMPLERATE $SampleRate 1 0
  <RENDER_CFG
    ZXZhdyAAAQ==
  >
  TEMPO 120 4 4 0
  <TRACK
    NAME "T"
    VOLPAN $vol 0 -1 -1 1
    NCHAN 2
    MAINSEND 1 0
$chainText    <ITEM
      POSITION 0
      LENGTH $len
      <SOURCE WAVE
        FILE "$SourceWav"
      >
    >
  >
>

"@
}

function Write-ReaperText {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Text)
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

# 字符串、数字、布尔与它们的数组 / 哈希表 → Lua 字面量。
function ConvertTo-ReaperLuaValue($Value) {
    if ($null -eq $Value) { return 'nil' }
    if ($Value -is [bool]) { return $Value ? 'true' : 'false' }
    if ($Value -is [string]) { return '"' + ($Value -replace '\\', '\\' -replace '"', '\"' -replace "`r", '\r' -replace "`n", '\n') + '"' }
    if ($Value -is [System.Collections.IDictionary]) {
        return '{' + ((@($Value.Keys) | Sort-Object | ForEach-Object { "[$(ConvertTo-ReaperLuaValue ([string]$_))]=$(ConvertTo-ReaperLuaValue $Value[$_])" }) -join ',') + '}'
    }
    if ($Value -is [System.Collections.IEnumerable]) { return '{' + ((@($Value) | ForEach-Object { ConvertTo-ReaperLuaValue $_ }) -join ',') + '}' }
    if ($Value -is [ValueType]) { return ([double]$Value).ToString('R', [Globalization.CultureInfo]::InvariantCulture) }
    throw "写不成 Lua 的值：$($Value.GetType().Name)"
}

function ConvertTo-ReaperLuaTable([Parameter(Mandatory)][System.Collections.IDictionary]$Job) {
    'return ' + (ConvertTo-ReaperLuaValue $Job) + "`n"
}

Export-ModuleMember -Function New-ReaperProjectText, Write-ReaperText, ConvertTo-ReaperLuaTable, Get-ReaperPluginIdentities, Get-ReaperAudioClassId,
    Get-ReaperProbeStateLines, Get-ReaperProbeCaseKeys, Get-ReaperProbeAutoIndex
