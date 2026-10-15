# 探针日志格式（v1）

探针构建写出、`analyze-probe.py` 严格读入的 CSV 契约。分析器拒绝任何取值不合本文的行（退出码 2），所以探针一侧照本文写，不必猜分析器的宽容度。机读版本是 `analyze-probe.py --spec`，`checks.d/probe-analysis` 把本文与它对拍：行类型及其列名与列序、H 的枚举取值、事件名、全部 case、variant、guard、阈值与取值上界，本文漏列、改序或写错任何一项都会判红。事件参数的类型与含义不自动对拍，由 `fixtures/` 的严格解析兜底。合成样例在 `fixtures/`，由 `probe_synth.py` 生成。

## 文件

- 每个探针实例（一个 Input 或一个 Output）一个文件：`probe-<role>-<pid>-<tok4>.csv`，`role` 为 `I` 或 `O`。目录位置见探针构建的说明；分析器只读目录里 `probe-*.csv` 与 `compare-*.csv`，别的文件忽略。
- 纯 ASCII，行尾只能是 `LF`（出现 `CR` 判格式错）。字段用逗号分隔，没有引号与转义，所以任何字段都不得含逗号；文本字段一律百分号编码（见「文本字段」）。
- 每行写完才算一行。最后一行没有换行视为写到一半被截断，分析器丢掉它，并把该实例判为未正常收尾（A8 不通过）。文件是空的、只有写到一半的首行、或没有合法的 `H` 行，同样不算格式错误：这是实例刚创建就崩溃的样子，分析器记入 A8 不通过，其余文件照常分析。非法取值才是格式错误，整次分析退出码 2。
- 第 1 行是魔数 `rinne-probe-log,1`，第 2 行是唯一的 `H` 行，之后是下表的各类行，`Z` 行出现后不得再有内容。
- 块与事件归属于哪个 `C`，分析器按时间戳（`mono_ns`、`t_ns`）算，不看它们在文件里的先后；所以不同线程写出的行在文件里交错是允许的。

## 行类型

第一列是行类型，其余列按表中顺序。整数一律十进制无前导零（`0` 除外）；`bool` 为 `0` 或 `1`；时间 `t_ns` 与 `mono_ns` 是同一个单调时钟的纳秒数。

| 类型 | 含义 | 列 |
|---|---|---|
| `H` | 实例头，恰好一行，必须在第 2 行 | `role` `format` `host_kind` `os` `arch` `pid` `ppid` `tok4` `build` |
| `C` | 开始一个测试格（case 上下文），之后到下一个 `C` 之前的块与事件都属于它 | `t_ns` `case` `variant` `buf` `guard` `sr` |
| `B` | 逐块记录，只在「开始记录」窗口内写 | `seq` `call_seq` `role` `n` `n_decl` `host_ns` `host_ns_valid` `playing` `nonrt` `bypassed` `mono_ns` `thread` `m` `w` `off` `rs` `d` |
| `S` | 1 Hz 摘要，窗口外也写 | `t_ns` `blocks` `dropped` `max_n` `resync` `trips` |
| `E` | 事件 | `t_ns` `name` `a` `b` |
| `Z` | 正常收尾，实例析构时写 | `t_ns` `blocks_total` `dropped_total` |

`H` 的取值：`role` ∈ `I` `O`；`format` ∈ `vst3` `au`；`host_kind` ∈ `cubase` `luna` `other`；`os` ∈ `win` `mac`；`arch` ∈ `x64` `arm64`；`pid` 与 `ppid` 是不超过 4294967295 的整数；`tok4` 是 4 位小写十六进制；`build` 是 8 位小写十六进制（提交散列前 8 位）。

`C` 的取值：`case` 与 `variant` 取下面两张表里的值；`buf` 是缓冲区大小（样本，不超过 65536）或 `-`；`guard` ∈ `off` `low` `normal` `high` `na`（ASIO-Guard 档位，不适用写 `na`）；`sr` 是不超过 768000 的采样率。

