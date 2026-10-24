// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针构建的接线：两个处理器在探针构建里各持有一个 ProbeHost，正式构建里不实例化它（正式接线是另一回事）。
// 它把引擎、会合、探针日志、Ping、隐藏参数与探针 state 接到一起，并实现面板要的 ProbeEditorHost：
//   - 引擎：Input 端一个 InputEngine，Output 端一个 OutputEngine；块首经绑定快照租一块（BoundBlock），
//     只有租约成立且槽仍是自己的才把会合段交给引擎；
//   - 会合：手动组（面板或探针 state 给），25 Hz 的 tick 驱动心跳、认领、重试；验证宿主下 IPC 整体关闭；
//   - 日志：默认目录（文档目录下的 Rinne Probe Logs），tok4 自动分配；验证宿主下不开；宿主第一次真用这个实例时才开，
//     只构造一下就析构的场合（插件扫描、构建时生成 VST3 模块信息的工具）不留文件；
//   - 每次回调对会合段头的 probe_call_seq 做 fetch_add（布局里已有这一格，正式版恒为 0），取不到就不写 B 行；
//   - Ping 只在 Input 端：注入在引擎输出之后、读回取引擎的原始读头（安全链之前），回送的 500 ms 淡入借引擎的
//     「载入预设」命令（presetSeq）触发，不改 core；
//   - 两端一起被挂起超过 250 ms（Input 两块之间隔了这么久、环的写头也没走）→ 请求清空本地延迟线；
//   - 旁路块不碰 Ping：宿主旁路 Input 时输出与输入逐位相同（引擎照常拉环、回送 10 ms 淡到 0），Ping 当场结束。
// 线程：process 在音频线程，不分配、不加锁、不做 I/O（时钟只读单调时钟）；reset 可能在任意线程，只记数；
// 其余（prepare、release、state、轨道信息、tick、面板）在非实时线程，共用一把锁。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ProbeEditor.h"
#include "ProbeSupport.h"
#include "diag/ProbeLog.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/IpcDiag.h"
#include "ipc/Naming.h"
#include "ipc/Rendezvous.h"

namespace rinne::probe
{

// 默认值就是插件里的行为；测试只改注入项
struct ProbeHostConfig
{
    ipc::Role role = ipc::Role::kInput;
    bool validationHost = false; // 验证宿主：不开共享内存段，也不开日志
    // ---- 测试注入 ----
    ipc::NameScope scope = ipc::NameScope::formal();
    ipc::ISegmentBackend* backend = nullptr; // 空 = 自带的平台后端；不拥有
    std::string logDirectory; // 空 = resolveLogDir()
    bool startLogThread = true; // 假 = 日志不起落盘线程，tick 里排空（测试用）
    bool startTimer = true; // 假 = 不起 25 Hz 定时器，由调用方调 tick
    // 单调纳秒时钟。空 = 日志用 probeMonoNs、会合用 MonoClock（跨进程比心跳只能用同一个时钟）；
    // 给了就两处都用它（会合取它除以 10^6 的毫秒）
    diag::ProbeClock clock = nullptr;
    void* clockContext = nullptr;
};

class ProbeHost final : public ProbeEditorHost, private juce::Timer
{
public:
    static constexpr int kTickHz = 25;
    static constexpr std::uint64_t kSuspendFlushNs = 250'000'000; // 两端一起挂起超过它就清空本地延迟线
    static constexpr float kSilentPeak = 1.0e-4f; // 干声块峰值不超过它算静音（-80 dBFS）
    static constexpr int kScratchFrames = 32768; // Ping 的暂存；更长的块（环路不走的超长块）上 Ping 取消
    static constexpr double kPingMarginSeconds = 0.05;

    ProbeHost(juce::AudioProcessor& processor, ProbeHostConfig config);
    ~ProbeHost() override;

    ProbeHost(const ProbeHost&) = delete;
    ProbeHost& operator=(const ProbeHost&) = delete;

