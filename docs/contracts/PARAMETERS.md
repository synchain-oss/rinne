# 参数契约：Rinne Input 的宿主参数

status: frozen

## Summary (EN)

- **What**: the 11 host parameters of Rinne Input: IDs, order, types, ranges, defaults, steps, scales, automation, and the numbers hosts save them under. Rinne Output has no parameters of its own; a VST3 host sees only the bypass parameter the JUCE wrapper adds (AU bypass is a host property, not a parameter).
- **Frozen surface**: the 11 IDs, their order and their types; version hint 1 for all of them; the bridge IDs and order of existing choice steps (new steps may only be appended); the range and scale of every automatable parameter, which also never becomes non-automatable; no parameter is ever removed. `contract-static-params` hard-codes these, so regenerating the golden does not get past it.
- **Two stages**: the 11 parameter IDs never change, at any stage, beta and preview builds included: hosts save projects and automation under the ID, and projects saved with a beta or preview build are real projects too. The rest of the frozen surface has two stages. Until the official release (beta and preview builds) it may still change, but only through the contract-change process: a change document and a CHANGELOG entry in the same PR, plus an update of the hard-coded list in `contract-static-params`, which is infrastructure the maintainer approves; projects and automation saved with a beta may then no longer match. From the first public release candidate on, the whole frozen surface never changes.
- **Needs a contract-change document**: defaults, host-facing names and step labels, units, steps, the range and scale of a non-automatable parameter, making a non-automatable parameter automatable, appending a parameter (version hint 2 or higher) or a choice step. Changing a safety default (feedback 0, limiter on, ceiling −6 dB) is also a compatibility event for CHANGELOG.
- **Host IDs**: VST3 and AU both use JUCE's hash of the ID string with the top bit cleared; the version hint is not part of the hash. The VST3 column is measured: the unit tests build real JUCE parameter objects and read the ID through JUCE's own conversion function. The AU column and the AU list order are recomputed, not measured: the tests do not link JUCE's AU wrapper and redo what its code does in JUCE 8.0.8 (`generateAUParameterID` and `GetParameterList`), so that source must be re-checked by hand whenever JUCE is upgraded. AU hosts list the parameters by version hint and then by that number, not by index.
- **Values**: state, presets and the UI bridge carry plain values (%, ms, Hz, dB, step index); only the host sees normalised 0…1 values, mapped linearly or logarithmically as listed here.
- **Truth**: `src/input/ParamTable.h` mirrors this document; `tests/golden/params-input.json` is checked against it twice (compiled in the unit tests, uncompiled by `contract-static-params`), and the tables and the status line below are checked against the golden. The version hint is machine-checked as the one top-level golden value (the macro table and the golden have no per-parameter column), and the unit tests check every parameter of the production parameter tree against that golden value.
- **Status**: frozen. From now on a PR that touches this document, the golden or the `RINNE_PARAM` lines carries a contract-change document or a `contract-neutral:` line.

## 范围

宿主按参数 ID 保存自动化与工程里的参数值。版本发出之后，ID、顺序或可自动化参数的范围一改，用户已存的工程与自动化就对不上，所以这些在这里冻结：参数 ID 任何时候都不改；顺序、范围等其余冻结面正式发布之前改要走变更流程，正式发布之后永不改（见「冻结规则」）。本契约规定：

- Input 的 11 个宿主参数：ID、序号、类型、范围、默认、步进、刻度、可否自动化，以及宿主保存它们用的参数 ID；
- 明文值与宿主归一化值之间怎么换算；
- 冻结之后哪些任何时候都不改、哪些在正式发布后永不改、哪些改了要带变更文档。

不在本契约里的：非参数的工程状态（组号、`reset_on_play`、`tail_mode`、显示单位等，见 [STATE_SCHEMA.md](STATE_SCHEMA.md)）；界面上的中文名与三语文案（归界面词条）；Panic、恢复回送、分组与修复按钮（界面动作，不是参数）。

代码里的唯一镜像是 [`src/input/ParamTable.h`](../../src/input/ParamTable.h)（`RINNE_PARAM` 与 `RINNE_CHOICE` 宏行）。参数在 JUCE 里挂在名为 `PARAMS` 的参数树（AudioProcessorValueTreeState）上，每个参数的 ID 写成 `juce::ParameterID{id, 1}`。

## 参数表

中文名一栏是阅读用的，界面上的叫法以界面词条为准；其余各栏与 golden 逐格对拍。宿主参数 ID 一栏同时是 VST3 与 AU 的参数 ID（两边算法相同，见下文）。

<!-- table:params -->

