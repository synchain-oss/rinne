// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProbeHost.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

#include "diag/DiagReport.h"
#include "ipc/IpcDiag.h"
#include "ipc/LoopRing.h"
#include "loop/LoopEvents.h"
#include "loop/LoopStatus.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/ProcessInfo.h"
#include "ipc/SegmentLayout.h"
#include "util/MonoClock.h"
#include "util/RinneVersion.h"

namespace rinne::probe
{
namespace
{

constexpr const char* kAutoCaseId = "auto_case";
constexpr const char* kAutoVariantId = "auto_variant";
constexpr const char* kAutoRecordId = "auto_record_s";

#if defined(_WIN32)
constexpr std::uint64_t kBackendKind = 0; // backend 事件的 a：win32
#else
constexpr std::uint64_t kBackendKind = 1; // posix
#endif

constexpr std::uint64_t kGroupUnsetArg = 0xFF; // kGroup 参数：0..7 以外写 -

// 后端失败的错误码只经全局的诊断出口给出：装一个转发出口，记下最后一个非零的错误码，再交给原来的出口
std::atomic<int> gLastIpcError{0};
std::atomic<ipc::IpcDiagSink> gPreviousSink{nullptr};

void recordIpcDiag(const ipc::IpcDiagEvent& event) noexcept
{
    if (event.error != 0)
        gLastIpcError.store(event.error, std::memory_order_relaxed);
    if (const ipc::IpcDiagSink previous = gPreviousSink.load(std::memory_order_acquire))
        previous(event);
}

void installIpcDiagSink() noexcept
{
    if (ipc::ipcDiagSink() != &recordIpcDiag)
        gPreviousSink.store(ipc::setIpcDiagSink(&recordIpcDiag), std::memory_order_release);
}

bool isAu(const juce::AudioProcessor& processor) noexcept
{
    return processor.wrapperType == juce::AudioProcessor::wrapperType_AudioUnit ||
           processor.wrapperType == juce::AudioProcessor::wrapperType_AudioUnitv3;
}

diag::ProbeHost hostKind()
{
    const juce::PluginHostType host;
    if (host.isCubase())
        return diag::ProbeHost::kCubase;
    // JUCE 不认 LUNA，按宿主可执行文件的名字判
    if (juce::File(juce::PluginHostType::getHostPath()).getFileNameWithoutExtension().containsIgnoreCase("luna"))
        return diag::ProbeHost::kLuna;
    return diag::ProbeHost::kOther;
}

// H 行的 build 要提交散列的前 8 位：取构建标签最后一段（+ - . 之后）开头的 8 位小写十六进制，取不到给空串
// （日志写成 00000000）
std::string buildHash()
{
    const std::string label = build::buildLabel();
    const std::size_t cut = label.find_last_of("+-.");
    const std::string tail = cut == std::string::npos ? label : label.substr(cut + 1);
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    if (tail.size() < 8 || !std::all_of(tail.begin(), tail.begin() + 8, hex))
        return {};
    return tail.substr(0, 8);
}

std::uint64_t groupArg(int group) noexcept
{
    return group >= 0 && group < static_cast<int>(ipc::kGroupCount) ? static_cast<std::uint64_t>(group)
                                                                    : kGroupUnsetArg;
}

const char* rendezvousName(ipc::RendezvousState state) noexcept
{
    switch (state)
    {
    case ipc::RendezvousState::kValidator:
        return "validator";
    case ipc::RendezvousState::kUnassigned:
        return "unassigned";
    case ipc::RendezvousState::kWaitingPrepare:
        return "waiting-prepare";
    case ipc::RendezvousState::kOpening:
        return "opening";
    case ipc::RendezvousState::kIpcUnavailable:
        return "ipc-unavailable";
    case ipc::RendezvousState::kAbiMismatch:
        return "abi-mismatch";
    case ipc::RendezvousState::kConflict:
        return "conflict";
    case ipc::RendezvousState::kActive:
        return "active";
    case ipc::RendezvousState::kDormant:
        return "dormant";
    }
    return "unknown";
}

const char* safetyName(std::uint8_t state) noexcept
{
    static constexpr const char* kNames[] = {"UNPAIRED", "FADE_IN", "RUN", "STALLED", "GUARD", "TRIPPED"};
    return state < std::size(kNames) ? kNames[state] : "UNKNOWN";
}

std::uint32_t toU32(std::int64_t v) noexcept
{
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, 0, 0xFFFFFFFF));
}

std::uint32_t toU32(std::uint64_t v) noexcept
{
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, 0xFFFFFFFFu));
}

} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout ProbeHost::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{kAutoCaseId, 1}, kAutoCaseId, 0,
                                                         static_cast<int>(caseCount()), 0));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{kAutoVariantId, 1}, kAutoVariantId, 0,
                                                         static_cast<int>(variantCount()), 0));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{kAutoRecordId, 1}, kAutoRecordId, 0,
                                                         static_cast<int>(kAutoRecordMaxSeconds), 0));
    return layout;
}

