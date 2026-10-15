# 探针日志格式（v1）

探针构建写出、`analyze-probe.py` 严格读入的 CSV 契约。分析器拒绝任何不符合本文的文件（退出码 2），所以探针一侧照本文写，不必猜分析器的宽容度。机读版本是 `analyze-probe.py --spec`，`checks.d/probe-analysis` 把本文与它对拍：本文漏列任何一个行类型、事件、case 或 variant 都会判红。合成样例在 `fixtures/`，由 `probe_synth.py` 生成。

## 文件

- 每个探针实例（一个 Input 或一个 Output）一个文件：`probe-<role>-<pid>-<tok4>.csv`，`role` 为 `I` 或 `O`。目录位置见探针构建的说明；分析器只读目录里 `probe-*.csv` 与 `compare-*.csv`，别的文件忽略。
- 纯 ASCII，行尾只能是 `LF`（出现 `CR` 判格式错）。字段用逗号分隔，没有引号与转义，所以任何字段都不得含逗号；文本字段一律百分号编码（见「文本字段」）。
- 每行写完才算一行：最后一行没有换行视为写到一半被截断，分析器丢掉它，并把该实例判为未正常收尾（A8 不通过）。
- 第 1 行是魔数 `rinne-probe-log,1`，第 2 行是唯一的 `H` 行，之后是下表的各类行，顺序不限，`Z` 行出现后不得再有内容。

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

`H` 的取值：`role` ∈ `I` `O`；`format` ∈ `vst3` `au`；`host_kind` ∈ `cubase` `luna` `other`；`os` ∈ `win` `mac`；`arch` ∈ `x64` `arm64`；`pid` 与 `ppid` 是整数；`tok4` 是 4 位小写十六进制；`build` 是 8 位小写十六进制（提交散列前 8 位）。

`C` 的取值：`case` 与 `variant` 取下面两张表里的值；`buf` 是缓冲区大小（样本）或 `-`；`guard` ∈ `off` `low` `normal` `high` `na`（ASIO-Guard 档位，不适用写 `na`）；`sr` 是采样率。同一个 `(case, variant, buf, guard)` 叫一个「格」，In 与 Out 两端对同一个格各写一段，分析器靠这个键把两端配起来。重做某一格时换一个 variant 或先清掉旧文件，不要在同一键下重复记录。

`B` 的列：

| 列 | 含义 |
|---|---|
| `seq` | 本实例的块序号，从窗口内第一块起每块加 1，严格递增；丢一条记录就出现缺口，分析器据此判「数据不全」 |
| `call_seq` | 全局回调顺序号（共享内存头里 `fetch_add` 得到），严格递增，用来还原 In / Out 的交错 |
| `role` | 必须与 `H` 的 `role` 相同 |
| `n` `n_decl` | 本次回调样本数；`prepare` 时宿主声明的最大块长 |
| `host_ns` `host_ns_valid` | 宿主给的时间戳及有效位；无效时 `host_ns` 写 `0` |
| `playing` `nonrt` `bypassed` | 走带状态；是否离线渲染；是否走了 `processBlockBypassed` |
| `mono_ns` | 本块开始时的单调时钟 |
| `thread` | 线程标识的 8 位小写十六进制散列，不是原始线程号 |
| `m` | 读头与期望位置的偏差（样本，有符号），稳态应恒为 0 |
| `w` `off` | 写位置；读头相对写位置的偏移（样本，有符号） |
| `rs` | 本块内发生的重同步次数（Input 一侧计，Output 写 0） |
| `d` | 当前生效的环路延迟 D（样本） |

`S` 的列：`blocks` 是累计记录块数，`dropped` 是累计丢弃数，`max_n` 是见过的最大块长，`resync` 是累计重同步，`trips` 是累计安全跳闸（Panic 除外）。

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
| `track_props` | 轨道名（百分号编码，可空） | 颜色（`RRGGBB` 或 `RRGGBBAA`，可空） | 宿主送来轨道信息；每个 `C` 行之后立即写一条当前快照，从未送达则两项都空 |
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
| `CB-01` | Cubase：缓冲 64 / 256 / 1024 乘 ASIO-Guard 关 / 低 / 常规 / 高，每格稳态播放 30 s |
| `CB-02` | Cubase：同一条轨反复开关输入监听 |
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
| `LU-03` | LUNA：缓冲 64 / 512 / 2048 乘录音使能开关 |
| `LU-04` | LUNA：离线与实时 Export Mixdown、Bounce、Freeze |
| `LU-05` | LUNA：旁路与停用 |
| `LU-06` | LUNA：轨道名与颜色，AU 与 VST3 各一次 |
| `LU-07` | LUNA：Core Audio 模式下的可用性 |
| `LU-08` | LUNA：沙盒宿主下的进程隔离（可选） |
| `LU-09` | LUNA：保存重开 |

