# State 格式（RNST）

status: draft

## Summary (EN)

- **What**: the bytes each Rinne instance hands to the host as its state, and how they are read back. Input stores
  parameter values and every setting; Output stores only what it needs for grouping and its own window.
- **Container**: `RNST`, abi 1, little-endian. The state is at least 16 bytes: a 16-byte header (magic, abi, role,
  chunk count ≤ 64), then chunks of `fourcc | size ≤ 64 KiB | payload | zero padding to 4 bytes`. Nothing may follow
  the last chunk. The first 16 bytes never move or change meaning; the container framing (header length, chunk header,
  padding, the meaning of the chunk count) is fixed across abi, and migrations only change payloads.
- **Payloads**: `PRMS` (Input only) is a list of `{parameter ID, f32 plain value}`, at most 256 entries when written
  and 224 when read (32 slots are left for the parameter layer); `CFGS` (both roles) is a key/value table with 11
  numbered keys, of which Output knows 1, 2, 4, 8 and 9.
- **Compatibility rules**: unknown chunks, parameter IDs and keys are kept and written back unchanged. A higher abi
  is refused and its original bytes are written back untouched, as long as the header names this end's role (role is
  compared as a whole u32: Input writes 1, Output writes 2; new values only for new ends, no flag bits). Probe
  builds' abi 0 is recognised and ignored. A lower abi is migrated one step at a time; migrations only change
  payloads. Anything else that fails leaves the instance on its current state; nothing is ever half applied.
- **State application**: the first `setState` of an instance applies everything. Later calls (host presets, A/B,
  undo) only take parameter values and crossfade via `presetSeq`; a `RejectedNewer` result on a later call is only
  reported and does not replace the bytes to write back.
- **Sources of truth**: the key table in `src/core/state/StateCodec.h`. `tests/golden/state-schema.json` is
  generated from it and the key table below must match it; both are checked by unit tests. The two baselines
  `tests/golden/state/{input,output}-abi1.bin` pin the byte layout, and the unit tests parse the byte annotations
  below and compare them with the baselines.
- **Status**: draft. It is frozen together with the parameter table.

## 范围

本文规定插件实例交给宿主保存的 state 是怎样一串字节、读回来时怎样处理。宿主把它当不透明的字节存进工程文件，
加载时原样交还。

| 真源 | 内容 |
|---|---|
| [`StateCodec.h`](../../src/core/state/StateCodec.h) | 线格式常量、PRMS 与 CFGS 的编解码、CFGS 键表 `kConfigFields` |
| [`StateDocument.h`](../../src/core/state/StateDocument.h) | 每一端认识哪些块，整份 state 的编解码 |
| [`StateMigration.h`](../../src/core/state/StateMigration.h) | 加载入口 `loadState`：abi 分流与逐级迁移 |
| [`StateMigrationTesting.h`](../../src/core/state/StateMigrationTesting.h) | 只给单测的加载入口 `loadStateWith`；处理器只经 `loadState` |
| [`state-schema.json`](../../tests/golden/state-schema.json) | 机读说明，由单测按代码生成并逐字节比较 |
| [`input-abi1.bin`](../../tests/golden/state/input-abi1.bin)、[`output-abi1.bin`](../../tests/golden/state/output-abi1.bin) | 两端的 abi 1 基线，由单测逐字节锁定 |

本文与代码不一致时以代码为准，并在同一个 PR 里改正本文。下文 CFGS 键表由单测与代码逐字比对。

## 容器

整数一律小端，与主机字节序无关；f32 按 IEEE 754 位型原样存。

```text
头 16 字节
  0   u32  magic       'RNST'（52 4e 53 54）
  4   u32  abi         本格式为 1；探针构建写 0
  8   u32  role        1 = Input，2 = Output
  12  u32  chunkCount  ≤ 64
块 × chunkCount
  u32  fourcc      写盘后前 4 个字节依次就是这四个字符
  u32  sizeBytes   ≤ 65536
  载荷 sizeBytes 字节
  补 0 到 4 字节对齐（补齐字节读入时不看内容）
最后一块之后不能再有字节。
```

