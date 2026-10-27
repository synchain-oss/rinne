// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路层与引擎层之间的接缝：InputEngine 每块把整份参数与走带交给环路层，
// 环路层算好 D、建好读头、把混好的回送交给引擎。
//
// 分工（这样分是刻意的，理由见本文件末尾与 ADR-0002）：
//   - 参数与走带整体下传：D 的目标值（时间 / 节拍、毫秒与节拍到样本的换算、钳位与抬升）全在环路层算，
//     引擎不预先把 delay_ms 换算成样本。节拍同步因此只需要改环路层，不必改引擎。
//   - 读头归环路层：读头槽、每个读头的包络、读头混合都在环路层。引擎只拿走混好的回送。
//   - 违约路径在环路层：「块长超过声明值 → 补 0 并抬升 D 下限」在环路层实现，导出锁存的计数也挂在同一处。
//   - 拉环归环路层：每块把环里 [pulled, W) 的帧全部搬进本地延迟线（一圈之内），pulled 每块追到 W。
//   - 状态对象归环路层：诊断与界面要的量都落在一个结构里，引擎原样交出去。
//   - 对端槽归环路层读：宿主时间戳、块序号与实时标志是「谁在谁前面」的证据，判据也在环路层。
//
// 「本块是不是有界」是块级事实，不是参数：违约（n 大于声明值）与超长（n 大于 32768）都按块长 n 判定，
// 结果由 BlockResult 的标志给出，参数面不装它。
//
// 线程：BlockContext 与 BlockResult 都是纯值，可以按值传；LoopSession 持有全部可变状态，
// 音频线程独占它（不加锁、不分配、不读墙钟）。
#pragma once

#include <cstdint>
#include <vector>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopBinding.h"
#include "loop/BeatSync.h"
#include "loop/DelayRetarget.h"
#include "loop/ExportLatch.h"
#include "loop/LocalLine.h"
#include "loop/LoopEvents.h"
#include "loop/LoopStatus.h"
#include "loop/LoopTiming.h"
#include "loop/OrderEvidence.h"
#include "loop/StreamSync.h"
#include "loop/Taps.h"

namespace rinne::loop
{

// 一块的全部输入。壳每块填一份新的（按值，不复用同一份），因此场景可以逐块改参数。
//
// 参数与走带按引用传：漏填不该是「静默失败」（漏填一个空指针，引擎会悄悄用默认值跑一整块）。
// 引用让「必须给」这件事在编译期就成立。
struct BlockContext
{
    BlockContext(const engine::EngineParams& paramsIn, const engine::TransportInfo& transportIn) noexcept
        : params(paramsIn), transport(transportIn)
    {
    }

    // 参数与走带（整体下传）
    const engine::EngineParams& params;
    const engine::TransportInfo& transport;

    // 环：调用方经租约拿到的段。环路层用它读写头与样本。
    ipc::RingSegment* ring = nullptr;
    // 段的映射代号（EngineContext::ringGeneration 原样传下来）。环路层据此（连同段的地址）判「换了段」：
    // 换了段时旧读头只读旧段已经拉进来的数据，新段从它自己的写头起重建读头。0 = 壳没给（只按地址判）。
    std::uint64_t ringGeneration = 0;
    // 会合段：读对端槽（宿主时间戳、块序号、实时标志）。可以是空（没有对端）。
    ipc::RvSegment* rendezvous = nullptr;
    // 组号 1..8。会合段按组块排布，组号 0 或越界时不去读对端槽（读会越界）。
    std::uint32_t group = 0;
    // 本端在会合段里的角色。环路层按它找对端槽（对端是它的反面）。InputEngine 恒填 kInput。
    ipc::Role role = ipc::Role::kInput;

    // 本端准备时声明的最大块长（N_prep）
    std::int64_t declaredBlock = 0;
    // 本会话观察到的最大块长（N_obs）。引擎每块更新。
    std::int64_t observedBlock = 0;
    // 对端 prepare 声明的最大块长（读对端槽得到；读不到时用这一项）
    std::int64_t peerMaxBlock = 0;

    // 宿主旁路
    bool bypassed = false;
    // 宿主调用了 reset()：只记诊断
    bool resetRequested = false;

    // 本块的样本数 n。0 表示「没有音频」（引擎必须立刻返回、不推进任何计数）。
    // 环里读到的帧数由环路层自己算：读不到就补 0，补 0 的个数是 n 减读到的帧数。
    int blockLength = 0;

