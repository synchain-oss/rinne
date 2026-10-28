// SPDX-License-Identifier: GPL-3.0-or-later

#include "InstanceHost.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include "PlatformLog.h"
#include "diag/ProbeLog.h"
#include "engine/Meters.h"
#include "ipc/LoopRing.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/ProcessInfo.h"
#include "ipc/SegmentLayout.h"
#include "loop/LoopEvents.h"
#include "loop/LoopStatus.h"
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

// 对端视图的段头规格：与会合打开会合段时用的同一套（magic、abi、本 abi 的段大小）
constexpr ipc::SegmentHeaderSpec kViewSpec{ipc::kRvMagic, ipc::kIpcAbi, ipc::kRvBytes, ipc::kRvHeaderBytes};

std::size_t roleIndex(ipc::Role role) noexcept
{
    return role == ipc::Role::kOutput ? 1u : 0u;
}

const char* reasonName(ipc::PairingReason reason) noexcept
{
    switch (reason)
    {
    case ipc::PairingReason::kSameNameInput:
        return "same-name input";
    case ipc::PairingReason::kLatestInput:
        return "latest input";
    case ipc::PairingReason::kWaitingOutput:
        return "waiting output";
    case ipc::PairingReason::kEmptyGroup:
        return "empty group";
    case ipc::PairingReason::kAllFull:
        break;
    }
    return "no group";
}

const char* safetyName(std::uint8_t state) noexcept
{
    static constexpr const char* kNames[] = {"UNPAIRED", "FADE_IN", "RUN", "STALLED", "GUARD", "TRIPPED"};
    return state < std::size(kNames) ? kNames[state] : "UNKNOWN";
}

std::uint32_t toU32(std::int64_t v) noexcept
{
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, 0, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t toU32(std::uint64_t v) noexcept
{
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, std::numeric_limits<std::uint32_t>::max()));
}

diag::DiagTrackRelation relationOf(pairing::NameCheck check) noexcept
{
    switch (check)
    {
    case pairing::NameCheck::kSame:
        return diag::DiagTrackRelation::kSame;
    case pairing::NameCheck::kDifferent:
        return diag::DiagTrackRelation::kDifferent;
    case pairing::NameCheck::kUnknown:
        break;
    }
    return diag::DiagTrackRelation::kUnknown;
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

    backend_ = backend;
    version_ = rc.version;
    if (pairingOn())
    {
        std::string name;
        if (ipc::Naming(config_.scope).name(ipc::SegmentId::rendezvous(), name) == ipc::NameStatus::kOk)
            viewName_ = std::move(name);
        else
            viewPermanent_ = true; // 名字不合法：会合自己也开不了，配对状态显示 IPC 不可用
    }
    if (config_.pairing)
        foldPairState(nowNs(), nowMs()); // 第一拍之前就有一份（验证宿主下一开始就是 validator）

    if (config_.startTimer)
        startTicking();
}

// 析构时已经没有音频回调会进来（宿主先停了包装层）。最后一个实例的会合析构里还要把剩下的映射立即收掉，所以顺序是：
// 先撤下绑定快照（之后谁也拿不到段），再析构会合（它放开槽、放下句柄、按需做最后的回收），再放下对端视图，后端最后
InstanceHost::~InstanceHost()
{
    stopTimer();
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_.store(false, std::memory_order_release);
    rendezvous_->bindings().clear();
    rendezvous_.reset();
    if (view_.base != nullptr)
        backend_->unmap(view_);
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
    const std::uint64_t ms = nowMs();
    rendezvous_->prepare({loop::publishedSampleRate(sampleRate), static_cast<std::uint32_t>(maxBlock_), channels}, ms);
    channels_ = channels;
    if (!firstPrepared_)
    {
        firstPrepared_ = true;
        firstPrepareMs_ = ms;
    }
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

    if (config_.pairing)
    {
        // 配对层的账：只有音频线程写，relaxed 就够（读方只要最终看到，不靠它们排序别的写）。实时块数饱和不回绕
        const std::uint32_t blocks = realtimeBlocks_.load(std::memory_order_relaxed);
        if (!transport.nonrealtime && blocks != std::numeric_limits<std::uint32_t>::max())
            realtimeBlocks_.store(blocks + 1u, std::memory_order_relaxed);
        lastBypassed_.store(bypassed, std::memory_order_relaxed);
        lastLoopOn_.store(params.loopOn, std::memory_order_relaxed);
        lastAudioNs_.store(std::max<std::uint64_t>(startNs, 1u), std::memory_order_relaxed);
        blockCounters_.onBlock(static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(maxBlock_), bypassed);
    }

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
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        tickLocked(nowNs(), nowMs());
    }
    flushLog();
}

