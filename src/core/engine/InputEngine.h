// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine：Rinne Input 的 core 侧引擎。宿主壳只做映射（参数 → EngineParams，宿主播放头 → TransportInfo，
// 宿主缓冲 → AudioView，块首租约 → EngineContext），然后调这里。
//
// 引擎负责：把参数与走带整体下传给环路层、把环路层混好的回送送进安全链、把安全链的输出加到干声上、
// 把测量量、诊断事件与本端的端点槽发布出去。
//
// 回送链（每个样本，固定顺序；安全链见 SafetyMachine.h）：
//   环路层的读头 → ① 清洗 → ② DC 阻断（SafetyChain::processFront）
//   → ③ 低切 → ④ 高切 → ⑤ 极性（音色级，还没有接入：这里是空位，接入方夹在前后两段之间）
//   → ⑥ ×g → 超大值检测 → ⑦ 限制器 → ⑧ 软削波 → ⑨ 有限性检查（SafetyChain::processBack）
//   → out = g_dry·干声 + 回送。
// 回送为 ±0 且 g_dry 恰为 1 的样本，宿主缓冲一位不碰（−0.0 与 NaN 载荷都原样留着）：加 +0.0 会把 −0.0 变成 +0.0，
// 任何算术都会改 NaN 的载荷，所以只能不写。g_dry 不为 1 时只乘干声，回送为 ±0 照样不加。
// 淡入（新配对、换写方、预设、宿主设状态、恢复）、上界、跳闸与锁存全由安全链负责；本引擎只把事实与命令交给它。
//
// 命令序号（panicSeq / recoverSeq / presetSeq / stateSeq）：
//   引擎记基线：构造时四个基线都是 0，并把 panic / recover 的基线交给安全链。之后的变化（包括首个音频块之前的）
//   在首块生效：首块之前按下的紧急切断不会被当成基线吞掉。新建引擎时计数器已经不是 0 的（重建引擎、重载工程），
//   壳在构造后、prepare 前用当时的计数调 setCommandBaseline，旧命令就不会重放。
//     panicSeq / recoverSeq 变了 → 原样交给安全链的块首控制量，由它执行（切断 / 恢复）
//     presetSeq 变了  → 安全链淡入，原因 FadeReason::Preset（载入工厂预设）
//     stateSeq 变了   → 安全链淡入，原因 FadeReason::SetState（宿主设置工程状态）
//   环路层报出换写方 → FadeReason::NewWriter；换绑定（换段、换组、写头倒退）→ FadeReason::FirstPair；
//   失去配对之后重新配上由安全链自己按配对位判（FirstPair）。淡入都从首个有效读样本起 500 ms，从运行中进入时先 10 ms
//   降到 0。
//
// 线程：
//   - setCommandBaseline / setFadeObserver（非实时线程）：构造后、prepare 前调。
//   - prepare（非实时线程）：分配本地延迟线与暂存、重算全部按样本计的时长。换采样率才重新分配本地延迟线；
//     同采样率重新 prepare 不动读头、延迟线与安全链（流保持连续）。可以分配。
//   - process（音频线程）：拉环、读回送、过安全链、加干声、发布端点槽。不加锁、不分配、不做 I/O、不读墙钟、不抛异常。
//   - requestFlushAfterSuspend（任意线程）：置一个原子标志，下一块块首执行。
//   - 0 样本调用：立刻返回，不推进任何计数。
//
// 分层：core 不 include JUCE。
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "diag/EventQueue.h"
#include "engine/AudioView.h"
#include "engine/EngineContext.h"
#include "engine/EngineEvents.h"
#include "engine/EngineParams.h"
#include "engine/Meters.h"
#include "engine/SafetyEvents.h"
#include "engine/SafetyMachine.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"
#include "loop/LoopSession.h"
#include "loop/LoopStatus.h"

namespace rinne::engine
{

// 诊断安全段的计数与标志：音频线程写、消息线程读，必须无锁。
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "诊断计数必须是无锁的原子量");
static_assert(std::atomic<bool>::is_always_lock_free, "停用清空的请求标志必须是无锁的原子量");

// 淡入请求的观察者：引擎每向自带的安全链请求一次淡入，就把原因原样通知它一次（诊断与场景用；不接也照常淡入）。
// 函数指针加 context（不用 std::function）：音频线程上调用，不能有分配或异常。
using FadeObserver = void (*)(void* context, FadeReason reason) noexcept;

