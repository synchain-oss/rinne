// SPDX-License-Identifier: GPL-3.0-or-later
//
// 一个插件实例的接线：两种构建共用。正式构建里处理器直接持有一个 InstanceHost；探针构建里持有的是 probe::ProbeHost，
// 它建在本类之上（继承），只在下面的扩展点里加探针自己的事（日志、Ping、记录窗口），接线本身只有这一份。
//   - 后端与会合：自带平台后端（测试可注入）；25 Hz 的 tick 驱动会合（心跳、认领、重试、映射回收与旧句柄的延后释放都在
//     会合的 tick 里），组号由调用方给（工程 state 或手动选组），验证宿主下 IPC 整体关闭；
//   - 引擎：Input 端一个 InputEngine，Output 端一个 OutputEngine；块首经绑定快照租一块（BoundBlock），只有租约成立且
//     槽仍是自己的才把段交给引擎；
//   - 两端一起被挂起超过 250 ms（Input 两块之间隔了这么久、环的写头也没走）→ 请求清空本地延迟线；
//   - 命令序号：Panic 与恢复各一个计数，块首填进引擎参数；基线在构造后、第一次 prepare 前由调用方给一次；
//   - 后端失败的错误码经全进程唯一的 IpcDiag 槽转发：装一个转发出口，把错误码记给正在调会合的那个实例。
//   - 配对层（只在 config.pairing 为真时，即正式构建）：智能默认与 Output 等待中的重评、认领转正、轨道标签、每拍折出的
//     配对状态、诊断文本的各项。智能默认要在组号 0 上做，会合在组号 0 时不开段，所以本类另持一份会合段的只读快照
//     （core 的 pairing::PairingSnapshot：只开已存在的段、只读、自带观察者与标签读取），本类只持有它、每拍调它。
//     规则以 ipc/PairingPolicy.h 为准。
// 线程：process 在音频线程，不分配、不加锁、不做 I/O，只读单调时钟（挂起判定要两块之间的间隔；不是墙钟）；reset 可能在
// 任意线程，只记数；panic、recover 只加原子计数；其余（prepare、release、tick、改组、转正、标签、读出）在非实时线程，
// 共用一把锁。走带只在音频线程读（宿主只许在处理回调里读播放头）。
// 锁序：胶水的 setState 锁 → 本类的锁 → 胶水的设置锁，反过来的顺序不出现（胶水在自己的锁外调转正回调，本类在锁内调
// 写设置的回调，那个回调只取胶水的设置锁）。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "diag/DiagCounters.h"
#include "diag/DiagReport.h"
#include "diag/EventQueue.h"
#include "engine/AudioView.h"
#include "engine/EngineContext.h"
#include "engine/EngineParams.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "engine/SafetyEvents.h"
#include "engine/TransportInfo.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/IpcDiag.h"
#include "ipc/LoopBinding.h"
#include "ipc/Naming.h"
#include "ipc/PairingPolicy.h"
#include "ipc/Rendezvous.h"
#include "pairing/PairStateModel.h"
#include "pairing/PairingSnapshot.h"

namespace rinne
{

// 单调纳秒时钟（与探针日志的时钟同一种签名）
using MonoNsClock = std::uint64_t (*)(void* context) noexcept;

// 默认值就是插件里的行为；测试只改注入项。role、validationHost 与 pairing 由处理器基类填
struct InstanceHostConfig
{
    ipc::Role role = ipc::Role::kInput;
    bool validationHost = false; // 验证宿主：不开共享内存段
    bool pairing = false; // 配对层（见文件头）：正式构建为真，探针构建为假（探针有自己的选组与轨道信息）
    // ---- 测试注入 ----
    ipc::NameScope scope = ipc::NameScope::formal();
    ipc::ISegmentBackend* backend = nullptr; // 空 = 自带的平台后端；不拥有
    bool startTimer = true; // 假 = 不起 25 Hz 定时器，由调用方调 tick
    // 单调纳秒时钟。空 = 挂起判定用本类的 monoNs（转调 diag::probeMonoNs，正式构建也用）、会合用 util::monoNowMs
    // （跨进程比心跳只能用同一个时钟）；给了就两处都用它（会合取它除以 10^6 的毫秒）
    MonoNsClock clock = nullptr;
    void* clockContext = nullptr;
    // 诊断与日志的出口，空 = 插件里的行为：
    //   hostDescription  诊断里的宿主名；空 = juce::PluginHostType 的固定名字表（不读可执行文件的路径）
    //   logLine          写一行诊断日志；空 = platformlog::write（没装日志时什么都不做）
    //   installLog       装诊断日志（处理器基类用）；空 = platformlog::install
    //   copyToClipboard  复制诊断（处理器基类用）；空 = juce::SystemClipboard
    const char* hostDescription = nullptr;
    void (*logLine)(const juce::String& line) = nullptr;
    bool (*installLog)(const juce::String& role) = nullptr;
    void (*copyToClipboard)(const juce::String& text) = nullptr;
};

class InstanceHost : private juce::Timer
{
public:
    static constexpr int kTickHz = 25;
    static constexpr std::uint64_t kSuspendFlushNs = 250'000'000; // 两端一起挂起超过它就清空本地延迟线