// 配对层的位置：先扫对端视图、做智能默认或重评，再走会合。决定落在会合这一拍之前，已 prepare 时同一拍就认领了，
// 同一线程上紧接着 tick 的下一个实例扫视图时就看得见这次认领，两个实例不会选中同一个空组。配对状态在会合之后折
void InstanceHost::tickLocked(std::uint64_t ns, std::uint64_t ms)
{
    const bool nonrealtime = processor_.isNonRealtime();
    if (nonrealtime != lastNonrealtime_)
    {
        rendezvous_->setNonrealtime(nonrealtime);
        lastNonrealtime_ = nonrealtime;
        nonrealtimeSeen_ = nonrealtimeSeen_ || nonrealtime;
    }
    if (pairingOn())
    {
        refreshView(ms);
        smartTick(ms);
    }
    {
        // 打开段只发生在会合的 tick 里；这一拍里后端在本线程上报出的错误码记给本实例
        const IpcErrorCapture capture(ipcError_);
        rendezvous_->tick(ms);
    }
    if (config_.pairing)
        foldPairState(ns, ms);
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
    noteGroupIntent(intent);
    setGroupLocked(group, intent, nowMs());
}

// 任何改组都结束等待与「组全满」；来源跟着意图走，手选与一键修复记为本会话手选过
void InstanceHost::noteGroupIntent(ipc::ClaimIntent intent) noexcept
{
    smartWaiting_ = false;
    allGroupsFull_ = false;
    switch (intent)
    {
    case ipc::ClaimIntent::kSmart:
        groupOrigin_ = diag::DiagOrigin::kSmart;
        break;
    case ipc::ClaimIntent::kRestore:
        groupOrigin_ = diag::DiagOrigin::kRestore;
        break;
    case ipc::ClaimIntent::kManual:
    case ipc::ClaimIntent::kManualConfirmed:
        groupOrigin_ = diag::DiagOrigin::kManual;
        userPicked_ = true;
        break;
    case ipc::ClaimIntent::kConflictRetry:
        break;
    }
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

// ---- 配对层：工程恢复、转正、标签 ----

// 智能认领写胶水在本类的锁内，begin 也要取这把锁：一次智能认领要么在 begin 之前做完（随后胶水的全量应用整份换掉
// 设置，组号以工程为准），要么等到 end 之后（那时 restoreGroup 已经记下组号定没定）。胶水的全量应用与 restoreGroup
// 之间隔着锁外的一段，没有这一对，tick 恰好在那一段里认领，就会把设置写成智能组、与会合拿到的工程组号不一致
void InstanceHost::beginStateLoad()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stateLoads_;
    if (prepareCount_ == 0)
        setStateBeforePrepare_ = true;
}

void InstanceHost::endStateLoad()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stateLoads_ > 0)
        --stateLoads_;
}

void InstanceHost::restoreGroup(std::uint32_t group, bool decided)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    noteGroupIntent(ipc::ClaimIntent::kRestore);
    savedGroup_ = decided;
    if (!decided)
        smartDecided_ = false; // 这份 state 把组号放回未定：条件满足时重新决定，不让实例停在 0 上
    setGroupLocked(group, ipc::ClaimIntent::kRestore, nowMs());
}

void InstanceHost::setSmartGroupSink(std::function<void(std::uint32_t group)> sink)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    smartSink_ = std::move(sink);
}

// 同步转正，不交给下一拍：复制轨道时宿主先对原实例 getState、再建复制体并 setState。转正在 getState 返回之前做完，
// 原实例的认领就已经是正式的，复制体的工程恢复抢不动它；推到 tick 会留下最长一拍的窗口。这里只碰会合（整字 CAS 与
// 绑定发布），二者本来就由本类的锁串起，可在任何非实时线程上做；不碰 MessageManager、定时器或编辑器
void InstanceHost::confirmClaim()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!pairingOn())
        return;
    const IpcErrorCapture capture(ipcError_);
    rendezvous_->confirmClaim(nowMs());
}