**格与同键重复。** 同一个 `(case, variant, buf, guard)` 叫一个「格」，In 与 Out 两端对同一个格各写一段，分析器靠这个键把两端配起来。一个实例可以在一个文件里连续写很多格（每格一个 `C` 行）。同一个键下不得出现第二份记录：某格做坏了，就记下来，等该实例整体重采一遍（关掉插件再开，生成新文件），并把旧文件挪走。分析器遇到同键重复（`rip`、`freeze`、`dup`、`undo`、`twoproj` 除外，这几种会多出实例是预期内的）时不会静默挑一份，而是把相关判据判为数据不全、在输出的「提示」里列出，并让 F1 不满足。

**快照。** 配对、后端、状态恢复这类事件往往发生在用户选 case 之前。分析器把第一个 `C` 行之前的事件归为实例级，A5 与 A9 会同时看实例级与分段内的事件。为了让每一格自含，探针在每个 `C` 行之后立即补写一遍当前状态的快照：`prepare`、`backend`、`paired`、`shm_denied`（如发生过）、`set_state`（如发生过）、`track_props`。从未发生过的就不写，只有 `track_props` 例外：从未送达则写 `a` 与 `b` 都为空的一条。原事件照常在发生时写，快照是**额外的**事件，它的 `t_ns` 是补写的时间；A9 按 `t_ns` 比较 `set_state` 与 `paired` 的先后，所以只写快照而不写原事件的实现会让顺序失真。第一个 `C` 之前出现的 `B` 行无法归入任何格，分析器计数、列入「提示」并让 F1 不满足，所以探针应当先取时间戳、写 `C` 行，再打开块记录：`C` 行的 `t_ns` 不得晚于该窗口第一块的 `mono_ns`。违反时分析器不中止整次分析：那块被计为孤块（落在第一个 `C` 之前），或被归进上一格，前者会列入提示并让 F1 不满足。

`B` 的列：

| 列 | 含义 |
|---|---|
| `seq` | 实例级的块序号，只对实际写出的 `B` 行递增，窗口之间不归零、也不为没有记录的回调递增；全文件严格递增。丢一条记录就在 `seq` 上留下缺口，分析器据此判「数据不全」 |
| `call_seq` | 全局回调顺序号（共享内存头里 `fetch_add` 得到），严格递增，用来还原 In / Out 的交错 |
| `role` | 必须与 `H` 的 `role` 相同 |
| `n` `n_decl` | 本次回调样本数；`prepare` 时宿主声明的最大块长；都不超过 1048576 |
| `host_ns` `host_ns_valid` | 宿主给的时间戳及有效位；无效时 `host_ns` 写 `0` |
| `playing` `nonrt` `bypassed` | 走带状态；是否离线渲染；是否走了 `processBlockBypassed` |
| `mono_ns` | 本块开始时的单调时钟 |
| `thread` | 线程标识的 8 位小写十六进制散列，不是原始线程号 |
| `m` | 读头与期望位置的偏差（样本，有符号），稳态应恒为 0。Input 与 Output 都写；Output 写自己的写位置与期望位置的偏差 |
| `w` `off` | 写位置；读头相对写位置的偏移（样本，有符号）。Output 的 `off` 写它的写位置相对时间线的偏移。稳态下 `off` 恒定，分析器对两端都这样要求 |
| `rs` | 本块内发生的重同步次数（Input 一侧计，Output 写 0） |
| `d` | 当前生效的环路延迟 D（样本，不超过 2147483648） |

`S` 的列：`blocks` 是累计记录块数，`dropped` 是累计丢弃数，`max_n` 是见过的最大块长，`resync` 是累计重同步，`trips` 是累计安全跳闸（Panic 除外）。`S` 行应由探针自己的日志线程每秒写一条，不要依赖宿主的消息线程；若实现上依赖它，宿主在导出、载入工程、扫描插件时长时间不跑消息线程，摘要就会出现长间隔。分析器把这种情形报成「摘要间隔超限」，与「日志没有收尾」分开说，阈值 5 s 是 1 Hz 摘要的 5 倍，足以容忍偶发抖动，又远短于一次真正的卡死。

丢弃一律要计数：任何丢记录的路径都要增加 `S` 的 `dropped`。分析器据此把受影响的格按数据不全处理：`seq` 缺口落在哪一格就是哪一格，没有缺口可定位时整个实例的各格都按数据不全，并在「提示」里说明。

