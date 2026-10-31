// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 原生桥的纯逻辑，分层与事件规则见 InputBridge.h。

#include "InputBridge.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "InputBridgeApi.h"
#include "UiDefaultsStore.h"
#include "diag/DiagReport.h"
#include "engine/InputEngine.h"
#include "engine/SafetyMachine.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "loop/LoopTiming.h"
#include "loop/OrderEvidence.h"
#include "pairing/PairStateModel.h"
#include "state/StateCodec.h"
#include "state/StateMigration.h"
#include "util/RinneVersion.h"

namespace rinne::input
{

namespace
{
namespace common = rinne::bridge;
namespace api = rinne::input::bridge;

// Input 专用函数的失败原因（docs/UI_BRIDGE.md 的 input 节）
constexpr const char* kReasonBadParam = "bad-param";
constexpr const char* kReasonBadId = "bad-id";
constexpr const char* kReasonNotCut = "not-cut";
constexpr const char* kReasonCooldown = "cooldown";
constexpr const char* kReasonNotConflict = "not-conflict";
constexpr const char* kReasonNoFreeGroup = "no-free-group";
constexpr const char* kReasonNeedsConfirm = "needs-confirm";

// rinne.error 的 code
constexpr const char* kErrorStateEncode = "stateEncode";

// 页面确认导出提示时给 dismissHint 的 ID：它不是「关掉一条提示」，是确认（见 InputBridge::dismissHint）
constexpr const char* kHintExportMismatch = "exportMismatch";

// 参数表的一行里桥要用的几项。kind 是表里的 F（连续）、B（开关）、C（档位）
enum class ParamKind
{
    F,
    B,
    C,
};

struct ParamSpec
{
    const char* id;
    ParamKind kind;
    double lo;
    double hi;
    double step;
};

constexpr ParamSpec kParamSpecs[] = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) \
    {#id, ParamKind::kind, lo, hi, step},
#include "ParamTable.h"
};

// 档位在桥上的字符串（delay_mode 的 time、delay_note 的 n4 这些）
struct ChoiceSpec
{
    const char* param;
    int index;
    const char* name;
};

constexpr ChoiceSpec kChoiceSpecs[] = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name)
#define RINNE_CHOICE(id, index, bridge, label) {#id, index, #bridge},
#include "ParamTable.h"
};

const ParamSpec* findParam(const juce::String& id)
{
    for (const auto& spec : kParamSpecs)
        if (id == spec.id)
            return &spec;
    return nullptr;
}

bool isPresetParam(const juce::String& id)
{
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
        if (id == presetParamId(i))
            return true;
    return false;
}

// 明文值换成桥上的值：开关是布尔、档位是字符串、连续量落到步进上再去掉 float 的尾数噪声。回执与回声用同一个换算，
// 页面拿回执的值与回声比较才对得上
juce::var bridgeValue(const ParamSpec& spec, float plain)
{
    switch (spec.kind)
    {
    case ParamKind::B:
        return juce::var(plain >= 0.5f);
    case ParamKind::C: {
        const int index = juce::roundToInt(plain);
        for (const auto& choice : kChoiceSpecs)
            if (choice.index == index && juce::String(spec.id) == choice.param)
                return juce::var(juce::String(choice.name));
        return {};
    }
    case ParamKind::F:
        break;
    }
    const double stepped = std::round(static_cast<double>(plain) / spec.step) * spec.step;
    return juce::var(std::round(stepped * 1.0e6) / 1.0e6);
}

// 桥上的值换成明文值；类型不对或档位不认识返回空。连续量夹进范围（回执报实际值），非有限值算不合法
std::optional<float> plainFromBridge(const ParamSpec& spec, const juce::var& value)
{
    switch (spec.kind)
    {
    case ParamKind::B:
        if (!value.isBool())
            return std::nullopt;
        return static_cast<bool>(value) ? 1.0f : 0.0f;
    case ParamKind::C:
        if (!value.isString())
            return std::nullopt;
        for (const auto& choice : kChoiceSpecs)
            if (juce::String(spec.id) == choice.param && value.toString() == choice.name)
                return static_cast<float>(choice.index);
        return std::nullopt;
    case ParamKind::F:
        break;
    }
    if (!(value.isDouble() || value.isInt() || value.isInt64()))
        return std::nullopt;
    const double number = static_cast<double>(value);
    if (!std::isfinite(number))
        return std::nullopt;
    return static_cast<float>(juce::jlimit(spec.lo, spec.hi, number));
}

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

const char* fbUnitName(state::FbUnit unit)
{
    return unit == state::FbUnit::Decibel ? "db" : "pct";
}

const char* delayUnitName(state::DelayUnit unit)
{
    return unit == state::DelayUnit::Samples ? "samples" : "ms";
}

