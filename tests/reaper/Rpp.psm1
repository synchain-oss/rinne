# SPDX-License-Identifier: GPL-3.0-or-later
#
# REAPER 工程文本与 Lua 任务文件的生成器。工程是手写的最小 .rpp：一条轨道、一个测试信号 item、按顺序排好的插件。
# 插件块里的状态是 REAPER 自己存出的默认状态（缺了它 REAPER 报「效果不可用」）；插件的类标识来自构建产物的 VST3 清单。
#   New-ReaperProjectText   工程文本
#   ConvertTo-ReaperLuaTable 把哈希表写成 Lua 的 return { … }，Lua 驱动用 dofile 读
#   Write-ReaperText        无 BOM 的 UTF-8 写文件

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

Export-ModuleMember -Function New-ReaperProjectText, Write-ReaperText, ConvertTo-ReaperLuaTable