// 名字是 UTF-8 的完整轨道名，进会合的标签（槽里只有它的散列与显示前缀）；诊断与日志只记「知道 / 不知道」。
// 名字没给或是空串：从没知道过就不写标签（槽里全 0 就是未知；空串的散列不是 0，写了就成了一个「已知的名字」）；
// 知道过之后又说不知道，留着上一份（会合没有发布「未知」的接口）。颜色没给（AU 只给名字）按未知写 0
void InstanceHost::setTrackProperties(const juce::AudioProcessor::TrackProperties& properties)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!config_.pairing)
            return;
        const bool named = properties.name.has_value() && properties.name->isNotEmpty();
        const bool coloured = properties.colour.has_value();
        if (named)
        {
            const std::string utf8 = properties.name->toStdString();
            const std::uint32_t argb = coloured ? properties.colour->getARGB() : 0u;
            rendezvous_->setLabel(utf8, argb);
            nameHash_ = ipc::fnv1a32(utf8);
        }
        queueLog(juce::String("track properties: name ") + (named ? "known" : "unknown") + ", colour " +
                 (coloured ? "known" : "unknown"));
    }
    flushLog();
}

ipc::RepairPlan InstanceHost::planRepair() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!pairingOn() || rendezvous_->state() != ipc::RendezvousState::kConflict || !viewUsable())
        return {};
    // 冲突里本实例并不占着请求的那一组，修复按「要离开的组」把它排除
    const ipc::PairingSelf self{config_.role, ipc::currentProcessId(), nameHash_, rendezvous_->group()};
    const ipc::GroupViews groups = groupViews(nowMs());
    return isInput() ? ipc::repairInput(self, groups) : ipc::repairOutput(self, groups);
}

pairing::PairModel InstanceHost::pairState() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return pairModel_;
}

std::uint32_t InstanceHost::trackNameHash() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return nameHash_;
}

// ---- 配对层：对端视图 ----

// 会合段的只读映射：只开已存在的段（不建）、只读校验段头（绝不写）。段不存在时每拍再试（同一线程上前一个实例这一拍
// 刚建了段、认领了组，后一个实例这一拍就要看见，否则两个都会挑中同一个空组）；ABI 不同的段每个冲突重试间隔看一次
// （与会合同一个口径）；后端拒绝或名字不合法就不再试。映射上之后一直留着，析构时放下。
// 映射着的段不会被删：Windows 上句柄让它活着，POSIX 后端的只读映射同样持着段锁文件的共享锁
void InstanceHost::refreshView(std::uint64_t ms)
{
    if (view_.base != nullptr)
    {
        viewObserver_.scan(*viewSegment(), ms);
        readLabels();
        return;
    }
    if (viewPermanent_ || backend_ == nullptr || viewName_.empty())
        return;
    // 没到重试的点就不试；时钟倒退得比一个间隔还多时不卡住
    if (viewRetryMs_ != 0 && ms < viewRetryMs_ && viewRetryMs_ - ms <= ipc::RendezvousTiming::kConflictRetryMs)
        return;
    ipc::SegmentView view;
    ipc::InitResult result = ipc::InitResult::kFailed;
    {
        const IpcErrorCapture capture(ipcError_);
        result = backend_->openExistingReadOnly(viewName_, view);
        if (result == ipc::InitResult::kOk)
        {
            const auto* rv = static_cast<const ipc::RvSegment*>(view.base);
            result = backend_->checkHeaderReadOnly(view, rv->header.magic, rv->header.abi, kViewSpec);
            // 几何不对的段当作没有（会合自己也不用它）
            if (result == ipc::InitResult::kOk &&
                (rv->header.groups.load(std::memory_order_relaxed) != ipc::kGroupCount ||
                 rv->header.group_stride.load(std::memory_order_relaxed) != ipc::kGroupStride ||
                 rv->header.layout_bytes.load(std::memory_order_relaxed) < ipc::kRvBytes))
                result = ipc::InitResult::kBusy;
        }
    }
    viewResult_ = result;
    if (result == ipc::InitResult::kOk)
    {
        view_ = std::move(view);
        viewMisses_ = 0;
        viewObserver_.reset();
        labels_ = {};
        viewObserver_.scan(*viewSegment(), ms);
        readLabels();
        return;
    }
    if (view.base != nullptr)
        backend_->unmap(view);
    viewMisses_ = result == ipc::InitResult::kFailed ? viewMisses_ + 1u : 0u;
    viewPermanent_ = result == ipc::InitResult::kFailedDenied || result == ipc::InitResult::kFailedBadName;
    viewRetryMs_ = result == ipc::InitResult::kAbiMismatch ? ms + ipc::RendezvousTiming::kConflictRetryMs : 0u;
}

