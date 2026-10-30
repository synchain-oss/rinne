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
| `setLang` | `code`：`zh` / `en` / `fr` | `{ ok, lang }` | 写全局位，所有实例共用；不是这三个字符串之一回 `{ ok: false, reason: "bad-lang" }`，不写；兜底面板在场时回 `{ ok: false, reason: "not-ready" }`（见下），先于参数校验，不写。 |
| `setUiScale` | `f`：该角色设计盒的档位之一 | `{ ok, scale }` | 档位不在表内回 `{ ok: false, reason: "bad-scale" }`；缩放被锁定（系统 WebKit 过旧，窗口钉在 100%）时回 `{ ok: false, reason: "scale-locked" }`，窗口不变；兜底面板在场时回 `{ ok: false, reason: "not-ready" }`（见下），先于其余判断，窗口不变；应用后进入 10 秒确认。 |
| `commitUiScale` | 无 | `{ ok, scale }` | 确认保留当前档位并落盘；没有待确认的档位回 `{ ok: false, reason: "no-pending" }`；缩放被锁定时回 `{ ok: false, reason: "scale-locked" }`，不落盘；兜底面板在场时回 `{ ok: false, reason: "not-ready" }`（见下），先于其余判断，不落盘。 |
| `setRulesAck` | `version`：非负整数 | `{ ok }` | 记下已确认的规则页版本，写全局位。 |
| `setGuideSeen` | `true` | `{ ok }` | 记下本角色的引导已看，写全局位；Output 据此判断是否跳过自己的引导。 |
| `setGroup` | `g`：`A` 到 `H` | `{ ok, group }` | 手动选组，之后永不自动迁组；`groupOrigin` 变为 `manual`。 |
| `copyDiagnostics` | `commit`：布尔 | `{ ok, copied, text }` | `false` 只返回预览文本，`true` 才写剪贴板。文本是 `rinne-diag v1`，只含下面「诊断文本」一节名字表里的字段，末行 `ui:` 段已包含在 `text` 里。 |
| `openExternal` | `url` | `{ ok }` | 原生侧解析后精确匹配：scheme 为 `https`，host 为 `synchain.ca`，或 host 为 `github.com` 且路径等于 `/synchain-oss/rinne` 或以 `/synchain-oss/rinne/` 开头。其余一律回 `{ ok: false, reason: "not-allowed" }`。 |
| `setDiagLog` | `on`：布尔 | `{ ok, value }` | 诊断日志开关，本机设置，写全局位，不进工程。 |
| `resolveTakeover` | `accept`：布尔 | `{ ok, accepted }` | 休眠实例占着本组时的确认条：`true` 接管，`false` 取消。没有待确认的接管回 `{ ok: false, reason: "no-pending" }`。 |
| `showLicense` | 无 | `{ ok }` | 查看许可证：原生侧用系统方式打开嵌入的 LICENSE。 |

兜底面板在场：运行时缺失、前端启动失败或加载超时后，原生侧换上兜底面板并把网页隐藏，用户看不到网页。此时万一还有页面请求到达（切面板之前已排进队列的调用，或 WebView2 的控制器在超时之后才建好、页面在隐藏状态下加载完），会改窗口、改设置文件或改内存状态的函数一律先拒：`setLang`、`setUiScale`、`commitUiScale` 回 `{ ok: false, reason: "not-ready" }`，先于参数校验，不改窗口、不写设置文件、不改语言与缩放、不发事件。窗口在切面板时已放大到不小于设计盒，拒绝也是为了不再把它缩回去。`requestInitialState` 在这种情形下只回 `{ ok: true }`：不发首帧（网页不可见，首帧没有收件人），也不当作桥已就绪，上面时序里「立刻发首帧」这一条对它不适用。用户点重试后入口页整页重新加载，之后一切照常。页面只判 `ok`，不依赖具体原因。

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
| `peer` | 对端信息：`name`（轨道名，宿主不给时为 `null`）、`color`、`channels`、`version`、`holder`（占组者的轨道名），另带可选的 `sampleRate`（对端采样率，`srMismatch` 的提示用它；缺席显示「—」）。 |
| `self` | 本端信息：`name`，另带可选的 `channels`（本端声道数，1 或 2，与 `peer.channels` 比较决定声道徽标；缺席只显示对端声道）。 |
| `verify` | 两端轨道名核验：`match`、`diff` 或 `unknown`。 |

