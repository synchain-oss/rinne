# ADR-0003：安全模型

## Summary (EN)

- **Decision**: a fixed safety chain on the return path with hard bounds (|ret| < 1.0, ≤ ceiling with the limiter on), a sample-counted state machine with latched trips, a 500 ms fade-in from the first valid sample, and runaway protection.
- **Consequences**: no step in any gain; trips only clear on Recover or reload; thresholds are compatibility events.
- **Compatibility**: changing a default, bound or fade time must be listed under Compatibility in the changelog.

## 状态

已采纳（2026-10-17）。

## 背景

反馈环路一旦失控，几秒内就能伤耳、伤音箱；环里还有第三方插件，可能输出 NaN、直流或巨大的值。宿主切块方式不固定，导出与实时必须一致，所以安全逻辑不能依赖块长或墙钟。

## 决定

- **固定顺序**：清洗 → DC 阻断 5 Hz → 音色级 → ×g → 超大值检测 → 峰值限制器 → 软削波 → 有限性检查。限制器无前视（不增加延迟），末级钳 ±上限；软削波末级钳到小于 1.0，且逐位不放大。限制器关时反馈有效上限 100 %。
- **按样本计时**：全部时长换算成样本数，平滑终点精确；参数恒定时输出与切块无关。
- **淡变无阶跃**：从运行中进入淡入先 10 ms 降到 0，再从首个有效读样本起 500 ms 上行；失去配对与跳闸 10 ms 淡出；环路开关与旁路 10 ms；反馈 30 ms。
- **锁存**：Panic、内部非有限值、超大值、失控都跳闸，淡出后清空延迟线并锁存；锁存优先于配对，只有冷却 1.5 s 后的恢复或重载工程能清，冷却中的恢复不排队。
- **失控保护**：3 s 内限制器压 ≥ 6 dB 累计 2 s（限制器关时软削波占比 50 %）进入，−6 dB/s 下降，平静 3 s 后 +3 dB/s 回升，压到 −24 dB 仍命中 1 s 跳闸。反馈 ≤ 90 % 且估计的链增益 ≤ 0 dB 时一个命中都不计：热信号进稳定环路时限制器也会压得很重，只看增益缩减会误报。
- **NaN 分工**：环内插件的 NaN 由 Output 清洗、只计数；Input 内部的非有限值立即跳闸。

## 后果

- 引擎每个样本都要走安全链（快速路径也不能跳过），每块交出环路延迟 D，并处理清空请求。
- 阈值与时长写在 `SafetyMachine.h` 的常量里，每个都有边界两侧的测试；改动按兼容性事件处理。
- 失控保护的链增益是估计值：带相位变化的环内链可能被判成放大，此时只会多介入，不会少保护。

## 被否决的备选

- **前视限制器**：要向宿主报延迟，与两端上报延迟恒为 0 冲突。
- **NaN 后自动恢复**：故障源还在时会反复爆音，并让用户误以为问题已解决。
- **只靠增益缩减判失控**：热干声在稳定环路里同样压满限制器，误报太多。
- **按块计时**：块长随宿主、缓冲与导出方式变化，判据跟着变，导出与实时就不一致。
