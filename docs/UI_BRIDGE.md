<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# 界面桥契约（UI_BRIDGE）

status: draft

## Summary (EN)

The bridge between the web UI and the native plugin: function names, event names, Input parameter IDs, pairing states and the diagnostics whitelist. This file, `src/input/InputBridgeApi.h`, `src/output/OutputBridgeApi.h`, the name tables in `web/shared/bridge.js` and the mock backends under `web-preview/mock/` must agree name for name; `scripts/check-bridge-parity.mjs` compares all of them and fails when any one side drops or adds an entry. The contract is not frozen: change all sides in the same commit.

## 怎么读这份文档

- 本文件分三节：`common`（两个角色共用）、`input`、`output`。每节开头有 `section` 标记，后续页面实现只改自己角色的那一节。
- 带 `names:` 标记的表是机读真源：第一列的反引号名字会被 `scripts/check-bridge-parity.mjs` 抽出来，和 `src/{input,output}/*BridgeApi.h`、`web/shared/bridge.js` 的名表、`web-preview/mock/` 逐名对拍。加一个函数、事件、参数或状态，要在同一个提交里改这四处；少改任何一处，检查判红。
- 契约不冻结：参数 ID 的冻结归参数文档，这里只登记界面用到的名字和载荷形状。
- 约定：载荷都是 JSON；函数的回执是 `{ ok: true, ... }` 或 `{ ok: false, reason }`，越界与类型不符一律由原生侧夹取或拒绝，界面不做第二份校验；事件是纯下行，界面只订阅。

<!-- section:common -->

## common：两个角色共用

### 时序

1. 页面装载后调一次 `requestInitialState`。在此之前原生侧丢掉所有周期回声，也不记账。
2. 收到请求后，原生侧立刻把本角色所有状态类事件各发一帧（首帧必发）。
3. 之后 `rinne.state` 和 `rinne.pair` 变更即发，另有 1 Hz 兜底；预览里的 mock 用 4 Hz 异步回声，并且隔几拍在新帧后补发一帧过期的全量快照。
4. 每帧都带单调递增的 `seq`。页面丢弃 `seq` 不大于已收到最大值的帧，不据此回写任何控件。
5. 编辑器关闭后重建，页面要重新调 `requestInitialState`；桥不缓存快照，也不重放事件。

### 函数（界面调原生，两个角色共用）

<!-- names:fn -->

| 函数 | 参数 | 回执 | 说明 |
|---|---|---|---|
| `requestInitialState` | 无 | `{ ok }` | 见上面的时序。 |
| `setLang` | `code`：`zh` / `en` / `fr` | `{ ok, lang }` | 写全局位，所有实例共用；不是这三个字符串之一回 `{ ok: false, reason: "bad-lang" }`，不写。 |
| `setUiScale` | `f`：该角色设计盒的档位之一 | `{ ok, scale }` | 档位不在表内回 `{ ok: false, reason: "bad-scale" }`；缩放被锁定（系统 WebKit 过旧，窗口钉在 100%）时回 `{ ok: false, reason: "scale-locked" }`，窗口不变；应用后进入 10 秒确认。 |
| `commitUiScale` | 无 | `{ ok, scale }` | 确认保留当前档位并落盘；没有待确认的档位回 `{ ok: false, reason: "no-pending" }`；缩放被锁定时回 `{ ok: false, reason: "scale-locked" }`，不落盘。 |
| `setRulesAck` | `version`：非负整数 | `{ ok }` | 记下已确认的规则页版本，写全局位。 |
| `setGuideSeen` | `true` | `{ ok }` | 记下本角色的引导已看，写全局位；Output 据此判断是否跳过自己的引导。 |
| `setGroup` | `g`：`A` 到 `H` | `{ ok, group }` | 手动选组，之后永不自动迁组；`groupOrigin` 变为 `manual`。 |
| `copyDiagnostics` | `commit`：布尔 | `{ ok, copied, text }` | `false` 只返回预览文本，`true` 才写剪贴板。文本只含下面的白名单字段。 |
| `openExternal` | `url` | `{ ok }` | 原生侧解析后精确匹配：scheme 为 `https`，host 为 `synchain.ca`，或 host 为 `github.com` 且路径等于 `/synchain-oss/rinne` 或以 `/synchain-oss/rinne/` 开头。其余一律回 `{ ok: false, reason: "not-allowed" }`。 |
| `setDiagLog` | `on`：布尔 | `{ ok, value }` | 诊断日志开关，本机设置，写全局位，不进工程。 |
| `resolveTakeover` | `accept`：布尔 | `{ ok, accepted }` | 休眠实例占着本组时的确认条：`true` 接管，`false` 取消。没有待确认的接管回 `{ ok: false, reason: "no-pending" }`。 |
| `showLicense` | 无 | `{ ok }` | 查看许可证：原生侧用系统方式打开嵌入的 LICENSE。 |

