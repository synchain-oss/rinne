// SPDX-License-Identifier: GPL-3.0-or-later

#include "InstanceHost.h"

#include <algorithm>
#include <iterator>

#include "diag/ProbeLog.h"
#include "engine/Meters.h"
#include "ipc/LoopRing.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/ProcessInfo.h"
#include "ipc/SegmentLayout.h"
#include "loop/LoopEvents.h"
#include "loop/LoopTiming.h"
#include "util/MonoClock.h"
#include "util/RinneVersion.h"

namespace rinne
{
namespace
{

// 后端失败的错误码只经全局的诊断出口给出（IpcDiag 只有一个槽）：装一个转发出口，把错误码记给正在调会合的那个实例，
// 再交给装它之前的那个出口。全进程一份，不随实例卸：出口本身不带实例状态；卸要从链中间把自己摘掉，做不成原子的，
// 反而会把别人后来装的出口冲掉
std::atomic<ipc::IpcDiagSink> gPreviousSink{nullptr};
std::atomic<InstanceHost::InstallHook> gInstallHook{nullptr};

// 每个线程一份：本线程正替哪个实例调会合（指向它的 ipcError_）。后端在调用线程上同步报告，所以记进去的只有该实例
// 这一拍里报出的错误码，同进程的别的实例在别的线程或别的拍里出的错记不进来
thread_local int* tIpcErrorTarget = nullptr;

void recordIpcDiag(const ipc::IpcDiagEvent& event) noexcept
{
    if (event.error != 0 && tIpcErrorTarget != nullptr)
        *tIpcErrorTarget = event.error;
    if (const ipc::IpcDiagSink previous = gPreviousSink.load(std::memory_order_acquire))
        previous(event);
}

// 作用域内本线程报出的后端错误码记到 target
class IpcErrorCapture
{
public:
    explicit IpcErrorCapture(int& target) noexcept : saved_(tIpcErrorTarget) { tIpcErrorTarget = &target; }
    ~IpcErrorCapture() { tIpcErrorTarget = saved_; }
    IpcErrorCapture(const IpcErrorCapture&) = delete;
    IpcErrorCapture& operator=(const IpcErrorCapture&) = delete;

private:
    int* saved_;
};

// 本类读单调纳秒都经这里：默认时钟（两端一起挂起的判定）与会合退避的种子。正式构建也靠它，不是探针专用。
// 它和 util::monoNowMs 不是同一个时钟，不能拿去和共享内存里的心跳比（会合与心跳一律用 monoNowMs，见 nowMs）。
// 转调的 diag::probeMonoNs 名字带 probe 是 core 里的历史命名：毫秒时钟不够细，纳秒读数当初是为探针日志写的
std::uint64_t monoNs() noexcept
{
    return diag::probeMonoNs();
}

} // namespace

// 一步换槽：两个实例在不同线程上同时构造时，后换的那个拿回来的是转发出口自己，不记，转发目标就不会指向自己
// （先查再装的写法里两边都可能看到「还没装」，后装的把自己记成转发目标，第一次报错就无限递归）
void InstanceHost::installIpcDiagSink() noexcept
{
    if (const InstallHook hook = gInstallHook.exchange(nullptr, std::memory_order_acq_rel))
        hook();
    const ipc::IpcDiagSink previous = ipc::setIpcDiagSink(&recordIpcDiag);
    if (previous != &recordIpcDiag)
        gPreviousSink.store(previous, std::memory_order_release);
}

ipc::IpcDiagSink InstanceHost::ipcDiagRecorder() noexcept
{
    return &recordIpcDiag;
}

ipc::IpcDiagSink InstanceHost::ipcDiagForwardTarget() noexcept
{
    return gPreviousSink.load(std::memory_order_acquire);
}

void InstanceHost::setIpcDiagInstallHook(InstallHook hook) noexcept
{
    gInstallHook.store(hook, std::memory_order_release);
}

InstanceHost::InstanceHost(juce::AudioProcessor& processor, const InstanceHostConfig& config)
    : processor_(processor), config_(config)
{
    input_.setFadeObserver(this, &InstanceHost::onFade);

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
    rc.format = isAuWrapper() ? ipc::kFormatAu : ipc::kFormatVst3;
    rc.hostPpid = ipc::parentProcessId();
    rc.seed = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^ monoNs();
    rendezvous_ = std::make_unique<ipc::Rendezvous>(rc);

    if (config_.startTimer)
        startTicking();
}

// 析构时已经没有音频回调会进来（宿主先停了包装层）。最后一个实例的会合析构里还要把剩下的映射立即收掉，所以顺序是：
// 先撤下绑定快照（之后谁也拿不到段），再析构会合（它放开槽、放下句柄、按需做最后的回收），后端最后
InstanceHost::~InstanceHost()
{
    stopTimer();
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_.store(false, std::memory_order_release);
    rendezvous_->bindings().clear();
    rendezvous_.reset();
}

std::uint64_t InstanceHost::nowNs() const noexcept
{
    return config_.clock != nullptr ? config_.clock(config_.clockContext) : monoNs();
}

std::uint64_t InstanceHost::nowMs() const noexcept
{
    return config_.clock != nullptr ? config_.clock(config_.clockContext) / 1'000'000u : util::monoNowMs();
}

bool InstanceHost::isAuWrapper() const noexcept
{
    return processor_.wrapperType == juce::AudioProcessor::wrapperType_AudioUnit ||
           processor_.wrapperType == juce::AudioProcessor::wrapperType_AudioUnitv3;
}

// ---- 处理器转发 ----

void InstanceHost::prepare(double sampleRate, int maxBlock)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_.store(false, std::memory_order_release);
    // 宿主挂起再恢复（release 再 prepare）正是要判的「两端一起停了多久」：同采样率时上一块的账留着
    const bool rateChanged = sampleRate != sampleRate_;
    onPrepare(rateChanged);
    if (rateChanged)
        haveLastBlock_ = false;
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(maxBlock, 1);
    ++prepareCount_;
    if (isInput())
        input_.prepare(sampleRate, maxBlock_);
    else
        output_.prepare(sampleRate, maxBlock_);
    const auto channels = static_cast<std::uint32_t>(std::max(processor_.getMainBusNumOutputChannels(), 1));
    rendezvous_->prepare({loop::publishedSampleRate(sampleRate), static_cast<std::uint32_t>(maxBlock_), channels},
                         nowMs());
    onPrepared();
    ready_.store(true, std::memory_order_release);
}