- role 写在头里：Input 的字节交给 Output（或反过来）时整份拒收，不会把对端的设置套到本端。role 按整字（u32）比较：Input 恒写 1，Output 恒写 2；新值只分给新的端；不得拿 role 的高位做标志位。
- 整份 state 至少 16 字节：不足 16 字节连头都读不出来，判 `Corrupt`。前 16 字节（magic、abi、role、块数）的位置与含义永不改变，更高的 abi 也靠它们判读。
- 容器框架跨 abi 不变：头的长度（16 字节）、块头（fourcc 与 sizeBytes）、4 字节补齐、块数字段与块数的含义都不随
  abi 变。迁移只改载荷，不改框架；低 abi 的字节在新构建里先解出容器，再交给迁移函数。
- 块数与单块长度的上限在读任何块之前就生效，坏字节换不来大块内存。
- 每一端认识的块：Input = `PRMS` + `CFGS`；Output = `CFGS`。其余的块（未知 fourcc，以及 Output 收到的 `PRMS`）
  连原始字节一起保留，保存时写在已知块之后、彼此顺序不变。
- 写出的顺序固定：Input 先 `PRMS` 后 `CFGS`，Output 只有 `CFGS`，然后是保留下来的块。已知块总是写出来，
  哪怕读入时缺了它。
- 同一个已知块出现两次，整份失败。
- 读入时为写回留足余量：保留的块数加上这一端的已知块数不超过 64，否则整份失败。CFGS 给已知键留的预算见下；PRMS
  另有条数与载荷的预留（见下）。由 `decodeDocument` 解出来、没再加东西的文档一定编得出来；参数层补自己的参数不
  超过预留槽数时也一定编得回去。

## 加载

加载入口按下面的顺序判读，顺序是有意的。

| 情形 | 结果 | 实例的 state | 交回原字节 |
|---|---|---|---|
| 零字节 | `Empty` | 保持当前（第一次 setState 时即默认），不提示 | 否 |
| 不足 16 字节，或 magic 不符 | `Corrupt` | 保持当前，提示 state 未能恢复 | 否 |
| abi 高于本构建，role 是这一端 | `RejectedNewer` | 保持当前，提示工程由更新的版本保存 | 是，整份；只有第一次 setState 会原样写回 |
| abi 高于本构建，role 不是这一端 | `Corrupt` | 保持当前，提示 state 未能恢复 | 否 |
| abi 0（探针） | `IgnoredProbe` | 保持当前，不提示 | 否 |
| abi 低于本构建（≥ 1） | 逐级迁移后解码，`Migrated` | 迁移后的 state（第一次）；之后的 setState 只取参数 | 否 |
| abi 等于本构建 | 解码，`Ok` | 读入的 state（第一次）；之后的 setState 只取参数 | 否 |
| 以上任何一步之后失败 | `Corrupt` | 保持当前，提示 state 未能恢复 | 否 |

- **第一次与之后的 setState**：实例一生的第一次 setState 全量应用（参数与全部设置）；之后的 setState（宿主预设、A/B、撤销）只改参数，并走 `presetSeq` 的两段式淡变。加载表里的「保持当前」对第一次就是默认值，对之后就是实例当前的设置。
- **高 abi 先于块结构检查**：更高的 abi 可能换了载荷，按本构建的规则挑它的错，代价是把用户的新版数据丢掉。
  拒载时交回的字节与收到的逐字节相同；只有第一次 setState 会在处理器保存工程时把它们原样写回去，不用实例当前的
  state 覆盖。之后的 setState 收到 `RejectedNewer` 只提示，不替换要写回的字节：旧版载入新版存的 .vstpreset 时，
  保存工程不能拿这份旧版读不了的预设替换实例的真实设置。只有 role 照样比对：它的位置不随 abi 变，role 不符说明
  这是对端的字节，原样写回只会把它固化进本端的工程。
- **探针只认头**：探针构建写的 state 只在探针身份下有效。头之后是什么、role 写的什么都不看。
- **逐级迁移**：迁移在容器层上做，从 abi n 升到 n + 1 用第 n 个迁移函数，任何一级失败即整份失败。迁移函数只读写
  载荷，容器框架（头的长度、块头、补齐、块数）不动。迁移完再按本构建的规则解码：低版本的载荷未必过得了本构建的
  载荷校验，先迁移才不会把能迁移的旧工程当成坏字节；迁移之后再核对一遍容器上限（块数、单块长度），越界判
  `Corrupt`。abi 1 是第一版，还没有迁移函数。