| 序号 | ID | 中文名 | 宿主名 | 类型 | 范围 | 默认 | 步进 | 刻度 | 自动化 | 宿主参数 ID |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `feedback` | 反馈量 | Feedback | 连续 | 0 … 150 % | 0 % | 0.1 % | 线性 | 是 | 1955982213 |
| 1 | `loop_on` | 环路 | Loop | 开关 | 关 / 开 | 开 | — | — | 是 | 349927770 |
| 2 | `mix` | 混合 | Mix | 连续 | 0 … 100 % | 25 % | 0.1 % | 线性 | 是 | 108124 |
| 3 | `delay_mode` | 时间 / 节拍 | Delay Mode | 档位 | 2 档 | Time | — | — | 否 | 500413151 |
| 4 | `delay_ms` | 环路延迟 D | Loop Delay | 连续 | 1 … 4000 ms | 100 ms | 0.001 ms | 对数 | 否 | 816162274 |
| 5 | `delay_note` | 时值 | Sync Note | 档位 | 16 档 | 1/4 | — | — | 否 | 500443438 |
| 6 | `low_cut` | 低切 | Low Cut | 连续 | 20 … 2000 Hz | 20 Hz | 0.1 Hz | 对数 | 是 | 356813527 |
| 7 | `high_cut` | 高切 | High Cut | 连续 | 1000 … 20000 Hz | 20000 Hz | 1 Hz | 对数 | 是 | 462548517 |
| 8 | `polarity` | 极性反转 | Invert Polarity | 开关 | 关 / 开 | 关 | — | — | 是 | 539953328 |
| 9 | `limiter` | 限制器 | Return Limiter | 开关 | 关 / 开 | 开 | — | — | 否 | 176117160 |
| 10 | `ceiling` | 限制器上限 | Ceiling | 连续 | -24 … 0 dB | -6 dB | 0.1 dB | 线性 | 否 | 660387005 |

- `feedback`：界面可同时显示 dB。限制器关闭时有效上限是 100 %：参数本身不夹，夹的是引擎里的有效值，界面显示「设定 x %，有效 100 %」。
- `loop_on`：关 = 回送淡出，不锁存、不清空环路。
- `mix`：中点两路全开的线性律，干声比例 min(1, 2·(1 − m))，回送比例 min(1, 2·m)；默认 25 % 时干声 100 %、回送 50 %。
- `delay_mode`：Time 用 `delay_ms`，Sync 用 `delay_note` 按宿主速度换算。
- `delay_ms`：连续可调。实际的 D 会被抬到随宿主块长变化的最小可用值、夹到上限，参数值本身不变。界面可以按样本数显示与输入；步进 0.001 ms 小于 192 kHz 下一个样本（约 0.0052 ms），按样本数设的值换成毫秒再换回来仍是同一个样本数。
- `low_cut`：下限 20 Hz 表示关（5 Hz 的隔直恒开，不是参数）。`high_cut`：上限 20000 Hz 表示关。
- `limiter`、`ceiling`：随工程保存，载入工程时不强制复位；工厂预设与用户预设都不含它们，也不含 `loop_on`。`ceiling` 的单位是 dBFS。
- 「可自动化」的原则：改变环路时序的参数与安全设置不可自动化；音量与音色可以。

## 档位

宿主的档位下标与界面桥的取值 id 一一对应，以下表为准。桥上的取值一律小写，宿主文字首字母大写，换算在参数层。已有档位的先后与桥 id 属于冻结面（正式发布之后永不改），新档只能追加在末尾：state 存的是档位下标，两个档位参数都不可自动化，追加不会挪动已存的值。

<!-- table:choices:delay_mode -->

| 序号 | 桥取值 | 宿主文字 |
|---|---|---|
| 0 | `time` | Time |
| 1 | `sync` | Sync |

`delay_note` 的 16 档按时值（拍数）严格升序，默认 1/4（序号 10）。D = round(拍数 · 60 / BPM · 采样率)。

<!-- table:choices:delay_note -->

| 序号 | 桥取值 | 宿主文字 | 拍数 |
|---|---|---|---|
| 0 | `n64` | 1/64 | 0.0625 |
| 1 | `n32t` | 1/32T | 0.0833 |
| 2 | `n32` | 1/32 | 0.125 |
| 3 | `n16t` | 1/16T | 0.1667 |
| 4 | `n16` | 1/16 | 0.25 |
| 5 | `n8t` | 1/8T | 0.3333 |
| 6 | `n16d` | 1/16D | 0.375 |
| 7 | `n8` | 1/8 | 0.5 |
| 8 | `n4t` | 1/4T | 0.6667 |
| 9 | `n8d` | 1/8D | 0.75 |
| 10 | `n4` | 1/4 | 1 |
| 11 | `n2t` | 1/2T | 1.3333 |
| 12 | `n4d` | 1/4D | 1.5 |
| 13 | `n2` | 1/2 | 2 |
| 14 | `n2d` | 1/2D | 3 |
| 15 | `n1` | 1/1 | 4 |

