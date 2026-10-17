# Rinne 架构总览

## Summary (EN)

How Rinne is layered, which thread does what, how the return path is protected, and the nine safety invariants
with the tests that hold each one. The safety chain and state machine live in `src/core/engine/SafetyMachine.h`;
the rationale is in [ADR-0003](adr/0003-safety-model.md). Weakening any invariant is a release blocker.

本文是分层、线程模型、回送安全与九条安全不变量的真源。每条不变量都对应测试，测试名写在第 6 节；
削弱其中任何一条都按红旗处理。设计取舍见 [ADR-0003](adr/0003-safety-model.md)。

## 1. 分层

| 层 | 内容 | 规则 |
| --- | --- | --- |
| `rinne_core`（STATIC，C++17） | IPC、环路、全部 DSP、安全状态机、state、诊断 | 不 include、不链接 JUCE；能脱离宿主做单测，sanitizer 也只跑这一层 |
| `rinne_plugin_common` | WebView 宿主、桥、Fallback 面板、JUCE 胶水 | 两个插件共用的 JUCE 代码只放这里 |
| `src/input`、`src/output` | 两个 JUCE 外壳 | 只做映射：参数 → `EngineParams`，playhead → `TransportInfo`，然后调用引擎；互不 include，不写 DSP 与安全逻辑 |

`rinne_core` 按子目录组织：`dsp/` 是无状态或单一职责的 DSP 原件（清洗、DC 阻断、限制器、软削波、平滑器、
10 ms 格上的滑动统计）；`engine/` 是把原件串起来、带状态的部分（安全状态机与安全链、测量量、安全事件）。
配置的唯一真源是 Input 的 state；Output 只有组与配对。

## 2. 线程模型

| 线程 | 做什么 | 与别的线程怎么交换 |
| --- | --- | --- |
| 音频线程（`processBlock`） | 读环、回送链、安全状态机、写环 | 每块开头读一次参数与命令序号；块末尾把测量量写进原子量、把本块的安全事件交给引擎 |
| 消息线程 | 参数、state、配对与心跳、界面桥 | 只写参数与命令序号，只读测量量（峰值字段读走即清零） |

- 音频线程不加锁、不分配、不做 I/O、不读墙钟、不抛异常。跨线程只用无锁原子量
  （`static_assert(is_always_lock_free)`）与单生产者单消费者队列。
- 一切计时按样本数：采样率在 prepare 时注入，平滑、淡变、限制器释放、判据窗口都换算成样本。宿主怎么切块不影响结论。
- Panic 与恢复不是参数：界面让命令序号加一，音频线程在下一块开头比对、执行一次。实例第一块只记下序号，不执行，
  所以重载工程不会重放旧命令。

## 3. 回送链

每个样本按固定顺序走。安全链分前后两段，音色级（低切、高切、极性）夹在中间：

```text
读头 → ① 清洗 → ② DC 阻断 5 Hz → ③ 低切 → ④ 高切 → ⑤ 极性 → ⑥ ×g → [超大值检测]
     → ⑦ 峰值限制器 → ⑧ 软削波 → ⑨ 有限性检查 → + g_dry·干声 → 输出（也就是送进环路的信号）
```

| 级 | 规格 |
| --- | --- |
| ① 清洗 | 非有限值 → 0（计数，不跳闸）；钳 ±16；绝对值小于 1e-30 的冲成 0 |
| ② DC 阻断 | 一阶高通 `y = x − x₁ + R·y₁`，`R = exp(−2π·5/sr)`，恒开 |
| ⑥ 增益 | `g = 平滑30ms(min(feedback, 上限)·g_ret) × 平滑10ms(loop_on·未旁路) × g_safety × g_guard`；反馈上限：限制器开 1.5，关 1.0；平滑终点精确 |
| 超大值 | 电平 ≥ +12 dBFS 持续 100 ms（相邻两个超限样本相隔不超过 10 ms 算连续）→ 跳闸。电平取 `max(\|tap\|, \|x\|)`，但 \|tap\| 只在链增益估计 > 0 dB 时计入，\|x\|（×g 之后）只在链增益估计 > 0 dB 或 g > 1 时计入；g = 0（反馈为 0、环路关、旁路）时都不计。理由见第 4 节 |
| ⑦ 限制器 | 峰值、左右联动、无前视；起音瞬时，释放一阶 80 ms；上限 c 为 −24…0 dBFS，默认 −6，30 ms 平滑；末级钳 ±c；开关时 10 ms 线性交叉 |
| ⑧ 软削波 | `\|x\| ≤ 0.7` 逐位直通；之上 `0.7 + 0.3·tanh((\|x\| − 0.7)/0.3)`；末级钳到小于 1.0 的最大 float；逐位不放大 |
| ⑨ 有限性检查 | 前后两段之间（音色级）或之后出现非有限值：本样本输出 0，立即跳闸（NaN） |