### 配对状态

颜色分组：红是回送已停、被拒或不可用，需要用户动作；琥珀是仍在工作但有风险；灰是主动关闭、宿主暂停或离线；绿是已连接。`connectedStill` 表示已连接但回送静音，绿点不脉冲。`unassigned` 是 8 组已满，灯为红色，文案「未分组」，消息「没有空闲组，请手动选择」。`bypassed` 只由 Output 的宿主旁路产生（Output 写静音，回送为零），Input 的 `rinne.pair` 里不会出现它：Input 被宿主旁路时没有自己的配对状态，壳把旁路并入回送静音（`loopSilent`）：判定上已连接（`connected`）因此落在 `connectedStill`；`connectedUnverified` 不看回送静音，配对态保持 `connectedUnverified`，界面按回送静音停止脉冲；其余状态不受旁路影响。宿主旁路的提示走界面的消息条，不借用 `bypassed`。

<!-- names:pair -->

| 状态 | 颜色 | 产生者 | 含义 |
|---|---|---|---|
| `ipcUnavailable` | red | 两端 | 共享内存不可用（宿主沙盒限制）。 |
| `abiMismatch` | red | 两端 | 两端版本不兼容。 |
| `srMismatch` | red | 两端 | 两端采样率不一致。 |
| `conflict` | red | 两端 | 组里已有另一个同角色实例（复制轨道造成）。Input 给「移到空闲组」，Output 给「重新配对」。 |
| `unassigned` | red | 两端 | 8 组已满，没有空闲组。 |
| `stale` | amber | 两端 | 失联：心跳陈旧超过 2 s 且 block_seq 静止 2 s 以上；离线导出期间不报。 |
| `crossProcess` | amber | 两端 | 对端在另一个进程里。 |
| `crossTrack` | amber | 两端 | 两端不在同一条轨道上。 |
| `searching` | gray | 两端 | 还没找到对端。 |
| `waitingAudio` | gray | 两端 | 宿主还没送实时音频（例如 Smart Plug-in Processing 挂起）。 |
| `validator` | gray | 两端 | 验证工具中，只出干声。 |
| `bypassed` | gray | 仅 Output | Output 被宿主旁路：写静音，回送为零。 |
| `connectedUnverified` | green | 两端 | 已连接，但宿主不给轨道名，无法核验。 |
| `connected` | green | 两端 | 已连接。 |
| `connectedStill` | green | 两端 | 已连接，回送静音。 |

### 诊断文本（rinne-diag v1）

文本由 core 的生成器（`src/core/diag/DiagReport.cpp`）一次生成，两个插件共用；格式以它和 `tests/core/diag` 的快照为准，这里登记的是名字表：不在表里的字段一律不输出。文本不含路径、用户名和主机名，轨道名只以 `same` / `different` / `unknown` 加自己这一端的 8 位散列出现。散列是指纹，不是匿名化，所以界面先用 `copyDiagnostics(false)` 取文本放进可滚动的预览区，用户确认后才调 `copyDiagnostics(true)`。

文本的形态：