void InstanceHost::release()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_.store(false, std::memory_order_release);
    rendezvous_->releaseResources(nowMs());
    onReleased();
}

void InstanceHost::reset() noexcept
{
    // VST3 的 setProcessing(false) 会走到这里，可能在音频线程：只记数
    resets_.fetch_add(1, std::memory_order_relaxed);
}

// 不在壳里切块：块长超过声明值是环路层要看见的违约（补 0、把 D 的下限抬到块长），
// 壳若按声明值切开再交，环路层看不见违约，D 下限不抬，读头就会读到还没写的帧。
// 超过 32768 的块两端引擎各自按超长处理（Input 只出干声，Output 不写环并置不支持块长），
// Output 写环按 8192 帧分段也在引擎里。引擎的暂存按 32768 预分配，任何块长都不越界
void InstanceHost::process(juce::AudioBuffer<float>& buffer, engine::EngineParams params, bool bypassed) noexcept
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

    params.panicSeq = panicSeq_.load(std::memory_order_relaxed);
    params.recoverSeq = recoverSeq_.load(std::memory_order_relaxed);

    engine::AudioView view;
    view.channels = buffer.getArrayOfWritePointers();
    view.numChannels = std::min(buffer.getNumChannels(), 2);
    view.length = n;

    // 块首租约：槽仍是自己的才把段交给引擎
    const ipc::BoundBlock lease(rendezvous_->bindings().current());
    engine::EngineContext context;
    const ipc::SegmentHandle* ring = nullptr; // 交给引擎的环是哪一个映射（只比地址，不经它读写）
    if (lease.ok() && lease.stillMine())
    {
        context.ring = &lease.ring();
        context.rendezvous = &lease.rv();
        context.group = lease.binding().group;
        context.token = lease.binding().writerToken;
        context.ringGeneration = lease.binding().seq;
        ring = lease.binding().ring;
    }

    const Block block{view, transport, context, lease, bypassed, haveTimeline, timeline, startNs};
    if (isInput())
    {
        detectSuspend(context, ring, transport, startNs);
        beforeEngine(block);
        input_.process(view, params, transport, bypassed, context);
        afterEngine(block);
        noteBlockForSuspend(context, ring, n, startNs);
    }
    else
    {
        beforeEngine(block);
        output_.process(view, params, transport, bypassed, context);
        afterEngine(block);
    }
}

