# 测试套件与场景一览

本文说明仓里各个测试套件测什么、在本机怎么跑，以及环路场景 S01–S25 各自对应哪个文件。REAPER 本机宿主烟测另见 [REAPER.md](REAPER.md)。

## 套件

测试按目录收，一个目录一个可执行文件；新增测试只往对应目录里放 `.cpp`，不改 `tests/CMakeLists.txt`。套件要额外的源码、链接或 ctest 设置，写在 `tests/<套件>/suite.cmake`。每条 ctest 都带显式 TIMEOUT，缺了会被 ctest 的超时门禁判红。

| 目标 | 目录 | 测什么 | ctest 超时 |
|---|---|---|---|
| `rinne_smoke` | `tests/smoke/` | 构建信息、Catch2 与线程基本盘；所有配置（含 sanitizer）都跑 | 60 s |
| `rinne_core_tests` | `tests/core/` | `rinne_core` 的单测：环、IPC、全部 DSP、安全状态机、state、诊断，不依赖 JUCE | 120 s |
| `rinne_loop_rig` | `tests/loop_rig/` | 环路试验台：用真实共享内存把 Input 引擎与 Output 引擎闭成一个环，按场景喂信号、改参数、跳块、切块，逐位或按推导出的阈值核对输出；不含 JUCE | 300 s |
| `rinne_stress` | `tests/stress/` | 压测：多线程下的环、读侧回收、标签头序列锁、控制块 | 600 s |
| `rinne_ipc_tests` | `tests/ipc/` | 跨进程契约：对端 ABI、对端接续、接管、残留共享内存 | 300 s |
| `rinne_params_tests` | `tests/params/` | 参数层、state 胶水、预设（需要 JUCE） | 120 s |
| `rinne_host_tests` | `tests/host/` | 免 DAW 宿主里的真处理器：配对、壳的接线、界面默认值、WebView 开窗、插件级场景复验（需要 JUCE） | 300 s |

`rinne_host_tests` 里的 `tests/host/plugin_host/` 是插件级场景宿主：同一进程里一对真处理器（Input 与 Output）像 DAW 那样按块调 `processBlock`，中间夹一段环内插件的替身，组成真的反馈环。块长、跳块、走带、离线渲染标志、挂起与重载都由宿主脚本给。它复验 loop_rig 里那些要经过壳才成立的判据。

## 本机怎么跑

JUCE 按 `.juce-commit` 钉版检出，经 `-JucePath` 或环境变量 `RINNE_JUCE_PATH` 给出（见 `scripts/build.ps1` 文件头）。需要 PowerShell 7。

```sh
# 完整的本地门禁：构建、全部 ctest、静态检查（子 PR 的唯一编译门）
pwsh scripts/gates.ps1

# 只构建并跑全部 ctest
pwsh scripts/build.ps1 -Test

# 构建后单独跑一个套件或一组场景（构建目录缺省 build）
build/tests/Release/rinne_loop_rig.exe "[s05]"
build/tests/Release/rinne_host_tests.exe "[s03b],[s05b]"
```

场景的 Catch2 标签是 `[loop_rig][sNN]`（环路试验台）与 `[plugin_host][sNN]`（插件级复验），用标签就能按场景挑着跑。Debug 构建加 `-Config Debug` 和独立的 `-BuildDir`。整套 `rinne_loop_rig` 本机实测约 9 s（Release）、约 1 分钟（Debug）；加场景时留意总耗时别逼近 ctest 超时（300 s），逼近了在 `tests/CMakeLists.txt` 注册这个套件的 `rinne_add_test_suite` 行里放宽（`suite.cmake` 不注册 ctest）。

场景文件头都写同一套内容：测什么、做法、断言（判据与取值理由）、以及标 ⛔ 的删除式验证点。⛔ 点的意思是：把对应的实现改坏，这个场景必须变红。改场景判据之前先读文件头，别把判据放宽到让改坏的实现也能过。

## 场景 S01–S25

文件都在 `tests/loop_rig/scenarios/`；「插件级」一栏是 `tests/host/plugin_host/` 里的复验文件，空着表示只有 loop_rig 一层。