「限制器开时 ≤ c」的精确含义：限制器刚打开的 10 ms 交叉里，输出介于限制前后之间，仍受软削波 < 1.0 约束；
交叉完成后逐样本 ≤ c。上限改动时 c 按 30 ms 线性平滑，期间以当前的 c 为界。

接线约定（引擎负责）：前后两段每个样本都要调用，快速路径也不能跳过，恢复冷却与「等首个有效读样本」靠它计时；
每块把当前环路延迟 D 交给安全链（链增益估计的窗口要盖住它；改 D 的交叉淡化期间给两个读头中较大的 D），并把
已乘 g_dry 的干声交给后段（本样本送进环路的信号 = 干声 + 回送，只用来估计链增益）；读到跳闸淡出完成的清空请求后清空本地延迟线；
起播沿复位时调用 `playEdgeReset`。

## 4. 安全状态机

对外的状态同时满足几种时按 TRIPPED > UNPAIRED > STALLED > FADE_IN > GUARD > RUN 报告。

| 状态 | 回送 | 进入 | 离开 |
| --- | --- | --- | --- |
| UNPAIRED | 10 ms 淡到 0 | 没有对端或读不了环 | 配上 → FADE_IN(first_pair) |
| FADE_IN | 从运行中进入时先 10 ms 降到 0；再从首个有效读样本起 500 ms 线性升到 1 | 实例准备好、重新配对、换写方、载入预设、setState、恢复 | 走完 → RUN |
| RUN | 1 | — | 由事件驱动 |
| STALLED | 不动（断档的 5 ms 淡出由环路层做） | 写头不动 | 写头恢复 |
| GUARD | g_guard 按下面的规则变化 | 失控判据成立 | g_guard 回到 0 dB |
| TRIPPED(panic、nan、huge、runaway) | 10 ms 淡到 0，然后请求清空延迟线，锁存 | Panic、Input 内部非有限值、超大值、失控 | 只有冷却 1.5 s 之后的恢复，或重载工程 |

- **锁存**优先于配对：失去又恢复配对、载入预设、setState、换写方、重 prepare 都不清锁存。
- **恢复**只有一个入口，切断后 1.5 s 冷却；冷却里的恢复直接丢掉、不排队，不存在「N 秒后自动恢复」。
  已切断时再按 Panic 不会恢复，只重新计冷却。冷却按样本计：宿主不调用音频回调时冷却不走，界面按状态字里的冷却位显示。
- **失控保护（GUARD，`guard_on` 默认开）**：3 s 窗（10 ms 格上的 300 个整格加正在累计的一格，实际 3.00–3.01 s）内
  限制器增益缩减 ≥ 6 dB 的累计时长 ≥ 2 s 进入；
  限制器关时改为软削波样本占比 ≥ 50 %。判据成立期间 g_guard 以 −6 dB/s 下降，最低 −24 dB；连续平静 3 s 后以
  +3 dB/s 回升，回到 0 dB 退出。压到 −24 dB 后又命中满 1 s → TRIPPED(runaway)。`guard_on` 关时不计数，
  已压低的部分按 +3 dB/s 放回。
- **防误报门**：反馈 ≤ 90 % 且链增益 ≤ 0 dB 时，失控保护一个命中都不计，零介入。
- **链增益估计**（防误报门与超大值共用）：每 10 ms 格记一次回读与送出信号的峰值，估计 = 最近 100 ms 回读的峰值 ÷
  覆盖 [0, D + 1 s] 时延的送出峰值。只依赖样本计数；峰值历史是对象里的定长环（1024 格 = 10.24 s，盖住 D 最大
  4000 ms + 1 s + 100 ms），音频线程上不分配。
  - 回读样本都来自窗口覆盖的那段送出信号，所以恒等链与衰减链下估计必然 ≤ 1（D 取 1 ms 与 4000 ms 两端、
    块长随机切分都有测试）。
  - 多出的 1 s 容纳环内插件自己的延迟；超过 1 s 时估计可能偏大，只会让防误报门打开、让超大值多计，都偏保守。
  - D 变大立即用；D 变小时旧的较大 D 继续用，直到旧内容移出回读窗（旧 D + 1 s + 100 ms）且 D 不再变化，
    所以交叉淡化期间按两个读头中较大的 D 计。
  - 送出为 0 而回读不为 0 时估计为 +∞、判为放大：坏插件在静音时自己吐出大信号也要能跳闸。