const char* cutName(engine::TripReason reason)
{
    switch (reason)
    {
    case engine::TripReason::Panic:
        return "panic";
    case engine::TripReason::Nan:
        return "nan";
    case engine::TripReason::Huge:
        return "huge";
    case engine::TripReason::Runaway:
        return "runaway";
    case engine::TripReason::None:
        break;
    }
    // 跳闸了却没有原因不该出现；切断态不能写成「没切断」，按手动切断报
    return "panic";
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

diag::DiagInputs diagFacts(const InputProcessor& processor)
{
    diag::DiagInputs in;
    processor.instanceHost().fillDiagnostics(in);
    return in;
}

double roundTo(double value, double scale)
{
    return std::round(value * scale) / scale;
}

const FactoryPreset* factoryById(const PresetStore& store, const juce::String& id)
{
    for (const auto& preset : store.factoryPresets())
        if (preset.id == id)
            return &preset;
    return nullptr;
}

juce::var factoryNameOf(const FactoryPreset& preset)
{
    auto* name = new juce::DynamicObject();
    name->setProperty("zh", preset.zh);
    name->setProperty("en", preset.en);
    name->setProperty("fr", preset.fr);
    return juce::var(name);
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
    if (facts.outputFirst)
        add("outOfOrder");
    if (facts.unstable)
        add("unstable");
    if (facts.heldNewerAbi != 0u)
    {
        // 工程里只有 state 头的 abi，没有写入方的版本串：两边都写 abi，单位一致
        auto& o = add("newerProject");
        o.setProperty("a", abiText(facts.heldNewerAbi));
        o.setProperty("b", abiText(state::kAbi));
    }
    if (facts.decodeFailed)
        add("stateDecodeFailed");
    return out;
}

void applyLoopFacts(NoticeFacts& facts, const loop::LoopStatusFields& loop) noexcept
{
    facts.outputFirst = static_cast<loop::OrderVerdict>(loop.orderVerdict) == loop::OrderVerdict::kOutputFirst;
    facts.unstable = loop.unstable != 0u;
}

const char* silentCauseOf(const SilentFacts& facts) noexcept
{
    if (facts.hostBypass)
        return "hostBypass";
    if (facts.hostPaused)
        return "hostPaused";
    if (facts.peerBypass)
        return "peerBypass";
    if (facts.stalled)
        return "stalled";
    return nullptr;
}

bool peerBypassOf(const ipc::SlotSample& peer) noexcept
{
    return peer.stable && ipc::ctlPhase(peer.ctl) == ipc::kCtlPhaseActive && (peer.rtFlags & ipc::kRtBypassed) != 0u;
}

void addPeerAudioFacts(juce::DynamicObject& peer, const diag::DiagPeer& facts)
{
    if (!facts.present)
        return;
    if (facts.channels == 1u || facts.channels == 2u)
        peer.setProperty("channels", static_cast<int>(facts.channels));
    if (facts.sampleRate > 0u)
        peer.setProperty("sampleRate", static_cast<int>(facts.sampleRate));
}

juce::var buildLamps(const LampFacts& facts)
{
    const juce::String cut = facts.cut != nullptr ? juce::String(facts.cut) : juce::String();
    // 压低量按显示精度算：不到 0.1 dB 写出来就是 0，不算「自动降低」
    const double guardDb = roundTo(-static_cast<double>(facts.guardGainDb), 10.0);

    const char* guard = "ok";
    if (cut == "runaway")
        guard = "cut";
    else if (guardDb > 0.0)
        guard = "reduced";
    else if (facts.limiterOn && facts.nearFullHeld)
        guard = "limiting";

    const char* nonfinite = "ok";
    if (cut == "nan")
        nonfinite = "latched";
    else if (facts.loopNan)
        nonfinite = "zeroed";

    auto* o = new juce::DynamicObject();
    o->setProperty("dc", facts.dcHeld ? "blocked" : "ok");
    o->setProperty("nonfinite", nonfinite);
    o->setProperty("guard", guard);
    o->setProperty("nonfiniteCount", static_cast<juce::int64>(facts.nonfiniteCount));
    o->setProperty("guardDb", guardDb > 0.0 ? guardDb : 0.0);
    return juce::var(o);
}

juce::var levelDb(float linearPeak)
{
    if (!(linearPeak > 0.0f) || !std::isfinite(linearPeak))
        return {};
    return juce::var(roundTo(20.0 * std::log10(static_cast<double>(linearPeak)), 10.0));
}

bool isL2(const char* cut, const juce::String& pairState, bool limiterOn, bool guardLit)
{
    return cut != nullptr || pairState == api::kPairConflict || pairState == api::kPairAbiMismatch ||
           (!limiterOn && guardLit);
}

const char* delayReasonOf(const loop::LoopStatusFields& loop, std::uint32_t sampleRate) noexcept
{
    if ((loop.delayHints & loop::kDelayHintClipped) != 0u)
        return "capped";
    // 下限的来源一直有值（通常是声明的块长），要用户要的 D 真的低于下限才算抬升
    const bool raised =
        sampleRate > 0u && loop::msToFrames(static_cast<double>(loop.delayUserMs), sampleRate) < loop.delayMin;
    if (!raised)
        return "ok";
    return loop.delayMinReason == static_cast<std::uint8_t>(loop::DelayMinReason::kHostViolation) ? "overrun"
                                                                                                  : "raised";
}

void trackExportCauses(InputProcessor::UiSession& session, const loop::LoopStatusFields& loop) noexcept
{
    // 引擎只留最近一块的原因位，并集在这里攒。窗口关着时没人读，只留得住最后一次的原因；计数不丢
    if (loop.exportEvents != session.exportLatch.seen())
        session.exportBits |= loop.exportCauses;
    session.exportLatch.update(loop.exportEvents);
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
// InputBridge
// -----------------------------------------------------------------------------

InputBridge::InputBridge(InputProcessor& processor, Hooks hooks) : processor_(processor), hooks_(std::move(hooks))
{
    // 编辑器打开之前的编码失败不补报：rinne.error 只报打开之后新发生的
    if (auto* glue = processor_.stateGlue())
        encodeFailuresSeen_ = glue->encodeFailures();
    lastSamples_ = processor_.getProcessedSamples();
    lastAdvanceMs_ = now();
    discardPeaks();
}

InputBridge::~InputBridge()
{
    endAllGestures();
}

void InputBridge::discardPeaks()
{
    auto& m = meters();
    m.inputPeak.take();
    m.returnPeak.take();
    m.outputPeak.take();
    m.dryPartPeak.take();
    m.returnPartPeak.take();
    m.limiterReductionDb.take();
}

std::uint64_t InputBridge::now() const
{
    if (hooks_.nowMs)
        return hooks_.nowMs();
    return static_cast<std::uint64_t>(juce::Time::getMillisecondCounterHiRes());
}

engine::Meters& InputBridge::meters() noexcept
{
    return processor_.instanceHost().inputMeters();
}

// ---- 两个角色共用的函数 ----

juce::var InputBridge::setRulesAck(const juce::Array<juce::var>& args)
{
    int version = 0;
    if (!common::parseRulesAckArg(args, version))
        return common::failResponse(common::Reason::BadVersion);
    uidefaults::setRulesAckVersion(version);
    forceState_ = true;
    return common::okResponse();
}

juce::var InputBridge::setGuideSeen(const juce::Array<juce::var>& args)
{
    bool seen = false;
    if (!common::jsBoolArg(args, seen) || !seen)
        return common::failResponse(common::Reason::BadValue);
    uidefaults::setGuideSeenGlobalInput(true);
    forceState_ = true;
    return common::okResponse();
}

juce::var InputBridge::setGroup(const juce::Array<juce::var>& args)
{
    int group = 0;
    if (!common::parseGroupArg(args, group) || !processor_.selectGroup(group))
        return common::failResponse(common::Reason::BadGroup);
    // 用户自己选了组：之前等确认的接管不再作数
    takeover_.reset();
    return common::okResponseWith("group", common::groupLetter(static_cast<std::uint32_t>(group)));
}

juce::var InputBridge::copyDiagnostics(const juce::Array<juce::var>& args, const UiContext& ui)
{
    const juce::String text =
        common::withUiDiagLine(processor_.diagnosticsText(), common::uiDiagLine(ui.webview, ui.uiScale, ui.lang));
    return common::respondCopyDiagnostics(args, text, hooks_.writeClipboard);
}

juce::var InputBridge::openExternal(const juce::Array<juce::var>& args)
{
    return common::respondOpenExternal(args, hooks_.launchUrl);
}

juce::var InputBridge::setDiagLog(const juce::Array<juce::var>& args)
{
    bool on = false;
    if (!common::jsBoolArg(args, on))
        return common::failResponse(common::Reason::BadValue);
    processor_.setDiagLogEnabled(on);
    forceState_ = true;
    return common::okResponseWith("value", on);
}

juce::var InputBridge::resolveTakeover(const juce::Array<juce::var>& args)
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

juce::var InputBridge::showLicense()
{
    if (!hooks_.showLicense || !hooks_.showLicense())
        return common::failResponse(common::Reason::Unavailable);
    return common::okResponse();
}

// ---- 参数与手势 ----

juce::var InputBridge::beginParamGesture(const juce::Array<juce::var>& args)
{
    auto* layer = processor_.parameterLayer();
    const juce::String id = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    auto* parameter = layer != nullptr && findParam(id) != nullptr ? layer->tree().getParameter(id) : nullptr;
    if (parameter == nullptr)
        return common::failResponse(kReasonBadParam);
    processor_.userParamGestureBegan();
    if (!gestures_.contains(id))
    {
        gestures_.add(id);
        parameter->beginChangeGesture();
    }
    return common::okResponse();
}

juce::var InputBridge::endParamGesture(const juce::Array<juce::var>& args)
{
    auto* layer = processor_.parameterLayer();
    const juce::String id = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    auto* parameter = layer != nullptr && findParam(id) != nullptr ? layer->tree().getParameter(id) : nullptr;
    if (parameter == nullptr)
        return common::failResponse(kReasonBadParam);
    processor_.userParamGestureBegan();
    if (gestures_.contains(id))
    {
        gestures_.removeString(id);
        parameter->endChangeGesture();
    }
    return common::okResponse();
}

void InputBridge::endAllGestures()
{
    auto* layer = processor_.parameterLayer();
    if (layer != nullptr)
        for (const auto& id : gestures_)
            if (auto* parameter = layer->tree().getParameter(id))
                parameter->endChangeGesture();
    gestures_.clear();
}

juce::var InputBridge::writeParam(const juce::String& id, const juce::var& value)
{
    const ParamSpec* spec = findParam(id);
    auto* layer = processor_.parameterLayer();
    auto* parameter = layer != nullptr && spec != nullptr ? layer->tree().getParameter(id) : nullptr;
    const auto* raw = layer != nullptr && spec != nullptr ? layer->tree().getRawParameterValue(id) : nullptr;
    if (parameter == nullptr || raw == nullptr)
        return common::failResponse(kReasonBadParam);
    const auto plain = plainFromBridge(*spec, value);
    if (!plain.has_value())
        return common::failResponse(common::Reason::BadValue);

    processor_.userParamGestureBegan();
    const float before = raw->load(std::memory_order_relaxed);
    const float normalised = parameter->convertTo0to1(*plain);
    // 手势开着时只写值；没有手势的单次写入（开关、分段控件）自己成一次手势，宿主据此记成一步可撤销的改动
    if (gestures_.contains(id))
    {
        parameter->setValueNotifyingHost(normalised);
    }
    else
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(normalised);
        parameter->endChangeGesture();
    }
    const float after = raw->load(std::memory_order_relaxed);
    if (after != before && isPresetParam(id))
        markPresetDirty();

    echoPending_ = true;
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty("value", bridgeValue(*spec, after));
    o->setProperty("seq", static_cast<juce::int64>(++paramSeq_));
    return juce::var(o);
}

juce::var InputBridge::setParam(const juce::Array<juce::var>& args)
{
    const juce::String id = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    return writeParam(id, args.size() >= 2 ? args[1] : juce::var());
}

juce::var InputBridge::setLimiter(const juce::Array<juce::var>& args)
{
    return writeParam(api::kParamLimiter, args.size() >= 1 ? args[0] : juce::var());
}

void InputBridge::markPresetDirty()
{
    auto* glue = processor_.stateGlue();
    if (glue == nullptr)
        return;
    const state::Configs configs = glue->configs();
    // 没有当前预设时无所谓改没改：state.preset 恒是 { id: null, dirty: false, name: null }
    if (configs.presetName.empty() || configs.presetDirty)
        return;
    glue->editConfigs([](state::Configs& next) { next.presetDirty = true; });
}

// ---- 安全 ----

void InputBridge::trackCooldown(bool cooling, std::uint64_t nowMs)
{
    if (cooling && !cooling_)
        coolingSinceMs_ = nowMs;
    cooling_ = cooling;
}

std::uint64_t InputBridge::cooldownLeftMs(std::uint64_t nowMs) const
{
    if (!cooling_)
        return 0u;
    const std::uint64_t elapsed = nowMs - coolingSinceMs_;
    // 冷却按样本计，宿主慢下来时会比墙钟长：这一位还在就不能写 0
    return elapsed < kCooldownMs ? kCooldownMs - elapsed : 1u;
}

juce::var InputBridge::panic()
{
    const std::uint32_t word = meters().safety.load(std::memory_order_relaxed);
    processor_.panic();
    // 已经切断的不改原因：连点不恢复，也不把别的切断原因改写成手动。没切断的要等下一块才生效，回执先写手动
    const bool tripped = engine::PackedSafety::state(word) == engine::SafetyState::Tripped;
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty("cut", tripped ? cutName(engine::PackedSafety::trip(word)) : "panic");
    return juce::var(o);
}

juce::var InputBridge::resumeLoop()
{
    const std::uint64_t t = now();
    const std::uint32_t word = meters().safety.load(std::memory_order_relaxed);
    if (engine::PackedSafety::state(word) != engine::SafetyState::Tripped)
        return common::failResponse(kReasonNotCut);
    trackCooldown(engine::PackedSafety::coolingDown(word), t);
    if (!processor_.canResumeLoop())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("ok", false);
        o->setProperty("reason", kReasonCooldown);
        o->setProperty("remainingMs", static_cast<juce::int64>(cooldownLeftMs(t)));
        return juce::var(o);
    }
    processor_.resumeLoop();
    return common::okResponse();
}