// 标签是跨进程的不可信数据：连续两次读到同一份才信它（散列只拿来比较两端相同与否）；读不稳的一拍留着上一次的，
// 槽换了主人（ctl 变了）又读不稳就当未知
void InstanceHost::readLabels() noexcept
{
    const ipc::RvSegment* rv = viewSegment();
    if (rv == nullptr)
        return;
    for (std::uint32_t group = 1; group <= ipc::kGroupCount; ++group)
    {
        for (const ipc::Role role : {ipc::Role::kInput, ipc::Role::kOutput})
        {
            LabelCache& cache = labels_[group - 1u][roleIndex(role)];
            const ipc::SlotSample& sample = viewObserver_.sample(group, role);
            if (!sample.stable)
                continue;
            if (ipc::ctlPhase(sample.ctl) == ipc::kCtlPhaseFree)
            {
                cache = LabelCache{};
                continue;
            }
            const ipc::RoleSlot& slot = ipc::roleSlot(*rv, group, role);
            ipc::LabelPayload first;
            ipc::LabelPayload second;
            if (ipc::readLabel(slot, first) && ipc::readLabel(slot, second) && first == second)
            {
                cache.ctl = sample.ctl;
                cache.known = true;
                cache.label = first;
            }
            else if (cache.ctl != sample.ctl)
            {
                cache = LabelCache{};
                cache.ctl = sample.ctl;
            }
        }
    }
}

// 能拿来做决定的快照：映射着，或者已经连着两拍确认段不存在（没有任何认领者，全空）。
// 只打不开一次不算：POSIX 后端在生命周期锁正被别人拿着时同样返回 kFailed
bool InstanceHost::viewUsable() const noexcept
{
    return view_.base != nullptr || (!viewPermanent_ && viewResult_ == ipc::InitResult::kFailed && viewMisses_ >= 2u);
}

const ipc::RvSegment* InstanceHost::viewSegment() const noexcept
{
    return static_cast<const ipc::RvSegment*>(view_.base);
}

// 一个端点槽的快照。不稳定的样本按「正在认领」算：既不是空位也不是活端，这一拍谁都不拿它当空组
ipc::EndView InstanceHost::endView(std::uint32_t group, ipc::Role role, std::uint64_t ms) const noexcept
{
    ipc::EndView view;
    if (view_.base == nullptr)
        return view;
    const ipc::SlotSample& sample = viewObserver_.sample(group, role);
    if (!sample.stable)
    {
        view.phase = ipc::kCtlPhaseClaiming;
        return view;
    }
    view.phase = ipc::ctlPhase(sample.ctl);
    view.fresh = ipc::ageMs(ms, sample.heartbeatMs) < ipc::RendezvousTiming::kLostHeartbeatMs;
    view.pid = sample.pid;
    view.claimMs = sample.claimMs;
    view.nameHash = labelHash(group, role);
    view.neverPaired = (sample.cfgFlags & ipc::kCfgNeverPaired) != 0u;
    return view;
}

ipc::GroupViews InstanceHost::groupViews(std::uint64_t ms) const noexcept
{
    ipc::GroupViews groups{};
    for (std::uint32_t group = 1; group <= ipc::kGroupCount; ++group)
    {
        groups[group - 1u].input = endView(group, ipc::Role::kInput, ms);
        groups[group - 1u].output = endView(group, ipc::Role::kOutput, ms);
    }
    return groups;
}

std::uint32_t InstanceHost::labelHash(std::uint32_t group, ipc::Role role) const noexcept
{
    const LabelCache& cache = labels_[group - 1u][roleIndex(role)];
    return cache.known && cache.label.nameLen != 0u ? cache.label.nameHash : 0u;
}

// 显示前缀：不是合法 UTF-8 就不要（对端可能是别的版本，或者读到的是混写的一份）
std::string InstanceHost::labelName(std::uint32_t group, ipc::Role role) const
{
    const LabelCache& cache = labels_[group - 1u][roleIndex(role)];
    if (!cache.known || cache.label.nameLen == 0u)
        return {};
    std::string name = ipc::labelDisplayName(cache.label);
    if (!juce::CharPointer_UTF8::isValidString(name.data(), static_cast<int>(name.size())))
        return {};
    return name;
}

