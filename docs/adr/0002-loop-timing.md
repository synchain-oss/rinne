# ADR-0002：环路时序

## Summary (EN)

- **Decision**: address the loop by stream-relative positions, not the host timeline; heads only advance, D is clamped; delay changes crossfade linearly over 40 ms. Order evidence needs 3 consecutive per-block votes and tolerates timestamp regressions; export mismatch is a count against a UI-acknowledged value.
- **Consequences**: reported latency is always zero; output is bit-identical however the host chops blocks.
- **Compatibility**: changing delay range, note order, fades, alignment or order criteria is a compat event.

## 状态

已采纳（2026-10-22）。

## 背景

宿主时间线不可信：块长、两端先后、时间戳有效性随宿主而变。

## 决定

### 寻址与 D

- 读位 R 与写头 W 只单调前进，差值一律有符号。只读 `[max(validFrom, lineValidFrom), min(pulled, validTo))` 内的帧，越界读成 0。
- 配对或清空时本地延迟线锚在当前 W，每块拉到 W（一圈之内）；混音用块首的 R。上报给宿主的延迟恒为 0。
- `D_min = max(N_prep, N_obs)`，违约的那一块当场计入，超长块（n > 32768）不计入，环路不走，回送块首起 5 ms 淡出、下一块从 0 淡回；`D_eff = clamp(D_user, D_min, 4 s × sr)`。

### 时值档位

16 档按拍数升序（1/4 拍为 1）：.0625 .0833 .125 .1667 .25 .3333 .375 .5 .6667 .75 1 1.333 1.5 2 3 4，默认下标 10，只追加，越界取默认；桥契约与预览的清单有三处逆序，以此为准。D = 拍数 × 60 / BPM × sr，四舍五入。BPM 取 [20, 999]，无效沿用上一个有效值，从无才按 120 并置 noTempo。与当前差不足 1 个样本不改；超 4000 ms 截断并置 clipped。

### 改 D：交叉淡化与去抖

- 新读头增益 k/L（L = 40 ms），旧读头 1 − k/L，逐样本之和恰为 1。
- 去抖为前沿式、按样本计的 100 ms 窗口：窗外第一次改动在该样本立即开始；窗内只留最新目标，到期与主读头不同则再淡一次。
- 起点起主读头（对外报的 D）就是新读头，估计窗口取较大的 D；其有效起点取旧读头的有效起点与自己首个可读位置中较晚者，晚于后者时带 20 ms onset。读头槽（3 个）满时顺延到下一块。
- 淡化中遇到重同步：两个读头各自淡出，尾巴各占一个槽。起播、换采样率、清域之后 D 直接设，不淡。

### 对齐判据

- m = W − (R + off)，off 为锚定偏移，正确摆放时恒为 0。
- 严格模式下 m 连续两次检查不为 0 才重同步；|m| > max(两端声明的最大块长, 8192) 立即重同步。改块长或 Output 晚一块，m 会回到 0，不重同步。

### 错序判据

- 两端同周期拿同一个宿主时间戳，每块评一次（Input 读 Output 槽，Output 镜像读 Input 槽，判法相反）：连续 3 次 Output 在前定为错序，连续 3 次 Input 在前才清除。
- 评不了的块清零连续计数、结论不变，按样本计满 1 s 无有效评估回到 unknown；时间戳回退或前跳超过 0.25 s 的块不投票，只计数。

### 导出不一致

- 离线块里宿主违约、主读头补 0、对端新块报超限、重同步（起播、绑定切换、首次对齐不算）任一件，该块计一次；干净导出不计。
- 音频线程只累加计数；已读值在消息线程，不等即锁存。

### 淡变与复位

- 重同步：旧读头 5 ms 淡出，新数据起 20 ms onset。断档 5 ms、对端消失 10 ms 淡出。数据不够一个淡出时缩短到恰好用完；写头落后取不到样本时先淡到 0 再补 0。
- 起播：旧回送淡出 T = min(5 ms, D_eff)；本地延迟线在沿上复位；DSP 复位点是绝对样本号 s0 + T。
- 容忍模式：10 s 内 ≥ 3 次对齐重同步。两端停用 > 250 ms 清空本地延迟线；同采样率重 prepare 保持流连续。

## 后果

按样本定位：参数恒定时输出与切块逐位无关。

## 被否决的备选

按宿主时间线寻址（延迟随切块漂移）；起播等淡出走完再清本地延迟线（复位点漂移）。