### 事件（原生推给界面，两个角色共用）

<!-- names:ev -->

| 事件 | 内容与频率 |
|---|---|
| `rinne.state` | 角色状态全量快照；首帧必发，变更即发，1 Hz 兜底。 |
| `rinne.pair` | 配对状态；首帧必发，变更即发，1 Hz 兜底。 |
| `rinne.error` | 桥或原生侧的错误，`{ seq, code, detail? }`；按需发，不属于首帧。 |

### rinne.state 的共用字段

| 字段 | 含义 |
|---|---|
| `seq` | 帧序号。 |
| `role` | `input` 或 `output`。 |
| `version` | 本插件版本串。 |
| `lang` | 当前语言；`langChosen` 为假表示用户还没选过，页面应显示语言选择卡。 |
| `rulesAck` | 全局位里已确认的规则页版本。 |
| `guideSeen` | 本角色的引导是否已看（Output 另带 `inputGuideSeen`）。 |
| `uiScale` | 当前缩放档位。 |
| `restored` | 本实例经过 `setStateInformation`。 |
| `takeover` | 休眠实例占着本组，界面显示接管确认条，等 `resolveTakeover`。 |
| `group` | `A` 到 `H`，没有空闲组时为 `null`。 |
| `groupOrigin` | `auto`、`manual` 或 `project`；`auto` 时页面显示「自动」徽标。 |
| `diagLog` | 诊断日志开关（本机设置，不进工程）。 |
| `sampleRate` | 当前采样率。 |

### rinne.pair 的字段

| 字段 | 含义 |
|---|---|
| `seq` | 帧序号。 |
| `state` | 下面 15 个状态之一。 |
| `groupMask` | 八位位图，第 n 位为 1 表示第 n 个组里有未配对的对端在等待。 |
| `peer` | 对端信息：`name`（轨道名，宿主不给时为 `null`）、`color`、`channels`、`version`、`holder`（占组者的轨道名）。 |
| `self` | 本端信息：`name`。 |
| `verify` | 两端轨道名核验：`match`、`diff` 或 `unknown`。 |

### 配对状态

颜色分组：红是回送已停、被拒或不可用，需要用户动作；琥珀是仍在工作但有风险；灰是主动关闭、宿主暂停或离线；绿是已连接。`connectedStill` 表示已连接但回送静音，绿点不脉冲。`unassigned` 是 8 组已满，灯为红色，文案「未分组」，消息「没有空闲组，请手动选择」。`bypassed` 由 Output 的宿主旁路产生，Input 被宿主旁路时界面按 `bypassed` 显示。

<!-- names:pair -->

| 状态 | 颜色 | 含义 |
|---|---|---|
| `ipcUnavailable` | red | 共享内存不可用（宿主沙盒限制）。 |
| `abiMismatch` | red | 两端版本不兼容。 |
| `srMismatch` | red | 两端采样率不一致。 |
| `conflict` | red | 组里已有另一个同角色实例（复制轨道造成）。Input 给「移到空闲组」，Output 给「重新配对」。 |
| `unassigned` | red | 8 组已满，没有空闲组。 |
| `stale` | amber | 失联：心跳陈旧超过 2 s 且 block_seq 静止 2 s 以上；离线导出期间不报。 |
| `crossProcess` | amber | 对端在另一个进程里。 |
| `crossTrack` | amber | 两端不在同一条轨道上。 |
| `searching` | gray | 还没找到对端。 |
| `waitingAudio` | gray | 宿主还没送实时音频（例如 Smart Plug-in Processing 挂起）。 |
| `validator` | gray | 验证工具中，只出干声。 |
| `bypassed` | gray | 宿主旁路。 |
| `connectedUnverified` | green | 已连接，但宿主不给轨道名，无法核验。 |
| `connected` | green | 已连接。 |
| `connectedStill` | green | 已连接，回送静音。 |

### 诊断文本（rinne-diag v1）

`key=value` 逐行。下面是白名单，不在表里的字段一律不输出；文本不含路径、用户名和主机名，轨道名只以「相同 / 不同 / 未知」加 8 位散列出现。界面先用 `copyDiagnostics(false)` 取文本放进可滚动的预览区，用户确认后才调 `copyDiagnostics(true)`。

<!-- names:diag -->

| 字段 | 含义 |
|---|---|
| `diag.format` | 固定为 `rinne-diag v1`。 |
| `diag.version` | 插件版本。 |
| `diag.role` | `input` 或 `output`。 |
| `diag.os` | 操作系统与版本。 |
| `diag.host` | 宿主名与版本。 |
| `diag.sample_rate` | 采样率。 |
| `diag.block_size` | 当前块长。 |
| `diag.group` | 组号。 |
| `diag.pair_state` | 配对状态。 |
| `diag.track_relation` | 两端轨道关系：相同、不同或未知。 |
| `diag.track_hash` | 轨道名的 8 位散列。 |
| `diag.lang` | 界面语言。 |
| `diag.ui_scale` | 缩放档位。 |
| `diag.webview` | WebView 版本。 |