ProbeHost::ProbeHost(juce::AudioProcessor& processor, ProbeHostConfig config)
    : processor_(processor), config_(std::move(config)),
      params_(processor, nullptr, "RinneProbe", createParameterLayout()),
      keeper_(isInput() ? state::Role::Input : state::Role::Output)
{
    autoCase_ = params_.getRawParameterValue(kAutoCaseId);
    autoVariant_ = params_.getRawParameterValue(kAutoVariantId);
    autoRecord_ = params_.getRawParameterValue(kAutoRecordId);
    input_.setFadeObserver(this, &ProbeHost::onFade);

    ipc::ISegmentBackend* backend = config_.backend;
    if (backend == nullptr)
    {
        ownBackend_ = std::make_unique<ipc::PlatformSegmentBackend>();
        backend = ownBackend_.get();
    }
    installIpcDiagSink();
    ipc::RendezvousConfig rc;
    rc.role = config_.role;
    rc.backend = backend;
    rc.scope = config_.scope;
    rc.ipcEnabled = !config_.validationHost;
    rc.version = ipc::packVersion(static_cast<std::uint32_t>(build::kVersionMajor),
                                  static_cast<std::uint32_t>(build::kVersionMinor),
                                  static_cast<std::uint32_t>(build::kVersionPatch));
    rc.format = isAu(processor_) ? ipc::kFormatAu : ipc::kFormatVst3;
    rc.hostPpid = ipc::parentProcessId();
    rc.seed = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^ diag::probeMonoNs();
    rendezvous_ = std::make_unique<ipc::Rendezvous>(rc);

    if (!config_.validationHost)
        logDir_ = config_.logDirectory.empty() ? resolveLogDir() : ProbeLogDir{config_.logDirectory};
    if (config_.startTimer)
        startTimerHz(kTickHz);
}

ProbeHost::~ProbeHost()
{
    stopTimer();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (log_ != nullptr)
    {
        log_->logEvent(diag::ProbeEventCode::kDestruct);
        log_->close();
    }
}

std::uint64_t ProbeHost::nowNs() const noexcept
{
    return config_.clock != nullptr ? config_.clock(config_.clockContext) : diag::probeMonoNs();
}

std::uint64_t ProbeHost::nowMs() const noexcept
{
    return config_.clock != nullptr ? config_.clock(config_.clockContext) / 1'000'000u : util::monoNowMs();
}

void ProbeHost::logEvent(diag::ProbeEventCode code, const diag::ProbeEventArgs& args)
{
    if (log_ != nullptr)
        log_->logEvent(code, args);
}

// 日志文件到宿主第一次真用这个实例（prepare、设状态、送轨道信息）才开：插件扫描与构建时生成 VST3 模块信息的工具
// 只构造一下就析构，不该在文档目录里留下只有析构事件的文件。持锁调用；建好之前音频回调还没开始（prepare 在 ready_
// 发布之前调它，另两处只可能在第一次 prepare 之前真的建），音频线程读 log_ 不需要别的同步
void ProbeHost::openLogLocked()
{
    if (log_ != nullptr || logDir_.path.empty()) // 验证宿主下 logDir_ 是空的
        return;
    diag::ProbeLogOptions options;
    options.directory = logDir_.path;
    options.header = diag::makeProbeHeader(isInput() ? diag::ProbeRole::kInput : diag::ProbeRole::kOutput,
                                           isAu(processor_) ? diag::ProbeFormat::kAu : diag::ProbeFormat::kVst3,
                                           hostKind(), buildHash());
    options.startThread = config_.startLogThread;
    options.clock = config_.clock;
    options.clockContext = config_.clockContext;
    log_ = std::make_unique<diag::ProbeLog>(std::move(options));
}

// ---- 处理器转发 ----

void ProbeHost::prepare(double sampleRate, int maxBlock)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_.store(false, std::memory_order_release);
    openLogLocked();
    // 宿主挂起再恢复（release 再 prepare）正是要判的「两端一起停了多久」：同采样率时上一块的账留着
    if (sampleRate != sampleRate_)
    {
        haveLastBlock_ = false;
        haveOutputW_ = false;
    }
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(maxBlock, 1);
    ++prepareCount_;
    if (isInput())
    {
        input_.prepare(sampleRate, maxBlock_);
        // 求得出的延迟上限：D 的上限加余量，再加两块（读回与注入各差至多一块）
        const auto maxDelay = static_cast<std::size_t>(
            std::ceil((engine::kMaxDelayMs / 1000.0 + kPingMarginSeconds) * sampleRate) + 2.0 * maxBlock_);
        ping_ = std::make_unique<ProbePing>(maxDelay, sampleRate);
        const auto frames = static_cast<std::size_t>(kScratchFrames);
        for (auto& channel : dry_)
            channel.assign(frames, 0.0f);
        inject_.assign(frames, 0.0f);
        dryScale_.assign(frames, 1.0f);
        returnScale_.assign(frames, 1.0f);
        resetSettle();
    }
    else
    {
        output_.prepare(sampleRate, maxBlock_);
    }
    const auto channels = static_cast<std::uint32_t>(std::max(processor_.getMainBusNumOutputChannels(), 1));
    rendezvous_->prepare(
        {static_cast<std::uint32_t>(std::llround(sampleRate)), static_cast<std::uint32_t>(maxBlock_), channels},
        nowMs());
    logEvent(diag::ProbeEventCode::kPrepare,
             {static_cast<std::uint64_t>(std::llround(sampleRate)), static_cast<std::uint64_t>(maxBlock_)});
    ready_.store(true, std::memory_order_release);
}

void ProbeHost::release()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_.store(false, std::memory_order_release);
    rendezvous_->releaseResources(nowMs());
    if (ping_ != nullptr)
    {
        ping_->reset();
        resetSettle();
    }
    logEvent(diag::ProbeEventCode::kRelease);
}

