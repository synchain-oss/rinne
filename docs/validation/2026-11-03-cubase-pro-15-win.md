# Cubase Pro 15（Windows）实测记录：2026-11-03

## Summary (EN)

- **Host / OS**: Cubase Pro 15.0.5 (internal version 15.0.5.114), Windows 10 Pro 22H2 (10.0.19045), x64; project at 48 kHz; VST3.
- **Build**: 0.9.0-preview.1, commit `968685ef` (a preview build, not a release build).
- **Verdict**: all seven acceptance items passed: realtime playback, offline export, Freeze, save and reopen, buffer sizes 64 / 256 / 1024 / 2048, ASIO-Guard Off / Normal / High, and the loop period measured against D (every echo interval equals D, deviation 0 samples). Not covered: plug-ins between the two ends (the slots were empty), 44.1 and 96 kHz, mono tracks, several projects open at once. Because this is a preview build, the host stands at Tier 2; Tier 1 needs the same matrix on a release build.

## 环境

| 项 | 记录 |
|---|---|
| 宿主与版本 | Cubase Pro 15.0.5（内部版本 15.0.5.114）。版次与版本号于 2026-11-07 在 Help → About 核对 |
| 系统与架构 | Windows 10 专业版 22H2（10.0.19045），x64 |
| 格式 | VST3 |
| 采样率 | 48 kHz（工程设置） |
| 导出格式 | WAV，32 bit float，立体声，不加抖动，不归一化 |
| 构建 | 0.9.0-preview.1，提交前 8 位：`968685ef`（预览构建） |
| 上机日期 | 2026-11-03 |
| 摆放 | 一条立体声测试轨，输入设为 No Bus，Monitor 与 Record 关，Cycle 关；第 1 格 Rinne Input，第 2 格 Rinne Output，都在推子分隔线之上，两者之间没有别的插件；组选 A |
| 分析工具 | 随包的 `rinne_nulltest`（`echo` 量回声间隔，`compare` 对拍两份导出） |

回声间隔与对拍的数值按导出的 wav 用 `rinne_nulltest` 判读，表中的数值照判读结果抄录；标「现场报告」的项没有数值，是上机的人当场报告的结论。原始 wav 不入库。

## 验收矩阵（七项）

| 项 | 结论 | 依据与说明 |
|---|---|---|
| 实时播放 | 通过 | 六格实时导出，回声间隔都等于 D，偏差 0（见下表）。30 秒首用没有问题；监听键来回切换、Input 旁路、Output 旁路、停用 / 启用、禁用 / 启用轨道五步宿主操作都没有问题（现场报告） |
| 离线导出 | 通过 | 实时导出与离线导出对拍：整体残差峰值 −162.56 dBFS，开头 2 秒 −174.60 dBFS，lag 0，长度相同（D 为 4800） |
| Freeze | 通过 | 就地渲染与冻结都测了，数值见下；解冻后重新连接，设置没丢（现场报告） |
| 保存重开 | 通过 | 保存、关闭、重开后数值都对（现场报告） |
| 缓冲 | 通过 | 64 / 256 / 1024 / 2048 四档（ASIO-Guard 为 Normal），回声间隔都等于 D，偏差 0 |
| ASIO-Guard | 通过 | 256 缓冲下 Off / Normal / High 三档，回声间隔都等于 D，偏差 0 |
| 环路周期对 D | 通过 | 六份导出的间隔都是 17299 样本，与界面上的 D 读数一致；另有两次 D 设为 4800，量到 4800，偏差 0 |

## 实测数据

### 回声间隔：缓冲 × ASIO-Guard

测试信号是 `echo-in.wav`（8 秒，1.5 秒处一个单冲激，前面留 1 秒静音让回送的淡入走完）。Rinne Input：反馈量 50 %、混合 50 %，其余默认。实时导出，文件名 `echo-<缓冲>-<档>.wav`。

| 缓冲 | ASIO-Guard | 宿主最大块长读数（样本） | 间隔（样本） | 偏差 |
|---|---|---|---|---|
| 64 | Normal | 512 | 17299 × 9 | 0 |
| 256 | Normal | 512 | 17299 × 9 | 0 |
| 1024 | Normal | 1024 | 17299 × 9 | 0 |
| 2048 | Normal | 2048 | 17299 × 9 | 0 |
| 256 | Off | 512 | 17299 × 9 | 0 |
| 256 | High | 1024 | 17299 × 9 | 0 |

- D 读数：界面上 D 为 360.4 ms，即 17299 样本（360.396 ms）；每份导出 9 个间隔，全部等于 17299。
- 六份导出两两对拍：残差不高于 −187 dBFS，lag 0。
- 命令：`rinne_nulltest echo --in echo-<缓冲>-<档>.wav --d 17299 --min-peak-db -60 --sr 48000`，六格都是 PASS，最大偏差 0 样本。
- 如果按测试步骤预设的 `--d 2048` 去跑，六格都是 FAIL，原因是量到的间隔 17299 不等于 2048，而界面上的 D 实际是 17299 样本；以界面读数为准重跑才是上面的结果。

### 实时与离线导出对拍

D 为 4800。命令：`rinne_nulltest compare --a cb-rt.wav --b cb-offline.wav --head-seconds 2 --threshold-db -120`，PASS。

| 量 | 值 |
|---|---|
| 整体残差峰值 | −162.56 dBFS |
| 开头 2.00 秒残差峰值 | −174.60 dBFS |
| 对齐偏移（lag） | 0 |
| 长度 | 相同 |

### 就地渲染与冻结

- 就地渲染（Render in Place），D 为 4800（100 ms）：`rinne_nulltest echo --d 4800` 为 PASS，3 个间隔都是 4800，偏差 0；每圈 −6.02 dB，与普通导出同形，没有重影。
- 冻结：冻结文件的内容与普通导出（上面的实时导出）逐段对拍，12 段素材的差异都不高于 −130.8 dBFS。
- 冻结文件比时间线短，是因为 Cubase 把静音段省掉了（每份里干声之后 4352 个样本，峰值 −194 dBFS；尾部 58322 个样本，峰值 −142.5 dBFS）。直接量冻结文件时，第一个回声看起来提前，是这个原因，不是环路的问题；量冻结文件要按段对齐。
- 冻结后的播放听感没有做判读；现场没有报告异常。

## 用户报告的其他项

下列项目由上机的人目视或听感确认，没有数值：30 秒首用；Panic 连点 5 次；复制轨道时的冲突提示与一键修复；配对指示灯的颜色；错序与放错位置的提示显示正常；三种界面语言与缩放回退；开窗不再二次闪白、占位为圆角。都没有问题。

## 不涵盖

- 环内放插件：两端之间是空的，环内插件的情形见 2026-11-07 的记录。
- 44.1 kHz 与 96 kHz，单声道轨，同时打开多个工程。
- Cubase 的其他版次（Artist、Elements）与其他版本，macOS 上的 Cubase。
- 音频接口与驱动的型号没有记录。

## 未解决与后续

- 这是预览构建：升到 Tier 1 要在发布构建上把七项再做一遍。