- 第 1 行是固定文本 `rinne-diag v1`。
- 之后每节一行，字段是空格分隔的 `key=value`，例如 `track: relation=same hash=1a2b3c4d`。`product`、`backend` 两行没有冒号，节名取该行的第一个键（`product=Rinne Input version=…`）。没有对端时 `peer` 一行整行写 `peer: none`，不产生字段。
- 然后是最近事件段：标题行 `events(last N of M):`，其后每条事件一行，行首缩进两格（`  t+<秒>.<毫秒> <事件名>[ <参数>]`，参数是一个整数，或用斜杠连起来的两个整数，例如 `prepare 48000/512`）。
- 末行是 `ui:` 段（`ui: webview=… scale=… lang=…`）。它不在 core 生成的文本里，由承载网页的原生界面层补在最后（WebView 版本、缩放档位、语言都在它手里），所以回执的 `text` 和写进剪贴板的内容都含这一行，页面不自己拼。补的字段只用固定名字表，或过与 core 同一套清洗。
- Output 只输出字段子集：没有 `loop`、`sync`、`order`、`safety` 四节，即下表「角色」一栏写「仅 Input」的那些。

名字的写法是 `<节名>.<键名>`，节名按上面的规则取（`track.hash`、`backend.group`、`product.product`），顺序与文本里的出现顺序一致。整行的名字有两个，不带点：`rinne-diag`（第一行）和 `events`（标题行加事件行合起来算一个）。

<!-- names:diag -->