    InstanceHost(juce::AudioProcessor& processor, const InstanceHostConfig& config);
    ~InstanceHost() override;

    InstanceHost(const InstanceHost&) = delete;
    InstanceHost& operator=(const InstanceHost&) = delete;

    // ---- 处理器转发 ----
    void prepare(double sampleRate, int maxBlock);
    void release();
    void reset() noexcept;
    // 音频线程。params 里的参数、presetSeq、stateSeq 与三个工程开关由调用方填好，panicSeq 与 recoverSeq 在这里填；
    // 走带在这里读。n 为 0 或还没 prepare 时立即返回，不推进任何计数。整块交给引擎，不在壳里切块（见 .cpp）
    void process(juce::AudioBuffer<float>& buffer, engine::EngineParams params, bool bypassed) noexcept;

    // 25 Hz 的非实时工作：会合、引擎事件，再加派生类的扩展点。定时器调它，测试直接调
    void tick();

    // 组号 0..8（0 = 不在任何组）。手动选组给 kManual，一键修复给 kManual 或 kManualConfirmed：这两种都记为
    // 「本会话手选过」，之后不再自动认领或迁组。工程恢复走 restoreGroup
    void setGroup(std::uint32_t group, ipc::ClaimIntent intent);

    // ---- 配对层（config.pairing 为假时都是空操作，读出是默认值） ----
    // 宿主的 setState 从头到尾包在这一对里（任何非实时线程；处理器基类用 StateLoadScope 保证成对）：期间不做智能默认
    // 与重评，胶水的全量应用与 restoreGroup 之间不会插进一次智能认领去改胶水的设置。begin 另记诊断的「setState 早于
    // 首次 prepare」
    void beginStateLoad();
    void endStateLoad();
    // 工程恢复（全量应用）：组号交给会合（kRestore，抢得动别人的 provisional 认领）。decided = 这份 state 的组号已定
    // （非 0，或来源是用户选的）：定了就不再做智能默认；没定就清掉「已做过智能决定」，条件满足时重新决定
    void restoreGroup(std::uint32_t group, bool decided);
    // 智能认领（含重评迁组）之后在锁内调用：处理器把组号写进胶水的设置（来源 Smart，不定型、不标脏）。传空就是不接；
    // 处理器析构时先撤下
    void setSmartGroupSink(std::function<void(std::uint32_t group)> sink);
    // 认领转正：胶水的转正回调转到这里（任何非实时线程，同步做完，见 .cpp）。没有 provisional 认领时是空操作
    void confirmClaim();
    // 宿主的轨道信息（JUCE 规定在消息线程）：名字与颜色写进本端的标签。名字没给就是未知，不写空串冒充
    void setTrackProperties(const juce::AudioProcessor::TrackProperties& properties);
    // 一键修复要去的组：只在会合处于冲突时有，否则 group 为 0。消息线程
    ipc::RepairPlan planRepair() const;
    // 上一拍折出的配对状态（一份拷贝，会分配；消息线程）
    pairing::PairModel pairState() const;
    // 诊断文本的各项（state 一段除外，由处理器从胶水填）。消息线程
    void fillDiagnostics(diag::DiagInputs& in) const;
    // 本端轨道名的散列（0 = 未知）。测试与诊断用
    std::uint32_t trackNameHash() const;

    // 构造后、第一次 prepare 前调一次：调用方当时的 presetSeq 与 stateSeq 加上本实例当时的 Panic / 恢复计数，作为引擎的
    // 命令基线。之后的变化（包括第一块之前的）才执行，旧命令不重放
    void setCommandBaseline(std::uint32_t presetSeq, std::uint32_t stateSeq) noexcept;

    // 界面动作（任意非实时线程）。只在 Input 端有意义（回送与安全链都在 Input）；Output 端什么都不做。
    // 恢复的 1.5 s 冷却由安全链判：冷却里的点击被安全链丢掉，这里不排队
    void panic() noexcept;
    void recover() noexcept;
    // 现在点恢复有没有用：跳闸了且不在冷却里。Output 端恒为假
    bool canRecover() const noexcept;