### 全局位

语言、规则确认版本、两个角色各自的引导已看、两个角色各自的缩放档位、诊断日志开关存在按用户的全局位里，不进工程。预览里存在 `localStorage["rinne.uiDefaults"]`，同源的两个页面共享；每个实例的已关闭提示存 `sessionStorage`。

### 预览专用（不属于契约）

web-preview 的 mock 后端另有 `previewScene`：当前场景的 `id`、`lang`、`title` 和设计稿给的 `state` 描述。覆盖层（语言卡、规则页、tour、预设菜单、关于、缩放确认）是界面自己的状态，原生侧不会推，页面实现可以读 `previewScene.state.overlay` 在预览里直接打开它们。真桥上没有这个字段。预览地址的查询参数：`?role=input|output`、`?scenario=<场景 ID>`、`?lang=zh|en|fr`、`?scale=<档位>`、`?fresh=1`（先清空全局位）。

预览里要走到缩放锁定态：在 `localStorage["rinne.uiDefaults"]` 的 JSON 里加一项 `"scaleLocked": true`（预览专用的全局位，真桥没有这一项；`?fresh=1` 会连它一起清掉）。之后 mock 的 `setUiScale` 与 `commitUiScale` 回 `{ ok: false, reason: "scale-locked" }`，丢掉没确认的预览，并让窗口档位回到 1。

<!-- section:input -->

## input：Rinne Input

### 专用函数

<!-- names:fn -->

| 函数 | 参数 | 回执 | 说明 |
|---|---|---|---|
| `beginParamGesture` | `id`：参数 ID | `{ ok }` | 手势开始。拖动期间收到的回声只更新显示，不回写。 |
| `setParam` | `id`、`value` | `{ ok, value, seq }` | 值由原生侧夹取并回报实际值；`seq` 与随后回声帧的 `seq` 对得上。 |
| `endParamGesture` | `id` | `{ ok }` | 手势结束；这之前发出的回声帧一律丢弃（按 `seq` 比对）。 |
| `setLimiter` | `on`：布尔 | `{ ok, value, seq }` | 等价于对 `limiter` 调 `setParam`。 |
| `panic` | 无 | `{ ok, cut }` | 紧急切断。连点不会恢复。 |
| `resumeLoop` | 无 | `{ ok }` | 恢复回送，唯一入口。切断后 1.5 s 冷却，冷却内回 `{ ok: false, reason: "cooldown", remainingMs }`。 |
| `dismissHint` | `id` | `{ ok }` | 关闭一条提示，每实例有效，不进工程。 |
| `loadPreset` | `id`：工厂或用户预设 ID | `{ ok }` | 载入后从首个有效读样本起 500 ms 淡入。 |
| `savePreset` | `name`、`overwrite` | `{ ok, id }` | 名字去控制字符、首尾空白，限 24 个字符；重名且未允许覆盖回 `{ ok: false, reason: "exists" }`。 |
| `deletePreset` | `id` | `{ ok }` | 只删用户预设。 |
| `setGuardOn` | `on`：布尔 | `{ ok, value }` | 失控保护开关（`cfg.guardOn`，默认开）。 |
| `setResetOnPlay` | `on`：布尔 | `{ ok, value }` | 起播时清空环路（`cfg.resetOnPlay`，默认开），在关于面板的高级设置里。 |
| `setTailMode` | `mode`：`auto`、`infinite`、`30s` | `{ ok, value }` | tail 模式（`cfg.tailMode`），在高级设置里。 |
| `moveToFreeGroup` | 无 | `{ ok, group }` | `conflict` 时的一键修复：移到空闲组，不自动迁移的纪律不变；不在冲突态回 `{ ok: false, reason: "not-conflict" }`，没有空闲组回 `reason: "no-free-group"`。 |

### 专用事件

<!-- names:ev -->

| 事件 | 内容与频率 |
|---|---|
| `rinne.params` | 参数值的宿主回声，`{ seq, values }`；首帧必发。宿主自动化回放时，指针用 rAF 指数逼近最新回声，时间常数 40 ms，不用 CSS 过渡。 |
| `rinne.meters` | 25 Hz，先 diff 再发：`in`、`ret`（增益前）、`out` 三条电平（dBFS，静音为 `null`）、增益缩减 `gr`、三盏灯 `lamps`（`dc`、`nonfinite`、`guard`）。 |
| `rinne.presets` | 预设列表：`factory`（8 个，三语名）、`user`、`current`。 |