void ProbeHost::reset() noexcept
{
    // VST3 的 setProcessing(false) 会走到这里，可能在音频线程：只记数，tick 里补写事件
    resets_.fetch_add(1, std::memory_order_relaxed);
}

void ProbeHost::process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    const int n = buffer.getNumSamples();
    if (n <= 0 || !ready_.load(std::memory_order_acquire))
        return;
    const std::uint64_t startNs = nowNs();

    engine::TransportInfo transport;
    bool haveTimeline = false;
    std::int64_t timeline = 0;
    if (auto* playHead = processor_.getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            transport.isPlaying = position->getIsPlaying();
            if (const auto hostNs = position->getHostTimeNs())
            {
                transport.hostNs = *hostNs;
                transport.hostNsValid = true;
            }
            if (const auto bpm = position->getBpm())
                transport.bpm = static_cast<float>(*bpm);
            if (const auto samples = position->getTimeInSamples())
            {
                haveTimeline = true;
                timeline = *samples;
            }
        }
    }
    transport.nonrealtime = processor_.isNonRealtime();
    playing_.store(transport.isPlaying, std::memory_order_relaxed);

    engine::AudioView view;
    view.channels = buffer.getArrayOfWritePointers();
    view.numChannels = std::min(buffer.getNumChannels(), 2);
    view.length = n;
    float peak = 0.0f;
    for (int c = 0; c < view.numChannels; ++c)
        for (int i = 0; i < n; ++i)
            peak = std::max(peak, std::abs(view.channels[c][i]));
    drySilent_.store(peak <= kSilentPeak, std::memory_order_relaxed);

    engine::EngineParams params;
    params.feedbackPercent = feedback_.load(std::memory_order_relaxed);
    params.loopOn = loopOn_.load(std::memory_order_relaxed);
    params.delayMs = delayMs_.load(std::memory_order_relaxed);
    params.ceilingDb = ceilingDb_.load(std::memory_order_relaxed);
    params.presetSeq = presetSeq_;
    params.stateSeq = stateSeq_.load(std::memory_order_relaxed);
    params.panicSeq = panicSeq_.load(std::memory_order_relaxed);
    params.recoverSeq = recoverSeq_.load(std::memory_order_relaxed);

    // 块首租约：会合段在就取 call_seq；槽仍是自己的才把段交给引擎
    const ipc::BoundBlock block(rendezvous_->bindings().current());
    engine::EngineContext context;
    std::uint64_t callSeq = 0;
    if (block.ok())
    {
        callSeq = block.rv().header.probe_call_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        if (block.stillMine())
        {
            context.ring = &block.ring();
            context.rendezvous = &block.rv();
            context.group = block.binding().group;
            context.token = block.binding().writerToken;
            context.ringGeneration = block.binding().seq;
        }
    }

    diag::BlockFields fields;
    fields.callSeq = callSeq;
    fields.monoNs = startNs;
    fields.hostNs = transport.hostNs;
    fields.hostNsValid = transport.hostNsValid;
    fields.n = static_cast<std::uint32_t>(n);
    fields.nDecl = static_cast<std::uint32_t>(maxBlock_);
    fields.thread = diag::probeThreadHash(reinterpret_cast<std::uintptr_t>(juce::Thread::getCurrentThreadId()));
    fields.playing = transport.isPlaying;
    fields.nonrt = transport.nonrealtime;
    fields.bypassed = bypassed;

    if (isInput())
        processInput(view, params, transport, bypassed, context, startNs, fields);
    else
        processOutput(view, params, transport, bypassed, context, block, haveTimeline, timeline, fields);

    if (!firstBlockSeen_)
    {
        firstBlockSeen_ = true;
        audioEvent(diag::ProbeEventCode::kFirstBlock, 0, startNs);
    }
    if (!modeSeen_ || lastModeNonrt_ != transport.nonrealtime)
    {
        if (modeSeen_)
            audioEvent(diag::ProbeEventCode::kOfflineRender, transport.nonrealtime ? 1u : 0u, startNs);
        audioEvent(diag::ProbeEventCode::kProcessMode, transport.nonrealtime ? 1u : 0u, startNs);
        modeSeen_ = true;
        lastModeNonrt_ = transport.nonrealtime;
    }
    // call_seq 必须全文件严格递增：没拿到、或比上一次的不大（会合段换过），这一块就不写 B 行
    if (log_ != nullptr && callSeq > lastCallSeq_)
    {
        lastCallSeq_ = callSeq;
        log_->onBlock(fields);
    }
}

