# IPC 契约：共享内存段名、布局与后端

status: draft

## Summary (EN)

- **What**: the shared-memory contract between Rinne Input and Rinne Output: segment names, the rendezvous segment (`rv`, 16 384 bytes, endpoint slots and labels for groups A–H), one data segment per group (`g1`…`g8`, 540 672 bytes, a 65 536-frame stereo transfer ring with a monotonic write head and `write_intent`), the backends (Win32, POSIX, in-process) and how failures surface.
- **Frozen forever**: the rendezvous name never carries the abi; the first 16 bytes of the rendezvous header and the absolute position of every endpoint slot's `version` field never move, so builds with different abis still see each other and report "incompatible" instead of searching forever.
- **Truth**: `src/core/ipc/SegmentLayout.h` mirrors this document; `tests/golden/ipc-layout.json` is checked against it twice (compiled in `rinne_core_tests`, uncompiled by `contract-static-ipc`). Any layout change bumps `kIpcAbi`.
- **Status**: draft until the probe week; the memory-order rules and the slot ownership transition table are completed together with the rendezvous code.

## 范围

两个插件是两个独立加载的二进制，彼此的 static 变量看不见，只能经命名共享内存会合（[ADR-0001](../adr/0001-two-binaries.md)）。本契约规定：

- 段名怎么拼、谁能注入测试命名空间；
- 会合段与数据段的每个字段：偏移、大小、类型、写者、取值；
- 哪些东西永久冻结，哪些随 abi 升级；
- 段头的初始化协议、后端的行为与失败的去向。

代码里的唯一镜像是 [`src/core/ipc/SegmentLayout.h`](../../src/core/ipc/SegmentLayout.h)（字段表、结构表、常量）与 [`src/core/ipc/Naming.h`](../../src/core/ipc/Naming.h)（前缀族）。本文的表与它们不一致时以 golden 对拍的结果为准：两边必须同一个 PR 改。

## 段名

一个会合段 `rv`，加组 A–H 各一个数据段 `g1`…`g8`。名字 = 平台头 + 前缀族 + 作用域 + 段：

| 平台 | 正式 | 测试作用域 |
|---|---|---|
| Windows | `Local\SynchainRinne.rv`、`Local\SynchainRinne.v1.g1`…`g8` | `Local\SnchRinne.<tag7>.rv`、`Local\SnchRinne.<tag7>.v1.g1`…`g8` |
| POSIX | `/SnchRinne.<uid8>.rv`、`/SnchRinne.<uid8>.v1.g1`…`g8` | `/SnchRinne.<tag7>.rv`、`/SnchRinne.<tag7>.v1.g1`…`g8` |