## 事件

`E` 的 `a` 与 `b` 因事件而异；不用的写空串。未列出的事件名判格式错。

| 事件 | `a` | `b` | 何时写 |
|---|---|---|---|
| `prepare` | 采样率 | 声明的最大块长 | `prepareToPlay` / `setupProcessing` |
| `release` | | | 释放资源 |
| `reset` | | | `reset` |
| `set_processing` | bool | | `setProcessing` |
| `offline_render` | bool | | AU 离线渲染切换 |
| `set_state` | 组 `A`-`H` 或 `-` | | 恢复状态 |
| `get_state` | | | 保存状态 |
| `first_block` | | | 本实例的第一次回调 |
| `first_paired_block` | | | 配对后的第一次回调 |
| `paired` | 对端 pid | 组 `A`-`H` | 配对成功 |
| `track_props` | 轨道名（百分号编码，可空） | 颜色（`RRGGBB` 或 `RRGGBBAA`，可空） | 宿主送来轨道信息，另见「快照」 |
| `backend` | `win32` 或 `posix` | errno，成功写 `0` | 打开共享内存后 |
| `shm_denied` | | | 共享内存被拒 |
| `monitor_toggle` | 切换后的状态 bool | | 监听开关 |
| `arm_toggle` | 切换后的状态 bool | | 录音使能开关 |
| `process_mode` | `realtime` 或 `offline` | | 宿主的处理模式 |
| `ping` | 实测环路延迟（样本） | | 一次 Ping 结束 |
| `destruct` | | | 实例析构 |

## 测试格

`case` 取值与含义：

| case | 测什么 |
|---|---|
| `CB-01` | Cubase：缓冲 64 / 256 / 1024 乘 ASIO-Guard 关 / 低 / 常规 / 高，共 12 格，variant 都是 `steady`，每格稳态播放 30 s |
| `CB-02` | Cubase：同一条轨反复开关输入监听（variant `toggle`） |
| `CB-03` | Cubase：只对 In、只对 Out、对两者关闭 ASIO-Guard |
| `CB-04` | Cubase：实时与离线导出、Freeze、Render in Place、Direct Offline Processing |
| `CB-05` | Cubase：旁路、停用、禁用轨道 |
| `CB-06` | Cubase：Smart Plug-in Processing 下的静音段 |
| `CB-07` | Cubase：复制轨道、保存重开、轨道预设、FX Chain 预设、删除撤销、改轨道名与颜色 |
| `CB-08` | Cubase：错放（Out 在 In 前、Out 跨轨、Out 在推子后） |
| `CB-09` | Cubase：单声道轨 |
| `CB-10` | Cubase：播放中改缓冲、改采样率 |
| `CB-11` | Cubase：Constrain Delay Compensation |
| `CB-12` | Cubase：同时开两个工程并切换 |
| `LU-01` | LUNA：解除隔离与签名后扫描、加载 AU 与 VST3 |
| `LU-02` | LUNA：AU 一对、VST3 一对、AU In 配 VST3 Out |
| `LU-03` | LUNA：缓冲 64 / 512 / 2048 共 3 格（variant `steady`），另有录音使能开关的切换（variant `toggle`） |
| `LU-04` | LUNA：离线与实时 Export Mixdown、Bounce、Freeze |
| `LU-05` | LUNA：旁路与停用 |
| `LU-06` | LUNA：轨道名与颜色，AU 与 VST3 各一次 |
| `LU-07` | LUNA：Core Audio 模式下的可用性 |
| `LU-08` | LUNA：沙盒宿主下的进程隔离（可选） |
| `LU-09` | LUNA：保存重开 |

`variant` 取值，`-` 表示不区分。**规则按 (case, variant) 查表，不按 variant 单独查**；表外的组合（例如 `CB-01` 配 `deactivate`）在解析时判格式错，所以每个 case 允许哪些 variant 就是下面这张表。一格要不要两端成对、各至少 32 块，取决于它喂给的判据是否需要两端的稳态块数据，而不是按 case 一刀切；分析器读同一张表（`--spec` 的 `rules`），run-checks 逐行对拍。