void ProbeHost::processInput(const engine::AudioView& view, const engine::EngineParams& params,
                             const engine::TransportInfo& transport, bool bypassed,
                             const engine::EngineContext& context, std::uint64_t startNs,
                             diag::BlockFields& fields) noexcept
{
    const int n = view.length;
    // 两端一起被挂起：两次回调隔了 250 ms 以上，而这段时间里环的写头几乎没走（Output 照常跑时它会走过差不多同样多的
    // 样本）。离线渲染时回调间隔不代表时间，不判
    if (haveLastBlock_ && !transport.nonrealtime && startNs > lastStartNs_ &&
        startNs - lastStartNs_ > kSuspendFlushNs + lastBlockNs_)
    {
        const double gapSamples = static_cast<double>(startNs - lastStartNs_) * sampleRate_ / 1.0e9;
        const std::uint64_t head = context.ring != nullptr ? ipc::LoopRing::writeHead(*context.ring) : lastHead_;
        const std::int64_t moved = ipc::frameDiff(head, lastHead_);
        if (static_cast<double>(std::max<std::int64_t>(moved, 0)) * 2.0 < gapSamples)
        {
            input_.requestFlushAfterSuspend();
            suspendFlushes_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Ping 是否跑这一块在块首定一次：中途被点开的从下一块起
    const bool pingBlock = ping_ != nullptr && n <= kScratchFrames &&
                           (ping_->active() || settle_ != Settle::kOff || pingRequest_.load(std::memory_order_relaxed));
    if (pingBlock)
        for (int c = 0; c < view.numChannels; ++c)
            std::copy(view.channels[c], view.channels[c] + n, dry_[c].begin());

    input_.process(view, params, transport, bypassed, context);
    const loop::BlockResult& result = input_.lastBlockResult();
    if (pingBlock)
        applyPing(view, n);

    const loop::LoopStatusFields status = input_.loopStatus();
    fields.m = status.drift;
    fields.w = static_cast<std::uint64_t>(std::max<std::int64_t>(status.writeHead, 0));
    fields.off = status.offset;
    fields.rs = status.resyncCount - lastResyncCount_;
    lastResyncCount_ = status.resyncCount;
    fields.d = toU32(result.delayEffective);
    if (result.paired && !pairedSeen_)
        audioEvent(diag::ProbeEventCode::kFirstPairedBlock, 0, startNs);
    pairedSeen_ = result.paired;

    haveLastBlock_ = true;
    lastStartNs_ = startNs;
    lastBlockNs_ = static_cast<std::uint64_t>(static_cast<double>(n) * 1.0e9 / sampleRate_);
    if (context.ring != nullptr)
        lastHead_ = ipc::LoopRing::writeHead(*context.ring);
}

// Ping 分两段。先是这里的静音段：干声与回送一起用 10 ms 线性降到 0，再静音「当前 D + 两块 + 50 ms」，让读头将要
// 读到的环里的旧内容都换成静音（否则注入窗口的前 D 个样本读回的是 Ping 之前的信号，互相关的次峰会盖过真峰）；然后才
// 交给 ProbePing。它自己的 10 ms 下行乘在 0 上，听不见；它的两个系数都到 0（开始注入）的那个样本上，静音段的增益直接
// 回到 1（乘在 0 上，没有阶跃），之后的注入、读回与恢复都归它。
// 合成：out = 静音段增益 ·（干声系数·g_dry·干声 + 回送系数·（引擎输出 − g_dry·干声））+ 注入。系数为 0 的项不乘
// （干声或回送里的 NaN 乘 0 还是 NaN）。读回是安全链之前的原始读头
void ProbeHost::applyPing(const engine::AudioView& view, int n) noexcept
{
    const loop::BlockResult& result = input_.lastBlockResult();
    const auto count = static_cast<std::size_t>(n);
    const std::size_t ramp = ping_->rampSamples();
    if (pingCancel_.exchange(false, std::memory_order_acq_rel) &&
        (settle_ == Settle::kDown || settle_ == Settle::kHold))
        restoreSettle();
    if (settle_ == Settle::kOff && !ping_->active() && pingRequest_.exchange(false, std::memory_order_acq_rel))
        settle_ = Settle::kDown;
    // ProbePing 在接手之前就被取消了：等不到交接，静音段自己升回去
    if (settle_ == Settle::kHandOver && !ping_->active())
        restoreSettle();

    const float* readback = result.returnLength >= n ? result.returnLeft : nullptr;
    const bool running = ping_->process(inject_.data(), readback, dryScale_.data(), returnScale_.data(), count);
    if (!running)
    {
        std::fill_n(inject_.begin(), count, 0.0f);
        std::fill_n(dryScale_.begin(), count, 1.0f);
        std::fill_n(returnScale_.begin(), count, 1.0f);
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        switch (settle_)
        {
        case Settle::kDown:
            settlePos_ = settlePos_ > 0 ? settlePos_ - 1 : 0;
            if (settlePos_ == 0)
            {
                settle_ = Settle::kHold;
                settleLeft_ = std::max<std::int64_t>(result.delayEffective, 0) + 2 * maxBlock_ +
                              static_cast<std::int64_t>(std::llround(kPingMarginSeconds * sampleRate_));
            }
            break;
        case Settle::kHold:
            if (settleLeft_ > 0)
                --settleLeft_;
            else
                settle_ = ping_->start() ? Settle::kHandOver : Settle::kUp; // 下一块起由 ProbePing 接手
            break;
        case Settle::kHandOver:
            if (running && dryScale_[i] == 0.0f && returnScale_[i] == 0.0f)
            {
                settlePos_ = ramp;
                settle_ = Settle::kOff;
            }
            break;
        case Settle::kUp:
            settlePos_ = std::min(settlePos_ + 1, ramp);
            if (settlePos_ == ramp)
                settle_ = Settle::kOff;
            break;
        case Settle::kOff:
            break;
        }
        const float gain = static_cast<float>(settlePos_) / static_cast<float>(ramp);
        dryScale_[i] *= gain;
        returnScale_[i] *= gain;
    }
    settling_.store(settle_ != Settle::kOff, std::memory_order_relaxed);

    const float dryGain = input_.dryGain();
    for (int c = 0; c < view.numChannels; ++c)
    {
        float* out = view.channels[c];
        const float* dry = dry_[c].data();
        for (std::size_t i = 0; i < count; ++i)
        {
            if (dryScale_[i] == 1.0f && returnScale_[i] == 1.0f && inject_[i] == 0.0f)
                continue; // 原样
            const float scaledDry = dry[i] * dryGain;
            float v = inject_[i];
            if (dryScale_[i] != 0.0f)
                v += dryScale_[i] * scaledDry;
            if (returnScale_[i] != 0.0f)
                v += returnScale_[i] * (out[i] - scaledDry);
            out[i] = v;
        }
    }
    // 回读攒满后回送要走安全链的 500 ms 淡入：借「载入预设」的命令序号，下一块块首生效
    if (ping_->takeFadeInRequest())
        ++presetSeq_;
}

// 静音段在交给 ProbePing 之前结束（取消、或 ProbePing 没接手）：增益用 10 ms 升回 1，回送另走一次预设淡入
void ProbeHost::restoreSettle() noexcept
{
    settle_ = Settle::kUp;
    ++presetSeq_;
}

void ProbeHost::processOutput(const engine::AudioView& view, const engine::EngineParams& params,
                              const engine::TransportInfo& transport, bool bypassed,
                              const engine::EngineContext& context, const ipc::BoundBlock& block, bool haveTimeline,
                              std::int64_t timeline, diag::BlockFields& fields) noexcept
{
    // w 是块首的环写头（没有环时按上一块推算）；m 是它与「上一块的 w + n」之差；off 是它相对时间线的偏移
    const std::uint64_t expected = lastOutputW_ + lastOutputN_;
    const std::uint64_t w = context.ring != nullptr ? ipc::LoopRing::writeHead(*context.ring) : expected;
    output_.process(view, params, transport, bypassed, context);
    fields.w = w;
    fields.m = haveOutputW_ ? ipc::frameDiff(w, expected) : 0;
    const std::int64_t position = haveTimeline ? timeline : static_cast<std::int64_t>(localSamples_);
    fields.off = static_cast<std::int64_t>(w) - position;
    fields.rs = 0;

    bool peerActive = false;
    if (block.ok())
    {
        const ipc::RoleSlot& peer = block.peer();
        fields.d = peer.d_eff.load(std::memory_order_relaxed);
        peerActive = ipc::ctlPhase(peer.ctl.load(std::memory_order_acquire)) == ipc::kCtlPhaseActive;
    }
    if (peerActive && !pairedSeen_)
        audioEvent(diag::ProbeEventCode::kFirstPairedBlock, 0, fields.monoNs);
    pairedSeen_ = peerActive;
    outputPeerActive_.store(peerActive, std::memory_order_relaxed);

    haveOutputW_ = true;
    lastOutputW_ = w;
    lastOutputN_ = static_cast<std::uint32_t>(view.length);
    localSamples_ += static_cast<std::uint64_t>(view.length);
}

void ProbeHost::audioEvent(diag::ProbeEventCode code, std::uint64_t a, std::uint64_t startNs) noexcept
{
    if (log_ != nullptr)
        log_->onAudioEvent(code, a, 0, startNs);
}

void ProbeHost::onFade(void* context, engine::FadeReason reason) noexcept
{
    auto* self = static_cast<ProbeHost*>(context);
    const auto index = static_cast<std::size_t>(reason);
    if (index < std::size(self->fadeIns_))
        self->fadeIns_[index].fetch_add(1, std::memory_order_relaxed);
}

void ProbeHost::getState(juce::MemoryBlock& dest)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<std::uint8_t> bytes = keeper_.save(stateData_);
    dest.replaceAll(bytes.data(), bytes.size());
    logEvent(diag::ProbeEventCode::kGetState);
}

void ProbeHost::setState(const void* data, int size)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    openLogLocked();
    ProbeStateData next = stateData_;
    const StateStatus status =
        keeper_.load(static_cast<const std::uint8_t*>(data), size > 0 ? static_cast<std::size_t>(size) : 0u, next);
    if (status == StateStatus::kOk)
    {
        stateData_ = next;
        feedback_.store(next.feedbackPercent, std::memory_order_relaxed);
        delayMs_.store(next.delayMs, std::memory_order_relaxed);
        ceilingDb_.store(next.ceilingDb, std::memory_order_relaxed);
        applyGroup(next.group, ipc::ClaimIntent::kRestore, nowMs());
    }
    // 宿主设了工程状态：不论读进来什么，回送都从 0 淡入（原因 SetState）
    stateSeq_.fetch_add(1, std::memory_order_relaxed);
    logEvent(diag::ProbeEventCode::kSetState, {groupArg(stateData_.group)});
}

