// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存段的布局（abi 1）：一个会合段 rv（组 A–H 的端点槽与标签），加每组一个数据段 g1..g8（单调写头的传输环）。
// 契约文档是 docs/contracts/IPC_CONTRACT.md；本文件是它在代码里的唯一镜像。下面的字段表同时展开成 static_assert，
// 并与 tests/golden/ipc-layout.json 对拍两遍：rinne_core_tests 编译后逐行比，contract-static-ipc 不编译直接读本文件比。
//
// 改动规则：
//   - 任何字段的偏移、大小、类型或取值改了，都是布局改动：kIpcAbi + 1（数据段名随之换成 v2），按冻结契约流程走。
//   - 永久冻结、连 abi 升级都不许动的只有这几处：会合段头的前 16 字节（magic、abi、generation、layout_bytes）与
//     初始化者 pid（偏移 32），每个端点槽里 version 字段的绝对位置（1024·G + {0, 256} + 56），以及会合段大小的下限
//     kRvMinBytes。会合段名不带 abi，新旧两个 abi 的实例打开的是同一个 rv：靠前 16 字节与 version，任何版本都能读出
//     对方的 abi 与版本号，显示「版本不兼容」，而不是都停在「搜索中」；靠初始化者 pid，任何版本都能接管一个初始化者
//     已经死掉的 rv。magic 字段里会出现的两个值（'RNRV' 与初始化标记 'RNIN'）也随之永久不变。
//   - 保留区必须是 0；后续版本只能在保留区里加字段，加了也算布局改动。
// 所有跨进程字段都是 std::atomic，并且 lock-free；结构体按段内的真实偏移排布，映射基址按页对齐，
// 所以 8 字节字段的偏移都是 8 的倍数（字段表的对拍会检查）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "util/Fourcc.h"

