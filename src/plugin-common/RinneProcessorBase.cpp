// SPDX-License-Identifier: GPL-3.0-or-later

#include "RinneProcessorBase.h"

#include <limits>
#include <utility>

#include "PlatformLog.h"
#include "ProbeHost.h"
#include "UiDefaultsStore.h"
#include "engine/EngineParams.h"
#include "util/RinneVersion.h"

namespace rinne
{

// 外壳的 TailMode、state 里的 tail_mode、引擎的 TailMode 三份数值必须逐个相同（state 是线格式，只能追加）
static_assert(static_cast<int>(RinneProcessorBase::TailMode::Auto) == static_cast<int>(state::TailMode::Auto));
static_assert(static_cast<int>(RinneProcessorBase::TailMode::Infinite) == static_cast<int>(state::TailMode::Infinite));
static_assert(static_cast<int>(RinneProcessorBase::TailMode::ThirtySeconds) ==
              static_cast<int>(state::TailMode::ThirtySeconds));
static_assert(static_cast<int>(RinneProcessorBase::TailMode::Auto) == static_cast<int>(engine::TailMode::kAuto));
static_assert(static_cast<int>(RinneProcessorBase::TailMode::Infinite) ==
              static_cast<int>(engine::TailMode::kInfinite));
static_assert(static_cast<int>(RinneProcessorBase::TailMode::ThirtySeconds) ==
              static_cast<int>(engine::TailMode::kThirtySeconds));

RinneProcessorBase::RinneProcessorBase()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      validationHost_(detectValidationHost())
{
    setLatencySamples(0);
}

// 子类的成员（参数层、胶水）先于这里析构；InstanceHost 的定时器回调只碰它自己的成员，不碰子类
RinneProcessorBase::~RinneProcessorBase() = default;

bool RinneProcessorBase::detectValidationHost()
{
    const juce::PluginHostType host;
    return host.isPluginval() || host.isAUVal() || host.isAppleInfoHelper() || host.isVBVSTScanner();
}

void RinneProcessorBase::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    host_->prepare(sampleRate, maximumExpectedSamplesPerBlock);
}

void RinneProcessorBase::releaseResources()
{
    host_->release();
}

void RinneProcessorBase::reset()
{
    host_->reset();
}

// JUCE 规定只在消息线程调。正式构建写进本端的标签（InstanceHost），探针构建照旧转给探针
void RinneProcessorBase::updateTrackProperties(const TrackProperties& properties)
{
    if constexpr (build::kProbeBuild)
        static_cast<probe::ProbeHost&>(*host_).trackProperties(properties);
    else
        host_->setTrackProperties(properties);
}

bool RinneProcessorBase::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

double RinneProcessorBase::getTailLengthSeconds() const
{
    switch (getTailMode())
    {
    case TailMode::Infinite:
        return std::numeric_limits<double>::infinity();
    case TailMode::ThirtySeconds:
        return 30.0;
    case TailMode::Auto:
        break;
    }
    // AU 宿主拿无限长的 tail 没有统一的处理，自动模式给 30 秒；VST3 有专门的无限 tail 取值
    const bool au = wrapperType == wrapperType_AudioUnit || wrapperType == wrapperType_AudioUnitv3;
    return au ? 30.0 : std::numeric_limits<double>::infinity();
}