// JUCE 8.0.8 的公开接口只给轨道名与颜色两项
void ProbeHost::trackProperties(const juce::AudioProcessor::TrackProperties& properties)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    openLogLocked();
    const std::string name = properties.name.has_value() ? properties.name->toStdString() : std::string();
    std::uint64_t colourArg = 0;
    std::uint32_t argb = 0;
    if (properties.colour.has_value())
    {
        argb = properties.colour->getARGB();
        const std::uint32_t alpha = argb >> 24;
        colourArg = alpha == 0xFFu ? diag::probeColourArg(argb & 0xFFFFFFu)
                                   : diag::probeColourArgWithAlpha(((argb & 0xFFFFFFu) << 8) | alpha);
    }
    logEvent(diag::ProbeEventCode::kTrackProps, {0, colourArg, name});
    rendezvous_->setLabel(name, argb);
}

juce::AudioProcessorEditor* ProbeHost::createEditor()
{
    return new ProbeEditor(processor_, *this);
}

// ---- tick ----

void ProbeHost::tick()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    tickLocked(nowNs(), nowMs());
}

void ProbeHost::tickLocked(std::uint64_t ns, std::uint64_t ms)
{
    const bool nonrealtime = processor_.isNonRealtime();
    if (nonrealtime != lastNonrealtime_)
    {
        rendezvous_->setNonrealtime(nonrealtime);
        lastNonrealtime_ = nonrealtime;
        nonrealtimeSeen_ = nonrealtimeSeen_ || nonrealtime;
    }
    rendezvous_->tick(ms);
    noteRendezvous();
    notePeer();
    drainEngine();
    const std::uint32_t resets = resets_.load(std::memory_order_relaxed);
    for (; resetsLogged_ != resets; ++resetsLogged_)
        logEvent(diag::ProbeEventCode::kReset);
    tickPing();
    if (log_ != nullptr && recordDeadlineNs_ != 0 && ns >= recordDeadlineNs_)
    {
        recordDeadlineNs_ = 0;
        if (log_->isRecording())
            log_->endCase();
    }
    tickAutoRecording(ns);
    if (log_ != nullptr && !config_.startLogThread)
        log_->pump();
}