juce::var InputBridge::dismissHint(const juce::Array<juce::var>& args)
{
    if (args.size() < 1 || !args[0].isString() || args[0].toString().isEmpty())
        return common::failResponse(kReasonBadId);
    const juce::String id = args[0].toString();
    auto& session = processor_.uiSession();
    if (id == kHintExportMismatch)
    {
        // 确认导出提示：已读值追上当前计数，rinne.state 里的 exportCauses 清零，之后的新事件重新锁存。不记进
        // dismissedHints：页面重开时把它当「以当前锁存为基线」，窗口关着时新来的事件就看不到了
        trackExportCauses(session, processor_.instanceHost().inputEngine().loopStatus());
        session.exportLatch.acknowledge();
        session.exportBits = 0u;
    }
    else
    {
        session.dismissedHints.addIfNotAlreadyThere(id);
    }
    return common::okResponse();
}

// ---- 预设 ----

void InputBridge::setCurrentPreset(const juce::String& id, const juce::String& name)
{
    auto& session = processor_.uiSession();
    session.presetId = id;
    session.presetName = name;
    if (auto* glue = processor_.stateGlue())
    {
        const std::string utf8 = name.toStdString();
        glue->editConfigs([&utf8](state::Configs& configs) {
            configs.presetName = utf8;
            configs.presetDirty = false;
        });
    }
}

