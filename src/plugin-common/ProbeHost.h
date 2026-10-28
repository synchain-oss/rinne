// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针构建的接线：两个处理器在探针构建里各持有一个 ProbeHost，正式构建里不实例化它。
// 它建在 InstanceHost 之上（后端、会合、引擎、块首租约、tick、手动组、挂起清空、命令序号都在那里，两种构建共用一份），
// 这里只在 InstanceHost 的扩展点里加探针自己的事，并实现面板要的 ProbeEditorHost：
//   - 日志：默认目录（文档目录下的 Rinne Probe Logs），tok4 自动分配；验证宿主下不开；宿主第一次真用这个实例时才开，
//     只构造一下就析构的场合（插件扫描、构建时生成 VST3 模块信息的工具）不留文件；
//   - 每次回调对会合段头的 probe_call_seq 做 fetch_add（布局里已有这一格，正式版恒为 0），每个文件里严格递增；
//     取不到（块首没拿到绑定）就写不出 B 行，记录窗口内按丢弃计（占一个 seq、S 与 Z 的 dropped 加一）；
//   - Ping 只在 Input 端：注入在引擎输出之后、读回取引擎的原始读头（安全链之前），回送的 500 ms 淡入借引擎的
//     「载入预设」命令（presetSeq）触发，不改 core；
//   - 参数（反馈、延迟、上限、环路开关）与组号来自面板与探针 state，不是正式的参数树与 state 胶水；
//   - 旁路块不碰 Ping：宿主旁路 Input 时输出与输入逐位相同（引擎照常拉环、回送 10 ms 淡到 0），Ping 当场结束。
// 线程：同 InstanceHost。音频线程的两个扩展点不分配、不加锁、不做 I/O；其余在非实时线程，用 InstanceHost 的那一把锁。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "InstanceHost.h"
#include "ProbeEditor.h"
#include "ProbeSupport.h"
#include "diag/ProbeLog.h"

namespace rinne::probe
{

// 默认值就是插件里的行为；测试只改注入项。InstanceHostConfig 的几项（role、validationHost、scope、backend、startTimer、
// clock）意思不变，验证宿主下另外不开日志；时钟给了就日志、挂起判定、会合三处都用它
struct ProbeHostConfig : InstanceHostConfig
{
    std::string logDirectory; // 空 = resolveLogDir()
    bool startLogThread = true; // 假 = 日志不起落盘线程，tick 里排空（测试用）
};

class ProbeHost final : public InstanceHost, public ProbeEditorHost
{
public:
    static constexpr float kSilentPeak = 1.0e-4f; // 干声块峰值不超过它算静音（-80 dBFS）
    static constexpr int kScratchFrames = 32768; // Ping 的暂存；更长的块（环路不走的超长块）上 Ping 取消
    static constexpr double kPingMarginSeconds = 0.05;

    ProbeHost(juce::AudioProcessor& processor, ProbeHostConfig config);
    ~ProbeHost() override;

    ProbeHost(const ProbeHost&) = delete;
    ProbeHost& operator=(const ProbeHost&) = delete;

    // ---- 处理器转发（prepare、release、reset、tick 是 InstanceHost 的，探针的部分在扩展点里） ----
    // 音频线程：按面板与探针 state 填好参数，交给 InstanceHost::process。n 为 0 的回调由处理器先挡掉，不进这里
    void process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept;
    void getState(juce::MemoryBlock& dest);
    void setState(const void* data, int size);
    void trackProperties(const juce::AudioProcessor::TrackProperties& properties);
    juce::AudioProcessorEditor* createEditor();

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

    // ---- 读出（诊断与测试；counts、rendezvousState、inputEngine 与后端错误码的转发出口是 InstanceHost 的） ----
    const diag::ProbeLog* log() const noexcept { return log_.get(); }
    void pumpLog(); // 落盘线程没起时由调用方排空
    juce::AudioProcessorValueTreeState& parameters() noexcept { return params_; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    // ---- InstanceHost 的扩展点 ----
    void onPrepare(bool rateChanged) override;
    void onPrepared() override;
    void onReleased() override;
    void onRendezvousTicked() override;
    void onEngineEvent(const diag::Event& event) override;
    void onTicked(std::uint64_t ns) override;
    void beforeEngine(const Block& block) noexcept override;
    void afterEngine(const Block& block) noexcept override;

    void beforeInput(const Block& block) noexcept;
    void afterInput(const Block& block) noexcept;
    void beforeOutput(const Block& block) noexcept;
    void afterOutput(const Block& block) noexcept;
    void applyPing(const engine::AudioView& view, int frames) noexcept;
    void audioEvent(diag::ProbeEventCode code, std::uint64_t a, std::uint64_t startNs) noexcept;

    // 以下在锁内调用
    void noteRendezvous();
    void notePeer();
    void tickPing();
    void collectPing();
    std::size_t pingSettleSamples() const;
    void tickAutoRecording(std::uint64_t ns);
    bool beginRecording(const diag::ProbeCase& c, std::uint32_t seconds, std::uint64_t ns);
    void applyGroup(int group, ipc::ClaimIntent intent, std::uint64_t ms);
    ProbeStatus statusLocked() const;
    void logEvent(diag::ProbeEventCode code, const diag::ProbeEventArgs& args = {});
    void openLogLocked();

    ProbeHostConfig config_;
    juce::AudioProcessorValueTreeState params_;
    std::atomic<float>* autoCase_ = nullptr;
    std::atomic<float>* autoVariant_ = nullptr;
    std::atomic<float>* autoRecord_ = nullptr;

    std::unique_ptr<diag::ProbeLog> log_; // 第一次 prepare / 设状态 / 送轨道信息时才建（见 openLogLocked）
    std::unique_ptr<ProbePing> ping_; // Input 端，prepare 时按采样率建
    ProbeStateKeeper keeper_;
    ProbeStateData stateData_;
    ProbeLogDir logDir_;

    // ---- 非实时线程的账（锁内） ----
    diag::DiagOrigin origin_ = diag::DiagOrigin::kNone;
    bool backendLogged_ = false;
    bool peerActive_ = false;
    bool wasPlaying_ = false;
    std::uint64_t recordDeadlineNs_ = 0;
    std::string autoKey_; // 上一次自动开窗的 (case, variant, 秒数)，同一组值不再开第二次
    std::uint32_t resetsLogged_ = 0;
    std::uint32_t lastPingSamples_ = 0;
    bool havePing_ = false;

    // ---- 跨线程的原子量 ----
    std::atomic<float> feedback_{0.0f};
    std::atomic<float> delayMs_{engine::kDefaultDelayMs};
    std::atomic<float> ceilingDb_{kCeilingDefaultDb};
    std::atomic<bool> loopOn_{true};
    std::atomic<std::uint32_t> stateSeq_{0};
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
    diag::BlockFields fields_; // 这一块的 B 行：beforeEngine 起头，afterEngine 写完交给日志
    bool pingBlock_ = false; // 这一块跑不跑 Ping（块首定一次）
    int pingFrames_ = 0; // 这一块暂存了干声的样本数
    std::uint64_t outputW_ = 0; // Output：块首的环写头（没有环时按上一块推算）
    std::uint64_t outputExpected_ = 0; // Output：上一块的 w + n
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