// ---- 配对层：智能默认与重评 ----

void InstanceHost::smartTick(std::uint64_t ms)
{
    if ((smartDecided_ && !smartWaiting_) || stateLoads_ > 0)
        return;
    // 等待中每 kPairingReevalMs 重评一次；时钟倒退得比一个间隔还多时不卡住
    if (smartWaiting_ && ms < nextReevalMs_ && nextReevalMs_ - ms <= ipc::kPairingReevalMs)
        return;
    ipc::PairingTrigger trigger;
    trigger.prepared = firstPrepared_;
    trigger.realtimeBlocks = realtimeBlocks_.load(std::memory_order_relaxed);
    trigger.sincePrepareMs = firstPrepared_ && ms > firstPrepareMs_ ? ms - firstPrepareMs_ : 0u;
    trigger.hasSavedGroup = savedGroup_;
    trigger.userPicked = userPicked_;
    if (!ipc::pairingTriggered(trigger) || !viewUsable())
        return;

    ipc::PairingSelf self{config_.role, ipc::currentProcessId(), nameHash_, 0u};
    if (smartWaiting_)
    {
        nextReevalMs_ = ms + ipc::kPairingReevalMs;
        // 冲突里不重评（复制轨道一类的冲突只给一键修复）；
        // 还没持有槽（在开段、在退避、没 prepare）就不知道自己在等哪一组
        if (rendezvous_->state() == ipc::RendezvousState::kConflict || rendezvous_->myCtl() == 0u)
            return;
        self.heldGroup = rendezvous_->group();
    }
    const ipc::GroupViews groups = groupViews(ms);
    const ipc::PairingDecision decision = isInput() ? ipc::decideInput(self, groups) : ipc::decideOutput(self, groups);

    if (!smartDecided_)
    {
        smartDecided_ = true;
        if (decision.group == 0u)
        {
            allGroupsFull_ = true; // 不认领，配对状态显示未分组
            queueLog("pairing: smart default found no free group");
            return;
        }
        claimSmart(decision.group, ms);
        smartWaiting_ = decision.waiting;
        nextReevalMs_ = ms + ipc::kPairingReevalMs;
        queueLog(juce::String("pairing: smart default chose group ") + juce::String(diag::groupLetter(decision.group)) +
                 " (" + reasonName(decision.reason) + (decision.waiting ? ", waiting for input" : "") + ")");
        return;
    }
    // 重评：拿到非 waiting 的决定（含留在原组）就停；结果是别的组才迁过去；没有可去的组时不离开原组，下一秒再看
    smartWaiting_ = decision.waiting || decision.group == 0u;
    if (decision.group != 0u && decision.group != self.heldGroup)
    {
        claimSmart(decision.group, ms);
        smartWaiting_ = decision.waiting;
        queueLog(juce::String("pairing: re-evaluated, moved to group ") +
                 juce::String(diag::groupLetter(decision.group)) + " (" + reasonName(decision.reason) + ")");
    }
}

// 智能认领带 provisional（会合的 kSmart）。组号经处理器写进胶水的设置，来源记 Smart，不定型、不标脏
void InstanceHost::claimSmart(std::uint32_t group, std::uint64_t ms)
{
    noteGroupIntent(ipc::ClaimIntent::kSmart);
    setGroupLocked(group, ipc::ClaimIntent::kSmart, ms);
    if (smartSink_)
        smartSink_(group);
}

// ---- 配对层：配对状态与诊断 ----