- **超大值只计回送路径自己放大出来的电平**：干声随整条通道进环，恒等链下热干声绕一圈回来就是 \|tap\|，绝对阈值
  单看 tap 会对热轨道误跳；链不放大且 g ≤ 1 时环路增益 ≤ 1、\|x\| ≤ \|tap\|，同理。实测：若无条件计入
  max(\|tap\|, \|x\|)，+12 dBFS 正弦干声、反馈 90 %、恒等链的 9 格（110 / 1000 / 3150 Hz × D 为 480 / 4800 /
  7777 样本）里有 8 格误跳。设计同时要求「带干声 +12 dBFS、反馈 90 % 不误报」，加上这两个条件后两者都满足：
  上面 9 格、+18 dBFS 干声、反馈 0 / 90 % 在两种混合下、反馈 150 % 加混合 25 %（g = 0.75）都不跳；
  链内 +24 dB 加反馈 20 %、静音时环内插件吐 +40 dBFS、混合 50 % 下反馈 101 % 与 110 % 的热信号都在 100 ms 级跳闸；
  反馈为 0 时链内 +24 dB 不跳，调到反馈 > 0 后 100 ms 跳。
- **NaN**：环内插件产生的 NaN 由 Output 写环前清洗，Input 的第 ① 级再清一次，只计数、点琥珀灯，不跳闸；
  Input 内部产生的非有限值立即 TRIPPED(nan)。
- **起播沿**：环路层在起点淡出旧回送、清空延迟线之后复位安全链：DSP 状态清零、平滑器落到目标、判据与失控保护
  清零；原因是 load 或 first_pair 的淡入直接置满（这时回送为 0，没有可听的跳变），其余原因的淡入照常走完。

### 止啸路径

| 用户动作 | 回送归零 | 是否清空延迟线 |
| --- | --- | --- |
| Panic | ≤ 1 块 + 10 ms | 是 |
| `loop_on` 关、Input 旁路 | ≤ 1 块 + 10 ms | 否 |
| Output 旁路 | ≤ D + 10 ms | 否 |

窗口关着时最快的办法是旁路 Rinne Input。

## 5. 测量量

`engine/Meters.h`，音频线程每块写一次：回送直流均值（清洗后、DC 阻断前，时间常数 300 ms）、回送贴近满幅
（×g 之后 |x| ≥ 0.7）的连续时长、限制器增益缩减（峰值字段，界面读走即清零）、失控保护当前的压低量、
打包的安全状态（状态、跳闸原因、淡入原因、冷却位）、环里读到的非有限样本累计数。
它们只供安全灯与失控保护判据使用，不参与任何别的音频决策。

## 6. 九条安全不变量

| # | 不变量 | 守住它的测试（Catch2 用例名） |
| --- | --- | --- |
| 1 | 任何失败只会让回送变成 0，干声照常 | `a non-finite value produced inside the Input trips immediately`；`panic fades out in 10 ms, asks for one flush, and stays latched`；`losing the pairing fades out in 10 ms; re-pairing fades in as first_pair`；`an unprepared machine gives zero gain`；`stereo audio passes through bit for bit at every block size` |
| 2 | 永不读旧数据或别名数据 | `panic fades out in 10 ms, asks for one flush, and stays latched`（跳闸后清空延迟线）；读位下界与撕裂判定的用例随环路层与 IPC 层的代码一起入库，届时补进这一格 |
| 3 | 回送有硬上界：\|ret\| < 1.0；限制器开时 ≤ 上限 | `return stays below 1.0 and within the ceiling for random and extreme input`；`a million parameter jumps never make the chain diverge`；`soft clip stays finite and strictly below 1.0f at the extremes`；`soft clip never expands, is odd and monotonic over a dense sweep`；`limiter output never exceeds the ceiling, bit for bit` |
| 4 | 默认安全：反馈默认 0；新配对、换写方、载入预设、恢复之后，都从首个有效读样本起 500 ms 淡入 | `safety controls default to the safe settings`；`a new instance stays silent until paired, then fades in over 500 ms from the first valid sample`；`entering a fade-in from RUN ramps down over 10 ms before the 500 ms ramp, without a step`；`a fade-in after the ramp-down still waits for the first valid read sample`；`recover is refused during the 1.5 s cooldown and never queued` |
| 5 | 音频线程不加锁、不分配、不做 I/O、不读墙钟 | `test_safety_chain.cpp` 开头的静态断言（安全链可平凡析构、不持有堆内存，处理函数 noexcept）与 `Meters.h` 的无锁断言；`stereo audio passes through bit for bit at every block size`（回调内零分配） |
| 6 | 两端向宿主上报的延迟恒为 0 | `latency is always zero` |
| 7 | 参数恒定时，输出与宿主怎么切块逐位无关 | `output and state transitions do not depend on how the host splits blocks`；`stereo audio passes through bit for bit at every block size` |
| 8 | Output 永不改动宿主 buffer（逐位直通） | `stereo audio passes through bit for bit at every block size`；`mono audio passes through bit for bit at every block size` |
| 9 | 环头与标签头只由该槽的属主写 | 由 IPC 层的测试守住，随该层代码入库时补进这一格 |