| 名字 | 角色 | 含义 |
|---|---|---|
| `rinne-diag` | 两端 | 第一行，固定文本 `rinne-diag v1`。 |
| `product.product` | 两端 | 产品名：`Rinne Input` 或 `Rinne Output`（据此区分角色）。 |
| `product.version` | 两端 | 插件版本。 |
| `product.build` | 两端 | 构建标签。 |
| `product.format` | 两端 | 插件格式：`VST3` 或 `AU`。 |
| `product.host` | 两端 | 宿主名与版本，不是主机名。 |
| `product.os` | 两端 | 操作系统与版本。 |
| `product.arch` | 两端 | 处理器架构。 |
| `backend.backend` | 两端 | 共享内存后端。 |
| `backend.ipc_abi` | 两端 | 会合布局的 abi 版本号。 |
| `backend.prefix` | 两端 | 共享内存段名前缀。 |
| `backend.group` | 两端 | 组字母 `A` 到 `H`，没有组时为 `-`。 |
| `backend.origin` | 两端 | 组号的来源：`none`、`smart`、`restore` 或 `manual`。 |
| `backend.pid` | 两端 | 本进程号。 |
| `backend.pair` | 两端 | 配对状态的机读标识，取值与 `rinne.pair` 的 `state` 是同一张表。 |
| `track.relation` | 两端 | 两端轨道关系：`same`、`different` 或 `unknown`。 |
| `track.hash` | 两端 | 本端轨道名的 8 位十六进制散列，宿主没给轨道名时为 `none`。散列是指纹，不是匿名化。 |
| `peer.role` | 两端 | 对端角色：`input` 或 `output`。 |
| `peer.state` | 两端 | 对端槽的状态：`active` 或 `dormant`。 |
| `peer.same_pid` | 两端 | 对端是否在本进程：`1` 或 `0`。 |
| `peer.version` | 两端 | 对端版本，`主.次.修`。 |
| `peer.sr` | 两端 | 对端采样率。 |
| `peer.max_block` | 两端 | 对端声明的最大块长。 |
| `peer.ch` | 两端 | 对端声道数。 |
| `peer.name` | 两端 | 对端轨道名关系：`same`、`different` 或 `unknown`。 |
| `peer.hb_age_ms` | 两端 | 对端心跳的年龄，毫秒。 |
| `prepare.count` | 两端 | prepare 调用次数。 |
| `prepare.sr` | 两端 | 最近一次 prepare 的采样率。 |
| `prepare.max` | 两端 | 最近一次 prepare 声明的最大块长。 |
| `prepare.nonrealtime_seen` | 两端 | 是否见过离线渲染：`1` 或 `0`。 |
| `prepare.setstate_before_prepare` | 两端 | setState 是否先于 prepare：`1` 或 `0`。 |
| `prepare.reset_calls` | 两端 | reset 调用次数。 |
| `blocks.total` | 两端 | 处理过的块数。 |
| `blocks.zero_len` | 两端 | 0 样本的块数。 |
| `blocks.over_max` | 两端 | 超过声明最大块长的块数。 |
| `blocks.bypassed` | 两端 | 宿主旁路期间的块数。 |
| `blocks.max_n_seen` | 两端 | 本次 prepare 以来见到的最大块长。 |
| `nsamples.min` | 两端 | 最小块长。 |
| `nsamples.max` | 两端 | 最大块长（终身）。 |
| `nsamples.hist` | 两端 | 块长直方图，8 档计数，逗号分隔。 |
| `loop.d_user_ms` | 仅 Input | 用户设定的环路延迟 D，毫秒。 |
| `loop.d_eff` | 仅 Input | 实际使用的 D，样本。 |
| `loop.d_min` | 仅 Input | D 的下限（随实测块长抬升），样本。 |
| `loop.mode` | 仅 Input | 延迟模式：`ms`（按时间）或 `sync`（按拍）；`delay_mode` 参数的 `time` 在诊断文本里写成 `ms`。 |
| `loop.bpm` | 仅 Input | 速度，拍/分钟。 |
| `loop.bpm_src` | 仅 Input | 速度来源：`none`、`host` 或 `manual`。 |
| `loop.taps_active` | 仅 Input | 当前活跃的读头数。 |
| `loop.tap_drop` | 仅 Input | 累计丢掉的读头数。 |
| `sync.resync` | 仅 Input | 累计重同步次数。 |
| `sync.overflow` | 仅 Input | 被环覆盖而丢掉的帧数。 |
| `sync.torn` | 仅 Input | 撕裂的帧数。 |
| `sync.overrun` | 仅 Input | 补 0 的样本数。 |
| `sync.w_cas_fail` | 仅 Input | 写头 CAS 失败次数。 |
| `sync.unstable` | 仅 Input | 是否进过时序容忍模式：`0` 或 `1`，进过就不再清零。 |
| `sync.off_drift_blocks` | 仅 Input | 漂移被吸收的检查次数。 |
| `sync.frozen_max` | 仅 Input | 写头连续没动的最多块数。 |
| `order.verdict` | 仅 Input | 两端处理顺序的判断：`unknown`、`correct` 或 `wrong`。 |
| `order.votes` | 仅 Input | 顺序判断的票数。 |
| `order.host_ns` | 仅 Input | 宿主时间戳是否可用：`valid` 或 `invalid`。 |
| `safety.state` | 仅 Input | 安全状态机的名字，如 `RUN`、`TRIPPED`。 |
| `safety.trips` | 仅 Input | 累计跳闸次数。 |
| `safety.tail_peak_max_db` | 仅 Input | 读头输出（未经安全链）本会话的最大峰值，dBFS。 |
| `safety.limiter_gr_max_db` | 仅 Input | 限制器增益缩减的最大值，dB。 |
| `safety.nonfinite_ring` | 仅 Input | 写方写环前清洗掉的非有限样本数。 |
| `safety.clamp_ring` | 仅 Input | 写方写环前被钳位的样本数。 |
| `events` | 两端 | 最近事件段：标题行 `events(last N of M):`，N 不超过 64，之后每条事件一行。 |
| `ui.webview` | 两端 | WebView 版本（`ui:` 段，界面补）。 |
| `ui.scale` | 两端 | 缩放档位（`ui:` 段，界面补）。 |
| `ui.lang` | 两端 | 界面语言（`ui:` 段，界面补）。 |

### 全局位

语言、规则确认版本、两个角色各自的引导已看、两个角色各自的缩放档位、诊断日志开关存在按用户的全局位里，不进工程。预览里存在 `localStorage["rinne.uiDefaults"]`，同源的两个页面共享；每个实例的已关闭提示存 `sessionStorage`。

### 预览专用（不属于契约）