void InputBridge::resolveCurrentPreset(const juce::String& name)
{
    auto& session = processor_.uiSession();
    if (name == session.presetName)
        return;
    session.presetName = name;
    session.presetId = {};
    auto* store = processor_.presetStore();
    if (name.isEmpty() || store == nullptr)
        return;
    for (const auto& preset : store->factoryPresets())
    {
        if (preset.en == name || preset.zh == name || preset.fr == name)
        {
            session.presetId = preset.id;
            return;
        }
    }
    for (const auto& preset : store->listUser().presets)
    {
        if (preset.name.equalsIgnoreCase(name))
        {
            session.presetId = preset.id;
            return;
        }
    }
}

juce::var InputBridge::loadPreset(const juce::Array<juce::var>& args)
{
    auto* store = processor_.presetStore();
    auto* layer = processor_.parameterLayer();
    auto* glue = processor_.stateGlue();
    if (store == nullptr || layer == nullptr || glue == nullptr)
        return common::failResponse(presetReason(PresetError::kNotReady));
    const juce::String id = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    const PresetResult result = store->load(id, layer->tree());
    if (!result.ok())
        return common::failResponse(presetReason(result.error));
    setCurrentPreset(result.id, result.name);
    glue->markUserAction();
    glue->markStateDirty();
    return common::okResponse();
}