void InstanceHost::foldPairState(std::uint64_t ns, std::uint64_t ms)
{
    pairing::PairInputs in;
    in.role = config_.role;
    in.selfPid = ipc::currentProcessId();
    in.selfVersion = version_;
    in.selfSampleRate = prepareCount_ > 0u ? loop::publishedSampleRate(sampleRate_) : 0u;
    in.selfChannels = channels_;
    in.selfNameHash = nameHash_;
    const ipc::RendezvousState state = rendezvous_->state();
    in.group = rendezvous_->group();
    in.validator = config_.validationHost || state == ipc::RendezvousState::kValidator;
    in.ipcUnavailable = state == ipc::RendezvousState::kIpcUnavailable || viewPermanent_;
    in.abiMismatch = state == ipc::RendezvousState::kAbiMismatch || viewResult_ == ipc::InitResult::kAbiMismatch;
    in.conflict = state == ipc::RendezvousState::kConflict;
    in.allGroupsFull = allGroupsFull_;
    in.realtimeAudioSeen = realtimeBlocks_.load(std::memory_order_relaxed) > 0u;
    in.selfBypassed = lastBypassed_.load(std::memory_order_relaxed);
    in.loopSilent = loopSilent(ns);
    if (in.group >= 1u && in.group <= ipc::kGroupCount && view_.base != nullptr)
    {
        const ipc::Role peerRole = ipc::peerOf(config_.role);
        const ipc::SlotSample& peer = viewObserver_.sample(in.group, peerRole);
        in.peer.present = peer.stable && ipc::ctlPhase(peer.ctl) == ipc::kCtlPhaseActive;
        if (in.peer.present)
        {
            in.peer.lost = rendezvous_->peerLost(ms);
            in.peer.pid = peer.pid;
            in.peer.version = peer.version;
            in.peer.sampleRate = peer.sampleRate;
            in.peer.channels = peer.channels;
            in.peer.nameHash = labelHash(in.group, peerRole);
            in.peer.name = labelName(in.group, peerRole);
            const LabelCache& label = labels_[in.group - 1u][roleIndex(peerRole)];
            in.peer.colourArgb = label.known ? label.label.colourArgb : 0u;
        }
        if (in.conflict)
            in.occupantName = labelName(in.group, config_.role);
    }
    pairModel_ = pairing::buildPairModel(in, groupViews(ms));
}

// 回送静音（PairStateModel 的 loopSilent）。Input：本端被宿主旁路（Input 没有自己的旁路状态，并进这里）、环路关、
// 安全链跳闸或断档、对端 Output 旁路；Output：对端 Input 旁路、断档或跳闸。两端都算「宿主停了音频回调超过 250 ms」。
// Output 看不到 Input 的环路开关（槽里没有这一位）
bool InstanceHost::loopSilent(std::uint64_t ns) const noexcept
{
    const std::uint64_t last = lastAudioNs_.load(std::memory_order_relaxed);
    const bool stopped = last != 0u && ns > last && ns - last > kSuspendFlushNs;
    std::uint32_t peerFlags = 0;
    bool peerTripped = false;
    const std::uint32_t group = rendezvous_->group();
    if (group >= 1u && group <= ipc::kGroupCount && view_.base != nullptr)
    {
        const ipc::Role peerRole = ipc::peerOf(config_.role);
        const ipc::SlotSample& sample = viewObserver_.sample(group, peerRole);
        if (sample.stable && ipc::ctlPhase(sample.ctl) == ipc::kCtlPhaseActive)
        {
            peerFlags = sample.rtFlags;
            const std::uint32_t safety =
                ipc::roleSlot(*viewSegment(), group, peerRole).safety_state.load(std::memory_order_relaxed);
            peerTripped = peerRole == ipc::Role::kInput && (safety & 0xffu) == ipc::kSafetyTripped;
        }
    }
    if (isInput())
    {
        const engine::SafetyState safety =
            engine::PackedSafety::state(input_.meters().safety.load(std::memory_order_relaxed));
        return lastBypassed_.load(std::memory_order_relaxed) || !lastLoopOn_.load(std::memory_order_relaxed) ||
               safety == engine::SafetyState::Tripped || safety == engine::SafetyState::Stalled ||
               (peerFlags & ipc::kRtBypassed) != 0u || stopped;
    }
    return (peerFlags & (ipc::kRtBypassed | ipc::kRtStalled)) != 0u || peerTripped || stopped;
}