- **不留半份状态**：失败与忽略时加载入口不碰出参，调用方传进去的是什么（第一次 setState 时是新实例的默认 state，
  之后是实例当前的 state），出来还是什么。只有 `Ok` 与 `Migrated` 会写出参。表中的提示由处理器按结果给出。
- **坏字节不交回**：`Corrupt` 的字节不该被原样存回工程文件，下一次保存写的是实例当前的 state。

## PRMS：参数值（只有 Input）

```text
u32 count（写回 ≤ 256；读入 ≤ 224）
count × { u8 idLen | id（idLen 字节）| f32 值 }
最后一条之后不能再有字节。
```

- 存参数的明文值（不是 0..1 的归一化值），而且只存 saved 值。
- id 是 1..255 字节的可打印 ASCII（0x21..0x7E），不含空格；id 重复时「按 id 匹配」有歧义，整份失败。
- 本层只搬运：按 id 匹配、夹到合法范围、缺失的补默认、非有限值的处理都归参数层。不认识的 id 原样留在列表里、
  顺序不变，保存时写回。
- 读入时给参数层留出写回余量：条数不超过 224、载荷不超过 57216 字节（64 KiB − 32 × 260），否则整份失败。32 是
  Input 参数个数的上界（参数表当前 11 个，留到 32 覆盖翻倍还有余）。参数层在解出来的列表上补自己的参数，最多再补
  32 个槽；一个槽按 1 字节 idLen + 255 字节 id + 4 字节值共 260 字节计，补满预留槽之后仍不超过 256 条与 64 KiB。
  多补的参数由参数层自己受参数表约束，不靠本层兜底。

## CFGS：非参数设置

```text
重复到载荷结束：{ u16 key | u16 len | bytes[len] }
条与条之间不补齐，也没有总条数。
```

键号是线格式：只能追加，不能改号或复用。

| 键 | 名字 | 类型 | 长度（字节） | 范围 | 默认 | 不合法时 | Input | Output |
| --: | --- | --- | --- | --- | --- | --- | :-: | :-: |
| 1 | `group` | u8 | 1 | 0..8 | 0 | 取默认 | 是 | 是 |
| 2 | `group_origin` | u8 | 1 | 0..2 | 0 | 取默认 | 是 | 是 |
| 3 | `reset_on_play` | u8 | 1 | 0..1 | 1 | 取默认 | 是 | — |
| 4 | `tail_mode` | u8 | 1 | 0..2 | 0 | 取默认 | 是 | 是 |
| 5 | `delay_unit` | u8 | 1 | 0..1 | 0 | 取默认 | 是 | — |
| 6 | `fb_unit` | u8 | 1 | 0..1 | 0 | 取默认 | 是 | — |
| 7 | `guard_on` | u8 | 1 | 0..1 | 1 | 取默认 | 是 | — |
| 8 | `ui_scale` | u16 | 2 | 33..300 | 100 | 夹到范围内 | 是 | 是 |
| 9 | `ui_lang` | ascii | ≤ 8 | — | 空串 | 取默认 | 是 | 是 |
| 10 | `preset_name` | utf8 | ≤ 96 | — | 空串 | 取默认 | 是 | — |
| 11 | `preset_dirty` | u8 | 1 | 0..1 | 0 | 取默认 | 是 | — |

取值：

- `group`：0 = 未设，1..8 = 界面上的 A..H。
- `group_origin`：0 = 未定，1 = 用户选的，2 = 智能分组给的。
- `reset_on_play`、`guard_on`、`preset_dirty`：0 = 关，1 = 开。
- `tail_mode`：0 = 自动（VST3 报无限长，AU 报 30 秒），1 = 无限长，2 = 30 秒。数值与插件外壳里的同名枚举逐个相同。
- `delay_unit`：0 = 毫秒，1 = 样本。`fb_unit`：0 = 百分比，1 = dB。
- `ui_scale`：界面缩放的百分比。
- `ui_lang`：语言代码，只有字母、数字与连字符（`zh`、`en`、`fr`、`zh-CN` 这一类）；空串 = 未设。
- `preset_name`：当前预置的名字，合法 UTF-8（过长编码、代理区码点、超出 U+10FFFF 都算坏），不含 C0 控制字符与 DEL。

规则：