// 诊断报告安全段（DiagSafety）的六个量，外加环内 NaN 的琥珀灯位。消息线程读的纯值快照。
//
// 来源：
//   state / trips      安全状态与累计跳闸次数：读 Meters::safety（安全链写），引擎数状态进入 Tripped 的次数。
//   tailPeakMaxDb      读头输出（未经安全链）的峰值，本会话最大，dBFS。没有回送时是 kSilentDb。
//   limiterGrMaxDb     限制器增益缩减的最大值，dB（≥ 0）。
//   nonfiniteRing      写方写环前清洗掉的非有限样本数（环头计数，Output 写）。
//   clampRing          写方写环前钳到 ±16 的样本数（环头计数，Output 写）。
//   loopNanLamp        环内插件产生过 NaN：当前写方的清洗计数最后一次增加之后 5 s 之内为真（按样本计）。
//                      只点灯、只计数，不跳闸（环内的 NaN 由 Output 清洗）；灯的显示归界面。
struct SafetyDiag
{
    static constexpr float kSilentDb = -200.0f;

    std::uint8_t state = 0; // SafetyState
    std::uint32_t trips = 0;
    float tailPeakMaxDb = kSilentDb;
    float limiterGrMaxDb = 0.0f;
    std::uint32_t nonfiniteRing = 0;
    std::uint32_t clampRing = 0;
    bool loopNanLamp = false;
};

// 琥珀灯在环内 NaN 最后一次计数之后亮多久（秒）
inline constexpr double kLoopNanLampSeconds = 5.0;

// InputEngine。
class InputEngine
{
public:
    InputEngine() noexcept;

    // 非实时线程：构造后、prepare 前调一次，告诉引擎当前四个命令序号是基线（panic / recover 的基线同时交给安全链）。
    // 之后每块在块首比对，基线之外的变化才算新命令；重建后不重放旧命令。不调时基线是 0：
    // 新实例的计数从 0 起，首个音频块之前按下的紧急切断在首块生效，不会被吞掉。
    void setCommandBaseline(const CommandBaseline& baseline) noexcept;

    // 非实时线程：接淡入请求的观察者（可空，见 FadeObserver）。只观察、不执行：安全链在引擎里，不接也照常淡入。
    void setFadeObserver(void* context, FadeObserver observer) noexcept;

    // 非实时线程：准备。sampleRate 与 maxBlock 是宿主声明的值。
    void prepare(double sampleRate, int maxBlock) noexcept;

    // 音频线程：处理一块（就地：干声进、干声 + 回送出）。bypassed 为真表示宿主旁路了这一端：
    // 照常拉环、推进读位，回送 10 ms 淡到 0（不跳闸、不清空），之后干声逐位直通。
    // 返回 true 表示这一块被处理了；false 表示 0 样本调用。
    bool process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport, bool bypassed,
                 const EngineContext& context) noexcept;

    // 任意线程：两端被宿主一起挂起超过 250 ms 之后，请求清空本地延迟线（防停用止啸后原样复燃）。
    // 宿主把两端一起挂起时音频线程根本不被调用，停了多久只有壳判得出来（它看得见 prepare / release 与处理回调的
    // 间隔）：壳判定之后调这里，引擎在恢复后的第一块块首执行（只改几个整数，不 memset）。回送不从挂起前的末值直接
    // 落到 0，而是保持它 5 ms 线性衰减；之后从写头起重建读头，新数据照常 onset。
    // 「Input 照常被调用、环上没有写方」那一种停用由环路层自己计时清空，不经这里。
    void requestFlushAfterSuspend() noexcept { flushRequest_.store(true, std::memory_order_release); }

    // 消息线程：排空诊断事件。事件码来自安全链（0x02xx）、环路层（0x03xx）与引擎层（0x04xx）三段。
    // 队列归引擎所有：音频线程往里推，消息线程经这里取走。
    template<typename Sink>
    int drainEvents(Sink&& sink) noexcept
    {
        return events_.drain(std::forward<Sink>(sink));
    }

    // 消息线程：环路层的状态快照（诊断与界面要的量都在这里）。
    // 之后往 LoopStatusFields 里加字段，引擎的代码不用变。
    loop::LoopStatusFields loopStatus() const noexcept { return loop_.statusFields(); }

    // 消息线程：诊断报告安全段的六个量与琥珀灯位。
    SafetyDiag safetyDiag() const noexcept;

    // 测量量（回送 DC 均值、贴近满幅时长、限制器增益缩减、失控保护、安全状态、读环后清洗的计数）：安全链每块末填。
    const Meters& meters() const noexcept { return meters_; }
    Meters& meters() noexcept { return meters_; }

    // 音频线程（同一线程、块之间）：上一块环路层的结果。DSP 复位点（resetDsp / resetAtSample）、
    // 补 0 的个数、逐样本有效位与混好的回送（安全链的输入）都在里面；试验台据此在复位点上复位中间链。
    const loop::BlockResult& lastBlockResult() const noexcept { return lastResult_; }

    // 音频线程（同一线程、块之间）：安全链本身（状态、锁存、跳闸原因、本块的安全事件，诊断与场景用，只读）。
    const SafetyChain& safetyChain() const noexcept { return chain_; }

    // 命令序号的基线（诊断与场景用）：引擎当前认为的基线值
    CommandBaseline commandBaseline() const noexcept;

    // 采样率与最大块长（诊断用）
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }
    // 本会话观察到的最大块长（N_obs）。每次 prepare 清零；超长块（n > 32768，环路不走的块）不计入。
    int maxBlockSeen() const noexcept { return maxBlockSeen_; }

    // 干声的增益（线性律：m = mixPercent / 100，g_dry = min(1, 2(1−m))）。
    float dryGain() const noexcept { return dryGain_; }
    // 回送增益的目标值（feedback × g_ret；旁路或环路关时为 0）。安全链按 30 ms（反馈与 g_ret）与 10 ms（环路开关
    // 与旁路）线性平滑滑过去，再乘它自己的 g_safety 与 g_guard。
    float returnGain() const noexcept { return returnGain_; }