- 类别 1「成对满块」：两端各恰有一个实例、各至少 32 块，缺了判数据不全。A1 按 case 判（`CB-01`、`CB-03`、`LU-03` 里除 `toggle` 外的每一格都进 A1，variant 叫什么都一样）；`CB-10` 的 `bufchange` 只喂 A4，不喂 A1 与 A7，因为中途换缓冲后 `m` 与 `off` 本来就会变。A4 与 A7 的「通过」必须至少有一个类别 1 的格得出结论。
- 类别 2「各自证据」：不要求成对和块数，但「不要求」不等于「没数据也算过」，每格必须有表里写的证据，一条都没有就判数据不全。这类格的证据不挂在 A 编号下，而是列在输出的「格观察」里；它们对 A4 只能给不通过或说明，不能单独撑出通过。`CB-05` 在 F1 里，所以它的格观察数据不全会挡住 F1。
- 类别 3「只记录」：不阻断，只留一条说明。
- `dup`、`undo`、`twoproj`、`rip`、`freeze` 允许同键多对实例，每个实例都要有证据：一个配好的实例不能掩盖没配好的。F1 的同键重复只看 F1 所列 case 里类别 1 的格。

| case | variant | 类别 | 喂给 | 本格自己的证据 |
|---|---|---|---|---|
| `CB-01` | `steady` | 1 成对满块 | A1 A4 A7 | 两端各至少 32 块 |
| `CB-02` | `toggle` | 1 成对满块 | A2 A4 A7 | 两端各至少 32 块，每次切换后的窗口里至少 8 块 |
| `CB-03` | `guard-both` | 1 成对满块 | A1 A4 A7 | 两端各至少 32 块 |
| `CB-03` | `guard-in` | 1 成对满块 | A1 A4 A7 | 两端各至少 32 块 |
| `CB-03` | `guard-out` | 1 成对满块 | A1 A4 A7 | 两端各至少 32 块 |
| `CB-04` | `dop` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `CB-04` | `export-offline` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `CB-04` | `export-rt` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `CB-04` | `freeze` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `CB-04` | `rip` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `CB-05` | `bypass-in` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 bypassed 为 1 的块 |
| `CB-05` | `bypass-out` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 bypassed 为 1 的块 |
| `CB-05` | `deactivate` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 set_processing（a 为 0）或 release 事件 |
| `CB-05` | `disable-track` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 set_processing（a 为 0）或 release 事件 |
| `CB-06` | `smart` | 2 各自证据 | 格观察 | 至少两条 ping 事件（前后各一次） |
| `CB-07` | `dup` | 2 各自证据 | 格观察 | 每个 In 实例都有 paired 事件，指向本格内某个 Out 的 pid |
| `CB-07` | `fxchain` | 3 只记录 | 格观察 | 只记录，不阻断 |
| `CB-07` | `preset` | 3 只记录 | 格观察 | 只记录，不阻断 |
| `CB-07` | `rename` | 2 各自证据 | A6 | 每个实例都收到非空轨道名 |
| `CB-07` | `reopen` | 2 各自证据 | A9 | set_state 之后出现 paired |
| `CB-07` | `undo` | 2 各自证据 | 格观察 | 每个 In 实例都有 paired 事件，指向本格内某个 Out 的 pid |
| `CB-08` | `misplaced-order` | 2 各自证据 | 格观察 | 至少一端有记录；paired 有无见格观察 |
| `CB-08` | `misplaced-post` | 2 各自证据 | 格观察 | 至少一端有记录；paired 有无见格观察 |
| `CB-08` | `misplaced-track` | 2 各自证据 | 格观察 | 至少一端有记录；paired 有无见格观察 |
| `CB-09` | `mono` | 1 成对满块 | A4 A7 | 两端各至少 32 块 |
| `CB-10` | `bufchange` | 1 成对满块 | A4 | 两端各至少 32 块 |
| `CB-11` | `constrain` | 1 成对满块 | A4 A7 | 两端各至少 32 块 |
| `CB-12` | `twoproj` | 2 各自证据 | 格观察 | 按 paired 的对端 pid 逐对检查：每个 In 都配到本格内的 Out，每个 Out 都被配到 |
| `LU-01` | `-` | 3 只记录 | 格观察 | 只记录，不阻断 |
| `LU-02` | `au-au` | 2 各自证据 | A5 | 同 pid、已配对、无 shm_denied |
| `LU-02` | `au-vst3` | 2 各自证据 | A5 | 同 pid、已配对、无 shm_denied |
| `LU-02` | `vst3-vst3` | 2 各自证据 | A5 | 同 pid、已配对、无 shm_denied |
| `LU-03` | `steady` | 1 成对满块 | A1 A4 A7 | 两端各至少 32 块 |
| `LU-03` | `toggle` | 1 成对满块 | A2 A4 A7 | 两端各至少 32 块，每次切换后的窗口里至少 8 块 |
| `LU-04` | `bounce` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `LU-04` | `export-offline` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `LU-04` | `export-rt` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `LU-04` | `freeze` | 2 各自证据 | A3 A4 | 导出比对行见 A3；块只查块长上限，不进 A7 |
| `LU-05` | `bypass-in` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 bypassed 为 1 的块 |
| `LU-05` | `bypass-out` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 bypassed 为 1 的块 |
| `LU-05` | `deactivate` | 2 各自证据 | 格观察 | 至少一端 32 块，且有 set_processing（a 为 0）或 release 事件 |
| `LU-06` | `rename` | 2 各自证据 | A6 | 每个实例都收到非空轨道名 |
| `LU-07` | `coreaudio` | 2 各自证据 | 格观察 | 有 backend 事件或任一事件，不要求块 |
| `LU-08` | `-` | 3 只记录 | 格观察 | 只记录，不阻断 |
| `LU-09` | `reopen` | 2 各自证据 | A9 | set_state 之后出现 paired |