juce::var InputBridge::savePreset(const juce::Array<juce::var>& args)
{
    auto* store = processor_.presetStore();
    auto* layer = processor_.parameterLayer();
    auto* glue = processor_.stateGlue();
    if (store == nullptr || layer == nullptr || glue == nullptr)
        return common::failResponse(presetReason(PresetError::kWriteFailed));
    const juce::String name = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    const bool overwrite = args.size() >= 2 && args[1].isBool() && static_cast<bool>(args[1]);
    const PresetResult result = store->save(name, overwrite, layer->tree());
    if (!result.ok())
        return common::failResponse(presetReason(result.error));
    setCurrentPreset(result.id, result.name);
    glue->markUserAction();
    glue->markStateDirty();
    presetsPending_ = true;
    refreshPresetList();
    return common::okResponseWith("id", result.id);
}

juce::var InputBridge::deletePreset(const juce::Array<juce::var>& args)
{
    auto* store = processor_.presetStore();
    if (store == nullptr)
        return common::failResponse(presetReason(PresetError::kWriteFailed));
    const juce::String id = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    const PresetResult result = store->remove(id);
    if (!result.ok())
        return common::failResponse(presetReason(result.error));
    if (processor_.uiSession().presetId == id)
    {
        // 删掉的是当前预设：参数不动，没有当前预设了。不定型（只动文件），但工程里的名字变了，要标脏
        setCurrentPreset({}, {});
        if (auto* glue = processor_.stateGlue())
            glue->markStateDirty();
    }
    presetsPending_ = true;
    refreshPresetList();
    return common::okResponse();
}

void InputBridge::refreshPresetList()
{
    juce::Array<juce::var> user;
    if (auto* store = processor_.presetStore())
    {
        for (const auto& preset : store->listUser().presets)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("id", preset.id);
            o->setProperty("name", preset.name);
            user.add(juce::var(o));
        }
    }
    userPresets_ = user;
}

juce::var InputBridge::presetsBody()
{
    juce::Array<juce::var> factory;
    if (auto* store = processor_.presetStore())
    {
        for (const auto& preset : store->factoryPresets())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("id", preset.id);
            o->setProperty("name", factoryNameOf(preset));
            factory.add(juce::var(o));
        }
    }
    const juce::String id = presetField().getProperty("id", juce::var()).toString();
    auto* o = new juce::DynamicObject();
    o->setProperty("factory", factory);
    o->setProperty("user", userPresets_);
    o->setProperty("current", id.isEmpty() ? juce::var() : juce::var(id));
    return juce::var(o);
}

juce::var InputBridge::presetField()
{
    auto* o = new juce::DynamicObject();
    juce::var result(o);
    auto* glue = processor_.stateGlue();
    const state::Configs configs = glue != nullptr ? glue->configs() : state::Configs{};
    const juce::String name = fromUtf8(configs.presetName);
    resolveCurrentPreset(name);
    const juce::String& id = processor_.uiSession().presetId;
    o->setProperty("id", id.isEmpty() ? juce::var() : juce::var(id));
    o->setProperty("dirty", name.isNotEmpty() && configs.presetDirty);
    auto* store = processor_.presetStore();
    const FactoryPreset* factory = store != nullptr ? factoryById(*store, id) : nullptr;
    if (factory != nullptr)
        o->setProperty("name", factoryNameOf(*factory));
    else
        o->setProperty("name", name.isEmpty() ? juce::var() : juce::var(name));
    return result;
}

// ---- 工程设置 ----

juce::var InputBridge::setGuardOn(const juce::Array<juce::var>& args)
{
    bool on = false;
    if (!common::jsBoolArg(args, on) || !processor_.selectGuardOn(on))
        return common::failResponse(common::Reason::BadValue);
    return common::okResponseWith("value", on);
}

juce::var InputBridge::setResetOnPlay(const juce::Array<juce::var>& args)
{
    bool on = false;
    if (!common::jsBoolArg(args, on) || !processor_.selectResetOnPlay(on))
        return common::failResponse(common::Reason::BadValue);
    return common::okResponseWith("value", on);
}

juce::var InputBridge::setTailMode(const juce::Array<juce::var>& args)
{
    const auto mode = args.size() >= 1 ? tailModeFromName(args[0]) : std::nullopt;
    if (!mode.has_value() || !processor_.selectTailMode(*mode))
        return common::failResponse(common::Reason::BadValue);
    return common::okResponseWith("value", tailModeName(*mode));
}

juce::var InputBridge::setFbUnit(const juce::Array<juce::var>& args)
{
    const juce::String name = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    if (name != "pct" && name != "db")
        return common::failResponse(common::Reason::BadValue);
    const auto unit = name == "db" ? state::FbUnit::Decibel : state::FbUnit::Percent;
    if (!processor_.selectFbUnit(unit))
        return common::failResponse(common::Reason::BadValue);
    return common::okResponseWith("value", fbUnitName(unit));
}

juce::var InputBridge::setDelayUnit(const juce::Array<juce::var>& args)
{
    const juce::String name = args.size() >= 1 && args[0].isString() ? args[0].toString() : juce::String();
    if (name != "ms" && name != "samples")
        return common::failResponse(common::Reason::BadValue);
    const auto unit = name == "samples" ? state::DelayUnit::Samples : state::DelayUnit::Milliseconds;
    if (!processor_.selectDelayUnit(unit))
        return common::failResponse(common::Reason::BadValue);
    return common::okResponseWith("value", delayUnitName(unit));
}

juce::var InputBridge::moveToFreeGroup()
{
    const auto outcome = processor_.repairGroup(false);
    if (outcome.applied)
    {
        takeover_.reset();
        return common::okResponseWith("group", common::groupLetter(outcome.group));
    }
    if (outcome.group != 0u && outcome.needsConfirm)
    {
        // 只剩休眠实例占着的组：先不动，等用户在确认条上点
        takeover_ = outcome.group;
        return common::failResponse(kReasonNeedsConfirm);
    }
    if (processor_.instanceHost().rendezvousState() != ipc::RendezvousState::kConflict)
        return common::failResponse(kReasonNotConflict);
    return common::failResponse(kReasonNoFreeGroup);
}