// backend 事件写一次：段打开（成功写 0），或后端不可用（写错误码，被拒再写 shm_denied）
void ProbeHost::noteRendezvous()
{
    if (backendLogged_)
        return;
    switch (rendezvous_->state())
    {
    case ipc::RendezvousState::kIpcUnavailable:
        logEvent(
            diag::ProbeEventCode::kBackend,
            {kBackendKind, static_cast<std::uint64_t>(std::max(gLastIpcError.load(std::memory_order_relaxed), 0))});
        if (rendezvous_->lastResult() == ipc::InitResult::kFailedDenied)
            logEvent(diag::ProbeEventCode::kShmDenied);
        backendLogged_ = true;
        break;
    case ipc::RendezvousState::kActive:
    case ipc::RendezvousState::kConflict:
    case ipc::RendezvousState::kDormant:
    case ipc::RendezvousState::kAbiMismatch:
        logEvent(diag::ProbeEventCode::kBackend, {kBackendKind, 0});
        backendLogged_ = true;
        break;
    default:
        break;
    }
}

// paired：本端持有槽、同组对端的槽是 active（扫描稳定）的上升沿
void ProbeHost::notePeer()
{
    const std::uint32_t group = rendezvous_->group();
    bool active = false;
    std::uint32_t pid = 0;
    if (group >= 1 && group <= ipc::kGroupCount && rendezvous_->state() == ipc::RendezvousState::kActive &&
        rendezvous_->observer().scanned())
    {
        const ipc::SlotSample& peer = rendezvous_->observer().sample(group, ipc::peerOf(config_.role));
        active = peer.stable && ipc::ctlPhase(peer.ctl) == ipc::kCtlPhaseActive;
        pid = peer.pid;
    }
    if (active && !peerActive_)
        logEvent(diag::ProbeEventCode::kPaired, {pid, group - 1});
    peerActive_ = active;
}

void ProbeHost::drainEngine()
{
    const auto sink = [this](const diag::Event& event) {
        switch (event.code)
        {
        case static_cast<std::uint16_t>(engine::SafetyEventCode::Tripped):
            if (event.a == static_cast<std::uint8_t>(engine::TripReason::Panic))
                ++counts_.panicsExecuted;
            else if (log_ != nullptr)
                log_->noteTrip();
            break;
        case static_cast<std::uint16_t>(engine::SafetyEventCode::Recovered):
            ++counts_.recoversExecuted;
            break;
        case static_cast<std::uint16_t>(loop::LoopEventCode::kLineCleared):
            if (event.a == 2)
                ++counts_.suspendClears;
            break;
        default:
            break;
        }
    };
    if (isInput())
        input_.drainEvents(sink);
    else
        output_.drainEvents(sink);
}

void ProbeHost::tickPing()
{
    if (ping_ == nullptr)
        return;
    if (ping_->state() == ProbePing::State::kDone)
    {
        const PingResult result = ping_->finish();
        havePing_ = result.ok;
        if (result.ok)
        {
            lastPingSamples_ = result.delaySamples;
            logEvent(diag::ProbeEventCode::kPing, {result.delaySamples});
        }
    }
    const bool playing = playing_.load(std::memory_order_relaxed);
    if (pingBusy() && playing && !drySilent_.load(std::memory_order_relaxed))
    {
        pingRequest_.store(false, std::memory_order_relaxed);
        pingCancel_.store(true, std::memory_order_release);
        ping_->cancel();
    }
    else if (autoPingTrigger(wasPlaying_, playing) && !pingBusy())
    {
        pingRequest_.store(true, std::memory_order_release);
    }
    wasPlaying_ = playing;
}

bool ProbeHost::pingBusy() const noexcept
{
    return ping_->active() || settling_.load(std::memory_order_relaxed) || pingRequest_.load(std::memory_order_relaxed);
}