### 参数（11 个）

参数 ID 随工程保存，ID 与范围的最终冻结归参数文档。`limiter` 与 `ceiling` 加载工程时不强制复位，工厂预设与用户预设都不含它们，也不含 `loop_on`。

<!-- names:param -->

| 参数 | 范围 | 默认 | 自动化 |
|---|---|---|---|
| `feedback` | 0–150 %（限制器关时有效上限 100 %） | `0` | 是 |
| `loop_on` | 开关（关为淡出，不锁存、不清空） | `true` | 是 |
| `mix` | 0–100 %，g_dry = min(1, 2(1 − m))，g_ret = min(1, 2m) | `25` | 是 |
| `delay_mode` | `time` 或 `sync` | `time` | 否 |
| `delay_ms` | 1–4000 ms，连续 | `100` | 否 |
| `delay_note` | 16 档 1/64 到 1/1，含三连音与附点；取值 `n64`、`n32t`、`n32`、`n16t`、`n16`、`n16d`、`n8t`、`n8`、`n8d`、`n4t`、`n4`、`n4d`、`n2t`、`n2`、`n2d`、`n1`，顺序以参数文档为准 | `n4` | 否 |
| `low_cut` | 20（关）–2000 Hz | `20` | 是 |
| `high_cut` | 1000–20000（关）Hz | `20000` | 是 |
| `polarity` | 极性反转 | `false` | 是 |
| `limiter` | 开关（切换 10 ms 交叉过渡） | `true` | 否 |
| `ceiling` | −24 到 0 dBFS，步进 0.1 | `-6` | 否 |

### rinne.state 的 Input 字段

| 字段 | 含义 |
|---|---|
| `cfg` | 非参数的工程状态：`resetOnPlay`（默认开）、`tailMode`、`delayUnit`（ms 或样本）、`fbUnit`（% 或 dB）、`guardOn`（失控保护，默认开）。 |
| `preset` | `{ id, dirty, name }`；`dirty` 为真时界面在名字后加「 *」。工厂预设的 `name` 是 `{ zh, en, fr }`，用户预设是字符串。 |
| `delay` | 环路延迟卡：`actualMs`、`minMs`、`reason`（`ok`、`raised`、`capped`、`overrun`）、`maxBlock`（本会话最大块长）。 |
| `safety` | `{ cut, cooldownMs, l2 }`。`cut` 是 `null`、`panic`、`nan`、`huge` 或 `runaway`；跳闸淡出后清空延迟线并锁存，只有 `resumeLoop` 或重载工程能清，不设自动恢复。`l2` 为真表示有任一 L2 条件。 |
| `dismissedHints` | 本实例已关闭的提示 ID，不进工程。 |

### 反馈安全在桥上的体现

- 反馈默认 0。限制器关闭时反馈有效上限 100 %：表盘 100–150 % 段画斜纹，显示「设定 130 %，有效 100 %」，不报错。
- 环内插件产生的 NaN 由 Output 清洗，不跳闸，`lamps.nonfinite` 为 `zeroed` 并带计数；Input 内部产生的非有限值立即 `cut = "nan"` 并锁存。
- 失控保护（`cfg.guardOn`）：降到 −24 dB 仍持续 1 s 时 `cut = "runaway"`。
- Panic 是界面动作，不是参数，不进预设、不可自动化。

<!-- section:output -->

## output：Rinne Output

Output 用共用的十二个函数加 `repairPair`，以及三个事件，没有参数，不接收 `rinne.params`、`rinne.meters`、`rinne.presets`。

### 专用函数

<!-- names:fn -->

| 函数 | 参数 | 回执 | 说明 |
|---|---|---|---|
| `repairPair` | 无 | `{ ok, group }` | `conflict` 时的一键修复，按钮文案「重新配对」：按智能默认重选组；不在冲突态回 `{ ok: false, reason: "not-conflict" }`。 |

### rinne.state 的 Output 字段

| 字段 | 含义 |
|---|---|
| `send` | 发送状态：`channels`（`ST` 或 `MONO`）、`sampleRate`、`bypassed`（旁路写入静音）、`hostPaused`。 |
| `inputGuideSeen` | Input 的引导是否已看；已看时 Output 首次打开跳过自己的引导。 |

### 配对卡

- `rinne.pair` 的 `self.name` 与 `peer.name` 对应配对卡里「本轨」与「Input 所在轨」两行；`verify` 决定核验徽标。
- `conflict` 时的修复按钮文案是「重新配对」（按智能默认重选组）。界面文字里不出现「自动配对」。
- 智能默认只看本进程内的实例，新 Output 的选组顺序是：同名轨上未配对的 Input、最近认领的未配对 Input、编号最小的空组等待（每 1 s 重评）。用户手选组之后永不自动迁组。
