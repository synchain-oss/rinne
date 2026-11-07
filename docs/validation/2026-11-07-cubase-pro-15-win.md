# Cubase Pro 15（Windows）实测记录：2026-11-07

## Summary (EN)

- **Host / OS**: Cubase Pro 15.0.5 (internal version 15.0.5.114), Windows 10 Pro 22H2 (10.0.19045), x64; project at 48 kHz; VST3.
- **Build**: 0.9.0-preview.2, commit `51bec023` (a preview build, not a release build).
- **Verdict**: a Cubase insert effect placed between the two ends (MonoDelay, delay 10 ms) is inside the loop, and its delay adds to the loop delay: with D at 4800 samples the echo interval is 5280 = 4800 + 480, all 9 intervals, deviation 0 samples. The first echo is at the expected level (−6.94 dBFS). The interface after the font and Output layout changes (fonts, Output states, placement hint, repeated Panic clicks, three languages, zoom fallback, window opening) was reported normal. The seven-item acceptance matrix was not repeated on this build; it is covered by the record of 2026-11-03.

## 环境

| 项 | 记录 |
|---|---|
| 宿主与版本 | Cubase Pro 15.0.5（内部版本 15.0.5.114）。版次与版本号于 2026-11-07 在 Help → About 核对 |
| 系统与架构 | Windows 10 专业版 22H2（10.0.19045），x64 |
| 格式 | VST3 |
| 采样率 | 48 kHz（工程设置） |
| 导出格式 | WAV，32 bit float，立体声，不加抖动，不归一化；离线导出（不勾 Real-Time Export） |
| 构建 | 0.9.0-preview.2，提交前 8 位：`51bec023`（预览构建） |
| 上机日期 | 2026-11-07 |
| 分析工具 | 随包的 `rinne_nulltest`（`echo` 量回声间隔） |

## 环内放宿主自带的插入效果器

目的：两端之间放一个 Cubase 自带的延迟，用回声间隔判断它在不在环里。在环里，每一圈就多出它的延迟。

设置：

- 缓冲 256，ASIO-Guard 为 Normal。
- Rinne Input：反馈量 50 %、混合 50 %，D 设为 4800 样本（100 ms），其余默认。
- 测试信号：`echo-in.wav`（9 秒，432000 帧，1.5 秒处即第 72000 个样本一个单冲激）。
- 链：第 1 格 Rinne Input，第 2 格 Cubase 自带的 MonoDelay（Delay 10 ms，Mix 100 %；按测试步骤 Sync 关、Feedback 0），第 3 格 Rinne Output，都在推子分隔线之上；回送在导出里照常出现，说明两端仍然配对。
- 导出：Audio Mixdown，离线，文件名 `cb-builtin.wav`。

结果（命令：`rinne_nulltest echo --in cb-builtin.wav --d 5280 --min-peak-db -60 --sr 48000`，PASS）：

| 量 | 值 |
|---|---|
| 录音 | 48000 Hz，2 声道，432000 帧，9.000 秒 |
| 期望的回声间隔 | 5280 样本（110.000 ms，即 D 的 4800 加 MonoDelay 的 480） |
| 找到的回声峰 | 10 个（门限：最大峰值以下 60.0 dB） |
| 相邻间隔（样本） | 5280，5280，5280，5280，5280，5280，5280，5280，5280 |
| 间隔数 | 9 |
| 最大偏差 | 0 样本 |
| 干声冲激的位置 | 72480（输入文件里在 72000；MonoDelay 的 Mix 为 100 %，把整条信号延后 480） |
| 首回声 | −6.94 dBFS |

结论：Cubase 自带的插入效果器在环里，它的延迟叠加到环路延迟上，与用户指南和 KNOWN_ISSUES 里「环路时间是 D 加上环内插件延迟」的说法一致。首回声的电平是应有值，没有被压低。

## 界面与回归（现场报告）

这一版相对 2026-11-03 的构建只改了界面拉丁字体、Output 的版式和位置提示；音频、配对与安全链没有改。下列项目由上机的人目视或操作确认，没有数值，都没有问题：

- 新的界面字体（正文与读数）在 Input 与 Output 各窗口、「关于」面板与预设菜单里的样子。
- Output 新版式在各状态下的样子。
- Output 的位置提示：点「知道了」关掉、对所有 Output 窗口生效并记在这台电脑上、在「关于」里恢复。
- 回归：Panic 连点 5 次；三种界面语言；缩放回退与保存。
- 开窗：Input 与 Output 各开关数次，没有二次闪白，占位为圆角。

## 不涵盖

- 七项验收矩阵在这一版上没有重做（缓冲四档、ASIO-Guard 三档、实时对离线、冻结、保存重开）；这些项见 2026-11-03 的记录。
- 环内插件只测了一个：Cubase 的 MonoDelay，10 ms。其他自带插件、带处理延迟的插件、几个插件串联都没有测。
- 44.1 kHz 与 96 kHz，单声道轨，同时打开多个工程，Cubase 的其他版次与版本。

## 未解决与后续

- 这是预览构建：升到 Tier 1 要在发布构建上把七项再做一遍。