// 只在音频没有在跑的时候（prepare / release）
void ProbeHost::resetSettle() noexcept
{
    settle_ = Settle::kOff;
    settlePos_ = ping_->rampSamples();
    settleLeft_ = 0;
    settling_.store(false, std::memory_order_relaxed);
    pingRequest_.store(false, std::memory_order_relaxed);
    pingCancel_.store(false, std::memory_order_relaxed);
}

// 隐藏参数：三项合规且与上一次自动开窗的不同、日志开着且没在记录，就开一个窗口；auto_record_s 回到 0 才忘掉上一次
void ProbeHost::tickAutoRecording(std::uint64_t ns)
{
    const auto read = [](const std::atomic<float>* p) {
        return p != nullptr ? static_cast<int>(std::lround(p->load(std::memory_order_relaxed))) : 0;
    };
    const AutoRequest request = parseAutoParams(read(autoCase_), read(autoVariant_), read(autoRecord_));
    if (request.status == AutoStatus::kIdle)
    {
        autoKey_.clear();
        return;
    }
    if (request.status != AutoStatus::kOk || sampleRate_ <= 0.0)
        return;
    const std::string key = request.caseId + ' ' + request.variant + ' ' + std::to_string(request.recordSeconds);
    if (key == autoKey_)
        return;
    diag::ProbeCase c;
    c.caseId = request.caseId;
    c.variant = request.variant;
    c.guard = diag::ProbeGuard::kNa;
    c.sampleRate = static_cast<std::uint32_t>(std::llround(sampleRate_));
    if (beginRecording(c, request.recordSeconds, ns))
        autoKey_ = key;
}

bool ProbeHost::beginRecording(const diag::ProbeCase& c, std::uint32_t seconds, std::uint64_t ns)
{
    if (log_ == nullptr || !log_->isOpen() || log_->isRecording() || seconds == 0 || !log_->beginCase(c))
        return false;
    recordDeadlineNs_ = ns + static_cast<std::uint64_t>(seconds) * 1'000'000'000u;
    return true;
}

void ProbeHost::applyGroup(int group, ipc::ClaimIntent intent, std::uint64_t ms)
{
    stateData_.group = group >= 0 && group < static_cast<int>(ipc::kGroupCount) ? group : -1;
    origin_ = stateData_.group < 0                   ? diag::DiagOrigin::kNone
              : intent == ipc::ClaimIntent::kRestore ? diag::DiagOrigin::kRestore
                                                     : diag::DiagOrigin::kManual;
    rendezvous_->setGroup(static_cast<std::uint32_t>(stateData_.group + 1), intent, ms);
}

// ---- 面板 ----

ProbeStatus ProbeHost::probeStatus() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return statusLocked();
}

ProbeStatus ProbeHost::statusLocked() const
{
    ProbeStatus status;
    status.transportPlaying = playing_.load(std::memory_order_relaxed);
    status.drySilent = drySilent_.load(std::memory_order_relaxed);
    status.paired = isInput() ? input_.loopStatus().paired != 0 : outputPeerActive_.load(std::memory_order_relaxed);
    status.logOpen = log_ != nullptr && log_->isOpen();
    status.recording = log_ != nullptr && log_->isRecording();
    status.pingRunning = ping_ != nullptr && pingBusy();
    status.tripped = isInput() && input_.safetyDiag().state == static_cast<std::uint8_t>(engine::SafetyState::Tripped);
    status.group = stateData_.group;
    status.lastPingSamples = havePing_ ? static_cast<int>(lastPingSamples_) : -1;
    if (status.recording && recordDeadlineNs_ != 0)
    {
        const std::uint64_t ns = nowNs();
        const std::uint64_t left = recordDeadlineNs_ > ns ? recordDeadlineNs_ - ns : 0;
        status.recordRemainingSeconds = static_cast<std::uint32_t>((left + 999'999'999u) / 1'000'000'000u);
    }
    status.sampleRate = static_cast<std::uint32_t>(std::llround(std::max(sampleRate_, 0.0)));
    status.feedbackPercent = feedback_.load(std::memory_order_relaxed);
    status.delayMs = delayMs_.load(std::memory_order_relaxed);
    status.ceilingDb = ceilingDb_.load(std::memory_order_relaxed);
    if (log_ != nullptr)
        status.counters = log_->counters();
    return status;
}

// Panic、恢复、Ping 只在 Input 端有意义（回送与安全链都在 Input）；Output 端什么都不做
void ProbeHost::probePanic()
{
    if (isInput())
        panicSeq_.fetch_add(1, std::memory_order_relaxed);
}

void ProbeHost::probeRecover()
{
    if (isInput())
        recoverSeq_.fetch_add(1, std::memory_order_relaxed);
}

void ProbeHost::probePing()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (ping_ != nullptr && pingAllowed(statusLocked()))
        pingRequest_.store(true, std::memory_order_release);
}

bool ProbeHost::probeStartRecording(const ProbeSelection& selection, std::uint32_t seconds)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    // 日志写 C 行时也会再查一遍格；这里先挡掉，被拒的选择也不进诊断
    if (!isValidSelection(selection) || sampleRate_ <= 0.0 ||
        !beginRecording(toProbeCase(selection, static_cast<std::uint32_t>(std::llround(sampleRate_))), seconds,
                        nowNs()))
        return false;
    lastSelection_ = selection;
    return true;
}

void ProbeHost::probeSetGroup(int group)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    applyGroup(group, ipc::ClaimIntent::kManual, nowMs());
}