T 是三连音（时值的 2/3），D 是附点（时值的 1.5 倍）。

## 明文值、归一化值与刻度

- **明文值**：state 的参数块、工厂与用户预设、界面桥，一律存明文值：百分数、毫秒、赫兹、分贝；开关是 0 或 1；档位是下标。读入时按 ID 匹配，值夹进范围，缺的取默认。
- **归一化值**：只在插件与宿主之间用。设 p ∈ [0, 1]，lo、hi 是范围的两端：
  - 线性：v = lo + p·(hi − lo)；
  - 对数：v = lo·(hi / lo)^p，p = ln(v / lo) / ln(hi / lo)。中点（p = 0.5）`delay_ms` 约 63.2 ms、`low_cut` 200 Hz、`high_cut` 约 4472 Hz；
  - 开关：0 或 1；档位：p = 下标 / (档数 − 1)。
- **步进**：宿主写来的值取整到 lo + k·step，再夹进范围。默认值与上限都落在步进格上（`contract-static-params` 核对）。
- 插件界面的旋钮与滑杆用同一条刻度，宿主的通用界面与插件界面看到的位置一致。

## 宿主参数 ID

- 两个 JUCE 宏钉在根 CMakeLists.txt 里（`tests/golden/identity.json` 记着）：`JUCE_FORCE_USE_LEGACY_PARAM_IDS=0`（ID 由字符串散列，不用下标），`JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1`（清掉最高位）。
- VST3 与 AU 的参数 ID 都是 `String::hashCode(id)`（乘子 31，32 位回绕）再清最高位，参数表最后一栏就是这个数。散列的输入是不带 versionHint 的 ID 字符串。11 个数互不相同。
  - VST3 一列是实测的：单测照参数表真建 JUCE 参数对象，经 JUCE 的 `VST3ClientExtensions::convertJuceParameterId` 取 ID，与 golden 相等；同一个 ID 换个 versionHint，算出的数不变。
  - AU 一列是复算的，不是实测的：`rinne_params_tests` 不链 JUCE 的 AU 包装层，用例照 JUCE 8.0.8 的 `juce_audio_plugin_client_AU_1.mm` 里 `generateAUParameterID`（ID 的散列与清最高位）的写法重算。升级 JUCE 时要人工复核这段源码；在 mac 宿主测试里核对一次真实 AU 实例的参数列表之前，AU 一列以复算为准。
- 11 个参数的 versionHint 都是 1。AU 宿主拿到的参数列表先按 versionHint、再按这个数升序排，与序号无关：`mix`、`limiter`、`loop_on`、`low_cut`、`high_cut`、`delay_mode`、`delay_note`、`polarity`、`ceiling`、`delay_ms`、`feedback`。这个顺序同样是复算的，不是实测的：用例照同一个文件里 `GetParameterList` 的两步（先按 ID 排，再按 versionHint 稳定排序）重排参数；升级 JUCE 时与上面那段一起人工复核。插件自己的界面不受影响；宿主的通用参数列表在 AU 下按这个顺序显示。
- 以后追加的参数 versionHint 必须大于此前任何版本里的最大值（第二次加参数用 2，再下一次用 3）：它们才会排在原有参数之后。versionHint 同为 1 的新参数会按散列插进原有参数中间，部分宿主里已存的自动化随之错位。
- Output：没有参数树，也不自建参数。VST3 宿主只看到 JUCE 包装层加的旁路参数（`byps`）；AU 的旁路是宿主的旁路属性，不是参数。Input 在 VST3 下同样另有这个旁路参数，它不在上面的参数表里。两个插件都只有一个 program，VST3 包装层因此不建 program 参数。

## 冻结规则

本契约在建立它的同一个 PR 里冻结。契约守卫按 base 上的 status 判，所以冻结那一次不需要变更文档；此后的 PR 只要碰到本文档、`tests/golden/params-input.json` 或 `ParamTable.h` 的 `RINNE_PARAM` 行，同一个 PR 要新增 `docs/contract-changes/<YYYYMMDD>-<slug>.md`，或在正文写 `contract-neutral: <理由>`。

**11 个参数的 ID 任何时候都不改**，不分正式发布之前还是之后（插件身份与参数 ID 永不改，是仓内 `CLAUDE.md` 的冻结契约一节写明的）。理由：宿主按 ID 存工程与自动化，测试版与预览包也会被人存进工程，ID 一改旧工程就对不上。这一条没有变更流程可走，下面的两段都管不到它。