- **缺失的已知键取默认**；写回时这一端的已知键一个不少，按键号升序写在前面。
- **已知键长度不对整份失败**：定长键的长度必须正好是 1 或 2；字符串键不能超过上限。同一个已知键出现两次也整份失败。
- **已知键的值不合法只纠正、不失败**：枚举、开关与组号取默认，`ui_scale` 夹到 33..300（0 也夹到 33，不是「未设」），
  字符串内容不合规取空串。写出时按同一套规则纠正；预置名超长时在 UTF-8 字符边界上截到上限，语言代码超长时写空串
  （截短了就是另一个代码）。
- **未知键原样保留**：这一端不认识的键（包括只有对端才认识的键，例如 Output 收到的键 3）连原始字节一起保留，
  写在已知键之后、彼此顺序不变；不认识的键重复出现照收。
- **读入时为写回留足余量**：未知条目的总字节数加上这一端全部已知键按最长的值写出来的字节数（Input 158，Output 33）
  不超过 65536，否则整份失败。

不进工程的设置：「引导已看过」、规则页确认过的版本和诊断日志开关是本机设置，存在全局的界面默认值里。

## 基线

两份基线由单测里的示例 state 编码而来：编码结果与文件逐字节相同，读回来还原那份示例。示例里每个 CFGS 键都取与默认
不同的值，另带一个未知参数、一个未知 CFGS 键和一个未知块。参数值只为钉住线格式，单位与范围以参数表为准。下面的
逐字节注解由 `[state][golden]` 用例解析：每一行的偏移与字节都要与 bin 相同，`output-abi1.bin` 的注解还必须完整
覆盖全部 60 字节；加载表也由同一个用例按 `state-schema.json` 的 `load` 数组逐行对照。

`output-abi1.bin`（60 字节）：

```text
偏移  字节                         含义
0000  52 4e 53 54                  magic 'RNST'
0004  01 00 00 00                  abi 1
0008  02 00 00 00                  role 2（Output）
000c  01 00 00 00                  1 块
0010  43 46 47 53                  'CFGS'
0014  21 00 00 00                  33 字节
0018  01 00 01 00 02               group = 2
001d  02 00 01 00 02               group_origin = 2（智能）
0022  04 00 01 00 02               tail_mode = 2（30 秒）
0027  08 00 02 00 96 00            ui_scale = 150
002d  09 00 02 00 65 6e            ui_lang = "en"
0033  c8 00 02 00 5a a5            未知键 200，原样保留
0039  00 00 00                     补齐
```

`input-abi1.bin`（284 字节）节选：

```text
偏移  字节                                      含义
0000  52 4e 53 54 01 00 00 00 01 00 00 00 03 00 00 00
                                                头：'RNST'、abi 1、role 1（Input）、3 块
0010  50 52 4d 53 9f 00 00 00                   'PRMS'，159 字节
0018  0c 00 00 00                               12 个参数
001c  08 66 65 65 64 62 61 63 6b 00 00 96 42    "feedback" = 75.0
0029  07 6c 6f 6f 70 5f 6f 6e 00 00 80 3f       "loop_on" = 1.0
      …                                         其余参数，最后一个是未知的 "future_param"
00b7  00                                        补齐
00b8  43 46 47 53 50 00 00 00                   'CFGS'，80 字节
00c0  01 00 01 00 02                            group = 2
      …                                         键 2..11，按键号升序
0109  c8 00 03 00 aa bb cc                      未知键 200，原样保留
0110  58 54 52 41 03 00 00 00 01 02 03 00       未知块 'XTRA'，3 字节加 1 字节补齐
```

重新生成：设环境变量 `RINNE_STATE_GOLDEN_REGEN=1` 跑 `[golden]` 用例，再审一遍 diff。

## 改格式

- 加新的 CFGS 键、新的块或新的参数不必升 abi：旧版把它们当未知项原样保留。新键只能用没用过的键号。
- 前 16 字节（magic、abi、role、块数）不能动，容器框架（头的长度、块头、补齐、块数的含义）跨 abi 不变；迁移只
  改载荷。
- 改已有载荷字段的布局或含义必须升 abi：把 `kAbi` 加一，在 `kMigrators` 末尾追加一个从旧 abi 升上来的迁移函数，
  新增一份新 abi 的基线；旧的基线保留，作迁移测试的输入。迁移函数只读写载荷，容器框架不动。
- 已冻结之后，改到本文、`src/core/state/` 或 `tests/golden/state*` 的 PR 要按契约变更的规程走。