`variant` 取值，`-` 表示不区分：

| 取值 | 用在 |
|---|---|
| `-` `steady` | 稳态播放 |
| `toggle` | 监听或录音使能反复切换 |
| `reopen` `dup` `preset` `fxchain` `undo` `rename` | 保存重开、复制轨道、轨道预设、FX Chain、删除撤销、改名与颜色 |
| `export-rt` `export-offline` `bounce` `freeze` `rip` `dop` | 实时导出、离线导出、Bounce、Freeze、Render in Place、Direct Offline Processing |
| `bypass-in` `bypass-out` `deactivate` `disable-track` `smart` | 旁路、停用、禁用轨道、Smart Plug-in Processing |
| `guard-in` `guard-out` `guard-both` | 只对 In、只对 Out、对两者关闭 ASIO-Guard |
| `misplaced-order` `misplaced-track` `misplaced-post` | 错放：Out 在 In 前、Out 跨轨、Out 在推子后 |
| `mono` `bufchange` `constrain` `twoproj` `coreaudio` | 单声道轨、播放中改缓冲、Constrain Delay Compensation、两个工程、Core Audio 模式 |
| `au-au` `vst3-vst3` `au-vst3` | LU-02 的三种配对组合（In 格式在前） |

## 文本字段与隐私

- 唯一允许携带人写内容的字段是 `track_props` 的 `a`（轨道名），必须百分号编码：保留 `A-Za-z0-9._~-`，其余字节写成 `%XX`，至多 200 个字符。原始 CSV 因此可能含轨道名，视为私有数据，不进公开仓。
- 其余字段都是整数、固定枚举或十六进制，没有路径、用户名、主机名的位置；`thread` 是散列。探针不得把这些信息塞进任何字段。
- 分析器的输出不含这些信息：轨道名只给 `same` / `different` / `unknown` 与一个 8 位散列（带每次运行随机的盐，只用于同一份报告内比较）；格式错误只报文件序号、行号和固定的列名；全文再过一遍路径、用户名、主机名的兜底清洗。

## 导出比对文件

`compare-<任意>.csv`，第 1 行是表头 `case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples`，其后每行一次 `rinne_nulltest compare` 的结果，由人抄入：`case` 与 `variant` 同上，`bit_exact` 为 bool，两个残差是 dBFS 小数或 `-inf`，`lag_samples` 是有符号整数。`head_residual_dbfs` 是开头 2 s 单列的残差，只报告、不参与判定。

## 判据概览

| 项 | 内容 | 用到的 case |
|---|---|---|
| A1 | 稳态对齐：播放中 `m` 恒为 0，首块之后 `rs` 为 0，`off` 恒定。不通过即停工 | CB-01 CB-03 LU-03 |
| A2 | 切换切片：每次切换后 2 s 内重同步不超过 1 次，`d` 不变，至少 10 次切换 | CB-02 LU-03 |
| A3 | 导出一致：离线与实时逐位相同或残差不高于 -120 dBFS | CB-04 LU-04 |
| A4 | 块长：每块的 `n` 不超过它之前最近一次 `prepare` 声明的最大块长，也不超过 `n_decl` | 所有带块记录的格 |
| A5 | 进程与后端：两端同 pid、已配对、无 `shm_denied`，三种格式组合齐全 | LU-02 |
| A6 | 轨道信息：两端都收到非空轨道名 | CB-07 LU-06 |
| A7 | hostNs：同一回调两端相等，逐回调递增，有效率不低于 99% | 所有两端都有块的格 |
| A8 | 稳定性：每个实例都正常收尾、摘要间隔不超过 5 s、无跳闸 | 全部 |
| A9 | 工程恢复：`reopen` 格里 `set_state` 之后出现 `paired` | CB-07 LU-09 |

任何一项里，用到的段有序号缺口、块数不足 32 或缺必需的数据，该项判「数据不全」。冻结条件 F1 要求 CB-01、CB-02、CB-04、CB-05、LU-02、LU-03、LU-04 都有数据且无缺口，A1 至 A8 都有结论；F2 至 F4 由人确认。
