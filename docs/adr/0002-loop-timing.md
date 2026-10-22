# ADR-0002：环路时序

## Summary (EN)

- **Decision**: address the loop by stream-relative positions, not the host timeline; heads only advance, D is clamped.
- **Consequences**: reported latency is always zero; output is bit-identical however the host chops blocks.
- **Compatibility**: changing the delay range, note order, fade lengths or the order criterion is a compat event.

## 状态

已采纳（2026-10-22）。

## 背景

宿主的时间线不可信：块长随宿主与导出方式变化，两端先后不固定，宿主时间戳只在部分时刻有效。按时间线寻址会让延迟随切块方式漂移。

## 决定

### 寻址与 D

- 读位 R 与写头 W 只单调前进，差值一律有符号（W 越过 2^64、位置为负都如此）。只读 `[max(validFrom, lineValidFrom), min(pulled, validTo))` 内的帧，越界读成 0。
- 配对或清空时本地延迟线锚在当前 W，每块拉到 W（一圈之内）；混音用块首的 R。
- 两端上报给宿主的延迟恒为 0。
- `D_min = max(N_prep, N_obs)`，违约的那一块（n > N_prep）当场计入；`D_eff = clamp(D_user, D_min, 4 s × sr)`。「本块是不是有界」是块级事实，由环路层按块长判、结果放在块结果里，不进参数面。
- 写环按 ≤ 8192 帧分段，由引擎自己切。

### 时值档位

16 档按拍数升序、严格递增（1/4 拍为 1）：.0625 .0833 .125 .1667 .25 .3333 .375 .5 .6667 .75 1 1.333 1.5 2 3 4，默认下标 10，只能追加。按音符族分组的清单（桥契约与页面预览）有三处逆序（1/16D、1/8D、1/4D 分别排在 1/8T、1/4T、1/2T 前），以本表为准。BPM 有效范围 [20, 999]，无效时沿用最后一个有效值，从未有过按 120。

### 去抖

改 D 用 40 ms 线性双读头交叉淡化（两路增益和恒为 1；相关信号上等功率会过冲），目标变化按 100 ms 去抖（按样本计）。常量在 `loop/LoopTiming.h`。

### 错序判据

- 五个量逐块发布：m = W − (R + off)（正 = 写头领先）、w、off、rs、d = D_eff。
- 同一周期两端拿同一个宿主时间戳。Input 块首读 Output 槽的时间戳：不小于本块的，说明 Output 已处理本块，记一票；更小则判正确并清零。连续 3 票定为错序。时间戳无效或对端未发布时记 unknown，不猜。

### 淡变与复位

- 重同步：旧读头 5 ms 淡出（预拷贝尾巴），新数据从首个有效位置起 20 ms onset。断档 5 ms、对端消失 10 ms 淡出。
- 起播：旧回送淡出 T = min(5 ms, D_eff)；本地延迟线在沿上复位（沿后 T 帧之前不再读）；DSP 复位点是绝对样本号 s0 + T。
- 容忍模式：10 s 内 ≥ 3 次对齐重同步（跳变、持续漂移）。两端停用 > 250 ms 清空本地延迟线；同采样率重 prepare 保持流连续；`reset()` 只记诊断。

## 后果

全部按样本定位：参数恒定时输出与切块逐位无关，起播复位点也与切块无关。

## 被否决的备选

- 按宿主时间线寻址：延迟随切块漂移。
- 起播在「淡出走完的那一块」才清本地延迟线：复位点随切块漂移，还会把新读头一起作废。