| 编号 | 测什么 | loop_rig 文件 | 插件级 |
|---|---|---|---|
| S01 | 冲激回声：回声峰位恰为 k·D，幅度与独立的参考模型相符 | `s01_impulse_echo.cpp` | |
| S02 | 块长不变性：参数恒定时，宿主怎么切块，闭合的环路输出都逐位相同 | `s02_block_invariance.cpp` | |
| S03 | 起播沿：起播之后的输出与预滚多久、怎么切块、实时还是离线无关，从起播点 + 5 ms 起逐位相同；宿主短暂报停不算停走带 | `s03_play_edge.cpp` | `test_plugin_s03b_s05b.cpp`（S03b） |
| S04 | null test：回送关掉的几种条件下，Input 输出与输入逐位相同，Output 对宿主缓冲逐位直通 | `s04_null_test.cpp` | |
| S05 | Output 停调：不同长度的停调（一块、30 ms、120 ms、2 s）、固定与随机块长，恢复后的读头重建、降级窗口与补零的账 | `s05_output_stall.cpp` | |
| S05b | 停调与起播沿：停调恢复之后再起播，起播沿之后的输出与停调与否无关 | `s05b_stall_edge.cpp` | `test_plugin_s03b_s05b.cpp` |
| S06 | Input 跳过 k 块：回来之后写头比读位领先 k 块 | `s06_input_skip.cpp` | |
| S07 | 错序（Output 先于 Input）：环路照常工作，延迟恒为 D − n0，宿主给了时间戳就记下证据 | `s07_output_first.cpp` | |
| S08 | 单块是声明块长的两倍（宿主违约一块）：D 下限抬升后不回落，没有补零 | `s08_oversized_block.cpp` | |
| S09 | 穿插 0 样本调用：不推进任何计数，输出与不穿插时逐位相同 | `s09_zero_sample.cpp` | |
| S10 | 重新 prepare：同采样率不打断流；换采样率重新来过，旧采样率下的内容不漏进新的流 | `s10_prepare_rate.cpp` | |
| S11 | 同组两个 Output（接管）：写权从一个换给另一个再换回来，环上只有一个写方，换写方后回送重新淡入 | `s11_two_outputs.cpp` | |
| S12 | 环内坏值突发（NaN、±Inf、1e6）：Output 写环前清洗，Input 输出始终有限，不跳闸 | `s12_loop_garbage.cpp` | `test_plugin_s12_s13.cpp` |
| S13 | 安全矩阵：环内增益 × 反馈 × 信号，加不误报、限制器关的联锁、默认上限与失控跳闸 | `s13_safety_matrix.cpp` | `test_plugin_s12_s13.cpp` |
| S14 | 改 D、D 下限抬升、交叉淡化进行中重同步：读头相邻样本跳变不超过推导出的阈值，读头槽不溢出 | `s14_delay_change.cpp` | `test_plugin_s14_s15.cpp` |
| S15 | 节拍同步：宿主给速度时 D 跟着时值走，不给时回退 120 BPM 并置提示 | `s15_beat_sync.cpp` | `test_plugin_s14_s15.cpp` |
| S16 | 单声道与立体声的四种组合：环恒按立体声步长，映射正确，不越界 | `s16_channel_layouts.cpp` | |
| S17 | 交替漂移：Output 块长在两个值之间交替，写头相对读位持续漂移 | `s17_alternating_drift.cpp` | |
| S18 | 中间链有延迟：实测环路延迟 = D + 链延迟 | `s18_chain_delay.cpp` | |
| S19 | Input 在两个组之间来回换：写头差很多的两个环之间切换，换组后静音到 D、之后只有新组的音频 | `s19_group_switch.cpp` | |
| S20 | 直流阻断、限制器关时的联锁、高采样率上界 | `s20_dc_block_interlock.cpp` | |
| S21 | 止啸路径矩阵与锁存保持：每条能停下回送的路径都在规定时间内归零，锁存只有恢复能清 | `s21_stop_paths_latch.cpp` | |
| S22 | 载入预设：先 10 ms 降到 0，再从首个有效读样本起 500 ms 线性淡入；锁存时载入预设不解除锁存 | `s22_load_preset.cpp` | |
| S23 | 停用复燃：两端一起挂起超过 250 ms 清空本地延迟线，不超过则旧内容照常回来（loop_rig 钉引擎一侧，插件级钉壳的 250 ms 阈值） | `s23_suspend_clear.cpp` | `test_plugin_s23_s24.cpp` |
| S24 | 单端切片：只有一端把块再切小，输出逐位不变（loop_rig）；工程重载后新 Input 还原 state、不读重载之前环里的内容（插件级） | `s24_single_end_slicing.cpp` | `test_plugin_s23_s24.cpp` |
| S25 | 环内 NaN 策略与 FADE_IN 入口无阶跃 | `s25_loop_nan_fade_in.cpp` | |

场景之外，`tests/loop_rig/rig/` 是试验台本身（引擎宿主、调度表、环段、场景注册），它有自己的框架用例与实时线程分配检查。