    // 混好的回送往哪儿写：引擎预分配好，环路层只往里写。指针为空时环路层只算账不出样本。
    // 长度必须不小于 blockLength。
    float* returnLeft = nullptr;
    float* returnRight = nullptr;
    // 逐样本的有效位（可选）：环路层写「这个样本是不是环里的真数据」。
    // 长度必须不小于 blockLength。为空时不产出。
    std::uint8_t* returnValid = nullptr;
};

// 一块的结果。引擎用它决定输出与后续动作。
struct BlockResult
{
    // 混好的回送（左右各一份，长度 = returnLength）。引擎提供缓冲（预分配），环路层往里写；
    // 指针为空时环路层只算账不出样本。
    // returnLength 一般等于块长；超长块（n > 32768）里只有开头那一段淡出（不超过 5 ms），returnLength 是它的长度，
    // 之后的样本一律当 0（它们不在缓冲里：缓冲只容得下支持的最大块长）。
    float* returnLeft = nullptr;
    float* returnRight = nullptr;
    int returnLength = 0;

    // 逐样本的有效位（可选，长度 = 块长）：1 = 这个样本是环里的真数据，0 = 补出来的 0。
    // 引擎据此区分「补出来的 0」与「真的是 0」。
    std::uint8_t* returnValid = nullptr;

    // 本块实际的 D（样本）：主读头正在读的 D，不是目标 D。改 D 的新目标落在 100 ms 限速窗口里时先挂着，
    // 主读头还在读旧的 D，这里就还是旧的；交叉淡化开始的那一块起新读头成为主读头，这里就是新的 D
    // （两个读头同时在跑的 40 ms 里，要覆盖两者的量看 estimatorDelay）。还没有主读头
    // （没配对、延迟线刚清空）时没有「正在读的位置」，退回目标 D。
    // 延迟下限（样本）是下一行。
    std::int64_t delayEffective = 0;
    std::int64_t delayMin = 0;
    // 用户要的 D（毫秒）
    float delayUserMs = 0.0f;
    // 估计窗口用的 D：全部活跃读头里最大的那个。安全链的链增益估计窗口要盖住它。
    std::int64_t estimatorDelay = 0;

    // 本块实际写了多少个非零回送样本（快速路径的判据之一）
    std::uint64_t nonZeroReturn = 0;
    // 补 0 的样本数：主读头本该有数据（落在它的有效区间里）、但环里还没写到的那些样本。
    std::uint64_t overrun = 0;
    // 宿主违约：块长超过声明值（D 的下限已在本块抬到块长）
    bool hostViolation = false;
    // 块长超过支持上限（32768）
    bool unsupportedBlock = false;

    // 配对与断档（本块的状态，不是边沿）：有对端且能读环；写头不动已判成断档。安全链每块的控制量要这两个
    bool paired = false;
    bool stalledState = false;

    // 事件
    bool resynced = false;
    // 重同步的原因（resynced 为真时有效）。换写方、换绑定之后引擎要从首个有效读样本起重新淡入
    SyncReason resyncReason = SyncReason::kOffset;
    // DSP 的复位点落在本块里：引擎（与它后面的 DSP）在 resetAtSample 那个样本上复位。
    // 本地延迟线已由环路层在起播沿上复位过，引擎不必、也不该再清它（再清会把新读头一起作废）。
    bool resetDsp = false;
    // 起播沿的复位点（绝对样本号，与读位 R 同一坐标；本块内的偏移 = resetAtSample − 本块读位起点）。
    // 起播沿那一块就给出（预告），复位点所在的那一块再给一次并置 resetDsp。没有时是 -1。
    // 口径见 ADR-0002：导出与播放从 s0 + T 起逐位一致。
    std::int64_t resetAtSample = -1;
    // 本块的读位起点 R（绝对样本号）。resetAtSample 减去它就是块内偏移。
    std::int64_t blockStart = 0;
    bool lineCleared = false; // 本地延迟线被清空
    bool stalled = false;
    bool enteredTolerant = false;

    // 快速路径的判据：回送已经彻底安静（全部读头包络为 0、没有活跃读头、没有尾巴）。
    // 引擎据此决定要不要走逐位直通的快速路径。
    bool tapSettledSilent = false;
};

// 环路层的一次会话。音频线程独占；没有第二个人碰它，所以不需要锁。
class LoopSession
{
public:
    // 非实时线程：准备。采样率变了才重算常数与重分配延迟线；同采样率重新 prepare 什么都不动
    // （流保持连续）。可以分配。
    bool prepare(double sampleRate) noexcept;