void ProbeHost::probeSetFeedbackPercent(float percent)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    stateData_.feedbackPercent = clampFeedbackPercent(percent);
    feedback_.store(stateData_.feedbackPercent, std::memory_order_relaxed);
}

void ProbeHost::probeSetDelayMs(float ms)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    stateData_.delayMs = clampDelayMs(ms);
    delayMs_.store(stateData_.delayMs, std::memory_order_relaxed);
}

std::string ProbeHost::probeDiagnosticsText()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const ProbeStatus status = statusLocked();
    diag::DiagInputs in;
    in.role = isInput() ? diag::DiagRole::kInput : diag::DiagRole::kOutput;
    in.product = processor_.getName().toStdString();
    in.version = build::kVersion;
    in.build = build::buildLabel();
    in.format = isAu(processor_) ? "AU" : "VST3";
    in.host = juce::PluginHostType().getHostDescription();
    in.os = juce::SystemStats::getOperatingSystemName().toStdString();
#if defined(__aarch64__) || defined(_M_ARM64)
    in.arch = "arm64";
#else
    in.arch = "x64";
#endif
    in.backend = ipc::platformSegmentBackendName();
    in.prefix = build::kShmPrefix;
    in.ipcAbi = ipc::kIpcAbi;
    in.group = rendezvous_->group();
    in.origin = origin_;
    in.pid = ipc::currentProcessId();
    const ipc::RendezvousState state = rendezvous_->state();
    in.pairState =
        state == ipc::RendezvousState::kActive ? (peerActive_ ? "connected" : "waiting-peer") : rendezvousName(state);
    if (in.group >= 1 && in.group <= ipc::kGroupCount && rendezvous_->observer().scanned())
    {
        const ipc::SlotSample& peer = rendezvous_->observer().sample(in.group, ipc::peerOf(config_.role));
        const std::uint32_t phase = ipc::ctlPhase(peer.ctl);
        in.peer.present = peer.stable && (phase == ipc::kCtlPhaseActive || phase == ipc::kCtlPhaseDormant);
        in.peer.role = isInput() ? diag::DiagRole::kOutput : diag::DiagRole::kInput;
        in.peer.active = phase == ipc::kCtlPhaseActive;
        in.peer.samePid = peer.pid == in.pid;
        in.peer.version = peer.version;
        in.peer.sampleRate = peer.sampleRate;
        in.peer.maxBlock = peer.maxBlock;
        in.peer.channels = peer.channels;
        in.peer.heartbeatAgeMs = ipc::ageMs(nowMs(), peer.heartbeatMs);
    }
    in.prepare.count = prepareCount_;
    in.prepare.sampleRate = status.sampleRate;
    in.prepare.maxBlock = static_cast<std::uint32_t>(maxBlock_);
    in.prepare.nonrealtimeSeen = nonrealtimeSeen_;
    in.prepare.resetCalls = resets_.load(std::memory_order_relaxed);
    if (isInput())
    {
        const loop::LoopStatusFields loop = input_.loopStatus();
        in.loop.dUserMs = loop.delayUserMs;
        in.loop.dEff = toU32(loop.delayEffective);
        in.loop.dMin = toU32(loop.delayMin);
        in.loop.mode = loop.delayMode != 0 ? diag::DiagDelayMode::kSync : diag::DiagDelayMode::kTime;
        in.loop.bpm = loop.bpmCenti / 100.0;
        in.loop.tapsActive = loop.tapsActive;
        in.loop.tapDrop = loop.tapDrop;
        in.sync.resync = loop.resyncCount;
        in.sync.overflow = toU32(loop.overflow);
        in.sync.torn = toU32(loop.torn);
        in.sync.overrun = toU32(loop.overrun);
        in.sync.wCasFail = loop.wCasFail;
        in.sync.unstable = loop.unstable;
        in.sync.offDriftBlocks = loop.offsetDriftBlocks;
        in.sync.frozenMax = loop.frozenMax;
        in.order.verdict = loop.orderVerdict == static_cast<std::uint8_t>(loop::OrderVerdict::kCorrect)
                               ? diag::DiagOrderVerdict::kCorrect
                           : loop.orderVerdict == static_cast<std::uint8_t>(loop::OrderVerdict::kOutputFirst)
                               ? diag::DiagOrderVerdict::kWrong
                               : diag::DiagOrderVerdict::kUnknown;
        in.order.votes = loop.orderVotes;
        in.order.hostNsValid = loop.hostNsValid != 0;
        const engine::SafetyDiag safety = input_.safetyDiag();
        in.safety.state = safetyName(safety.state);
        in.safety.trips = safety.trips;
        in.safety.tailPeakMaxDb = safety.tailPeakMaxDb;
        in.safety.limiterGrMaxDb = safety.limiterGrMaxDb;
        in.safety.nonfiniteRing = safety.nonfiniteRing;
        in.safety.clampRing = safety.clampRing;
    }
    return buildDiagText(in, status, logDir_, lastSelection_);
}

// ---- 读出 ----

ProbeHost::Counts ProbeHost::counts() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Counts out = counts_;
    out.suspendFlushes = suspendFlushes_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < out.fadeIns.size(); ++i)
        out.fadeIns[i] = fadeIns_[i].load(std::memory_order_relaxed);
    return out;
}

ipc::RendezvousState ProbeHost::rendezvousState() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return rendezvous_->state();
}

void ProbeHost::pumpLog()
{
    if (log_ != nullptr)
        log_->pump();
}

} // namespace rinne::probe