    // ---- 读出（诊断与测试） ----
    struct Counts
    {
        std::uint32_t panicsExecuted = 0; // 引擎报的 Panic 跳闸
        std::uint32_t recoversExecuted = 0; // 引擎报的恢复
        std::uint32_t suspendFlushes = 0; // 判定两端一起挂起、请求清空的次数
        std::uint32_t suspendClears = 0; // 引擎回报「因挂起清空」的次数
        std::array<std::uint32_t, 6> fadeIns{}; // 按 FadeReason 计的淡入请求
    };
    Counts counts() const;
    ipc::RendezvousState rendezvousState() const;
    std::uint32_t group() const;
    bool isInput() const noexcept { return config_.role == ipc::Role::kInput; }
    // 引擎的读出（原子快照）在任何线程读；lastBlockResult 一类块间状态只许音频线程（测试里就是调 process 的线程）读
    const engine::InputEngine& inputEngine() const noexcept { return input_; }
    const engine::OutputEngine& outputEngine() const noexcept { return output_; }
    // 会合本身（测试读绑定快照与槽观察者）；非实时线程，不与 tick 并发
    const ipc::Rendezvous& rendezvous() const noexcept { return *rendezvous_; }

    // ---- 后端错误码的转发出口（全进程一份，见 InstanceHost.cpp）----
    // 构造时调用：把转发出口装进 IpcDiag 唯一的槽，换下来的出口记为转发目标（换下来的是自己就不记）
    static void installIpcDiagSink() noexcept;
    static ipc::IpcDiagSink ipcDiagRecorder() noexcept; // 转发出口本身
    static ipc::IpcDiagSink ipcDiagForwardTarget() noexcept; // 它转发给谁
    using InstallHook = void (*)() noexcept;
    // 测试用：下一次 installIpcDiagSink 在换槽之前调它一次（模拟别的线程恰在这时也在装），调过就清掉
    static void setIpcDiagInstallHook(InstallHook hook) noexcept;

protected:
    // 一块里交给扩展点的东西，只在这一块之内有效
    struct Block
    {
        const engine::AudioView& view; // 宿主缓冲（Input 端就地改写）
        const engine::TransportInfo& transport;
        const engine::EngineContext& context; // 租约成立且槽仍是自己的才有段
        const ipc::BoundBlock& lease;
        bool bypassed;
        bool haveTimeline; // 宿主给了时间线（样本）
        std::int64_t timeline;
        std::uint64_t startNs; // 块首读的单调纳秒
    };

    // ---- 扩展点：默认什么都不做，正式构建不覆盖。非实时的几个在锁内调用，音频线程的两个不分配、不加锁 ----
    // prepare：ready 已关，采样率与块长还是上一次的值；rateChanged = 这次换了采样率
    virtual void onPrepare(bool rateChanged) { juce::ignoreUnused(rateChanged); }
    // prepare：引擎与会合都已 prepare，ready 还没开
    virtual void onPrepared() {}
    virtual void onReleased() {}
    // tick：会合这一拍走完之后、排空引擎事件之前
    virtual void onRendezvousTicked() {}
    // tick：排空引擎事件时每条一次（本类的计数已经记过）
    virtual void onEngineEvent(const diag::Event& event) { juce::ignoreUnused(event); }
    // tick 的最后；ns 是这一拍开头读的单调纳秒
    virtual void onTicked(std::uint64_t ns) { juce::ignoreUnused(ns); }
    // 音频线程：租约之后、引擎之前（缓冲里还是干声）
    virtual void beforeEngine(const Block& block) noexcept { juce::ignoreUnused(block); }
    // 音频线程：引擎之后（Input 端缓冲里已是引擎的输出）
    virtual void afterEngine(const Block& block) noexcept { juce::ignoreUnused(block); }

    // 派生类先停定时器再拆自己的成员：定时器的回调会走到扩展点
    void stopTicking() { stopTimer(); }
    void startTicking() { startTimerHz(kTickHz); }

    std::uint64_t nowNs() const noexcept;
    std::uint64_t nowMs() const noexcept;
    bool isAuWrapper() const noexcept;

    // 以下在持 mutex_ 时调用
    void setGroupLocked(std::uint32_t group, ipc::ClaimIntent intent, std::uint64_t ms);
    ipc::Rendezvous& rendezvousLocked() noexcept { return *rendezvous_; }
    double sampleRate() const noexcept { return sampleRate_; } // 音频线程在 ready 之后也可以读
    int maxBlock() const noexcept { return maxBlock_; } // 同上
    std::uint32_t prepareCount() const noexcept { return prepareCount_; }
    bool nonrealtimeSeen() const noexcept { return nonrealtimeSeen_; }
    int lastIpcError() const noexcept { return ipcError_; }
    std::uint32_t resetCount() const noexcept { return resets_.load(std::memory_order_relaxed); }

