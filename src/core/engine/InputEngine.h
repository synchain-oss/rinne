// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine：Rinne Input 的 core 侧引擎。宿主壳只做映射（参数 → EngineParams，宿主播放头 → TransportInfo，
// 宿主缓冲 → AudioView），然后调这里。
//
// 引擎负责：把参数与走带整体下传给环路层、把环路层混好的回送送进安全链、干声与回送相加、
// 把测量量与诊断事件发布出去。
//
// 安全链（清洗、DC 阻断、限制器、软削波、失控保护）由接入安全链的那一张卡接进来；
// 本引擎的最小实现是 tap → 干声 + feedback 增益。回送的硬上界、淡入规则与锁存由安全链接管，
// 本引擎不削弱它们：没有安全链时回送增益恒为 0（默认安全），接上之后由安全链给增益。
//
// 命令序号（panicSeq / recoverSeq / presetSeq / stateSeq）：
//   引擎自己记基线、自己比对。基线在构造后、prepare 前给一次（setCommandBaseline）。
//   之后每块在块首比对「与上一块不同就执行一次」：
//     panicSeq 变了   → 紧急切断（安全链接上之后由安全链执行；本引擎只记事件）
//     recoverSeq 变了 → 恢复回送（同上）
//     presetSeq 变了  → 请求淡入，原因 FadeReason::Preset（载入工厂预设）
//     stateSeq 变了   → 请求淡入，原因 FadeReason::SetState（宿主设置工程状态）
//   四个序号各自记基线，基线之外的变化才算「新命令」：重建后不重放旧命令。
//   **安全链那条 setCommandBaseline 只收 panic 与 recover 两个序号**，本引擎不改它的签名：
//   引擎侧留一个可选的安全链入口（setSafetyChain），把四个序号的判定结果按各自的原因送进去。
//
// 线程：
//   - 构造（非实时线程）：可以带命令序号基线。基线让引擎知道「哪个序号算旧命令」，
//     于是首块之前的紧急切断不会被吞掉，重建后也不会重放旧命令。
//   - prepare（非实时线程）：分配本地延迟线、重算全部按样本计的时长。换采样率才重新分配；
//     同采样率重新 prepare 什么都不动（流保持连续）。可以分配。
//   - process（音频线程）：拉环、读回送、加干声。不加锁、不分配、不做 I/O、不读墙钟、不抛异常。
//   - 0 样本调用：立刻返回，不推进任何计数。
//
// 分层：core 不 include JUCE。
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "diag/EventQueue.h"
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

// 安全链接入点：本引擎只「请求淡入」，不实现淡入本身。
// 安全链接上之后由它执行；没接上时请求被记成事件，回送增益恒为 0（默认安全）。
//
// 签名固定成 std::function 是有意的：安全链那一张卡接入时不需要改本引擎的头文件。
using FadeInRequest = void (*)(void* context, FadeReason reason) noexcept;

// 一块要用的宿主侧上下文：段、会合段、组号、角色。
//
// 两端 process 的形状因此一致：宿主只要写一套调用代码。
// Input 侧要 ring 与 rendezvous；Output 侧要 seg 与 token。
struct EngineContext
{
    // 数据段（环）。Input 侧读它，Output 侧写它。可以为空（没有对端 → 只出干声）。
    ipc::RingSegment* ring = nullptr;
    // 会合段。Input 侧从中读对端槽（宿主时间戳、块序号、实时标志）。可以为空。
    ipc::RvSegment* rendezvous = nullptr;
    // 组号 1..8。会合段按组块排布，组号 0 或越界时不去读对端槽。
    std::uint32_t group = 0;
    // 本端在会合段里的角色。
    ipc::Role role = ipc::Role::kInput;
    // 自己的写权 token（Output 用；0 = 没有写权）。Input 侧不用。
    std::uint64_t token = 0;
};

// InputEngine。
class InputEngine
{
public:
    InputEngine() = default;