web-preview 的 mock 后端另有 `previewScene`：当前场景的 `id`、`lang`、`title` 和设计稿给的 `state` 描述。覆盖层（语言卡、规则页、tour、预设菜单、关于、缩放确认）是界面自己的状态，原生侧不会推，页面实现可以读 `previewScene.state.overlay` 在预览里直接打开它们。真桥上没有这个字段。预览地址的查询参数：`?role=input|output`、`?scenario=<场景 ID>`、`?lang=zh|en|fr`、`?scale=<档位>`、`?fresh=1`（先清空全局位）。

预览里要走到缩放锁定态：在 `localStorage["rinne.uiDefaults"]` 的 JSON 里加一项 `"scaleLocked": true`（预览专用的全局位，真桥没有这一项；`?fresh=1` 会连它一起清掉）。之后 mock 的 `setUiScale` 与 `commitUiScale` 回 `{ ok: false, reason: "scale-locked" }`，丢掉没确认的预览，并让窗口档位回到 1。

预览里要走到兜底面板在场态：在同一个 JSON 里加一项 `"fallbackShown": true`（同样是预览专用的全局位，真桥没有这一项；`?fresh=1` 会连它一起清掉）。之后 mock 的 `setLang`、`setUiScale`、`commitUiScale` 在参数校验之前就回 `{ ok: false, reason: "not-ready" }`，什么都不改；`requestInitialState` 只回 `{ ok: true }`，不发首帧。

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
| `rinne.meters` | 25 Hz，先 diff 再发：`in`、`ret`（增益前）、`out` 三条电平（dBFS，静音为 `null`）、Mix 推子的两路分量 `dryPart`、`retPart`（dBFS，静音为 `null`）、增益缩减 `gr`、高切截断位 `highCutLimited`、三盏灯 `lamps`（`dc`、`nonfinite`、`guard`，另带可选的 `nonfiniteCount`、`guardDb`）。字段含义见下面「电平帧补充字段」。 |
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
| `delay_note` | 16 档 1/64 到 1/1，含三连音与附点；取值 `n64`、`n32t`、`n32`、`n16t`、`n16`、`n8t`、`n16d`、`n8`、`n4t`、`n8d`、`n4`、`n2t`、`n4d`、`n2`、`n2d`、`n1`，顺序按拍数严格升序（1/8 三连音是 1/3 拍，附点 1/16 是 3/8 拍，依此类推），与 `src/input/ParamTable.h` 的档位表逐档一致 | `n4` | 否 |
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
| `delay` | 环路延迟卡：`actualMs`、`minMs`、`reason`（`ok`、`raised`、`capped`、`overrun`）、`maxBlock`（本会话最大块长），另带可选的 `bpm`（同步档换算用的速度）与 `noTempo`（真为宿主没给速度、`bpm` 是兜底的 120；缺席按有速度处理，`bpm` 缺席按 120）。 |
| `safety` | `{ cut, cooldownMs, l2 }`，另带可选的 `fadeIn`（布尔，回送正在 500 ms 淡入：新配对、恢复回送、载入预设之后；缺席按假）。`cut` 是 `null`、`panic`、`nan`、`huge` 或 `runaway`；跳闸淡出后清空延迟线并锁存，只有 `resumeLoop` 或重载工程能清，不设自动恢复。`l2` 为真表示有任一 L2 条件。 |
| `dismissedHints` | 本实例已关闭的提示 ID，不进工程。 |
| `exportCauses` | 导出不一致的锁存信息：`{ bits, count }`。`bits` 是自上次确认起各原因位的并集（1 = 宿主违约，2 = 主读头补 0，4 = 对端报告违约，8 = 重同步），`count` 是自上次确认起被计入的事件数（无符号 32 位，回绕无碍，只用「不等」比较）。`count` 为 0 表示没有锁存。 |
| `silentCause` | 可选。回送被静音时的原因：`null`、`hostBypass`（本插件被宿主旁路）、`peerBypass`（对端被宿主旁路）、`hostPaused`（宿主暂停）、`stalled`（块序号停滞）。只在 `rinne.pair.state` 为 `connectedStill` 或 `connectedUnverified` 时有意义；缺席按 `null` 处理，界面只显示通用的「回送静音」而不给原因。 |
| `notices` | 可缺省。数组，元素 `{ id, a?, b? }`，`id` 取 `ipcSandbox`（后端被沙盒拒绝；出现时界面不再另报泛泛的共享内存不可用）、`outOfOrder`（错序证据成立）、`unstable`（宿主时序不稳定，已切到容忍模式）、`newerProject`（工程由更新的版本存下，`a` 是工程里的版本、`b` 是本插件版本）、`stateDecodeFailed`（state 解码失败）之一。这五条没有对应的配对状态，只能由这个字段带；缺省、非数组、元素残缺、未知 `id` 一律当没有，同一个 `id` 只取第一条。导出不一致的原因位不走这里，仍在 `exportCauses`。 |

