// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 原生桥的纯逻辑，分层与事件规则见 OutputBridge.h。

#include "OutputBridge.h"

#include <utility>

#include "UiDefaultsStore.h"
#include "diag/DiagReport.h"
#include "engine/OutputEngine.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Rendezvous.h"
#include "loop/OrderEvidence.h"
#include "pairing/PairStateModel.h"
#include "state/StateCodec.h"
#include "state/StateMigration.h"
#include "util/RinneVersion.h"

namespace rinne::output
{

namespace
{
namespace common = rinne::bridge;

// Output 专用函数的失败原因（docs/UI_BRIDGE.md 的 output 节）
constexpr const char* kReasonNotConflict = "not-conflict";
constexpr const char* kReasonNoFreeGroup = "no-free-group";
constexpr const char* kReasonNeedsConfirm = "needs-confirm";

// rinne.error 的 code
constexpr const char* kErrorStateEncode = "stateEncode";

juce::String fromUtf8(const std::string& text)
{
    return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
}

// 颜色写成 #rrggbb（页面的颜色清洗认这个形状）。透明度不带：宿主给的轨道色都是不透明的
juce::String colourText(std::uint32_t argb)
{
    return "#" + juce::String::toHexString(static_cast<int>(argb & 0xffffffu)).paddedLeft('0', 6);
}

const char* originName(diag::DiagOrigin origin)
{
    switch (origin)
    {
    case diag::DiagOrigin::kManual:
        return "manual";
    case diag::DiagOrigin::kRestore:
        return "project";
    case diag::DiagOrigin::kSmart:
    case diag::DiagOrigin::kNone:
        break;
    }
    // 还没做过决定（等智能默认）也按「自动」显示：组是插件替用户找的，不是用户选的
    return "auto";
}

const char* verifyName(pairing::NameCheck check)
{
    switch (check)
    {
    case pairing::NameCheck::kSame:
        return "match";
    case pairing::NameCheck::kDifferent:
        return "diff";
    case pairing::NameCheck::kUnknown:
        break;
    }
    return "unknown";
}

const char* tailModeName(state::TailMode mode)
{
    switch (mode)
    {
    case state::TailMode::Infinite:
        return "infinite";
    case state::TailMode::ThirtySeconds:
        return "30s";
    case state::TailMode::Auto:
        break;
    }
    return "auto";
}

std::optional<state::TailMode> tailModeFromName(const juce::var& v)
{
    if (!v.isString())
        return std::nullopt;
    const juce::String name = v.toString();
    if (name == "auto")
        return state::TailMode::Auto;
    if (name == "infinite")
        return state::TailMode::Infinite;
    if (name == "30s")
        return state::TailMode::ThirtySeconds;
    return std::nullopt;
}

juce::String abiText(std::uint32_t abi)
{
    return "ABI " + juce::String(static_cast<juce::int64>(abi));
}

bool isLowerHex(const juce::String& text)
{
    return text.containsOnly("0123456789abcdef");
}

juce::String archName()
{
#if JUCE_WINDOWS
    const char* os = "Windows";
#elif JUCE_MAC
    const char* os = "macOS";
#else
    const char* os = "Linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* cpu = "arm64";
#else
    const char* cpu = "x64";
#endif
    return juce::String(os) + " " + cpu;
}

diag::DiagInputs diagFacts(const OutputProcessor& processor)
{
    diag::DiagInputs in;
    processor.instanceHost().fillDiagnostics(in);
    return in;
}
} // namespace

juce::Array<juce::var> buildNotices(const NoticeFacts& facts)
{
    juce::Array<juce::var> out;
    auto add = [&out](const char* id) -> juce::DynamicObject& {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", id);
        out.add(juce::var(o));
        return *o;
    };
    if (facts.ipcDenied)
        add("ipcSandbox");
    if (facts.heldNewerAbi != 0u)
    {
        // 工程里只有 state 头的 abi，没有写入方的版本串：两边都写 abi，单位一致
        auto& o = add("newerProject");
        o.setProperty("a", abiText(facts.heldNewerAbi));
        o.setProperty("b", abiText(state::kAbi));
    }
    if (facts.decodeFailed)
        add("stateDecodeFailed");
    if (facts.outputFirst)
        add("outOfOrder");
    return out;
}

juce::var buildInfo(const juce::String& buildLabel, juce::AudioProcessor::WrapperType wrapper)
{
    auto* o = new juce::DynamicObject();
    const int plus = buildLabel.lastIndexOfChar('+');
    const juce::String sha = plus >= 0 ? buildLabel.substring(plus + 1) : juce::String();
    if (sha.length() >= 7 && sha.length() <= 40 && isLowerHex(sha))
        o->setProperty("sha", sha);
    if (wrapper == juce::AudioProcessor::wrapperType_VST3)
        o->setProperty("fmt", "VST3");
    else if (wrapper == juce::AudioProcessor::wrapperType_AudioUnit)
        o->setProperty("fmt", "AU");
    o->setProperty("arch", archName());
    return juce::var(o);
}

// -----------------------------------------------------------------------------
// OutputBridge
// -----------------------------------------------------------------------------

OutputBridge::OutputBridge(OutputProcessor& processor, Hooks hooks) : processor_(processor), hooks_(std::move(hooks))
{
    // 编辑器打开之前的编码失败不补报：rinne.error 只报打开之后新发生的
    if (auto* glue = processor_.stateGlue())
        encodeFailuresSeen_ = glue->encodeFailures();
    lastSamples_ = processor_.getProcessedSamples();
    lastAdvanceMs_ = now();
}

std::uint64_t OutputBridge::now() const
{
    if (hooks_.nowMs)
        return hooks_.nowMs();
    return static_cast<std::uint64_t>(juce::Time::getMillisecondCounterHiRes());
}

juce::var OutputBridge::setRulesAck(const juce::Array<juce::var>& args)
{
    int version = 0;
    if (!common::parseRulesAckArg(args, version))
        return common::failResponse(common::Reason::BadVersion);
    uidefaults::setRulesAckVersion(version);
    forceState_ = true;
    return common::okResponse();
}

juce::var OutputBridge::setGuideSeen(const juce::Array<juce::var>& args)
{
    bool seen = false;
    if (!common::jsBoolArg(args, seen) || !seen)
        return common::failResponse(common::Reason::BadValue);
    uidefaults::setGuideSeenGlobalOutput(true);
    forceState_ = true;
    return common::okResponse();
}

juce::var OutputBridge::setGroup(const juce::Array<juce::var>& args)
{
    int group = 0;
    if (!common::parseGroupArg(args, group) || !processor_.selectGroup(group))
        return common::failResponse(common::Reason::BadGroup);
    // 用户自己选了组：之前等确认的接管不再作数
    takeover_.reset();
    return common::okResponseWith("group", common::groupLetter(static_cast<std::uint32_t>(group)));
}

juce::var OutputBridge::copyDiagnostics(const juce::Array<juce::var>& args, const UiContext& ui)
{
    const juce::String text =
        common::withUiDiagLine(processor_.diagnosticsText(), common::uiDiagLine(ui.webview, ui.uiScale, ui.lang));
    return common::respondCopyDiagnostics(args, text, hooks_.writeClipboard);
}

juce::var OutputBridge::openExternal(const juce::Array<juce::var>& args)
{
    return common::respondOpenExternal(args, hooks_.launchUrl);
}

juce::var OutputBridge::setDiagLog(const juce::Array<juce::var>& args)
{
    bool on = false;
    if (!common::jsBoolArg(args, on))
        return common::failResponse(common::Reason::BadValue);
    processor_.setDiagLogEnabled(on);
    forceState_ = true;
    return common::okResponseWith("value", on);
}

juce::var OutputBridge::resolveTakeover(const juce::Array<juce::var>& args)
{
    bool accept = false;
    if (!common::jsBoolArg(args, accept))
        return common::failResponse(common::Reason::BadValue);
    if (!takeover_.has_value())
        return common::failResponse(common::Reason::NoPending);
    const int group = static_cast<int>(*takeover_);
    takeover_.reset();
    if (!accept)
        return common::okResponseWith("accepted", false);
    const bool taken = hooks_.takeOverGroup ? hooks_.takeOverGroup(group) : processor_.takeOverGroup(group);
    // 没拿成：那一份已经接管不了，记录也已作废，回执与没有待接管的同一个原因
    if (!taken)
        return common::failResponse(common::Reason::NoPending);
    return common::okResponseWith("accepted", true);
}

juce::var OutputBridge::showLicense()
{
    if (!hooks_.showLicense || !hooks_.showLicense())
        return common::failResponse(common::Reason::Unavailable);
    return common::okResponse();
}

juce::var OutputBridge::repairPair()
{
    const auto outcome = processor_.repairGroup(false);
    if (outcome.applied)
    {
        takeover_.reset();
        return common::okResponseWith("group", common::groupLetter(outcome.group));
    }
    if (outcome.group != 0u && outcome.needsConfirm)
    {
        // 要去的组被休眠实例占着：先不动，等用户在确认条上点
        takeover_ = outcome.group;
        return common::failResponse(kReasonNeedsConfirm);
    }
    if (processor_.instanceHost().rendezvousState() != ipc::RendezvousState::kConflict)
        return common::failResponse(kReasonNotConflict);
    return common::failResponse(kReasonNoFreeGroup);
}

juce::var OutputBridge::setTailMode(const juce::Array<juce::var>& args)
{
    const auto mode = args.size() >= 1 ? tailModeFromName(args[0]) : std::nullopt;
    if (!mode.has_value() || !processor_.selectTailMode(*mode))
        return common::failResponse(common::Reason::BadValue);
    return common::okResponseWith("value", tailModeName(*mode));
}

NoticeFacts OutputBridge::noticeFacts() const
{
    NoticeFacts facts;
    const InstanceHost& host = processor_.instanceHost();
    // 会合的 lastResult 只在 tick 里写，tick 与这里都在消息线程。后端报系统拒绝是永久的：会合随即停在
    // kIpcUnavailable、不再重试，lastResult 也不会再被改写，所以只看它就够了
    facts.ipcDenied = host.rendezvous().lastResult() == ipc::InitResult::kFailedDenied;
    if (auto* glue = processor_.stateGlue())
    {
        facts.heldNewerAbi = glue->heldNewerAbi(); // 不在保留时为 0
        facts.decodeFailed = glue->lastLoadStatus() == state::StateLoadStatus::Corrupt;
    }
    facts.outputFirst = host.outputEngine().orderStatus().verdict == loop::OrderVerdict::kOutputFirst;
    return facts;
}

void OutputBridge::trackHostProgress(std::uint64_t nowMs)
{
    const std::int64_t samples = processor_.getProcessedSamples();
    if (samples != lastSamples_)
    {
        lastSamples_ = samples;
        lastAdvanceMs_ = nowMs;
    }
}

void OutputBridge::dropStaleTakeover()
{
    // 冲突自己解开了（占着的实例走了、重试拿到了槽），确认条就不该再挂着
    if (takeover_.has_value() && processor_.instanceHost().rendezvousState() != ipc::RendezvousState::kConflict)
        takeover_.reset();
}

juce::DynamicObject::Ptr OutputBridge::cheapStateFields()
{
    const std::uint64_t t = now();
    trackHostProgress(t);
    dropStaleTakeover();

    const diag::DiagInputs d = diagFacts(processor_);
    auto* glue = processor_.stateGlue();

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty("restored", glue != nullptr && glue->lastLoadStatus().has_value());
    o->setProperty("takeover", takeover_.has_value());
    if (takeover_.has_value())
        o->setProperty("takeoverGroup", common::groupLetter(*takeover_));
    const juce::String letter = common::groupLetter(d.group);
    o->setProperty("group", letter.isEmpty() ? juce::var() : juce::var(letter));
    o->setProperty("groupOrigin", originName(d.origin));
    if (d.prepare.sampleRate > 0u)
        o->setProperty("sampleRate", static_cast<int>(d.prepare.sampleRate));

    auto* send = new juce::DynamicObject();
    // 处理器只接受单声道与立体声两种布局（RinneProcessorBase::isBusesLayoutSupported）
    send->setProperty("channels", processor_.getMainBusNumInputChannels() == 1 ? "MONO" : "ST");
    if (d.prepare.sampleRate > 0u)
        send->setProperty("sampleRate", static_cast<int>(d.prepare.sampleRate));
    // 引擎的写状态是旁路的真源：配对态的 kBypassed 由同一个旁路位推出，而且会被冲突、过期等更高优先的态盖住
    send->setProperty("bypassed",
                      processor_.instanceHost().outputEngine().state() == engine::WriteState::kBypassedZeros);
    send->setProperty("hostPaused", lastSamples_ > 0 && t - lastAdvanceMs_ >= kHostPausedMs);
    o->setProperty("send", juce::var(send));

    const juce::Array<juce::var> notices = buildNotices(noticeFacts());
    if (!notices.isEmpty())
        o->setProperty("notices", notices);
    o->setProperty("build", buildInfo(build::buildLabel(), processor_.wrapperType));

    auto* cfg = new juce::DynamicObject();
    cfg->setProperty("tailMode", tailModeName(glue != nullptr ? glue->tailMode() : state::TailMode::Auto));
    o->setProperty("cfg", juce::var(cfg));
    return o;
}

void OutputBridge::addStateFields(juce::DynamicObject& snapshot)
{
    const auto fields = cheapStateFields();
    for (const auto& entry : fields->getProperties())
        snapshot.setProperty(entry.name, entry.value);
    snapshot.setProperty("inputGuideSeen", uidefaults::guideSeenGlobalInput());
}

juce::String OutputBridge::stateSignature(const UiContext& ui)
{
    return juce::JSON::toString(juce::var(cheapStateFields().get()), true) + "|" + ui.lang + "|" +
           juce::String(common::scaleForJson(ui.uiScale));
}

juce::var OutputBridge::pairBody() const
{
    const pairing::PairModel m = processor_.pairState();
    const diag::DiagInputs d = diagFacts(processor_);
    const auto& label = processor_.trackLabel();

    auto* peer = new juce::DynamicObject();
    peer->setProperty("name", m.peerName.empty() ? juce::var() : juce::var(fromUtf8(m.peerName)));
    peer->setProperty("color", m.peerColourArgb != 0u ? juce::var(colourText(m.peerColourArgb)) : juce::var());
    if (d.peer.present && (d.peer.channels == 1u || d.peer.channels == 2u))
        peer->setProperty("channels", static_cast<int>(d.peer.channels));
    peer->setProperty("version", m.peerVersion.empty() ? juce::var() : juce::var(fromUtf8(m.peerVersion)));
    peer->setProperty("holder", m.occupantName.empty() ? juce::var() : juce::var(fromUtf8(m.occupantName)));
    if (d.peer.present && d.peer.sampleRate > 0u)
        peer->setProperty("sampleRate", static_cast<int>(d.peer.sampleRate));

    auto* self = new juce::DynamicObject();
    self->setProperty("name", label.name.has_value() ? juce::var(*label.name) : juce::var());
    if (label.colour.has_value())
        self->setProperty("color", colourText(label.colour->getARGB()));

    auto* o = new juce::DynamicObject();
    o->setProperty("state", pairing::idOf(m.state));
    o->setProperty("groupMask", static_cast<int>(m.peerMask & 0xffu));
    o->setProperty("peer", juce::var(peer));
    o->setProperty("self", juce::var(self));
    o->setProperty("verify", verifyName(m.nameCheck));
    return juce::var(o);
}

juce::var OutputBridge::withSeq(const juce::var& body, std::uint32_t seq) const
{
    auto* o = new juce::DynamicObject();
    o->setProperty("seq", static_cast<int>(seq));
    if (auto* src = body.getDynamicObject())
        for (const auto& entry : src->getProperties())
            o->setProperty(entry.name, entry.value);
    return juce::var(o);
}

OutputBridge::Frames OutputBridge::poll(const UiContext& ui)
{
    Frames frames;
    const std::uint64_t t = now();

    const juce::String sig = stateSignature(ui);
    if (!stateSent_ || forceState_ || sig != lastStateSig_ || t - lastStateMs_ >= kHeartbeatMs)
    {
        frames.state = true;
        stateSent_ = true;
        forceState_ = false;
        lastStateSig_ = sig;
        lastStateMs_ = t;
    }

    const juce::var body = pairBody();
    const juce::String json = juce::JSON::toString(body, true);
    if (!pairSent_ || json != lastPairJson_ || t - lastPairMs_ >= kHeartbeatMs)
    {
        pairSent_ = true;
        lastPairJson_ = json;
        lastPairMs_ = t;
        frames.pair = withSeq(body, ++pairSeq_);
    }

    if (auto* glue = processor_.stateGlue())
    {
        const std::uint32_t failures = glue->encodeFailures();
        if (failures != encodeFailuresSeen_)
        {
            encodeFailuresSeen_ = failures;
            auto* e = new juce::DynamicObject();
            e->setProperty("seq", static_cast<int>(++errorSeq_));
            e->setProperty("code", kErrorStateEncode);
            frames.errors.add(juce::var(e));
        }
    }
    return frames;
}

juce::var OutputBridge::firstFrames(const UiContext& ui)
{
    const std::uint64_t t = now();
    lastStateSig_ = stateSignature(ui);
    lastStateMs_ = t;
    stateSent_ = true;
    forceState_ = false;

    const juce::var body = pairBody();
    lastPairJson_ = juce::JSON::toString(body, true);
    lastPairMs_ = t;
    pairSent_ = true;
    return withSeq(body, ++pairSeq_);
}

} // namespace rinne::output