    // ---- 处理器转发 ----
    void prepare(double sampleRate, int maxBlock);
    void release();
    void reset() noexcept;
    // 音频线程。n 为 0 的回调由处理器先挡掉，不进这里
    void process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept;
    void getState(juce::MemoryBlock& dest);
    void setState(const void* data, int size);
    void trackProperties(const juce::AudioProcessor::TrackProperties& properties);
    juce::AudioProcessorEditor* createEditor();

    // 25 Hz 的非实时工作：会合、引擎事件、Ping 收尾与自动 Ping、隐藏参数、记录窗口到时。定时器调它，测试直接调
    void tick();

    // ---- ProbeEditorHost ----
    ProbeStatus probeStatus() const override;
    bool probeHasReturn() const override { return isInput(); }
    void probePanic() override;
    void probeRecover() override;
    void probePing() override;
    bool probeStartRecording(const ProbeSelection& selection, std::uint32_t seconds) override;
    void probeSetGroup(int group) override;
    void probeSetFeedbackPercent(float percent) override;
    void probeSetDelayMs(float ms) override;
    void probeSetLoopOn(bool on) override { loopOn_.store(on, std::memory_order_relaxed); }
    std::string probeDiagnosticsText(const ProbeSelection& selection) override;

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
    const diag::ProbeLog* log() const noexcept { return log_.get(); }
    void pumpLog(); // 落盘线程没起时由调用方排空
    juce::AudioProcessorValueTreeState& parameters() noexcept { return params_; }
    const engine::InputEngine& inputEngine() const noexcept { return input_; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ---- 后端错误码的转发出口（全进程一份，见 ProbeHost.cpp）----
    // 构造时调用：把转发出口装进 IpcDiag 唯一的槽，换下来的出口记为转发目标（换下来的是自己就不记）
    static void installIpcDiagSink() noexcept;
    static ipc::IpcDiagSink ipcDiagRecorder() noexcept; // 转发出口本身
    static ipc::IpcDiagSink ipcDiagForwardTarget() noexcept; // 它转发给谁
    using InstallHook = void (*)() noexcept;
    // 测试用：下一次 installIpcDiagSink 在换槽之前调它一次（模拟别的线程恰在这时也在装），调过就清掉
    static void setIpcDiagInstallHook(InstallHook hook) noexcept;

private:
    void timerCallback() override { tick(); }

    std::uint64_t nowNs() const noexcept;
    std::uint64_t nowMs() const noexcept;
    bool isInput() const noexcept { return config_.role == ipc::Role::kInput; }

    void processInput(const engine::AudioView& view, const engine::EngineParams& params,
                      const engine::TransportInfo& transport, bool bypassed, const engine::EngineContext& context,
                      const ipc::SegmentHandle* ring, std::uint64_t startNs, diag::BlockFields& fields) noexcept;
    void processOutput(const engine::AudioView& view, const engine::EngineParams& params,
                       const engine::TransportInfo& transport, bool bypassed, const engine::EngineContext& context,
                       const ipc::BoundBlock& block, bool haveTimeline, std::int64_t timeline,
                       diag::BlockFields& fields) noexcept;
    void applyPing(const engine::AudioView& view, int frames) noexcept;
    void audioEvent(diag::ProbeEventCode code, std::uint64_t a, std::uint64_t startNs) noexcept;
    static void onFade(void* context, engine::FadeReason reason) noexcept;

    // 以下在锁内调用
    void tickLocked(std::uint64_t ns, std::uint64_t ms);
    void noteRendezvous();
    void notePeer();
    void drainEngine();
    void tickPing();
    void collectPing();
    std::size_t pingSettleSamples() const;
    void tickAutoRecording(std::uint64_t ns);
    bool beginRecording(const diag::ProbeCase& c, std::uint32_t seconds, std::uint64_t ns);
    void applyGroup(int group, ipc::ClaimIntent intent, std::uint64_t ms);
    ProbeStatus statusLocked() const;
    void logEvent(diag::ProbeEventCode code, const diag::ProbeEventArgs& args = {});
    void openLogLocked();

    juce::AudioProcessor& processor_;
    ProbeHostConfig config_;
    juce::AudioProcessorValueTreeState params_;
    std::atomic<float>* autoCase_ = nullptr;
    std::atomic<float>* autoVariant_ = nullptr;
    std::atomic<float>* autoRecord_ = nullptr;

    mutable std::mutex mutex_;
    std::unique_ptr<ipc::ISegmentBackend> ownBackend_;
    std::unique_ptr<ipc::Rendezvous> rendezvous_;
    std::unique_ptr<diag::ProbeLog> log_; // 第一次 prepare / 设状态 / 送轨道信息时才建（见 openLogLocked）
    engine::InputEngine input_;
    engine::OutputEngine output_;
    std::unique_ptr<ProbePing> ping_; // Input 端，prepare 时按采样率建
    ProbeStateKeeper keeper_;
    ProbeStateData stateData_;
    ProbeLogDir logDir_;

    // ---- 非实时线程的账（锁内）；采样率与最大块长在 ready_ 发布之前写好，音频线程也读 ----
    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    diag::DiagOrigin origin_ = diag::DiagOrigin::kNone;
    std::uint32_t prepareCount_ = 0;
    bool nonrealtimeSeen_ = false;
    bool lastNonrealtime_ = false;
    bool backendLogged_ = false;
    bool peerActive_ = false;
    bool wasPlaying_ = false;
    std::uint64_t recordDeadlineNs_ = 0;
    std::string autoKey_; // 上一次自动开窗的 (case, variant, 秒数)，同一组值不再开第二次
    std::uint32_t resetsLogged_ = 0;
    std::uint32_t lastPingSamples_ = 0;
    bool havePing_ = false;
    int ipcError_ = 0; // 本实例会合 tick 期间后端报出的最后一个非零错误码（backend 事件的 b）
    Counts counts_;

    // ---- 跨线程的原子量 ----
    std::atomic<bool> ready_{false}; // prepare 与 release 之间为真：音频线程只在这期间碰引擎
    std::atomic<float> feedback_{0.0f};
    std::atomic<float> delayMs_{engine::kDefaultDelayMs};
    std::atomic<float> ceilingDb_{kCeilingDefaultDb};
    std::atomic<bool> loopOn_{true};
    std::atomic<std::uint32_t> stateSeq_{0};
    std::atomic<std::uint32_t> panicSeq_{0};
    std::atomic<std::uint32_t> recoverSeq_{0};
    std::atomic<std::uint32_t> resets_{0};
    std::atomic<std::uint32_t> suspendFlushes_{0};
    std::atomic<std::uint32_t> fadeIns_[6]{};
    std::atomic<bool> playing_{false};
    std::atomic<bool> bypassed_{false};
    std::atomic<bool> drySilent_{true};
    std::atomic<bool> outputPeerActive_{false};
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    // ---- 只在音频线程动 ----
    std::uint32_t presetSeq_ = 0;
    std::uint64_t lastCallSeq_ = 0;
    bool firstBlockSeen_ = false;
    bool pairedSeen_ = false;
    bool modeSeen_ = false;
    bool lastModeNonrt_ = false;
    std::uint32_t lastResyncCount_ = 0;
    bool haveLastBlock_ = false;
    std::uint64_t lastStartNs_ = 0;
    std::uint64_t lastBlockNs_ = 0;
    std::uint64_t lastHead_ = 0;
    const ipc::SegmentHandle* lastRing_ = nullptr; // 上一块用的环（只比地址）；空 = 没拿到环，lastHead_ 无效
    bool haveOutputW_ = false;
    std::uint64_t lastOutputW_ = 0;
    std::uint32_t lastOutputN_ = 0;
    std::uint64_t localSamples_ = 0; // Output：宿主没给时间线时的替身
    std::vector<float> dry_[2];
    std::vector<float> inject_;
    std::vector<float> dryScale_;
    std::vector<float> returnScale_;
};

} // namespace rinne::probe