// ---- 读数 ----

NoticeFacts InputBridge::noticeFacts() const
{
    NoticeFacts facts;
    const InstanceHost& host = processor_.instanceHost();
    // 会合的 lastResult 只在 tick 里写，tick 与这里都在消息线程。后端报系统拒绝是永久的：会合随即停在
    // kIpcUnavailable、不再重试，lastResult 也不会再被改写，所以只看它就够了
    facts.ipcDenied = host.rendezvous().lastResult() == ipc::InitResult::kFailedDenied;
    applyLoopFacts(facts, host.inputEngine().loopStatus());
    if (auto* glue = processor_.stateGlue())
    {
        facts.heldNewerAbi = glue->heldNewerAbi(); // 不在保留时为 0
        facts.decodeFailed = glue->lastLoadStatus() == state::StateLoadStatus::Corrupt;
    }
    return facts;
}

void InputBridge::trackHostProgress(std::uint64_t nowMs)
{
    const std::int64_t samples = processor_.getProcessedSamples();
    if (samples != lastSamples_)
    {
        lastSamples_ = samples;
        lastAdvanceMs_ = nowMs;
    }
}

SilentFacts InputBridge::silentFacts()
{
    const std::uint64_t t = now();
    trackHostProgress(t);
    SilentFacts facts;
    const InstanceHost& host = processor_.instanceHost();
    facts.hostBypass = processor_.hostBypassed();
    facts.hostPaused = lastSamples_ > 0 && t - lastAdvanceMs_ >= kHostPausedMs;
    // 会合的观察者只在 tick 里扫，tick 与这里都在消息线程
    const std::uint32_t group = host.group();
    if (group >= 1u && group <= ipc::kGroupCount)
        facts.peerBypass = peerBypassOf(host.rendezvous().observer().sample(group, ipc::Role::kOutput));
    const std::uint32_t word = meters().safety.load(std::memory_order_relaxed);
    facts.stalled = engine::PackedSafety::state(word) == engine::SafetyState::Stalled;
    return facts;
}

void InputBridge::dropStaleTakeover()
{
    // 冲突自己解开了（占着的实例走了、重试拿到了槽），确认条就不该再挂着
    if (takeover_.has_value() && processor_.instanceHost().rendezvousState() != ipc::RendezvousState::kConflict)
        takeover_.reset();
}

juce::var InputBridge::delayBody(const loop::LoopStatusFields& loop, std::uint32_t sampleRate) const
{
    auto* o = new juce::DynamicObject();
    if (sampleRate > 0u && loop.delayEffective > 0)
        o->setProperty("actualMs", roundTo(loop::framesToMs(loop.delayEffective, sampleRate), 1000.0));
    if (sampleRate > 0u && loop.delayMin > 0)
        o->setProperty("minMs", roundTo(loop::framesToMs(loop.delayMin, sampleRate), 1000.0));
    o->setProperty("reason", delayReasonOf(loop, sampleRate));
    o->setProperty("maxBlock", static_cast<juce::int64>(loop.maxBlockSeen));
    if (loop.bpmCenti > 0u)
        o->setProperty("bpm", static_cast<double>(loop.bpmCenti) / 100.0);
    o->setProperty("noTempo", (loop.delayHints & loop::kDelayHintNoTempo) != 0u);
    return juce::var(o);
}

bool InputBridge::limiterOn() const
{
    if (auto* layer = processor_.parameterLayer())
        if (const auto* raw = layer->tree().getRawParameterValue(api::kParamLimiter))
            return raw->load(std::memory_order_relaxed) >= 0.5f;
    return true;
}

LampFacts InputBridge::lampFacts(std::uint32_t safetyWord, std::uint64_t nowMs)
{
    auto& m = meters();
    const engine::SafetyDiag diag = processor_.instanceHost().inputEngine().safetyDiag();
    const bool over = m.returnDcMean.load() > kDcLampLevel;
    if (over && !dcOver_)
        dcOverSinceMs_ = nowMs;
    dcOver_ = over;

    LampFacts facts;
    if (engine::PackedSafety::state(safetyWord) == engine::SafetyState::Tripped)
        facts.cut = cutName(engine::PackedSafety::trip(safetyWord));
    facts.dcHeld = over && nowMs - dcOverSinceMs_ >= kDcLampHoldMs;
    facts.loopNan = diag.loopNanLamp;
    facts.nonfiniteCount = m.returnNonFinite.load(std::memory_order_relaxed) + diag.nonfiniteRing;
    facts.guardGainDb = m.guardGainDb.load();
    facts.nearFullHeld = static_cast<double>(m.nearFullSeconds.load()) >= engine::safety::kGuardLimitSeconds;
    facts.limiterOn = limiterOn();
    return facts;
}

juce::var InputBridge::safetyBody(std::uint32_t safetyWord, const char* cut, const juce::String& pairState,
                                  std::uint64_t nowMs)
{
    const engine::SafetyState safety = engine::PackedSafety::state(safetyWord);
    trackCooldown(engine::PackedSafety::coolingDown(safetyWord), nowMs);
    // 页面只看有没有在冷却；取整到 100 ms，免得冷却那一秒多里每拍都因为这个数变了而多发一帧
    const std::uint64_t left = cooldownLeftMs(nowMs);

    const juce::var lamps = buildLamps(lampFacts(safetyWord, nowMs));
    const bool guardLit = lamps.getProperty("guard", juce::var()).toString() != "ok";

    auto* o = new juce::DynamicObject();
    o->setProperty("cut", cut != nullptr ? juce::var(cut) : juce::var());
    o->setProperty("cooldownMs", static_cast<juce::int64>((left + 99u) / 100u * 100u));
    o->setProperty("l2", isL2(cut, pairState, limiterOn(), guardLit));
    o->setProperty("fadeIn", safety == engine::SafetyState::FadeIn);
    return juce::var(o);
}

