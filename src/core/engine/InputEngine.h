// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine：Rinne Input 的 core 侧引擎。宿主壳只做映射（参数 → EngineParams，宿主播放头 → TransportInfo，
// 宿主缓冲 → AudioView，块首租约 → EngineContext），然后调这里。
//
// 引擎负责：把参数与走带整体下传给环路层、把环路层混好的回送乘上增益加到干声上、
// 把测量量、诊断事件与本端的端点槽发布出去。
//
// 最小实现与安全链的分工：
//   - 本引擎的回送路径是「读头 → 清洗（非有限 → 0、钳 ±16，逐声道）→ 增益（feedback × g_ret，30 ms 线性平滑）
//     → 硬钳 |ret| < 1.0 → 加到干声上」。清洗与硬钳是安全链接入之前的兜底（读环后清洗、回送有硬上界这两条不能等）；
//     反馈默认 0，所以默认就没有回送。
//   - 安全链（清洗、DC 阻断、限制器、软削波、失控保护、500 ms 淡入与锁存）接入之后，回送路径换成安全链处理过的那一路，
//     淡入、上界与锁存都由安全链负责；本引擎不削弱它们。
//
// 命令序号（panicSeq / recoverSeq / presetSeq / stateSeq）：
//   引擎自己记基线、自己比对。基线在构造后、prepare 前给一次（setCommandBaseline）。
//   之后每块在块首比对「与上一块不同就执行一次」：
//     panicSeq 变了   → 紧急切断（安全链接上之后由安全链执行；接上之前只记基线、不生效）
//     recoverSeq 变了 → 恢复回送（同上）
//     presetSeq 变了  → 请求淡入，原因 FadeReason::Preset（载入工厂预设）
//     stateSeq 变了   → 请求淡入，原因 FadeReason::SetState（宿主设置工程状态）
//   四个序号各自记基线，基线之外的变化才算「新命令」：重建后不重放旧命令。
//   安全链自己的 setCommandBaseline 只收 panic 与 recover 两个序号，本引擎不改它的签名：
//   引擎侧留一个可选的请求淡入入口（setSafetyChain），把 presetSeq / stateSeq 的判定结果按各自的原因送进去。
//
// 线程：
//   - setCommandBaseline / setSafetyChain（非实时线程）：构造后、prepare 前调。
//   - prepare（非实时线程）：分配本地延迟线与暂存、重算全部按样本计的时长。换采样率才重新分配本地延迟线；
//     同采样率重新 prepare 不动读头与延迟线（流保持连续）。可以分配。
//   - process（音频线程）：拉环、读回送、加干声、发布端点槽。不加锁、不分配、不做 I/O、不读墙钟、不抛异常。
//   - 0 样本调用：立刻返回，不推进任何计数。
//
// 分层：core 不 include JUCE。
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "diag/EventQueue.h"
#include "dsp/LinearSmoother.h"
#include "engine/AudioView.h"
#include "engine/EngineContext.h"
#include "engine/EngineEvents.h"
#include "engine/EngineParams.h"
#include "engine/Meters.h"
#include "engine/SafetyEvents.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"
#include "loop/LoopSession.h"
#include "loop/LoopStatus.h"

namespace rinne::engine
{

// 诊断安全段的计数：音频线程写、消息线程读，必须无锁。
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "诊断计数必须是无锁的原子量");

// 请求淡入的入口：本引擎只「请求」，不实现淡入本身。安全链接上之后由它执行；没接上时请求不发生。
// 函数指针加 context（不用 std::function）：音频线程上调用，不能有分配或异常；接入方也不必改本引擎的头文件。
using FadeInRequest = void (*)(void* context, FadeReason reason) noexcept;

// 诊断报告安全段（DiagSafety）的六个量。消息线程读的纯值快照。
//
// 来源：
//   state / trips      安全状态与累计跳闸次数：读 Meters::safety（安全链写），引擎数状态进入 Tripped 的次数。
//   tailPeakMaxDb      读头输出（未经安全链）的峰值，本会话最大，dBFS。没有回送时是 kSilentDb。
//   limiterGrMaxDb     限制器增益缩减的最大值，dB（≥ 0）。安全链接入之前恒为 0。
//   nonfiniteRing      写方写环前清洗掉的非有限样本数（环头计数，Output 写）。
//   clampRing          写方写环前钳到 ±16 的样本数（环头计数，Output 写）。
struct SafetyDiag
{
    static constexpr float kSilentDb = -200.0f;

    std::uint8_t state = 0; // SafetyState
    std::uint32_t trips = 0;
    float tailPeakMaxDb = kSilentDb;
    float limiterGrMaxDb = 0.0f;
    std::uint32_t nonfiniteRing = 0;
    std::uint32_t clampRing = 0;
};

// InputEngine。
class InputEngine
{
public:
    InputEngine() noexcept;

    // 非实时线程：构造后、prepare 前调一次，告诉引擎当前四个命令序号是基线。
    // 之后每块在块首比对，基线之外的变化才算新命令；重建后不重放旧命令。
    // 安全链接上之前，panicSeq / recoverSeq 的变化只记进基线、不生效（切断与恢复由安全链执行，本引擎不实现它们）；
    // presetSeq / stateSeq 的变化在接了请求淡入入口（setSafetyChain）时送出请求。
    void setCommandBaseline(const CommandBaseline& baseline) noexcept;

    // 非实时线程：接安全链的请求淡入入口。接上之后，presetSeq / stateSeq 的变化会请求淡入（各自的原因）。
    void setSafetyChain(void* chain, FadeInRequest request) noexcept;

    // 非实时线程：准备。sampleRate 与 maxBlock 是宿主声明的值。
    void prepare(double sampleRate, int maxBlock) noexcept;