void RinneProcessorBase::setTailMode(TailMode mode) noexcept
{
    tailMode_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

RinneProcessorBase::TailMode RinneProcessorBase::getTailMode() const noexcept
{
    return static_cast<TailMode>(tailMode_.load(std::memory_order_relaxed));
}

const juce::String RinneProcessorBase::getName() const
{
    return JucePlugin_Name;
}

// 探针构建存取探针 state。正式构建里两个子类经各自的胶水重写，这两个只在子类没有接线时兜底：存出空块、读入时忽略
void RinneProcessorBase::getStateInformation(juce::MemoryBlock& destData)
{
    if constexpr (build::kProbeBuild)
        static_cast<probe::ProbeHost&>(*host_).getState(destData);
    else
        destData.reset();
}

void RinneProcessorBase::setStateInformation(const void* data, int sizeInBytes)
{
    if constexpr (build::kProbeBuild)
        static_cast<probe::ProbeHost&>(*host_).setState(data, sizeInBytes);
    else
        juce::ignoreUnused(data, sizeInBytes);
}

std::int64_t RinneProcessorBase::getProcessedSamples() const noexcept
{
    return processedSamples_.load(std::memory_order_relaxed);
}

bool RinneProcessorBase::beginBlock(int numSamples) noexcept
{
    if (numSamples <= 0)
        return false;
    // 只有音频线程写，读一次再写回就够了，不必用读改写
    processedSamples_.store(processedSamples_.load(std::memory_order_relaxed) + numSamples, std::memory_order_relaxed);
    return true;
}

void RinneProcessorBase::attachHost(Side side, const InstanceHostConfig& config)
{
    InstanceHostConfig wiring = config;
    wiring.role = side == Side::Input ? ipc::Role::kInput : ipc::Role::kOutput;
    wiring.validationHost = validationHost_;
    wiring.pairing = !build::kProbeBuild;
    role_ = wiring.role;
    installLog_ = config.installLog;
    copyToClipboard_ = config.copyToClipboard;
    if constexpr (build::kProbeBuild)
    {
        probe::ProbeHostConfig probeConfig;
        static_cast<InstanceHostConfig&>(probeConfig) = wiring;
        host_.reset(new probe::ProbeHost(*this, std::move(probeConfig)));
    }
    else
    {
        // 日志先于 InstanceHost：它的构造把 IPC 诊断的转发出口装在最上面，转发给日志的出口，链不断
        if (!validationHost_ && uidefaults::diagLogEnabled())
            installDiagLog();
        host_ = std::make_unique<InstanceHost>(*this, wiring);
    }
}

const char* RinneProcessorBase::roleName() const noexcept
{
    return role_ == ipc::Role::kInput ? "input" : "output";
}

// 每个插件二进制只装一次：已经装过（同一宿主里的另一个实例先装了）时 install 返回假，什么都不用做
void RinneProcessorBase::installDiagLog()
{
    const bool installed = installLog_ != nullptr ? installLog_(roleName()) : platformlog::install(roleName());
    if (installed && host_ != nullptr)
        InstanceHost::installIpcDiagSink();
}

void RinneProcessorBase::unwirePairing()
{
    host_->setSmartGroupSink({});
}

pairing::PairModel RinneProcessorBase::pairState() const
{
    return host_->pairState();
}

// 只写本机的全局设置，不进工程、不定型。探针构建有自己的日志：只写设置，不装文件日志
void RinneProcessorBase::setDiagLogEnabled(bool enabled)
{
    uidefaults::setDiagLogEnabled(enabled);
    if constexpr (!build::kProbeBuild)
    {
        if (enabled && !validationHost_)
            installDiagLog();
    }
}

std::string RinneProcessorBase::diagnosticsWith(const diag::DiagState& stateSection) const
{
    diag::DiagInputs in;
    host_->fillDiagnostics(in);
    in.state = stateSection;
    return diag::buildDiagReport(in);
}

void RinneProcessorBase::copyText(const std::string& text) const
{
    const juce::String copied = juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
    if (copyToClipboard_ != nullptr)
        copyToClipboard_(copied);
    else
        juce::SystemClipboard::copyTextToClipboard(copied);
}

diag::DiagLoadStatus RinneProcessorBase::loadStatusOf(const std::optional<state::StateLoadStatus>& status) noexcept
{
    if (!status.has_value())
        return diag::DiagLoadStatus::kNone;
    switch (*status)
    {
    case state::StateLoadStatus::Ok:
        return diag::DiagLoadStatus::kOk;
    case state::StateLoadStatus::Migrated:
        return diag::DiagLoadStatus::kMigrated;
    case state::StateLoadStatus::Empty:
        return diag::DiagLoadStatus::kEmpty;
    case state::StateLoadStatus::IgnoredProbe:
        return diag::DiagLoadStatus::kIgnoredProbe;
    case state::StateLoadStatus::RejectedNewer:
        return diag::DiagLoadStatus::kRejectedNewer;
    case state::StateLoadStatus::Corrupt:
        return diag::DiagLoadStatus::kCorrupt;
    }
    return diag::DiagLoadStatus::kNone;
}

// 组号已定 = 非 0，或来源是用户选的（用户明确离组也算表过态）：已定的不再做智能默认
void RinneProcessorBase::restoreConfigs(const state::Configs& configs)
{
    const bool decided = configs.group != 0 || configs.groupOrigin == state::GroupOrigin::User;
    host_->restoreGroup(configs.group, decided);
    switch (configs.tailMode)
    {
    case state::TailMode::Infinite:
        setTailMode(TailMode::Infinite);
        return;
    case state::TailMode::ThirtySeconds:
        setTailMode(TailMode::ThirtySeconds);
        return;
    case state::TailMode::Auto:
        break;
    }
    // 解码已把越界值换成默认值；这里再兜一次，不认识的一律按自动
    setTailMode(TailMode::Auto);
}

void RinneProcessorBase::processProbe(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    if constexpr (build::kProbeBuild)
        static_cast<probe::ProbeHost&>(*host_).process(buffer, bypassed);
    else
        juce::ignoreUnused(buffer, bypassed);
}

juce::AudioProcessorEditor* RinneProcessorBase::createProbeEditor()
{
    if constexpr (build::kProbeBuild)
        return static_cast<probe::ProbeHost&>(*host_).createEditor();
    else
        return nullptr;
}

} // namespace rinne