## 文本字段与隐私

- 唯一允许携带人写内容的字段是 `track_props` 的 `a`（轨道名），必须百分号编码：保留 `A-Za-z0-9._~-`，其余字节写成 `%XX`。编码前至多 200 字节，超出的部分由探针在 UTF-8 字符边界截断后再编码；分析器对仍然超长的轨道名也只是截断到 200 字节、不判格式错（它只用于比较和算散列）。原始 CSV 因此可能含轨道名，视为私有数据，不进公开仓。
- 其余字段都是整数（带上界）、固定枚举或十六进制，没有路径、用户名、主机名的位置；`thread` 是散列。探针不得把这些信息塞进任何字段。
- 分析器的输出不含这些信息：轨道名只给 `same` / `different` / `unknown` 与一个 8 位散列（带每次运行随机的盐，只用于同一份报告内比较）；格式错误只报文件序号、行号和固定的列名；内部错误只报固定的一句话，不打印 traceback；全文再过一遍路径、用户名、主机名的兜底清洗，运行时可以用 `--redact <词>`（可重复）补充要清洗的词，例如另一台机器的用户名与主机名。输出固定为 UTF-8。

## 导出比对文件

`compare-<任意>.csv`，第 1 行是表头 `case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples`，其后每行一次 `rinne_nulltest compare` 的结果，由人抄入：`case` 与 `variant` 同上，`bit_exact` 为 bool，两个残差是 dBFS 小数或 `-inf`，`lag_samples` 是有符号整数。`bit_exact` 为 1 时残差必须是 `-inf`，反之亦然，否则判格式错。`head_residual_dbfs` 是开头 2 s 单列的残差，只报告、不参与判定。

每个 case 需要的行：`CB-04` 需要 variant 为 `export-offline` 的行（离线对实时的比较）；`LU-04` 需要 `export-offline` 或 `bounce` 的行。其余 variant（`freeze`、`rip` 等）的行只作记录。参与判定的行里，`lag_samples` 不为 0 判不通过：错位的两份导出对齐之后再比较会掩盖问题。

## 判据概览