    // 非实时线程：构造后、prepare 前调一次，告诉引擎当前四个命令序号是基线。
    // 之后的变化（包括首块之前按下的紧急切断）在首块生效；重建后不重放旧命令。
    void setCommandBaseline(const CommandBaseline& baseline) noexcept;

    // 非实时线程：接安全链。接上之后，presetSeq / stateSeq 的变化会请求淡入（各自的原因）。
    // 不接时请求只记事件，回送增益恒为 0（默认安全）。
    void setSafetyChain(void* chain, FadeInRequest request) noexcept;

    // 非实时线程：准备。sampleRate 与 maxBlock 是宿主声明的值。
    void prepare(double sampleRate, int maxBlock) noexcept;

    // 音频线程：处理一块。bypassed 为真表示宿主旁路了这一端（干声照常逐位直通，回送淡出）。
    // 返回 true 表示这一块被处理了；false 表示没有对端或块长超限，只出干声。
    bool process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport, bool bypassed,
                 const EngineContext& context) noexcept;

    // 消息线程：排空诊断事件。事件码来自环路层与引擎层两段。
    // 队列归引擎所有：音频线程往里推，消息线程经这里取走。
    template<typename Sink>
    int drainEvents(Sink&& sink) noexcept
    {
        return events_.drain(std::forward<Sink>(sink));
    }

    // 消息线程：环路层的状态快照（诊断与界面要的量都在这里）。
    // 后续卡往 LoopStatusFields 里加字段属于后续卡的范围，引擎的代码不用变。
    loop::LoopStatusFields loopStatus() const noexcept { return loop_.statusFields(); }

    // 音频线程：写测量量（回送 DC 均值、贴近满幅时长、限制器增益缩减、安全状态）。
    // 安全链接上之后由安全链填；本引擎的最小实现填自己知道的部分。
    const Meters& meters() const noexcept { return meters_; }

    // 命令序号的基线（诊断与场景用）：引擎当前认为的基线值
    CommandBaseline commandBaseline() const noexcept;

    // 采样率与最大块长（诊断用）
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }
    // 本会话观察到的最大块长（N_obs）。每次 prepare 清零。
    int maxBlockSeen() const noexcept { return maxBlockSeen_; }

    // 干声的增益（线性律：m = mixPercent / 100，g_dry = min(1, 2(1−m))）。
    // 诊断与界面要看「现在的干声增益」时用这个。
    float dryGain() const noexcept { return dryGain_; }
    // 回送的增益（线性律：g_ret = min(1, 2m)）。没有安全链时回送增益恒为 0。
    float returnGain() const noexcept { return returnGain_; }

private:
    void noteBlockLength(int n) noexcept;
    // 块首的命令序号比对：变了就执行一次（不重放旧命令）。
    void checkCommands(const EngineParams& params) noexcept;

    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    int maxBlockSeen_ = 0; // 本会话观察到的最大块长（N_obs）；每次 prepare 清零
    bool prepared_ = false;

    loop::LoopSession loop_;
    diag::EventQueue<256> events_;

    Meters meters_;

    // 拉环用的暂存与混好的回送：预分配在 prepare 里，音频线程上不再分配
    std::vector<float> pullLeft_;
    std::vector<float> pullRight_;
    std::vector<float> returnLeft_;
    std::vector<float> returnRight_;
    std::vector<std::uint8_t> returnValid_;

    // 命令序号的基线
    CommandBaseline baseline_;
    CommandBaseline seen_;

    // 安全链的接入口（可空）
    void* safetyChain_ = nullptr;
    FadeInRequest safetyRequest_ = nullptr;

    // 回送增益（最小实现：feedback × g_ret 的 30 ms 线性平滑；安全链接上之后由安全链给）
    float feedbackGain_ = 0.0f;
    float returnGain_ = 0.0f;
    float dryGain_ = 1.0f;
    std::int64_t gainRampSamples_ = 1;
    std::int64_t gainStep_ = 0;
    float gainStart_ = 0.0f;
};

} // namespace rinne::engine