冻结面的其余部分分两段生效。「正式发布」从第一个公开 rc（正式版的候选包）算起，测试版与预览包都在它之前：

- **正式发布之前**：下面「冻结面的其余部分」仍可调整，但照冻结流程走：同一个 PR 附变更文档、CHANGELOG 写明，并改 `contract-static-params` 里写死的冻结面（基础设施，由维护者批准）。测试版存下的工程与自动化可能因此对不上，变更文档与 CHANGELOG 要写清对不上的是哪些。
- **正式发布之后**：冻结面（含参数 ID）永不改。碰到冻结面的 PR 另加 `status/frozen-contract` 标签，变更文档的「批准」栏填维护者确认评论的链接。

**永不改的冻结面：参数 ID**（`contract-static-params` 写死核对，宏表与 golden 各核一遍）：

- 11 个参数的 ID。不改 ID、不删参数，新参数只追加在末尾。

**冻结面的其余部分**（正式发布之后永不改，之前照上面的流程可调；同样由 `contract-static-params` 写死核对，宏表与 golden 各核一遍）：

- 11 个参数的序号与类型；
- versionHint 对全部参数都是 1；追加的参数用更大的 versionHint。机检现在只到哪一步见「真源与检查」；
- 两个档位参数已有档位的桥 id 与先后；新档只追加在末尾；
- 可自动化的 6 个参数（`feedback`、`loop_on`、`mix`、`low_cut`、`high_cut`、`polarity`）仍可自动化，连续的 4 个范围与刻度不变：宿主按归一化值记自动化，范围或刻度一变，已录的自动化就走样。

**冻结面以外，任何时候都可以改，但要带变更文档**（契约守卫要求，评审按变更文档审）：

- 默认值。改反馈安全相关的默认值（`feedback` 0、`limiter` 开、`ceiling` −6 dB）是兼容性事件，CHANGELOG 的 Compatibility 一节要写，评审按红旗审；
- 宿主名、档位的宿主文字、单位；
- 步进；
- 不可自动化参数的范围与刻度（state 存明文值，载入时夹进新范围；变更文档写清夹取后的效果）；
- 把不可自动化的参数改成可自动化；
- 追加参数或档位。追加的参数要用更大的 versionHint：宏表与 golden 现在没有逐参数的 versionHint 一栏，第一次追加参数的 PR 同时加上这一栏（宏形状一变，`scripts/check-commits.mjs` 的自测样例与 `contract-static-params` 一起改）。

**不算契约改动**：只改 `ParamTable.h` 的注释、只改本文档里不进对拍的说明文字（例如中文名一栏）。这类 PR 在正文写一行 `contract-neutral: <理由>`。

## 真源与检查

- [`src/input/ParamTable.h`](../../src/input/ParamTable.h)：宏表，参数层与测试都从它展开。
- [`tests/golden/params-input.json`](../../tests/golden/params-input.json)：每个参数一行、键序固定；顶层的 `status` 与本文档的 status 行一致。`rinne_core_tests` 把宏表渲染成规范文本与它逐字节比（设 `RINNE_PARAMS_GOLDEN_REGEN=1` 跑 `[golden]` 用例重新生成）；`rinne_params_tests` 真建 JUCE 参数对象：VST3 一列经 JUCE 的函数实测后与它比，AU 一列与 AU 列表顺序按 JUCE 8.0.8 的 AU 包装层复算后与它比（复算，升级 JUCE 时人工复核，见「宿主参数 ID」）。
- `contract-static-params`（`node scripts/run-checks.mjs --only contract-static-params`）：不编译，读宏行比 golden，读本文档的两种表与 status 行比 golden，另核写死的冻结面。每条判据有名字，`--self-test` 的每个坏样例写明要触发哪几条并核对每条判据都被点名；`--mutation-check` 证明改一个默认值、改一个 ID、调换两档时值、golden 与本文档的 status 不一致都会变红。
- versionHint 现在机检到的范围：宏表与 golden 都没有逐参数的 versionHint 一栏，golden 顶层只有一个 `versionHint` 值，`contract-static-params` 核它等于 1，`rinne_core_tests` 核同一个值；`rinne_params_tests` 在生产代码建出来的真实参数树（`InputParams`）上逐个参数核对：每个参数的 `ParameterID` 的 versionHint 都等于 golden 顶层的值，该值从 golden 读，不写常量，建参数用的具名常量也与它相等。宿主参数 ID 的那组用例（`test_host_param_ids`）自己建参数、用常量 1，它证明的只是宿主 ID 与 versionHint 无关。