### 电平帧补充字段

| 字段 | 含义 |
|---|---|
| `dryPart` | Mix 推子的干声分量峰值（dBFS，静音为 `null`），即 g_dry 作用之后的干声。 |
| `retPart` | Mix 推子的回送分量峰值（dBFS，静音为 `null`），即加到干声上的那一项。输出是这两路逐样本相加，两路的峰值不一定同时出现，所以各自的峰值不能相加还原 `out`；推子从左填干声、从右填回送，各占一半量程。 |
| `highCutLimited` | 布尔。高切设定高于采样率允许的上限（0.45 倍采样率）而又不是「关」，实际按上限截断；界面据此给一条中性提示。 |
| `lamps.nonfiniteCount` | 环里读到并已置 0 的非有限样本累计数，`lamps.nonfinite` 为 `zeroed` 时界面显示它。 |
| `lamps.guardDb` | 可选，非负数。失控保护当前把回送压低了多少 dB（核心侧的增益值取负）；`lamps.guard` 为 `reduced` 时界面显示它，缺席则只写「自动降低」不带数。 |

### Input 页面读到的配对补充字段

Input 页面把 `peer.color` 当作 `#rrggbb` 字符串（宿主不给时为 `null`），格式不对的值一律当没有；`peer.channels` 是数字 1 或 2。

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
| `notices` | 可缺省。数组，元素 `{ id, a?, b? }`，`id` 取 `ipcSandbox`（后端被沙盒拒绝）、`newerProject`（工程由更新的版本存下，`a` 是工程里的版本、`b` 是本插件版本）、`stateDecodeFailed`（state 解码失败）、`outOfOrder`（错序证据成立）之一。这四条没有对应的配对状态，只能由这个字段带；缺省、非数组、未知 `id` 一律当没有。 |
| `build` | 可缺省。`{ sha, fmt, arch }`，关于面板的构建行（提交短散列、插件格式、平台）；缺省时只显示版本号。 |

### 配对卡

- `rinne.pair` 的 `self.name` 与 `peer.name` 对应配对卡里「本轨」与「Input 所在轨」两行；`verify` 决定核验徽标。
- 配对卡另读两个可缺省字段：`self.color`（本轨颜色点，缺省用默认点色）、`peer.channels`（对端声道，`1` / `2` 或 `"MONO"` / `"ST"`，缺省不画声道徽标）。`srMismatch` 消息里对端的采样率读 `peer.sampleRate`（字段登记在 common 节），缺省显示「—」。
- 消息位里「查看说明」按钮（版本不匹配、IPC 不可用、沙盒拒绝、跨进程四条消息带它）经 `openExternal` 打开当前版本钉死的用户指南（`docs/USER_GUIDE.zh-CN.md`，不带片段），地址必须通过原生白名单。
- `conflict` 时的修复按钮文案是「重新配对」（按智能默认重选组）。界面文字里不出现「自动配对」。
- 智能默认只看本进程内的实例，新 Output 的选组顺序是：同名轨上未配对的 Input、最近认领的未配对 Input、编号最小的空组等待（每 1 s 重评）。用户手选组之后永不自动迁组。