    juce::AudioProcessor& processor_;
    mutable std::mutex mutex_;

private:
    void timerCallback() override { tick(); }
    void tickLocked(std::uint64_t ns, std::uint64_t ms);
    void drainEngine();
    void detectSuspend(const engine::EngineContext& context, const ipc::SegmentHandle* ring,
                       const engine::TransportInfo& transport, std::uint64_t startNs) noexcept;
    void noteBlockForSuspend(const engine::EngineContext& context, const ipc::SegmentHandle* ring, int n,
                             std::uint64_t startNs) noexcept;
    static void onFade(void* context, engine::FadeReason reason) noexcept;

    // ---- 配对层（锁内） ----
    bool pairingOn() const noexcept { return config_.pairing && !config_.validationHost; }
    void noteGroupIntent(ipc::ClaimIntent intent) noexcept;
    bool smartDue(std::uint64_t ms) const noexcept;
    void smartTick(std::uint64_t ms);
    void claimSmart(std::uint32_t group, std::uint64_t ms);
    void foldPairState(std::uint64_t ns, std::uint64_t ms);
    bool loopSilent(std::uint64_t ns, const pairing::EndFacts& peer) const noexcept;
    void queueLog(juce::String line);
    void flushLog();

    InstanceHostConfig config_;
    std::unique_ptr<ipc::ISegmentBackend> ownBackend_;
    std::unique_ptr<ipc::Rendezvous> rendezvous_;
    engine::InputEngine input_;
    engine::OutputEngine output_;

    // ---- 非实时线程的账（锁内）；采样率与最大块长在 ready_ 发布之前写好，音频线程也读 ----
    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    std::uint32_t prepareCount_ = 0;
    bool nonrealtimeSeen_ = false;
    bool lastNonrealtime_ = false;
    int ipcError_ = 0; // 本实例会合 tick 期间后端报出的最后一个非零错误码
    Counts counts_;

    // ---- 跨线程的原子量 ----
    std::atomic<bool> ready_{false}; // prepare 与 release 之间为真：音频线程只在这期间碰引擎
    std::atomic<std::uint32_t> panicSeq_{0};
    std::atomic<std::uint32_t> recoverSeq_{0};
    std::atomic<std::uint32_t> resets_{0};
    std::atomic<std::uint32_t> suspendFlushes_{0};
    std::atomic<std::uint32_t> fadeIns_[6]{};
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    // ---- 只在音频线程动：挂起判定 ----
    bool haveLastBlock_ = false;
    std::uint64_t lastStartNs_ = 0;
    std::uint64_t lastBlockNs_ = 0;
    std::uint64_t lastHead_ = 0;
    const ipc::SegmentHandle* lastRing_ = nullptr; // 上一块用的环（只比地址）；空 = 没拿到环，lastHead_ 无效

    // ---- 配对层：对端视图（锁内）。会合段的只读快照，用的是本类的后端（自带的或注入的）；析构时先于后端放下 ----
    std::unique_ptr<pairing::PairingSnapshot> snapshot_;

    // ---- 配对层：智能默认与状态（锁内） ----
    std::uint32_t version_ = 0; // 本构建的 packVersion
    std::uint32_t channels_ = 0; // prepare 时发布的声道数
    bool firstPrepared_ = false;
    std::uint64_t firstPrepareMs_ = 0;
    bool savedGroup_ = false; // 最近一次全量应用的 state 里组号已定
    bool userPicked_ = false; // 本会话手选过组（或一键修复过）
    bool smartDecided_ = false; // 已做过一次智能决定
    bool smartWaiting_ = false; // Output 进了空组在等对端：每 kPairingReevalMs 重评
    std::uint64_t nextReevalMs_ = 0;
    bool allGroupsFull_ = false; // 智能默认找不到组；之后任何改组都清掉
    diag::DiagOrigin groupOrigin_ = diag::DiagOrigin::kNone;
    std::uint32_t nameHash_ = 0; // 本端轨道名的 FNV-1a；0 = 未知
    bool setStateBeforePrepare_ = false;
    std::uint32_t stateLoads_ = 0; // 正在进行的宿主 setState（beginStateLoad 与 endStateLoad 之间）
    pairing::PairModel pairModel_;
    std::function<void(std::uint32_t)> smartSink_;
    std::vector<juce::String> pendingLog_; // 锁内攒下、锁外写的日志行

    // ---- 配对层：音频线程写（relaxed），tick 与诊断读 ----
    diag::DiagCounters blockCounters_;
    std::atomic<std::uint32_t> realtimeBlocks_{0}; // 非离线的块数（触发条件）
    std::atomic<bool> lastBypassed_{false};
    std::atomic<bool> lastLoopOn_{true};
    std::atomic<std::uint64_t> lastAudioNs_{0}; // 上一块的块首单调纳秒；0 = 还没有过
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace rinne