private:
    void noteBlockLength(int n) noexcept;
    // 块首的命令序号比对：变了就执行一次（不重放旧命令）。
    void checkCommands(const EngineParams& params) noexcept;
    // 向安全链请求一次淡入，并通知观察者。
    void requestFadeIn(FadeReason reason) noexcept;
    // 安全链每块的控制量：参数映射加上环路层报的配对、断档与 D。
    SafetyControls makeControls(const EngineParams& params, const loop::BlockResult& result,
                                bool bypassed) const noexcept;
    // 整块逐样本过安全链并加到干声上；blockPeak 收读头（清洗后、安全链之前）的峰值。
    void renderBlock(const AudioView& buffer, const loop::BlockResult& result, float& blockPeak) noexcept;
    // 安全链本块的事件转进诊断队列（安全链每块开头清空它们），顺带数跳闸次数。
    void drainSafetyEvents() noexcept;
    // 环内 NaN 的琥珀灯：当前写方的清洗计数增加了就亮 5 s。换写方、换绑定、换段时重取基线，不把别人的历史算进来。
    void updateLoopNanLamp(const EngineContext& context, const loop::BlockResult& result, int n) noexcept;
    // 发布到本端（Input）的会合槽：块序号、宿主时间戳、实际环路延迟、实时标志、本会话最大块长、回送峰值、安全状态。
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
    SafetyChain chain_;
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

    // 淡入请求的观察者（可空）
    void* fadeObserver_ = nullptr;
    FadeObserver fadeObserverFn_ = nullptr;

    // 两端停用之后的清空请求（任意线程置位，音频线程块首取走）
    std::atomic<bool> flushRequest_{false};

    float returnGain_ = 0.0f;
    float dryGain_ = 1.0f;

    // 离线渲染的进入沿（只发一次事件）
    bool lastNonrealtime_ = false;

    // 环内 NaN 的琥珀灯：基线所在的段与它的清洗计数、还要亮多少个样本、亮灯的时长（样本）
    const ipc::RingSegment* lampRing_ = nullptr;
    std::uint64_t lampGeneration_ = 0;
    std::uint32_t lampBase_ = 0;
    std::int64_t lampLeft_ = 0;
    std::int64_t lampSamples_ = 0;
    std::atomic<bool> lampOn_{false};

    // 诊断安全段的账：读头峰值（线性，本会话最大）、跳闸次数、环头的清洗计数
    AtomicFloat tailPeakMax_;
    AtomicFloat limiterGrMax_;
    std::atomic<std::uint32_t> trips_{0};
    std::atomic<std::uint32_t> ringNonFinite_{0};
    std::atomic<std::uint32_t> ringClamp_{0};
};

} // namespace rinne::engine