    // 音频线程：处理一块。产出混好的回送（写进 ctx 指向的缓冲），并更新状态。
    // 不分配、不加锁、不读墙钟、不抛异常。
    BlockResult process(const BlockContext& ctx) noexcept;

    // 音频线程：清空本地延迟线（跳闸淡出之后）。只改两个整数，不 memset。读头与末值保持一并作废。
    void flushLocalDelayLine() noexcept;

    // 音频线程，块首、process 之前：两端停用（宿主挂起）超过 250 ms 之后清空本地延迟线，防停用止啸后原样复燃。
    // 与 flushLocalDelayLine 的区别：上一块的最后一个回送样本不直接落到 0，而是保持它、5 ms 线性衰减（末值保持，
    // 不读任何旧数据）。之后从写头起重建读头，新数据照常 20 ms onset。
    void flushAfterSuspend() noexcept;

    // 音频线程：状态快照（诊断与界面读）
    const LoopStatus& status() const noexcept { return status_; }

    // 消息线程：状态快照的一份拷贝
    LoopStatusFields statusFields() const noexcept { return status_.snapshot(); }

    // 事件出口（消息线程装，音频线程只调）
    void setEventSink(LoopEventSink sink) noexcept;

    // 只读：当前实际的 D（样本），含义同 BlockResult::delayEffective（主读头正在读的 D）
    std::int64_t delayEffective() const noexcept { return delayInUse_; }
    // 只读：目标 D（样本）：用户要的 D 抬到下限、截到上限之后的值。改 D 的过渡没走完时它与实际的 D 不同
    std::int64_t delayTarget() const noexcept { return delayTarget_; }
    // 只读：当前延迟下限（样本）
    std::int64_t delayMin() const noexcept { return delayMin_; }
    // 只读：读位 R（已处理帧数）。R 由判据那一半持有，这里只是转出来。
    std::int64_t processed() const noexcept { return sync_.processed(); }

private:
    std::int64_t computeDelay(const BlockContext& ctx) noexcept;
    void updateBpm(const BlockContext& ctx) noexcept;
    // 拉环：把环里 [pulled, W) 的帧搬进本地延迟线（经定长暂存分段搬）。比 W 早一圈减护栏的帧不读（可能正被覆盖），
    // 记 overflow；写方公告领先一圈以上的帧当作撕裂，记 torn。之后 pulled == W。
    void pullRing(const ipc::RingSegment& ring, i64 writeHead, i64 peerMaxBlock) noexcept;
    // 一个读头本块里「落在有效区间、但环里还没写到」的样本数：这些样本只能补 0。
    std::int64_t tapOverrun(const Tap& tap, i64 blockStart, i64 n) const noexcept;
    // 主读头（TapSet::mainSlot）的 tapOverrun；没有主读头时是 0。统计里的「补 0」认这一个。
    std::int64_t mainTapOverrun(i64 blockStart, i64 n) const noexcept;
    // 每个没在淡出的读头（含交叉淡化里的旧读头）本块要读到还没写到的位置时：在它还读得到的样本里淡到 0，
    // 补 0 之前不留阶跃。
    void fadeTapsAtUnderrun(i64 blockStart, i64 n) noexcept;
    // 块首就读不到数据、上一个样本还在出声的读头（写头恰好在块边界落后：补 0、断档、失去对端）：
    // 它在上一个样本上的贡献并进末值保持，5 ms 线性衰减，回送不从末值直接落到 0。
    void holdStrandedTaps(i64 blockStart) noexcept;
    // 读头整体作废时（两端停用之后的清空、写头倒退）：上一块最后一个回送样本按末值保持 5 ms 线性衰减。
    void holdLastReturn() noexcept;
    // 建一个新读头（onset 走 20 ms）。槽满时收掉包络最小的那个（它的末值接成 5 ms 衰减的保持，不阶跃），
    // 返回槽下标；没有槽时返回 -1。blockStart 是本块的读位起点。
    int newTap(i64 blockStart, i64 validFrom, i64 delay, std::int64_t onset) noexcept;
    // 改 D：开一段 40 ms 线性交叉淡化（旧读头 1 − x、新读头 x，两路增益之和恒为 1），受 100 ms 前沿限速约束。
    // 起点可以落在本块中间（限速窗口恰好在那个样本到点），所以结果与宿主怎么切块无关。n 是本块的样本数。
    // 同一时刻至多一段交叉淡化；读头槽用完、窗口没到点或上一段还没走完时新目标先挂着，下一块再试。
    // 返回本块是否开了一段交叉淡化。
    bool startDelayCrossfade(i64 blockStart, i64 n, i64 delay) noexcept;
    // 超长块：环路这一块不走（只读不推进），回送在开头淡到 0。返回写进回送的样本数（淡出的长度，可以是 0）。
    std::int64_t fadeOutUnsupportedBlock(const BlockContext& ctx) noexcept;
    // 对端槽的事实：宿主时间戳、块序号、实时标志。没有会合段或组号不合法时什么都不读。
    void readPeerSlot(const BlockContext& ctx) noexcept;
    // 错序证据（口径见 OrderEvidence.h 与 ADR-0002）：每个 n > 0 的支持块判一次，结果发布到状态里。
    void judgeOrder(const BlockContext& ctx) noexcept;
    // 导出不一致锁存（口径见 ExportLatch.h）用的对端事实：对端槽的 kRtOverMax 只在对端的一个新块上看一次。
    // 每个读了对端槽的块都要调（不管是不是离线块），块序号的记账才跟得上。
    bool peerOverMaxOnNewBlock() noexcept;