    // 音频线程：处理一块（就地：干声进、干声 + 回送出）。bypassed 为真表示宿主旁路了这一端：
    // 照常拉环、推进读位，回送增益滑到 0，干声逐位直通。
    // 返回 true 表示这一块被处理了；false 表示 0 样本调用。
    bool process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport, bool bypassed,
                 const EngineContext& context) noexcept;

    // 消息线程：排空诊断事件。事件码来自环路层（0x03xx）与引擎层（0x04xx）两段。
    // 队列归引擎所有：音频线程往里推，消息线程经这里取走。
    template<typename Sink>
    int drainEvents(Sink&& sink) noexcept
    {
        return events_.drain(std::forward<Sink>(sink));
    }

    // 消息线程：环路层的状态快照（诊断与界面要的量都在这里）。
    // 之后往 LoopStatusFields 里加字段，引擎的代码不用变。
    loop::LoopStatusFields loopStatus() const noexcept { return loop_.statusFields(); }

    // 两端同被宿主挂起 > 250 ms 之后清空本地延迟线（防停用止啸后原样复燃）：**本引擎还没有这个入口**。
    // 本引擎只处理得了「Input 照常被调用、环上没有写方」那一种停用（环路层自己计时清空）。宿主把两端一起挂起时
    // 音频线程根本不被调用，停了多久只有壳在消息线程上判得出来：flush 入口由壳在消息线程判定停用超过 250 ms 后
    // 触发（置一个请求，音频线程在恢复后的第一块块首执行环路层的 flushLocalDelayLine —— 它改的是音频线程独占的
    // 状态，只改两个整数），接线在后续接上壳的那一步，不在这里，本引擎也不声称已经做到。

    // 消息线程：诊断报告安全段的六个量。
    SafetyDiag safetyDiag() const noexcept;

    // 测量量（回送 DC 均值、贴近满幅时长、限制器增益缩减、安全状态）。
    // 安全链接上之后由安全链填；本引擎的最小实现填自己知道的部分。
    const Meters& meters() const noexcept { return meters_; }
    Meters& meters() noexcept { return meters_; }

    // 音频线程（同一线程、块之间）：上一块环路层的结果。DSP 复位点（resetDsp / resetAtSample）、
    // 补 0 的个数、逐样本有效位都在里面；试验台据此在复位点上复位中间链。
    const loop::BlockResult& lastBlockResult() const noexcept { return lastResult_; }

    // 命令序号的基线（诊断与场景用）：引擎当前认为的基线值
    CommandBaseline commandBaseline() const noexcept;

    // 采样率与最大块长（诊断用）
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }
    // 本会话观察到的最大块长（N_obs）。每次 prepare 清零；超长块（n > 32768，环路不走的块）不计入。
    int maxBlockSeen() const noexcept { return maxBlockSeen_; }

    // 干声的增益（线性律：m = mixPercent / 100，g_dry = min(1, 2(1−m))）。
    float dryGain() const noexcept { return dryGain_; }
    // 回送增益的目标值（feedback × g_ret；旁路或环路关时为 0）。实际增益按 30 ms 线性平滑滑过去。
    float returnGain() const noexcept { return returnGain_; }

private:
    void noteBlockLength(int n) noexcept;
    // 块首的命令序号比对：变了就执行一次（不重放旧命令）。
    void checkCommands(const EngineParams& params) noexcept;
    // 发布到本端（Input）的会合槽：块序号、宿主时间戳、实际环路延迟、实时标志、本会话最大块长、回送峰值。
    void publishToSlot(const EngineContext& context, const TransportInfo& transport, bool bypassed,
                       const loop::BlockResult& result, float blockPeak) noexcept;
    // 环路层的事件出口：推进本引擎的诊断队列（音频线程上调用）。
    static void onLoopEvent(void* context, std::uint16_t code, std::uint8_t a, std::uint8_t b,
                            std::int64_t sample) noexcept;

    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    int maxBlockSeen_ = 0; // 本会话观察到的最大块长（N_obs）；每次 prepare 清零
    bool prepared_ = false;

    loop::LoopSession loop_;
    diag::EventQueue<256> events_;
    loop::BlockResult lastResult_;

    Meters meters_;

    // 混好的回送与逐样本有效位：预分配在 prepare 里，音频线程上不再分配
    std::vector<float> returnLeft_;
    std::vector<float> returnRight_;
    std::vector<std::uint8_t> returnValid_;

    // 命令序号的基线
    CommandBaseline baseline_;
    CommandBaseline seen_;

    // 安全链的接入口（可空）
    void* safetyChain_ = nullptr;
    FadeInRequest safetyRequest_ = nullptr;

    // 回送增益：feedback × g_ret 的 30 ms 线性平滑（增益变化不得出现阶跃）
    dsp::LinearSmoother returnSmoother_{0.0f};
    float returnGain_ = 0.0f;
    float dryGain_ = 1.0f;

    // 离线渲染的进入沿（只发一次事件）
    bool lastNonrealtime_ = false;

    // 回送直流均值（清洗之后、乘增益之前）：左右各一个一阶平均，时间常数 300 ms（与安全链的同一个量相同）
    double dcMeanLeft_ = 0.0;
    double dcMeanRight_ = 0.0;
    double dcCoeff_ = 0.0;

    // 诊断安全段的账：读头峰值（线性，本会话最大）、跳闸次数、环头的清洗计数
    AtomicFloat tailPeakMax_;
    AtomicFloat limiterGrMax_;
    std::atomic<std::uint32_t> trips_{0};
    std::atomic<std::uint32_t> ringNonFinite_{0};
    std::atomic<std::uint32_t> ringClamp_{0};
    std::uint8_t lastSafetyState_ = 0;
};

} // namespace rinne::engine