// 两端一起被挂起：两次回调隔了 250 ms 以上，而这段时间里环的写头几乎没走（Output 照常跑时它会走过差不多同样多的
// 样本）。写头是唯一的证据：这一块与上一块拿到的是同一个环才判，没配对时的长间隔不算挂起、不计数。
// 停的期间换过环（换组：旧环退休、新环另开）时两个写头互不相干，不能相减，这一块当成「上一块没拿到环」。认环用绑定
// 里环的句柄地址：HandleKeeper 把放掉的句柄留到析构，地址不会给别的映射复用。绑定序号不行：同一个环在 release →
// prepare（休眠、激活各重发一次绑定）时它也会变，那正是要判的一格。换写方不换环，写头照常连续，照判。
// 离线渲染不判：回调间隔不代表时间，比实时慢的导出每两块之间都可能隔 250 ms 以上，而写头同样只走一块（Output 每块
// 照常写），从回调上分不出它和挂起；这一条只决定按单调时钟的判据启不启用，不改引擎的处理
void InstanceHost::detectSuspend(const engine::EngineContext& context, const ipc::SegmentHandle* ring,
                                 const engine::TransportInfo& transport, std::uint64_t startNs) noexcept
{
    const bool sameRing = ring != nullptr && ring == lastRing_;
    if (haveLastBlock_ && sameRing && !transport.nonrealtime && startNs > lastStartNs_ &&
        startNs - lastStartNs_ > kSuspendFlushNs + lastBlockNs_)
    {
        const double gapSamples = static_cast<double>(startNs - lastStartNs_) * sampleRate_ / 1.0e9;
        const std::int64_t moved = ipc::frameDiff(ipc::LoopRing::writeHead(*context.ring), lastHead_);
        if (static_cast<double>(std::max<std::int64_t>(moved, 0)) * 2.0 < gapSamples)
        {
            input_.requestFlushAfterSuspend();
            suspendFlushes_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void InstanceHost::noteBlockForSuspend(const engine::EngineContext& context, const ipc::SegmentHandle* ring, int n,
                                       std::uint64_t startNs) noexcept
{
    haveLastBlock_ = true;
    lastStartNs_ = startNs;
    lastBlockNs_ = static_cast<std::uint64_t>(static_cast<double>(n) * 1.0e9 / sampleRate_);
    lastRing_ = ring;
    if (ring != nullptr)
        lastHead_ = ipc::LoopRing::writeHead(*context.ring);
}

void InstanceHost::onFade(void* context, engine::FadeReason reason) noexcept
{
    auto* self = static_cast<InstanceHost*>(context);
    const auto index = static_cast<std::size_t>(reason);
    if (index < std::size(self->fadeIns_))
        self->fadeIns_[index].fetch_add(1, std::memory_order_relaxed);
}

// ---- tick ----

void InstanceHost::tick()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    tickLocked(nowNs(), nowMs());
}

void InstanceHost::tickLocked(std::uint64_t ns, std::uint64_t ms)
{
    const bool nonrealtime = processor_.isNonRealtime();
    if (nonrealtime != lastNonrealtime_)
    {
        rendezvous_->setNonrealtime(nonrealtime);
        lastNonrealtime_ = nonrealtime;
        nonrealtimeSeen_ = nonrealtimeSeen_ || nonrealtime;
    }
    {
        // 打开段只发生在会合的 tick 里；这一拍里后端在本线程上报出的错误码记给本实例
        const IpcErrorCapture capture(ipcError_);
        rendezvous_->tick(ms);
    }
    onRendezvousTicked();
    drainEngine();
    onTicked(ns);
}

void InstanceHost::drainEngine()
{
    const auto sink = [this](const diag::Event& event) {
        switch (event.code)
        {
        case static_cast<std::uint16_t>(engine::SafetyEventCode::Tripped):
            if (event.a == static_cast<std::uint8_t>(engine::TripReason::Panic))
                ++counts_.panicsExecuted;
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
        onEngineEvent(event);
    };
    if (isInput())
        input_.drainEvents(sink);
    else
        output_.drainEvents(sink);
}

// ---- 组与命令 ----

void InstanceHost::setGroup(std::uint32_t group, ipc::ClaimIntent intent)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    setGroupLocked(group, intent, nowMs());
}

void InstanceHost::setGroupLocked(std::uint32_t group, ipc::ClaimIntent intent, std::uint64_t ms)
{
    rendezvous_->setGroup(group, intent, ms);
}

void InstanceHost::setCommandBaseline(std::uint32_t presetSeq, std::uint32_t stateSeq) noexcept
{
    engine::CommandBaseline baseline;
    baseline.presetSeq = presetSeq;
    baseline.stateSeq = stateSeq;
    baseline.panicSeq = panicSeq_.load(std::memory_order_relaxed);
    baseline.recoverSeq = recoverSeq_.load(std::memory_order_relaxed);
    input_.setCommandBaseline(baseline);
}

void InstanceHost::panic() noexcept
{
    if (isInput())
        panicSeq_.fetch_add(1, std::memory_order_relaxed);
}

void InstanceHost::recover() noexcept
{
    if (isInput())
        recoverSeq_.fetch_add(1, std::memory_order_relaxed);
}

bool InstanceHost::canRecover() const noexcept
{
    if (!isInput())
        return false;
    const std::uint32_t word = input_.meters().safety.load(std::memory_order_relaxed);
    return engine::PackedSafety::state(word) == engine::SafetyState::Tripped &&
           !engine::PackedSafety::coolingDown(word);
}

// ---- 读出 ----

InstanceHost::Counts InstanceHost::counts() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Counts out = counts_;
    out.suspendFlushes = suspendFlushes_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < out.fadeIns.size(); ++i)
        out.fadeIns[i] = fadeIns_[i].load(std::memory_order_relaxed);
    return out;
}

ipc::RendezvousState InstanceHost::rendezvousState() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return rendezvous_->state();
}

std::uint32_t InstanceHost::group() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return rendezvous_->group();
}

} // namespace rinne
