# ADR-0002：环路时序

## Summary (EN)

- **Decision**: the loop is addressed by stream-relative positions, not by the host timeline. Read and write heads only advance; the delay D is clamped into a legal range.
- **Consequences**: the latency reported to the host is always zero; output is bit-identical regardless of how the host chops blocks; changing D goes through a linear two-tap crossfade plus a debounce window.
- **Compatibility**: changing the delay range, the note table order, the crossfade length or the debounce window is a compatibility event.

## 状态

已采纳（2026-10-22）。

## 背景

宿主给的时间线不可信：块长随宿主、缓冲与导出方式变化，两端的处理先后也不固定；宿主时间戳还只在部分格式与部分时刻有效。按时间线寻址会让延迟随切块方式漂移，导出与实时就对不上。

## 决定

### 流相对寻址与 D 的范围

- 读位 R（本地已处理帧数）与写头 W（对端已写总帧数）只单调前进；差值一律走有符号运算，W 越过 2^64、读位置为负、读位置超出已拉取区间都按有符号算。
- 读到的样本只落在 `[max(validFrom, lineValidFrom), min(pulled, validTo))` 之内，越界读成 0，绝不读旧数据或别名数据。
- 两端向宿主上报的延迟恒为 0：宿主会用自己的延迟补偿把上报值旁路掉，报上去只会引入错位。
- `D_min = max(N_prep, N_obs)`；`D_cap = 4.0 s × sr`；`D_eff = clamp(D_user, D_min, D_cap)`。宿主送来的块长超过声明值时立即抬升下限并补 0。
- **「本块是不是有界」不是参数**：违约与超长是块级事实，由环路层按块长 n 判定（n 大于声明值补 0 并抬升下限；n 大于 32768 判超长），结果由块结果的标志给出。参数面不装块级事实。
- 写环按 ≤ 8192 帧分段，**分段由引擎自己做**：宿主给 32768 帧时切成 4 段，写头照常推进，调用方不切段。

### 时值档位顺序

`delay_note` 共 16 档（下标 0..15），拍数以 1/4 拍为 1，三连 = 直值 × 2/3，附点 = 直值 × 3/2。拍数逐档为：.0625、.0833、.125、.1667、.25、.3333、.375、.5、.6667、.75、1、1.333、1.5、2、3、4。

- **顺序按拍数升序，严格递增。默认档是 1/4（下标 10）。**顺序冻结：只能追加新档，不能改已有档位的含义、次序或拍数；要改就是兼容性事件。
- 桥契约与页面预览另有一份按「音符族分组」排列的清单，与本表有三处真逆序（1/16D 在 1/8T 前、1/8D 在 1/4T 前、1/4D 在 1/2T 前）。**以本表为准**。
- 节拍同步：`D = round(beats · 60/BPM × sr)`。BPM 有效范围 [20, 999]；无效时沿用本会话最后一个有效值，从未拿到过就按 120 并提示。

### 去抖语义

- **改 D 时用 40 ms 线性双读头交叉淡化**：新旧两个读头同时在跑，增益是 `(1−x)` 与 `x`，两路增益之和恒为 1。**不用等功率交叉**：两路信号相关时等功率会抬高约 3 dB，线性不过冲。
- **按 100 ms 去抖**：重新定目标必须距上一次定目标 ≥ 100 ms（按样本计）；窗口内的目标变化被丢掉，不新开读头。
- 三个读头槽保证「淡化进行中又遇到重同步」时旧的两路能一起淡出、新读头能进空槽，不会硬丢读头。40 ms 与 100 ms 都是契约的一部分，常量在 `loop/LoopTiming.h`。

### 错序判据

- 五个量的定义与符号：`m` 漂移量 `m = W − (R + off)`，正数 = 写头领先、负数 = 读位跑到写头前面；`w` 写头 = Output 已写总帧数；`off` 读头的同步偏移；`rs` 重同步次数；`d` 实际环路延迟 D_eff（样本）。m / w / off 的当前值逐块发布在状态对象里。
- **判据：宿主时间戳有效时，连续 3 次检查都判为 Output 在 Input 之前，才定为错序**（界面给琥珀提示）。口径是「谁的时间戳小谁在前」：Output 的时间戳比 Input 小，说明 Output 那一端的块首更早被处理。
- 宿主时间戳无效（没给，或给了 0）时一律记 unknown，票数清零；**不猜、不用别的时间源代替**。票数是连续计数：一次判为正确就清零。

### 本层的时序决定

- 重同步：旧读头 5 ms 淡出（预拷贝的尾巴），新数据 20 ms onset。断档：5 ms 淡出。起播：5 ms 淡出旧回送，淡出长度取 `min(5 ms, D_eff)`；复位点给的是绝对样本号，于是「导出与播放从 s0 + T 起逐位一致」有样本级的依据。
- 两端停用 > 250 ms 清空本地延迟线。容忍模式：10 s 内 ≥ 3 次重同步。同采样率重新 prepare 保持流连续；`reset()` 只记诊断。

## 后果

块长不变性由「按样本计数」保证：参数恒定时，同一段输入按不同块长处理，逐位相同。