namespace rinne::ipc
{

using u32 = std::uint32_t;
using u64 = std::uint64_t;

static_assert(std::atomic<u32>::is_always_lock_free, "跨进程的 32 位字段必须 lock-free");
static_assert(std::atomic<u64>::is_always_lock_free, "跨进程的 64 位字段必须 lock-free");
static_assert(std::atomic<float>::is_always_lock_free, "环里的样本必须 lock-free");
static_assert(sizeof(std::atomic<u32>) == 4 && sizeof(std::atomic<u64>) == 8 && sizeof(std::atomic<float>) == 4,
              "原子量的大小必须与底层类型相同，否则两端看到的偏移不一样");

// ---- 常量（每一个都进 golden；只写字面量，派生关系在下面用 static_assert 钉住） ----

inline constexpr u32 kIpcAbi = 1;
inline constexpr u32 kRvMagic = util::fourcc("RNRV");
inline constexpr u32 kRingMagic = util::fourcc("RNRG");
// 段头 magic 的「初始化中」标记：恰好一个初始化者用 CAS 把 magic 从 0 换成它，写完段头再换成真值。
// 任何版本读到它都按「还没初始化完」处理，所以与会合段的 magic 一样永久不变
inline constexpr u32 kInitMarker = util::fourcc("RNIN");
inline constexpr u32 kGroupCount = 8;
// 段名（含平台前缀）的最长字符数：macOS 的 POSIX 共享内存名上限，两个平台统一按它检查
inline constexpr u32 kMaxSegmentNameLength = 31;

inline constexpr u32 kRvBytes = 16384;
// 会合段在任何 abi 下都不小于它（永久冻结）。abi 不同的一方按字节读对方的 version（最远到 8504..8507）之前，
// 先核自己的视图至少这么大
inline constexpr u32 kRvMinBytes = 16384;
inline constexpr u32 kRvHeaderBytes = 64;
inline constexpr u32 kGroupStride = 1024;
inline constexpr u32 kRoleSlotBytes = 256;
inline constexpr u32 kLabelNameWords = 28;
inline constexpr u32 kLabelNameBytes = 112;

inline constexpr u32 kRingHeaderBytes = 16384;
inline constexpr u32 kRingFrames = 65536;
inline constexpr u32 kRingChannels = 2;
inline constexpr u32 kRingDataOffset = 16384;
inline constexpr u32 kRingBytes = 540672;
// 两端声明的最大块长超过它时只出干声并显示红字；写环按不超过 kMaxWriteChunk 帧分段
inline constexpr u32 kMaxSupportedBlock = 32768;
inline constexpr u32 kMaxWriteChunk = 8192;

// 端点槽 ctl = (gen << 32) | state。state 的低 2 位是阶段，第 2 位是 provisional（智能默认的认领，可被工程
// state 的恢复抢占），其余位为 0。每次转移（认领、接管、抢占、休眠、恢复、释放）都对整字 CAS 并把 gen + 1：
// 属主记住自己最后写下的整字，每拍比对一次，整字变了就说明被人接管过，杜绝双属主与 ABA。
inline constexpr u32 kCtlPhaseMask = 3;
inline constexpr u32 kCtlPhaseFree = 0;
inline constexpr u32 kCtlPhaseClaiming = 1;
inline constexpr u32 kCtlPhaseActive = 2;
inline constexpr u32 kCtlPhaseDormant = 3;
inline constexpr u32 kCtlProvisional = 4;

// RoleSlot.format：宿主的插件格式；0 = 未知
inline constexpr u32 kFormatVst3 = 1;
inline constexpr u32 kFormatAu = 2;

// RoleSlot.cfg_flags（属主在消息线程写）
inline constexpr u32 kCfgAltBackend = 1; // 预留：同进程登记的备用后端
inline constexpr u32 kCfgSmartOrigin = 2; // 组号由智能默认选出
inline constexpr u32 kCfgNeverPaired = 4; // 认领以来还没有配上过对端

// RoleSlot.rt_flags（音频线程写，整字 relaxed store）
inline constexpr u32 kRtBypassed = 1;
inline constexpr u32 kRtNonrealtime = 2;
inline constexpr u32 kRtOverMax = 4; // 宿主送来的块长超过了 prepare 声明的值
inline constexpr u32 kRtStalled = 8;
inline constexpr u32 kRtUnstable = 16;
inline constexpr u32 kRtTripped = 32;
inline constexpr u32 kRtConflict = 64;
inline constexpr u32 kRtUnsupportedBlock = 128;

// RoleSlot.safety_state 的取值。Input 槽是安全状态机的状态，跳闸原因放在第 8–15 位；
// Output 槽同一个字段是写环状态（kWrite*）
inline constexpr u32 kSafetyUnpaired = 0;
inline constexpr u32 kSafetyFadeIn = 1;
inline constexpr u32 kSafetyRun = 2;
inline constexpr u32 kSafetyStalled = 3;
inline constexpr u32 kSafetyGuard = 4;
inline constexpr u32 kSafetyTripped = 5;
inline constexpr u32 kTripShift = 8;
inline constexpr u32 kTripPanic = 1;
inline constexpr u32 kTripNan = 2;
inline constexpr u32 kTripHuge = 3;
inline constexpr u32 kTripRunaway = 4;
inline constexpr u32 kWriteIdle = 0;
inline constexpr u32 kWriteActive = 1;
inline constexpr u32 kWriteBypassedZeros = 2;
inline constexpr u32 kWriteConflict = 3;
inline constexpr u32 kWriteUnsupportedBlock = 4;

// RingHeader.out_flags（Output 写）
inline constexpr u32 kOutBypass = 1;
inline constexpr u32 kOutFading = 2;

static_assert(kRingDataOffset == kRingHeaderBytes);
static_assert(kRingBytes == kRingHeaderBytes + kRingFrames * kRingChannels * sizeof(float));
static_assert(kGroupStride * (kGroupCount + 1) <= kRvBytes, "组块 G 在 1024·G，第 8 组要落在段内");
static_assert(kRvBytes >= kRvMinBytes, "会合段永远不小于永久冻结的下限");
static_assert(kLabelNameBytes == kLabelNameWords * 4);
static_assert((kRingFrames & (kRingFrames - 1)) == 0, "环的帧数必须是 2 的幂，按位与取模");
static_assert(kMaxWriteChunk <= kMaxSupportedBlock && kMaxSupportedBlock < kRingFrames);

constexpr u64 makeCtl(u32 gen, u32 state) noexcept
{
    return (static_cast<u64>(gen) << 32) | state;
}
constexpr u32 ctlGen(u64 ctl) noexcept
{
    return static_cast<u32>(ctl >> 32);
}
constexpr u32 ctlState(u64 ctl) noexcept
{
    return static_cast<u32>(ctl);
}
constexpr u32 ctlPhase(u64 ctl) noexcept
{
    return ctlState(ctl) & kCtlPhaseMask;
}

// RoleSlot.version：MAJ << 16 | MIN << 8 | PAT
constexpr u32 packVersion(u32 major, u32 minor, u32 patch) noexcept
{
    return (major << 16) | ((minor & 0xffu) << 8) | (patch & 0xffu);
}

// ---- 会合段 rv（kRvBytes 字节） ----

// 段头。前 16 字节与 init_pid 永久冻结。probe_call_seq 只有探针构建写（每次回调 fetch_add），正式版恒为 0。
// init_pid：认领了初始化的那个进程，认领之后立刻写；magic 停在初始化标记而它已经死了，别人才可以接管（见
// ISegmentBackend::initHeader）。magic 为 0 时它恒为 0。
struct alignas(64) RvHeader
{
    std::atomic<u32> magic;
    std::atomic<u32> abi;
    std::atomic<u32> generation; // 建段为 1，每次接管重初始化 + 1
    std::atomic<u32> layout_bytes;
    std::atomic<u32> groups;
    std::atomic<u32> group_stride;
    std::atomic<u64> probe_call_seq;
    std::atomic<u32> init_pid;
    u32 reserved[7];
};

// 端点槽：每组 Input、Output 各一个。前两条缓存行是属主的身份与配置（消息线程写），64..127 独占一条缓存行给
// 音频线程每块写的计数与标志，128 起是轨道名与颜色的标签（seqlock，只有属主在 ctl 仍是自己时写）。
struct alignas(64) RoleSlot
{
    std::atomic<u64> ctl;
    std::atomic<u32> pid;
    std::atomic<u32> host_ppid; // 预留，配对规则不看它
    std::atomic<u64> heartbeat_ms;
    std::atomic<u64> claim_ms; // 只在认领与接管时写，恢复休眠不改
    std::atomic<u64> dormant_ms;
    std::atomic<u32> sample_rate;
    std::atomic<u32> max_block; // prepare 声明的最大块长
    std::atomic<u32> channels;
    std::atomic<u32> format;
    std::atomic<u32> version; // 绝对位置永久冻结
    std::atomic<u32> cfg_flags;
    std::atomic<u64> block_seq;
    std::atomic<u64> last_host_ns; // 0 = 宿主没给
    std::atomic<u32> rt_flags;
    std::atomic<u32> max_n_seen; // 诊断：本次 prepare 以来见过的最大块长
    std::atomic<u32> peak_bits; // 回送峰值的 float 位型
    std::atomic<u32> d_eff; // 实际环路延迟（样本）
    std::atomic<u32> safety_state;
    std::atomic<u32> resync_count;
    std::atomic<u32> nonfinite_count;
    u32 reserved0[5];
    std::atomic<u32> label_seq; // 偶数 = 稳定；写时先变奇数
    std::atomic<u32> colour_argb; // 0 = 未知
    std::atomic<u32> name_len; // 完整 UTF-8 轨道名的字节数
    std::atomic<u32> name_hash; // 完整 UTF-8 的 FNV-1a-32
    std::atomic<u32> name_words[kLabelNameWords]; // UTF-8 按码点截到 ≤ 112 字节，每字 4 字节小端，只供显示
};

// 组块：组 G（1..8）在 1024·G。panic_seq 预留给 Output 侧的急停，现在恒为 0。
struct alignas(64) GroupBlock
{
    RoleSlot input;
    RoleSlot output;
    std::atomic<u32> panic_seq;
    u32 reserved[127];
};

struct alignas(64) RvSegment
{
    RvHeader header;
    u32 reserved0[240];
    GroupBlock groups[kGroupCount]; // groups[G - 1] 是组 G
    u32 reserved1[1792];
};

// ---- 数据段 g{G}（kRingBytes 字节） ----

// 环头。写方（属主 Output）先写 sample_rate、src_channels，再以 release 写 epoch 或 writer_token；
// 读方先 acquire 读 epoch、writer_token，再读 sample_rate。64..127 独占一条缓存行：写头 W 与 write_intent。
struct alignas(64) RingHeader
{
    std::atomic<u32> magic;
    std::atomic<u32> abi;
    std::atomic<u32> generation; // 只作校验，不触发重同步
    std::atomic<u32> ring_frames;
    std::atomic<u32> ring_channels;
    std::atomic<u32> sample_rate;
    std::atomic<u32> src_channels; // 只给界面显示
    std::atomic<u32> out_flags;
    std::atomic<u64> epoch; // 只在采样率改变时 + 1
    std::atomic<u64> writer_token; // 写权，与端点槽的 ctl 无关
    std::atomic<u32> nonfinite_count;
    std::atomic<u32> clamp_count;
    std::atomic<u32> w_cas_fail;
    std::atomic<u32> init_pid; // 认领了初始化的进程，同 RvHeader::init_pid（数据段名带 abi，这里不永久冻结）
    std::atomic<u64> write_head; // W：段寿命内单调递增的帧位置
    std::atomic<u32> last_block_len;
    u32 reserved1;
    std::atomic<u64> write_intent; // 写方动样本之前公告的「本段写完后的 W」，读方据此排除可能被覆盖的帧
    u32 reserved2[10];
    u32 reserved3[4064];
};

// 数据区：kRingFrames 帧 × 2 声道，左右交错，恒按立体声排；样本一律 relaxed 访问
struct alignas(64) RingSegment
{
    RingHeader header;
    std::atomic<float> data[kRingFrames * kRingChannels];
};

// ---- 字段表（唯一真源） ----
// 列：结构、字段、偏移、字节数、类型、是否永久冻结。保留区也列出来，对拍时检查同一结构的字段首尾相接、
// 不重叠不留缝，最后一项正好收在结构末尾。
// clang-format off
#define RINNE_IPC_LAYOUT_FIELDS(X) \
    X(RvHeader, magic,             0,      4,      "u32",        true)  \
    X(RvHeader, abi,               4,      4,      "u32",        true)  \
    X(RvHeader, generation,        8,      4,      "u32",        true)  \
    X(RvHeader, layout_bytes,      12,     4,      "u32",        true)  \
    X(RvHeader, groups,            16,     4,      "u32",        false) \
    X(RvHeader, group_stride,      20,     4,      "u32",        false) \
    X(RvHeader, probe_call_seq,    24,     8,      "u64",        false) \
    X(RvHeader, init_pid,          32,     4,      "u32",        true)  \
    X(RvHeader, reserved,          36,     28,     "reserved",   false) \
    X(RoleSlot, ctl,               0,      8,      "u64",        false) \
    X(RoleSlot, pid,               8,      4,      "u32",        false) \
    X(RoleSlot, host_ppid,         12,     4,      "u32",        false) \
    X(RoleSlot, heartbeat_ms,      16,     8,      "u64",        false) \
    X(RoleSlot, claim_ms,          24,     8,      "u64",        false) \
    X(RoleSlot, dormant_ms,        32,     8,      "u64",        false) \
    X(RoleSlot, sample_rate,       40,     4,      "u32",        false) \
    X(RoleSlot, max_block,         44,     4,      "u32",        false) \
    X(RoleSlot, channels,          48,     4,      "u32",        false) \
    X(RoleSlot, format,            52,     4,      "u32",        false) \
    X(RoleSlot, version,           56,     4,      "u32",        true)  \
    X(RoleSlot, cfg_flags,         60,     4,      "u32",        false) \
    X(RoleSlot, block_seq,         64,     8,      "u64",        false) \
    X(RoleSlot, last_host_ns,      72,     8,      "u64",        false) \
    X(RoleSlot, rt_flags,          80,     4,      "u32",        false) \
    X(RoleSlot, max_n_seen,        84,     4,      "u32",        false) \
    X(RoleSlot, peak_bits,         88,     4,      "u32",        false) \
    X(RoleSlot, d_eff,             92,     4,      "u32",        false) \
    X(RoleSlot, safety_state,      96,     4,      "u32",        false) \
    X(RoleSlot, resync_count,      100,    4,      "u32",        false) \
    X(RoleSlot, nonfinite_count,   104,    4,      "u32",        false) \
    X(RoleSlot, reserved0,         108,    20,     "reserved",   false) \
    X(RoleSlot, label_seq,         128,    4,      "u32",        false) \
    X(RoleSlot, colour_argb,       132,    4,      "u32",        false) \
    X(RoleSlot, name_len,          136,    4,      "u32",        false) \
    X(RoleSlot, name_hash,         140,    4,      "u32",        false) \
    X(RoleSlot, name_words,        144,    112,    "u32[28]",    false) \
    X(GroupBlock, input,           0,      256,    "RoleSlot",   false) \
    X(GroupBlock, output,          256,    256,    "RoleSlot",   false) \
    X(GroupBlock, panic_seq,       512,    4,      "u32",        false) \
    X(GroupBlock, reserved,        516,    508,    "reserved",   false) \
    X(RvSegment, header,           0,      64,     "RvHeader",   false) \
    X(RvSegment, reserved0,        64,     960,    "reserved",   false) \
    X(RvSegment, groups,           1024,   8192,   "GroupBlock[8]", false) \
    X(RvSegment, reserved1,        9216,   7168,   "reserved",   false) \
    X(RingHeader, magic,           0,      4,      "u32",        false) \
    X(RingHeader, abi,             4,      4,      "u32",        false) \
    X(RingHeader, generation,      8,      4,      "u32",        false) \
    X(RingHeader, ring_frames,     12,     4,      "u32",        false) \
    X(RingHeader, ring_channels,   16,     4,      "u32",        false) \
    X(RingHeader, sample_rate,     20,     4,      "u32",        false) \
    X(RingHeader, src_channels,    24,     4,      "u32",        false) \
    X(RingHeader, out_flags,       28,     4,      "u32",        false) \
    X(RingHeader, epoch,           32,     8,      "u64",        false) \
    X(RingHeader, writer_token,    40,     8,      "u64",        false) \
    X(RingHeader, nonfinite_count, 48,     4,      "u32",        false) \
    X(RingHeader, clamp_count,     52,     4,      "u32",        false) \
    X(RingHeader, w_cas_fail,      56,     4,      "u32",        false) \
    X(RingHeader, init_pid,        60,     4,      "u32",        false) \
    X(RingHeader, write_head,      64,     8,      "u64",        false) \
    X(RingHeader, last_block_len,  72,     4,      "u32",        false) \
    X(RingHeader, reserved1,       76,     4,      "reserved",   false) \
    X(RingHeader, write_intent,    80,     8,      "u64",        false) \
    X(RingHeader, reserved2,       88,     40,     "reserved",   false) \
    X(RingHeader, reserved3,       128,    16256,  "reserved",   false) \
    X(RingSegment, header,         0,      16384,  "RingHeader", false) \
    X(RingSegment, data,           16384,  524288, "f32[131072]", false)

// 结构的大小与对齐
#define RINNE_IPC_LAYOUT_STRUCTS(X) \
    X(RvHeader,    64,     64) \
    X(RoleSlot,    256,    64) \
    X(GroupBlock,  1024,   64) \
    X(RvSegment,   16384,  64) \
    X(RingHeader,  16384,  64) \
    X(RingSegment, 540672, 64)

// 进 golden 的常量（与上面的 inline constexpr 一一对应）
#define RINNE_IPC_LAYOUT_CONSTANTS(X) \
    X(kIpcAbi) X(kRvMagic) X(kRingMagic) X(kInitMarker) X(kGroupCount) X(kMaxSegmentNameLength) \
    X(kRvBytes) X(kRvMinBytes) X(kRvHeaderBytes) X(kGroupStride) X(kRoleSlotBytes) X(kLabelNameWords) X(kLabelNameBytes) \
    X(kRingHeaderBytes) X(kRingFrames) X(kRingChannels) X(kRingDataOffset) X(kRingBytes) \
    X(kMaxSupportedBlock) X(kMaxWriteChunk) \
    X(kCtlPhaseMask) X(kCtlPhaseFree) X(kCtlPhaseClaiming) X(kCtlPhaseActive) X(kCtlPhaseDormant) X(kCtlProvisional) \
    X(kFormatVst3) X(kFormatAu) \
    X(kCfgAltBackend) X(kCfgSmartOrigin) X(kCfgNeverPaired) \
    X(kRtBypassed) X(kRtNonrealtime) X(kRtOverMax) X(kRtStalled) X(kRtUnstable) X(kRtTripped) X(kRtConflict) \
    X(kRtUnsupportedBlock) \
    X(kSafetyUnpaired) X(kSafetyFadeIn) X(kSafetyRun) X(kSafetyStalled) X(kSafetyGuard) X(kSafetyTripped) \
    X(kTripShift) X(kTripPanic) X(kTripNan) X(kTripHuge) X(kTripRunaway) \
    X(kWriteIdle) X(kWriteActive) X(kWriteBypassedZeros) X(kWriteConflict) X(kWriteUnsupportedBlock) \
    X(kOutBypass) X(kOutFading)
// clang-format on

#define RINNE_IPC_ASSERT_FIELD(S, f, off, bytes, type, permanent)             \
    static_assert(offsetof(S, f) == (off), #S "::" #f " 的偏移与字段表不符"); \
    static_assert(sizeof(S::f) == (bytes), #S "::" #f " 的大小与字段表不符");
RINNE_IPC_LAYOUT_FIELDS(RINNE_IPC_ASSERT_FIELD)
#undef RINNE_IPC_ASSERT_FIELD

#define RINNE_IPC_ASSERT_STRUCT(S, bytes, align)                   \
    static_assert(sizeof(S) == (bytes), #S " 的大小与字段表不符"); \
    static_assert(alignof(S) == (align), #S " 的对齐与字段表不符");
RINNE_IPC_LAYOUT_STRUCTS(RINNE_IPC_ASSERT_STRUCT)
#undef RINNE_IPC_ASSERT_STRUCT

static_assert(sizeof(RvHeader) == kRvHeaderBytes && sizeof(RoleSlot) == kRoleSlotBytes);
static_assert(sizeof(GroupBlock) == kGroupStride && sizeof(RvSegment) == kRvBytes);
static_assert(offsetof(RvSegment, groups) == kGroupStride, "组 G 的组块在 1024·G");
static_assert(sizeof(RingHeader) == kRingHeaderBytes && sizeof(RingSegment) == kRingBytes);
static_assert(offsetof(RingSegment, data) == kRingDataOffset);
static_assert(sizeof(RoleSlot::name_words) == kLabelNameBytes);

// 永久冻结面的绝对位置，用字面量钉住（上面的字段表断言随表一起改就会跟着挪，这里不会）
static_assert(offsetof(RvSegment, header) == 0, "会合段头在段首：前 16 字节与 init_pid 的绝对位置依赖它");
static_assert(offsetof(RvHeader, magic) == 0 && offsetof(RvHeader, abi) == 4 && offsetof(RvHeader, generation) == 8 &&
                  offsetof(RvHeader, layout_bytes) == 12,
              "会合段头的前 16 字节永久冻结");
static_assert(offsetof(RvHeader, init_pid) == 32, "初始化者 pid 的位置永久冻结");
static_assert(offsetof(GroupBlock, input) == 0 && offsetof(GroupBlock, output) == kRoleSlotBytes,
              "端点槽 version 的绝对位置依赖组块里两个槽的位置");
static_assert(offsetof(RoleSlot, version) == 56, "端点槽 version 的槽内位置永久冻结");

// 组 G（1..8）的组块与端点槽在 rv 里的字节偏移，按结构体的真实偏移算；端点槽 version 的绝对位置由它决定，永久冻结
constexpr u32 groupBlockOffset(u32 group) noexcept
{
    return static_cast<u32>(offsetof(RvSegment, groups) + sizeof(GroupBlock) * (group - 1));
}
constexpr u32 roleSlotOffset(u32 group, bool output) noexcept
{
    return groupBlockOffset(group) +
           static_cast<u32>(output ? offsetof(GroupBlock, output) : offsetof(GroupBlock, input));
}
// 按字节读任意 abi 的会合段里那个端点槽的 version：视图先要不小于 kRvMinBytes
constexpr u32 slotVersionOffset(u32 group, bool output) noexcept
{
    return roleSlotOffset(group, output) + static_cast<u32>(offsetof(RoleSlot, version));
}
static_assert(groupBlockOffset(1) == 1024 && groupBlockOffset(8) == 8192);
static_assert(slotVersionOffset(1, false) == 1080);
static_assert(slotVersionOffset(8, true) == 8504);
static_assert(slotVersionOffset(kGroupCount, true) + sizeof(u32) <= kRvMinBytes, "最远的 version 落在冻结下限之内");

} // namespace rinne::ipc
