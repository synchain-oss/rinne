// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputParams.h"

#include <algorithm>
#include <cstring>

namespace rinne::input
{

namespace
{

enum class Kind
{
    F, // 连续
    B, // 开关
    C, // 档位
};

enum class Scale
{
    lin,
    log,
};

enum class Unit
{
    none,
    pct,
    ms,
    hz,
    db,
};

// 参数表的一行，展开自 ParamTable.h；在数组里的位置就是表里的序号
struct ParamSpec
{
    const char* id;
    Kind kind;
    double lo;
    double hi;
    double def;
    double step;
    Scale scale;
    bool automatable;
    Unit unit;
    const char* name;
};

constexpr ParamSpec kSpecs[] = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) \
    {#id, Kind::kind, lo, hi, def, step, Scale::skew, (automatable) != 0, Unit::unit, name},
#include "ParamTable.h"
};

// 表里写的序号，用来核对「行的位置 = 序号」
constexpr int kTableIndex[] = {
#define RINNE_PARAM(id, index, ...) index,
#include "ParamTable.h"
};

// 参数 ID 在 kSpecs 里的位置，同时也是 snapshot 取句柄的下标
enum class Slot : std::size_t
{
#define RINNE_PARAM(id, ...) id,
#include "ParamTable.h"
};

struct ChoiceSpec
{
    const char* id;
    int index;
    const char* label;
};

constexpr ChoiceSpec kChoices[] = {
#define RINNE_PARAM(...)
#define RINNE_CHOICE(id, index, bridge, label) {#id, index, label},
#include "ParamTable.h"
};

constexpr bool tableIsOrdered()
{
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        if (kTableIndex[i] != static_cast<int>(i))
            return false;
    }
    return true;
}

static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == kParamCount, "kParamCount 要和参数表的行数一致");
static_assert(sizeof(kTableIndex) / sizeof(kTableIndex[0]) == kParamCount, "kParamCount 要和参数表的行数一致");
static_assert(tableIsOrdered(), "参数表的序号要从 0 起连续，且与行的先后一致");
// 档位参数的上限就是引擎里的档数：delay_note 16 档，delay_mode 两档
static_assert(kSpecs[static_cast<std::size_t>(Slot::delay_note)].hi == engine::kDelayNoteCount - 1);
static_assert(kSpecs[static_cast<std::size_t>(Slot::delay_mode)].hi == 1.0);

const char* unitLabel(Unit unit)
{
    switch (unit)
    {
    case Unit::pct:
        return "%";
    case Unit::ms:
        return "ms";
    case Unit::hz:
        return "Hz";
    case Unit::db:
        return "dB";
    case Unit::none:
        break;
    }
    return "";
}

juce::StringArray choiceLabels(const char* paramId)
{
    juce::StringArray labels;
    for (const auto& choice : kChoices)
    {
        if (std::strcmp(choice.id, paramId) == 0)
            labels.add(choice.label);
    }
    return labels;
}

// ---- 对数刻度（契约：v = lo·(hi/lo)^p，p = ln(v/lo) / ln(hi/lo)）----
// 运算用 double，结果落回 float。两端直接返回端点，免得舍入让端点差一个 ulp。
float logFrom0to1(float start, float end, float proportion)
{
    if (!(proportion > 0.0f))
        return start;
    if (proportion >= 1.0f)
        return end;
    const double lo = start;
    const double hi = end;
    return static_cast<float>(lo * std::pow(hi / lo, static_cast<double>(proportion)));
}

float logTo0to1(float start, float end, float value)
{
    if (!(value > start))
        return 0.0f;
    if (value >= end)
        return 1.0f;
    const double lo = start;
    const double hi = end;
    const double p = std::log(static_cast<double>(value) / lo) / std::log(hi / lo);
    return std::clamp(static_cast<float>(p), 0.0f, 1.0f);
}

// 步进落在明文值上：取整到 lo + k·step，再夹进范围。对数刻度的步进也是明文值上的等间隔，不是归一化值上的
float snapToStep(float start, float end, float value, double step)
{
    if (!(value > start))
        return start;
    if (value >= end)
        return end;
    const double k = std::floor((static_cast<double>(value) - start) / step + 0.5);
    return std::clamp(static_cast<float>(start + k * step), start, end);
}

juce::NormalisableRange<float> makeRange(const ParamSpec& spec)
{
    const auto lo = static_cast<float>(spec.lo);
    const auto hi = static_cast<float>(spec.hi);
    const auto step = static_cast<float>(spec.step);
    if (spec.scale == Scale::lin)
        return juce::NormalisableRange<float>(lo, hi, step);

    const double stepPlain = spec.step;
    juce::NormalisableRange<float> range(
        lo, hi, [](float start, float end, float p) { return logFrom0to1(start, end, p); },
        [](float start, float end, float v) { return logTo0to1(start, end, v); },
        [stepPlain](float start, float end, float v) { return snapToStep(start, end, v, stepPlain); });
    // 自定义换算时 JUCE 不用 interval；写上它，参数对外报的步数和线性参数同一个口径
    range.interval = step;
    return range;
}