    LocalLine line_;
    TapSet taps_;
    ReturnHold hold_;
    StreamSync sync_;
    LoopStatus status_;
    LoopEventSink sink_;

    // 拉环的暂存：环里读到的帧先落在这里，再整段搬进延迟线。
    // 暂存是定长的（最大块长 32768 帧），在 prepare 里分配，音频线程上不再分配。
    std::vector<float> pullLeft_;
    std::vector<float> pullRight_;

    // 上一块是不是超长块：超限的提示码只在进入超限时发一条
    bool lastUnsupported_ = false;

    // 上一块最后一个回送样本（混好的，含末值保持）与它的输出下标（R 坐标；-1 = 没有）：停用后清空时保持它
    float lastReturnLeft_ = 0.0f;
    float lastReturnRight_ = 0.0f;
    std::int64_t lastReturnAt_ = -1;

    // 上一块读的是哪一个段（地址与映射代号）：只用来判「是不是同一个段」，从不经它去读（段只在块内有效）。
    const ipc::RingSegment* lastRing_ = nullptr;
    std::uint64_t lastRingGeneration_ = 0;

    double sampleRate_ = 0.0;
    bool prepared_ = false;

    std::int64_t offset_ = 0; // 主读头的同步偏移
    std::int64_t delayTarget_ = 0; // 目标 D（样本）：换算、抬到下限、截到上限之后。读头换上它之前，实际读的还是旧的 D
    std::int64_t delayInUse_ = 0; // 发布的实际 D：主读头正在读的 D（没有主读头时是目标 D）
    std::int64_t delayMin_ = 0;
    std::int64_t delayRequested_ = 0; // 用户要的 D（样本）
    std::int64_t xfadeLen_ = 1; // 交叉淡化的总长（样本）：40 ms，prepare 里按采样率换算
    RetargetLimiter retarget_; // 改 D 的 100 ms 前沿限速
    bool retargetHeld_ = false; // 新目标正被限速窗口挂着（每次挂起只发一条事件）
    TempoTracker tempo_; // 节拍同步用的速度：按会话计，prepare 不清
    SyncTargetFilter syncFilter_; // 节拍同步目标的抗抖滤波
    std::uint32_t delayHints_ = 0; // 当前的提示位（kDelayHint*）
    DelayMinReason delayMinReason_ = DelayMinReason::kNone; // 下限从哪来
    std::int64_t maxBlockSeen_ = 0; // 本会话见过的最大块长（引擎给的 N_obs 与本块 n 取大；prepare 换采样率清零）
    bool lastPlaying_ = false;
    std::uint64_t lastWriterToken_ = 0;
    std::uint64_t lastEpoch_ = 0;
    bool havePeer_ = false;
    std::uint32_t tapDropCount_ = 0; // 累计丢掉的读头数

    // 对端槽的快照（每块读一次）
    bool havePeerSlot_ = false;
    std::uint64_t peerHostNs_ = 0;
    std::uint64_t peerBlockSeq_ = 0;
    std::uint32_t peerRtFlags_ = 0;
    std::uint32_t peerMaxBlock_ = 0;

    // 错序证据（每块一次，口径见 OrderEvidence.h）
    OrderEvidence order_;

    // 导出不一致：对端槽的块序号只在新块上看一次它的 kRtOverMax（第一次只记下序号，不计）
    bool haveExportPeerSeq_ = false;
    std::uint64_t lastExportPeerSeq_ = 0;

    // 速度：宿主给的有效值、来源
    std::uint32_t bpmCenti_ = 12000u;
    std::uint8_t bpmSource_ = 2; // BpmSource::kFallback
};

} // namespace rinne::loop