- **会合段名永不带 abi**（永久冻结）。数据段名带 `v<abi>`：abi 不同的环互不相干，而 abi 不同的实例打开的是同一个 `rv`，读得出对方的 abi 与版本号。
- **前缀族**由构建期的 CMake 变量 `RINNE_SHM_PREFIX` 决定，只有三族：正式版 `SynchainRinne`（默认，CI 与发布构建断言用它）、本机 gates 构建 `SynchainRinneD`、探针 `SynchainRinneP`。本机测试与探针因此碰不到用户 DAW 里装的正式版。POSIX 用短写（`SnchRinne`、`SnchRinneD`、`SnchRinneP`）给 `<uid8>` 腾出位置。
- `<uid8>` 是有效用户 id 的 8 位小写十六进制：POSIX 共享内存是全机命名空间，带上 uid，同一台机器上不同用户的段互不相见。Windows 的 `Local\` 本来就按登录会话隔开。
- **测试作用域只经 API 注入**：`NameScope::forTesting(tag)` 或 `NameScope::randomForTesting()`，`<tag7>` 是 `t` 加 6 位小写十六进制（以 `t` 开头，与任何 uid8 都撞不上）。测试名在两个平台上同形同长。**发布代码不读任何环境变量**，插件代码不调用测试钩子（`contract-static-ipc` 扫描 `src/**` 判红）。
- **长度**：名字（含平台头）不超过 31 个字符（macOS 的 POSIX 共享内存名上限），两个平台统一按它检查。3 族 × 9 段 × 2 平台 × {正式, 测试} 共 108 个名字逐条登记在 golden 里，abi 1 时最长 30（Windows 上的测试数据段名）。给 abi 留了两位数的增长空间：abi 涨到 99 时所有名字仍不超过 31（golden 的 `nameAbiHeadroom`，单测与检查都核）。超长或字符不合法时拼名失败、后端也拒绝（`kFailedBadName`，诊断 `kNameTooLong`）。

## 会合段 `rv`（abi 1，16 384 字节）

| 偏移 | 内容 |
|---|---|
| 0 | `RvHeader`（64 字节） |
| 64–1023 | 保留 |
| 1024·G（G = 1…8） | 组 G 的 `GroupBlock`（1024 字节） |
| 9216–16383 | 保留 |

**RvHeader**（前 16 字节永久冻结）

| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | `magic` | u32 | `'RNRV'`，初始化完成的标志，最后以 release 发布 |
| 4 | `abi` | u32 | 1 |
| 8 | `generation` | u32 | 建段为 1，每次覆盖式重初始化 + 1 |
| 12 | `layout_bytes` | u32 | 16384 |
| 16 | `groups` | u32 | 8 |
| 20 | `group_stride` | u32 | 1024 |
| 24 | `probe_call_seq` | u64 | 只有探针构建写（每次回调 fetch_add），正式版恒为 0 |
| 32–63 | 保留 | | |

**GroupBlock**：+0 Input 的端点槽，+256 Output 的端点槽，+512 `panic_seq`（u32，预留给 Output 侧急停，现在恒为 0），+516–1023 保留。

**端点槽 RoleSlot**（256 字节，64 字节对齐）

| 偏移 | 字段 | 类型 | 写者 | 说明 |
|---|---|---|---|---|
| 0 | `ctl` | u64 | CAS | `(gen << 32) \| state`，见下节 |
| 8 | `pid` | u32 | 属主 | |
| 12 | `host_ppid` | u32 | 属主 | 预留，配对规则不看它 |
| 16 | `heartbeat_ms` | u64 | 属主 | 跨进程单调毫秒，每 ≥ 250 ms 写一次 |
| 24 | `claim_ms` | u64 | 属主 | 只在认领与接管时写，恢复休眠不改 |
| 32 | `dormant_ms` | u64 | 属主 | |
| 40 | `sample_rate` | u32 | 属主 | |
| 44 | `max_block` | u32 | 属主 | prepare 声明的最大块长 |
| 48 | `channels` | u32 | 属主 | |
| 52 | `format` | u32 | 属主 | 1 VST3，2 AU，0 未知 |
| 56 | `version` | u32 | 属主 | `MAJ << 16 \| MIN << 8 \| PAT`；**绝对位置永久冻结** |
| 60 | `cfg_flags` | u32 | 属主 | b0 预留给备用后端，b1 组号由智能默认选出，b2 认领以来还没配上过 |
| 64 | `block_seq` | u64 | 音频线程 | 64–127 独占一条缓存行 |
| 72 | `last_host_ns` | u64 | 音频线程 | 0 = 宿主没给 |
| 80 | `rt_flags` | u32 | 音频线程 | b0 旁路，b1 离线，b2 块长超声明，b3 断档，b4 时序不稳，b5 跳闸，b6 冲突，b7 块长超出支持范围 |
| 84 | `max_n_seen` | u32 | 音频线程 | 本次 prepare 以来的最大块长（诊断） |
| 88 | `peak_bits` | u32 | 音频线程 | 回送峰值的 float 位型 |
| 92 | `d_eff` | u32 | 音频线程 | 实际环路延迟（样本） |
| 96 | `safety_state` | u32 | 音频线程 | Input：安全状态（0 未配对、1 淡入、2 运行、3 断档、4 失控保护、5 跳闸，跳闸原因在第 8–15 位：1 Panic、2 NaN、3 超大值、4 失控）；Output：写环状态（0 空闲、1 写环、2 旁路写零、3 冲突、4 块长超出支持范围） |
| 100 | `resync_count` | u32 | 音频线程 | |
| 104 | `nonfinite_count` | u32 | 音频线程 | |
| 108–127 | 保留 | | | |
| 128 | `label_seq` | u32 | 属主 | 标签 seqlock：偶数 = 稳定 |
| 132 | `colour_argb` | u32 | 属主 | 0 = 未知 |
| 136 | `name_len` | u32 | 属主 | 完整 UTF-8 轨道名的字节数 |
| 140 | `name_hash` | u32 | 属主 | 完整 UTF-8 的 FNV-1a-32 |
| 144 | `name_words` | u32 × 28 | 属主 | UTF-8 按码点截到 ≤ 112 字节，每字 4 字节小端，只供显示 |

标签只有属主在 `ctl` 仍是自己最后写下的整字时才写（安全不变量：环头与标签头只由该槽的属主写）。

**ctl**：低 32 位 `state` 的低 2 位是阶段（0 空闲、1 认领中、2 活跃、3 休眠），第 2 位是 provisional（智能默认的认领，可被工程 state 的恢复抢占），其余位为 0；高 32 位 `gen`。认领、幽灵或死进程接管、provisional 抢占、休眠、恢复、释放，每一次转移都对整字 CAS，并且 `gen + 1`。属主记住自己最后写下的整字，每拍比对一次；整字变了就说明被接管过。这样双接管恰好一个胜者、释放的检查与动作之间不会被插队，也没有 ABA（同一个整字再出现要 2³² 次转移）。转移表与时间阈值随会合实现一起补入本文。

## 数据段 `g{G}`（abi 1，540 672 字节）

| 偏移 | 内容 |
|---|---|
| 0 | `RingHeader`（16 384 字节，128 起保留） |
| 16384 | 数据区：65 536 帧 × 2 声道的 `std::atomic<float>`，左右交错，恒按立体声排，relaxed 访问 |

**RingHeader**

| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | `magic` | u32 | `'RNRG'` |
| 4 | `abi` | u32 | 1 |
| 8 | `generation` | u32 | 只作校验，不触发重同步 |
| 12 | `ring_frames` | u32 | 65536 |
| 16 | `ring_channels` | u32 | 2 |
| 20 | `sample_rate` | u32 | |
| 24 | `src_channels` | u32 | 只给界面显示 |
| 28 | `out_flags` | u32 | b0 旁路，b1 淡变中 |
| 32 | `epoch` | u64 | 只在采样率改变时 + 1 |
| 40 | `writer_token` | u64 | 写权；与端点槽的 `ctl` 无关 |
| 48 | `nonfinite_count` | u32 | |
| 52 | `clamp_count` | u32 | |
| 56 | `w_cas_fail` | u32 | |
| 60 | 保留 | | |
| 64 | `write_head` | u64 | W：段寿命内单调递增的帧位置；64–127 独占一条缓存行 |
| 72 | `last_block_len` | u32 | |
| 76 | 保留 | | |
| 80 | `write_intent` | u64 | 写方动样本之前公告的「本段写完后的 W」 |
| 88–16383 | 保留 | | |

- 环只有 2¹⁶ 帧：它只做块间传输，环路延迟 D 的延迟线放在 Input 本地，D 的范围与节拍同步都不进冻结布局。
- 两端声明的最大块长超过 32 768 时只出干声并显示红字（诊断 `kUnsupportedBlock`）；写环按不超过 8192 帧分段。
- 写权用独立的 `writer_token`；W 的 CAS 只防写头被推两次，接管瞬间的双写靠 `write_intent` 判撕裂兜住。

## 冻结与 abi

- **永久冻结**（连 abi 升级都不许动）：会合段名不带 abi；`RvHeader` 的 0–15 字节；每个端点槽 `version` 的绝对位置 1024·G + {0, 256} + 56，以及它依赖的组块位置与步长、`'RNRV'` 与初始化标记 `'RNIN'`。`contract-static-ipc` 把这几项写死在检查里，不随 golden 改。
- **随 abi**：其余字段、取值与段大小。任何改动都是布局改动：`kIpcAbi + 1`（数据段名随之变成 `v2`），同一个 PR 改头文件、golden 与本文，并按冻结契约流程附变更说明。保留区必须为 0，后续版本只能在保留区里加字段。
- abi 不同的两端：读到对方的 `abi` 不同就停在「版本不兼容」（红），绝不写对方的段；只是版本号不同时可以连，界面给琥珀提示。

## 段头初始化

两种段头的开头都是 `magic`、`abi`、`generation`。`magic` 有三种状态：0（没初始化）、初始化标记 `'RNIN'`（有人认领了初始化、正在写）、真值（初始化完成）。谁来初始化只由一次 CAS 决定：把 `magic` 从 0 换成初始化标记的那一方是唯一的初始化者，别人只等；初始化者写完段头与几何字段，最后以 release 把 `magic` 换成真值。初始化标记与 `'RNRV'` 一样永久不变：任何版本读到它都按「还没初始化完」处理。

1. **建段者**（操作系统报告新建）：马上认领 → 逐页写触碰全段（绕开前 16 字节）→ 锁页（尽力而为）→ 写 `abi`、`generation = 1` 与几何字段 → 发布 `magic`。
2. **附着方**，`magic` 已发布：逐页只读触碰（只在看到发布之后，用原子读；严禁写：对端可能正在写环）→ `magic` 不是我们的就不用（诊断 `kForeign`）；`abi` 不同 → `kAbiMismatch`；段的真实大小小于本 abi 的布局 → 不用（`kBadSize`）。
3. **附着方**，`magic` 还没发布：只读探测的一方立刻返回失败、下一拍重试；建段的一方（Input 与 Output 都是）每 50 ms 看一次、最多 10 次。仍是 0 说明创建者死在认领之前：自己认领，覆盖式重初始化（从段头之后清零、`generation + 1`、写几何字段、发布）。认领输了、或看到的是初始化标记：别人正在初始化，再等一轮。输掉的一方从不清零，所以不会抹掉胜者已经开始用的槽与环。
4. 等满仍没发布：初始化者死在认领与发布之间的那几微秒里。返回 `kFailed`（诊断 `kBusy`），调用方**解映射**后下一拍重试；所有持有者都放手后段就消失，下一个建段者重新建。

等待只发生在附着到一个没初始化完的段时（建段者被杀之后），最多约 1 s，在调用它的非实时线程上。

后端一律回读段的真实大小：同名段已存在时请求的大小被忽略，拿请求值当真实值发布几何，小视图配大几何就会写越界。读不出真实大小时打开失败，不交出大小为 0 的视图。

## 后端

| 平台 | 后端 | 要点 |
|---|---|---|
| Windows | Win32 | `CreateFileMappingW` / `OpenFileMappingW`，`Local\` 命名空间；真实大小取映射整段的视图所在区域的大小（`VirtualQuery`，按页上取整）；段随最后一个句柄关闭由内核回收。锁页尽力而为：不改宿主进程的工作集（那是整个 DAW 的设置），宿主自己调大过时 `VirtualLock` 能锁住，锁不住只报诊断 |
| macOS | POSIX | `shm_open` + `mmap`（`fstat` 回读真实大小）；每个映射持有锁文件 `<锁目录>/<名字>.lock` 的共享 flock，所有建、附着、离开都在全局 `lifecycle.lock` 的独占 flock 下做，最后一个离开者 `shm_unlink`，崩溃留下的残段由下一个建段者清理（只清本用户名下的）。全局锁的等待：附着只试一次不等（25 Hz 重试路径），建段最多 2 s（只在加载与改组时），离开最多 200 ms；等满一次之后 1 s 内都只试一次。锁目录是真实 home 下的 `Library/Application Support/Synchain/Rinne/ipc`（不看 `HOME`，不放 `TMPDIR`）；测试只能经构造参数换目录 |
| 单测、Linux core-only | 进程内 | 名字 → 清零的 64 字节对齐缓冲，跨进程不可见，不是可发布的形态 |

选择在预处理期定死（`PlatformSegmentBackend`）。段操作只在持生命周期锁的非实时线程上做；音频线程只经映射句柄的按块租约拿裸指针，做原子读写，不碰任何系统调用。释放时先停发租约、等租约归零，再等 500 ms 宽限才解映射；宽限期按跨进程单调时钟算，时钟读不出或倒退时从头算。句柄析构或被覆盖时宽限期走不完的，控制块与映射都留到进程退出，音频线程手里的租约不会悬空。

**失败的去向**：任何打开或初始化失败都只让回送变成 0、干声照常（安全不变量 1）。

| 返回 | 界面 | 来源 |
|---|---|---|
| `kAbiMismatch` | `abiMismatch`（红） | 段头 abi 不同 |
| `kFailedDenied` | `ipcUnavailable`（红），「宿主沙盒限制」 | `EPERM` / `EACCES` / `ENOSYS`、Windows 的拒绝访问；本进程内不会自己好 |
| `kFailedBadName` | `ipcUnavailable`（红） | 段名不合法或超长 |
| `kFailed` | 重试中，持续失败显示 `ipcUnavailable` | 段还不存在、锁等待超时之类，下一拍重试 |

失败原因经进程内的诊断出口（`IpcDiag`）报给插件的诊断日志：哪一步、操作系统的错误码、归类（`kShmDenied`、`kNameTooLong`、`kAbiMismatch`、`kUnsupportedBlock` 等）、哪个段。诊断不进任何共享内存段。

**验证宿主**（pluginval、auval 等）下 IPC 整体关闭、只出干声，状态 `validator`：验证与维护者开着的 DAW、并行的测试互不干扰。

## 内存序

本节先写数据面的规则；会合段的转移与标签 seqlock 的细则随会合实现补入。

- 写环（每段不超过 8192 帧）：读 W（acquire）→ `write_intent` 写成 W + k（relaxed）→ release fence → 样本 relaxed 写 → W 从旧值 CAS 到 W + k（release）。
- 读环：W（acquire）→ 样本 relaxed 读 → acquire fence → 读 `write_intent`（relaxed）；凡帧位置 p < `write_intent` − 65536（有符号运算）的都视为可能已被覆盖、按 0 处理。只依赖 fence 的配对，不依赖额外的安全余量。
- `epoch`、`writer_token` 与 `sample_rate`：写方先写 `sample_rate` 与 `src_channels`，再以 release 写 `epoch` 或 `writer_token`；读方先 acquire 读它们，再读 `sample_rate`。
- `ctl`：acq_rel 的 CAS。映射句柄的租约握手四处访问一律 seq_cst。
- 内存序的缺陷在 x86 上查不出来，压测必须在原生 arm64 上跑，并用去掉读侧 fence、去掉 `write_intent` 的两个变异构建证明判据会变红。

## 对拍

- [`tests/golden/ipc-layout.json`](../../tests/golden/ipc-layout.json)：常量、结构、字段（每字段一行、键序固定）、前缀族与 108 个段名。
- `rinne_core_tests`（`tests/core/ipc/layout/`）：编译后用同一张字段表展开 `offsetof` 逐行比 golden；段名逐条比。
- `contract-static-ipc`（`node scripts/run-checks.mjs --only contract-static-ipc`）：不编译读头文件比 golden；检查永久冻结面、段名长度、前缀族与 CMake 一致、发布代码不读环境变量。`--mutation-check` 证明改一个偏移、注入超长前缀、发布代码读环境变量都会变红。