juce::DynamicObject::Ptr InputBridge::cheapStateFields()
{
    const std::uint64_t t = now();
    trackHostProgress(t);
    dropStaleTakeover();

    const diag::DiagInputs d = diagFacts(processor_);
    const loop::LoopStatusFields loop = processor_.instanceHost().inputEngine().loopStatus();
    trackExportCauses(processor_.uiSession(), loop);
    auto* glue = processor_.stateGlue();
    const state::Configs configs = glue != nullptr ? glue->configs() : state::Configs{};
    const std::uint32_t word = meters().safety.load(std::memory_order_relaxed);
    const char* cut = engine::PackedSafety::state(word) == engine::SafetyState::Tripped
                          ? cutName(engine::PackedSafety::trip(word))
                          : nullptr;

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

    auto* cfg = new juce::DynamicObject();
    cfg->setProperty("resetOnPlay", configs.resetOnPlay);
    cfg->setProperty("tailMode", tailModeName(configs.tailMode));
    cfg->setProperty("delayUnit", delayUnitName(configs.delayUnit));
    cfg->setProperty("fbUnit", fbUnitName(configs.fbUnit));
    cfg->setProperty("guardOn", configs.guardOn);
    o->setProperty("cfg", juce::var(cfg));

    o->setProperty("preset", presetField());
    o->setProperty("delay", delayBody(loop, d.prepare.sampleRate));
    o->setProperty("safety", safetyBody(word, cut, pairing::idOf(processor_.pairState().state), t));

    juce::Array<juce::var> dismissed;
    const auto& session = processor_.uiSession();
    for (const auto& id : session.dismissedHints)
        dismissed.add(id);
    o->setProperty("dismissedHints", dismissed);

    auto* exportCauses = new juce::DynamicObject();
    exportCauses->setProperty("bits", static_cast<juce::int64>(session.exportBits));
    exportCauses->setProperty("count", static_cast<juce::int64>(session.exportLatch.pending()));
    o->setProperty("exportCauses", juce::var(exportCauses));

    const char* silent = silentCauseOf(silentFacts());
    o->setProperty("silentCause", silent != nullptr ? juce::var(silent) : juce::var());

    const juce::Array<juce::var> notices = buildNotices(noticeFacts());
    if (!notices.isEmpty())
        o->setProperty("notices", notices);
    o->setProperty("build", buildInfo(build::buildLabel(), processor_.wrapperType));
    return o;
}

void InputBridge::addStateFields(juce::DynamicObject& snapshot)
{
    const auto fields = cheapStateFields();
    for (const auto& entry : fields->getProperties())
        snapshot.setProperty(entry.name, entry.value);
}

juce::String InputBridge::stateSignature(const UiContext& ui)
{
    return juce::JSON::toString(juce::var(cheapStateFields().get()), true) + "|" + ui.lang + "|" +
           juce::String(common::scaleForJson(ui.uiScale));
}

juce::var InputBridge::pairBody() const
{
    const pairing::PairModel m = processor_.pairState();
    const diag::DiagInputs d = diagFacts(processor_);
    const auto& label = processor_.trackLabel();

    auto* peer = new juce::DynamicObject();
    peer->setProperty("name", m.peerName.empty() ? juce::var() : juce::var(fromUtf8(m.peerName)));
    peer->setProperty("color", m.peerColourArgb != 0u ? juce::var(colourText(m.peerColourArgb)) : juce::var());
    peer->setProperty("version", m.peerVersion.empty() ? juce::var() : juce::var(fromUtf8(m.peerVersion)));
    peer->setProperty("holder", m.occupantName.empty() ? juce::var() : juce::var(fromUtf8(m.occupantName)));
    addPeerAudioFacts(*peer, d.peer);

    auto* self = new juce::DynamicObject();
    self->setProperty("name", label.name.has_value() ? juce::var(*label.name) : juce::var());
    if (label.colour.has_value())
        self->setProperty("color", colourText(label.colour->getARGB()));
    // 处理器只接受单声道与立体声两种布局（RinneProcessorBase::isBusesLayoutSupported）
    self->setProperty("channels", processor_.getMainBusNumInputChannels() == 1 ? 1 : 2);

    auto* o = new juce::DynamicObject();
    o->setProperty("state", pairing::idOf(m.state));
    o->setProperty("groupMask", static_cast<int>(m.peerMask & 0xffu));
    o->setProperty("peer", juce::var(peer));
    o->setProperty("self", juce::var(self));
    o->setProperty("verify", verifyName(m.nameCheck));
    return juce::var(o);
}

juce::var InputBridge::paramsBody() const
{
    auto* values = new juce::DynamicObject();
    juce::var result(values);
    auto* layer = processor_.parameterLayer();
    if (layer == nullptr)
        return result;
    for (const auto& spec : kParamSpecs)
        if (const auto* raw = layer->tree().getRawParameterValue(spec.id))
            values->setProperty(spec.id, bridgeValue(spec, raw->load(std::memory_order_relaxed)));
    return result;
}

