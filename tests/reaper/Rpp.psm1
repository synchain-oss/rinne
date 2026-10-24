# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 工程文本与 Lua 任务文件的生成器。工程是手写的最小 .rpp：一条轨道、一个测试信号 item、按顺序排好的插件。
# 插件块里的状态是 REAPER 自己存出的默认状态（缺了它 REAPER 报「效果不可用」）。块头里的类标识、显示名和默认状态都是写死的
# 值：插件身份或 state 头变了要在这里手动更新。类标识在预检里与构建产物的 VST3 清单（moduleinfo.json）比对，对不上整套 SKIPPED。
#   New-ReaperProjectText   工程文本
#   ConvertTo-ReaperLuaTable 把哈希表写成 Lua 的 return { … }，Lua 驱动用 dofile 读
#   Write-ReaperText        无 BOM 的 UTF-8 写文件
#   Get-ReaperPluginIdentities / Get-ReaperAudioClassId   工程模板里写的插件包名与类 ID；从 moduleinfo.json 文本取 Audio Module Class 的类 ID

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

# 工程模板里写的两个插件：@{ Role; Bundle（VST3 包名）; ClassId（32 位大写十六进制） }，从上面的块头里取，不另存一份。
function Get-ReaperPluginIdentities {
    foreach ($role in 'input', 'output') {
        $head = $script:Plugins[$role].Head
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
        [switch]$OneX,
        [string[]]$Chain = @('input', 'output'),
        [string[]]$Bypass = @(),
        [double]$TrackVolume = 1.0,
        [double]$ItemSeconds = 4.0
    )
    foreach ($c in $Chain) { if (-not $script:Plugins.ContainsKey($c)) { throw "不认识的链节点：$c" } }
    $fx = [Collections.Generic.List[string]]::new()
    foreach ($c in $Chain) {
        if ($c -in $Bypass) { $fx.Add('      BYPASS 1 0 0') }
        $fx.Add('      ' + $script:Plugins[$c].Head)
        $fx.Add('        ' + $script:Plugins[$c].State)
        foreach ($t in $script:StateTail) { $fx.Add('        ' + $t) }
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
  RENDER_1X $($OneX ? 1 : 0)
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

Export-ModuleMember -Function New-ReaperProjectText, Write-ReaperText, ConvertTo-ReaperLuaTable, Get-ReaperPluginIdentities, Get-ReaperAudioClassId