| 项 | 内容 | 用到的 case |
|---|---|---|
| A1 | 稳态对齐：每个稳态格（variant 不是 `toggle`）两端都在，且各自至少 32 块；播放中 `m` 恒为 0，首块之后 `rs` 为 0，`off` 恒定。不通过即停工。variant 为 `toggle` 的格归 A2，输出里会说明有几格未计入 | CB-01 CB-03 LU-03 |
| A2 | 切换切片：variant 为 `toggle` 的格，每次切换后 2 s 内（或到下一次切换为止）重同步不超过 1 次，`d` 不变；每个窗口至少 8 块，每个 case 至少 10 次切换 | CB-02 LU-03 |
| A3 | 导出一致：离线与实时逐位相同或残差不高于 -120 dBFS，且 `lag_samples` 为 0 | CB-04 LU-04 |
| A4 | 块长：每块的 `n` 不超过它之前最近一次 `prepare` 声明的最大块长，也不超过 `n_decl`；类别 1 的格两端都在、各至少 32 块，其余格单个实例各查各的，只能给不通过或说明；通过要求至少一个类别 1 的格得出结论 | 所有带块记录的格 |
| A5 | 进程与后端：两端同 pid、已配对、无 `shm_denied`，三种格式组合齐全 | LU-02 |
| A6 | 轨道信息：variant 为 `rename` 的格，逐个实例检查都收到非空轨道名 | CB-07 LU-06 |
| A7 | hostNs：同一回调两端相等，逐回调递增，有效率不低于 99%；两端交错成对且都在播放、hostNs 有效的回调至少 32 个 | 所有两端都有块的格 |
| A8 | 稳定性：每个实例都正常收尾、摘要间隔不超过 5 s、无跳闸；不完整的日志文件记不通过 | 全部 |
| A9 | 工程恢复：`reopen` 格里 `set_state` 之后出现 `paired`（实例级事件也算） | CB-07 LU-09 |

任何一项里，用到的段有序号缺口、块数不足 32、同键重复或缺必需的数据，该项判「数据不全」；可信数据里出现的不通过压过别处的数据不全。

**格观察**不带 A 编号、不阻断，`--json` 里是单独的 `observations` 键。它逐格给出类别 2 / 3 的本格证据，并把采到的数据算出来，对应计划里不在 A1 至 A9 内的几项，只观察不判定：

- 同通道顺序：所有类别 1 的格里，按 `call_seq` 是 In 先于 Out 的回调占比（`order.in_first` / `order.pairs`）。
- 旁路（`bypass-in`、`bypass-out`）：两端 `bypassed` 为 1 的块数，即走了 `processBlockBypassed` 的块；Output 的 `w` 按 `seq` 排序后相邻两块的停滞次数（相等）与回退次数（变小）。
- Smart（`smart`）：In 端块之间最长的回调间隙（ms）、重同步总次数（`rs` 之和，其中含 stall_resume，日志不区分原因）、`ping` 条数。
- 错放（`misplaced-*`）：两端都有块时给成对数、In 先于 Out 的数、hostNs 相等的数和两端 `off` 的取值；只有一端有块写「无法判断」。

**冻结条件 F1** 要求：`CB-01`、`CB-02`、`CB-04`、`CB-05`、`LU-02`、`LU-03`、`LU-04` 都有能下结论的数据且无缺口（`CB-04`、`LU-04` 可只靠导出比对行，`LU-02` 看配对与后端事件，其余要有块）；`CB-01` 的 12 格与 `LU-03` 的 3 格稳态矩阵一格不缺；没有无法归格的块、没有同键重复（只看上述 case 里类别 1 的格）；F1 所列 case 的格观察没有数据不全；A1 至 A8 都有结论。F2 至 F4 由人确认。

## 退出码与机读字段

| 退出码 | 含义 |
|---|---|
| 0 | A1 至 A9 全部通过，且 F1 满足 |
| 1 | 有不通过 |
| 2 | 输入取值不合格式，或用法错误 |
| 3 | 没有不通过，但有数据不全，或 A1 至 A9 虽然都过、F1 因缺数据没满足 |
| 4 | 内部错误 |

F1 因判据不通过而不满足时退出码是 1。现场先跑一次（宿主还开着）时 A8 与 F1 不作数，退出码多半是 3，只看 A1 一行。`--json` 输出里 `stop` 为真表示 A1 不通过，要停工上报；`items[].verdict` 取 `PASS` / `FAIL` / `INCOMPLETE`；`f1`、`warnings`、`extras` 同 Markdown 输出里的对应段。