juce::var InputBridge::metersBody()
{
    const std::uint64_t t = now();
    auto& m = meters();
    const std::uint32_t word = m.safety.load(std::memory_order_relaxed);
    const float gr = m.limiterReductionDb.take();

    auto* o = new juce::DynamicObject();
    o->setProperty("in", levelDb(m.inputPeak.take()));
    o->setProperty("ret", levelDb(m.returnPeak.take()));
    o->setProperty("out", levelDb(m.outputPeak.take()));
    o->setProperty("dryPart", levelDb(m.dryPartPeak.take()));
    o->setProperty("retPart", levelDb(m.returnPartPeak.take()));
    // 桥上的增益缩减是负数或 0（页面取绝对值画表）；0 不写成 -0
    const double grDb = roundTo(static_cast<double>(gr), 10.0);
    o->setProperty("gr", grDb > 0.0 ? -grDb : 0.0);
    o->setProperty("highCutLimited", m.highCutLimited.load(std::memory_order_relaxed));
    o->setProperty("lamps", buildLamps(lampFacts(word, t)));
    return juce::var(o);
}

juce::var InputBridge::withSeq(const juce::var& body, std::uint32_t seq) const
{
    auto* o = new juce::DynamicObject();
    o->setProperty("seq", static_cast<juce::int64>(seq));
    if (auto* src = body.getDynamicObject())
        for (const auto& entry : src->getProperties())
            o->setProperty(entry.name, entry.value);
    return juce::var(o);
}

InputBridge::Frames InputBridge::poll(const UiContext& ui)
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

    const juce::var pair = pairBody();
    const juce::String pairJson = juce::JSON::toString(pair, true);
    if (!pairSent_ || pairJson != lastPairJson_ || t - lastPairMs_ >= kHeartbeatMs)
    {
        pairSent_ = true;
        lastPairJson_ = pairJson;
        lastPairMs_ = t;
        frames.pair = withSeq(pair, ++pairSeq_);
    }

    const juce::var values = paramsBody();
    const juce::String paramsJson = juce::JSON::toString(values, true);
    if (echoPending_ || !paramsSent_ || paramsJson != lastParamsJson_ || t - lastParamsMs_ >= kHeartbeatMs)
    {
        // 回声用 setParam 回执里已经发出去的 seq；别的变化（自动化、预设）另取一个
        const std::uint32_t seq = echoPending_ ? paramSeq_ : ++paramSeq_;
        echoPending_ = false;
        paramsSent_ = true;
        lastParamsJson_ = paramsJson;
        lastParamsMs_ = t;
        auto* frame = new juce::DynamicObject();
        frame->setProperty("seq", static_cast<juce::int64>(seq));
        frame->setProperty("values", values);
        frames.params = juce::var(frame);
    }

    if (presetsPending_ || t - lastPresetsMs_ >= kHeartbeatMs)
    {
        if (userPresets_.isVoid())
            refreshPresetList();
        presetsPending_ = false;
        lastPresetsMs_ = t;
        frames.presets = presetsBody();
    }

    if (auto* glue = processor_.stateGlue())
    {
        const std::uint32_t failures = glue->encodeFailures();
        if (failures != encodeFailuresSeen_)
        {
            encodeFailuresSeen_ = failures;
            auto* e = new juce::DynamicObject();
            e->setProperty("seq", static_cast<juce::int64>(++errorSeq_));
            e->setProperty("code", kErrorStateEncode);
            frames.errors.add(juce::var(e));
        }
    }
    return frames;
}

InputBridge::Frames InputBridge::tick(const UiContext& ui)
{
    Frames frames = poll(ui);
    const std::uint64_t t = now();
    const juce::var body = metersBody();
    const juce::String json = juce::JSON::toString(body, true);
    if (!metersSent_ || json != lastMetersJson_ || t - lastMetersMs_ >= kHeartbeatMs)
    {
        metersSent_ = true;
        lastMetersJson_ = json;
        lastMetersMs_ = t;
        frames.meters = withSeq(body, ++meterSeq_);
    }
    return frames;
}

InputBridge::Frames InputBridge::firstFrames(const UiContext& ui)
{
    // 峰值两次 take 之间一直攒着：窗口关着或页面还没起来时攒下的最高点不能当现在的电平发出去，这一帧电平是空的，
    // 下一拍（40 ms）起才是真的。重发请求时丢掉的也不过是上一拍以来的一点
    discardPeaks();
    const std::uint64_t t = now();
    lastStateSig_ = stateSignature(ui);
    lastStateMs_ = t;
    stateSent_ = true;
    forceState_ = false;

    Frames frames;
    const juce::var pair = pairBody();
    lastPairJson_ = juce::JSON::toString(pair, true);
    lastPairMs_ = t;
    pairSent_ = true;
    frames.pair = withSeq(pair, ++pairSeq_);

    const juce::var values = paramsBody();
    lastParamsJson_ = juce::JSON::toString(values, true);
    lastParamsMs_ = t;
    paramsSent_ = true;
    const std::uint32_t seq = echoPending_ ? paramSeq_ : ++paramSeq_;
    echoPending_ = false;
    auto* params = new juce::DynamicObject();
    params->setProperty("seq", static_cast<juce::int64>(seq));
    params->setProperty("values", values);
    frames.params = juce::var(params);

    const juce::var meterBody = metersBody();
    lastMetersJson_ = juce::JSON::toString(meterBody, true);
    lastMetersMs_ = t;
    metersSent_ = true;
    frames.meters = withSeq(meterBody, ++meterSeq_);

    // 页面重载或重发请求时重读目录：别的实例可能存过新的
    refreshPresetList();
    presetsPending_ = false;
    lastPresetsMs_ = t;
    frames.presets = presetsBody();
    return frames;
}

} // namespace rinne::input