std::unique_ptr<juce::RangedAudioParameter> makeParameter(const ParamSpec& spec)
{
    const juce::ParameterID id{spec.id, kParamVersionHint};
    if (spec.kind == Kind::B)
    {
        return std::make_unique<juce::AudioParameterBool>(
            id, spec.name, spec.def != 0.0, juce::AudioParameterBoolAttributes().withAutomatable(spec.automatable));
    }
    if (spec.kind == Kind::C)
    {
        return std::make_unique<juce::AudioParameterChoice>(
            id, spec.name, choiceLabels(spec.id), static_cast<int>(spec.def),
            juce::AudioParameterChoiceAttributes().withAutomatable(spec.automatable));
    }
    return std::make_unique<juce::AudioParameterFloat>(
        id, spec.name, makeRange(spec), static_cast<float>(spec.def),
        juce::AudioParameterFloatAttributes().withAutomatable(spec.automatable).withLabel(unitLabel(spec.unit)));
}

// ---- 明文值到引擎字段 ----
bool toBool(float plain) noexcept
{
    return plain >= 0.5f;
}

// 四舍五入到下标并夹进 0…maxIndex；NaN 与负数取 0。先取整数部分再看小数部分，不做 +0.5：
// 0.49999997f + 0.5f 在 float 里会进位成 1.0f，把略小于 0.5 的值错判成 1
int toIndex(float plain, int maxIndex) noexcept
{
    if (!(plain > 0.0f))
        return 0;
    if (plain >= static_cast<float>(maxIndex))
        return maxIndex;
    const int whole = static_cast<int>(plain);
    return plain - static_cast<float>(whole) >= 0.5f ? whole + 1 : whole;
}

constexpr int maxIndexOf(Slot slot)
{
    return static_cast<int>(kSpecs[static_cast<std::size_t>(slot)].hi);
}

} // namespace

std::unique_ptr<juce::RangedAudioParameter> makeInputParameter(std::size_t index)
{
    if (index >= kParamCount)
        return nullptr;
    return makeParameter(kSpecs[index]);
}

juce::AudioProcessorValueTreeState::ParameterLayout makeInputLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (std::size_t i = 0; i < kParamCount; ++i)
        layout.add(makeParameter(kSpecs[i]));
    return layout;
}

bool InputParamHandles::bind(juce::AudioProcessorValueTreeState& tree)
{
    bound_ = false;
    raw_.fill(nullptr);
    std::array<std::atomic<float>*, kParamCount> found{};
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        std::atomic<float>* handle = tree.getRawParameterValue(kSpecs[i].id);
        // 参数树和参数表对不上是接线错误：这里只返回 false，断言放在调用方（InputParams 的构造函数），
        // 这样用例能在调试器下走到失败路径
        if (handle == nullptr)
            return false;
        found[i] = handle;
    }
    raw_ = found;
    bound_ = true;
    return true;
}

void InputParamHandles::snapshot(engine::EngineParams& out) const noexcept
{
    if (!bound_)
        return;
    const auto plain = [this](Slot slot) noexcept {
        return raw_[static_cast<std::size_t>(slot)]->load(std::memory_order_relaxed);
    };
    out.feedbackPercent = plain(Slot::feedback);
    out.loopOn = toBool(plain(Slot::loop_on));
    out.mixPercent = plain(Slot::mix);
    out.delayMode = static_cast<engine::DelayMode>(toIndex(plain(Slot::delay_mode), maxIndexOf(Slot::delay_mode)));
    out.delayMs = plain(Slot::delay_ms);
    out.delayNote = static_cast<std::uint8_t>(toIndex(plain(Slot::delay_note), maxIndexOf(Slot::delay_note)));
    out.lowCutHz = plain(Slot::low_cut);
    out.highCutHz = plain(Slot::high_cut);
    out.polarityInvert = toBool(plain(Slot::polarity));
    out.limiterOn = toBool(plain(Slot::limiter));
    out.ceilingDb = plain(Slot::ceiling);
}

InputParams::InputParams(juce::AudioProcessor& processor)
    : tree_(processor, nullptr, juce::Identifier(kParamsTreeName), makeInputLayout())
{
    // APVTS 给原子量的初值是默认值经归一化值来回算一趟的结果，不过步进（JUCE 为兼容旧行为这样做）。对数刻度上这一趟差
    // 一个 float 的最低位：delay_ms 的 100 ms 会读成 99.99999。把每个参数写回自己的默认值，原子量就落在步进格上，
    // 全新的实例读到的正是参数表里的默认值。此时还没有宿主连上来，没有监听者会收到通知。
    for (const auto& spec : kSpecs)
    {
        if (auto* parameter = tree_.getParameter(spec.id))
            parameter->setValueNotifyingHost(parameter->getDefaultValue());
    }
    const bool bound = handles_.bind(tree_);
    jassert(bound);
    juce::ignoreUnused(bound);
}

} // namespace rinne::input