// 诊断的各项，逐项来源见 design：会合与对端视图（锁内）、prepare 的账（锁内）、块计数与引擎读出（音频线程发布的
// 原子量）、宿主与系统的固定名字。原始轨道名不进来，只有本端的散列与两端的比较结果
void InstanceHost::fillDiagnostics(diag::DiagInputs& in) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t ms = nowMs();
    in.role = isInput() ? diag::DiagRole::kInput : diag::DiagRole::kOutput;
    in.product = processor_.getName().toStdString();
    in.version = build::kVersion;
    in.build = build::buildLabel();
    in.format = juce::AudioProcessor::getWrapperTypeDescription(processor_.wrapperType);
    in.host =
        config_.hostDescription != nullptr ? config_.hostDescription : juce::PluginHostType().getHostDescription();
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
    in.origin = groupOrigin_;
    in.pid = ipc::currentProcessId();
    in.pairState = pairing::idOf(pairModel_.state);
    in.trackHash = nameHash_;

    const bool inGroup = in.group >= 1u && in.group <= ipc::kGroupCount && view_.base != nullptr;
    if (inGroup)
    {
        const ipc::Role peerRole = ipc::peerOf(config_.role);
        const ipc::SlotSample& peer = viewObserver_.sample(in.group, peerRole);
        const std::uint32_t phase = ipc::ctlPhase(peer.ctl);
        in.peer.present = peer.stable && (phase == ipc::kCtlPhaseActive || phase == ipc::kCtlPhaseDormant);
        in.peer.role = isInput() ? diag::DiagRole::kOutput : diag::DiagRole::kInput;
        in.peer.active = phase == ipc::kCtlPhaseActive;
        in.peer.samePid = peer.pid == in.pid;
        in.peer.version = peer.version;
        in.peer.sampleRate = peer.sampleRate;
        in.peer.maxBlock = peer.maxBlock;
        in.peer.channels = peer.channels;
        in.peer.name = relationOf(pairing::checkNames(nameHash_, labelHash(in.group, peerRole)));
        in.peer.heartbeatAgeMs = ipc::ageMs(ms, peer.heartbeatMs);
    }

    in.prepare.count = prepareCount_;
    in.prepare.sampleRate = prepareCount_ > 0u ? loop::publishedSampleRate(sampleRate_) : 0u;
    in.prepare.maxBlock = static_cast<std::uint32_t>(maxBlock_);
    in.prepare.nonrealtimeSeen = nonrealtimeSeen_;
    in.prepare.setStateBeforePrepare = setStateBeforePrepare_;
    in.prepare.resetCalls = resets_.load(std::memory_order_relaxed);
    // 本次 prepare 以来的最大块长在自己的槽里（音频线程写）：持有槽时才有
    if (inGroup && rendezvous_->myCtl() != 0u)
        in.prepare.maxNSeen =
            ipc::roleSlot(*viewSegment(), in.group, config_.role).max_n_seen.load(std::memory_order_relaxed);
    in.blocks = blockCounters_.snapshot();

    if (isInput())
    {
        const loop::LoopStatusFields loop = input_.loopStatus();
        in.loop.dUserMs = loop.delayUserMs;
        in.loop.dEff = toU32(loop.delayEffective);
        in.loop.dMin = toU32(loop.delayMin);
        in.loop.mode = loop.delayMode != 0u ? diag::DiagDelayMode::kSync : diag::DiagDelayMode::kTime;
        in.loop.bpm = loop.bpmCenti / 100.0;
        in.loop.bpmSource = loop.bpmSource == static_cast<std::uint8_t>(loop::BpmSource::kHost)
                                ? diag::DiagBpmSource::kHost
                                : diag::DiagBpmSource::kNone;
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
        in.order.hostNsValid = loop.hostNsValid != 0u;
        const engine::SafetyDiag safety = input_.safetyDiag();
        in.safety.state = safetyName(safety.state);
        in.safety.trips = safety.trips;
        in.safety.tailPeakMaxDb = safety.tailPeakMaxDb;
        in.safety.limiterGrMaxDb = safety.limiterGrMaxDb;
        in.safety.nonfiniteRing = safety.nonfiniteRing;
        in.safety.clampRing = safety.clampRing;
    }
}

// ---- 配对层：日志（锁内攒、锁外写；文件只在消息线程写，见 PlatformLog.h） ----

void InstanceHost::queueLog(juce::String line)
{
    constexpr std::size_t kMaxPending = 16; // 一拍里最多就几行；万一没人取，不让它无界增长
    if (pendingLog_.size() < kMaxPending)
        pendingLog_.push_back(juce::String(isInput() ? "input: " : "output: ") + line);
}

void InstanceHost::flushLog()
{
    std::vector<juce::String> lines;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        lines.swap(pendingLog_);
    }
    for (const juce::String& line : lines)
    {
        if (config_.logLine != nullptr)
            config_.logLine(line);
        else
            platformlog::write(line);
    }
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
