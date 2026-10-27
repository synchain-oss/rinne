// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "BridgeBase.h"

#include <algorithm>

namespace rinne::bridge
{

float clampUiScale(float scale)
{
    return juce::jlimit(plugin::MinUiScale, plugin::MaxUiScale, scale);
}

std::vector<std::pair<juce::String, juce::var>> buildUiSeedPairs(const UiSeed& seed)
{
    return {
        {juce::String(Init::Version), juce::var(seed.version)},
        {juce::String(Init::Role), juce::var(seed.role)},
        {juce::String(Init::Lang), juce::var(seed.lang)},
        {juce::String(Init::UiScale), juce::var(scaleForJson(seed.uiScale))},
    };
}

juce::var buildUiSnapshot(const UiSeed& seed, const UiFlags& flags)
{
    auto* obj = new juce::DynamicObject();
    for (const auto& pair : buildUiSeedPairs(seed))
        obj->setProperty(pair.first, pair.second);
    obj->setProperty("langChosen", flags.langChosen);
    obj->setProperty("rulesAck", flags.rulesAck);
    obj->setProperty("guideSeen", flags.guideSeen);
    obj->setProperty("diagLog", flags.diagLog);
    return juce::var(obj);
}

bool parseUiScaleArg(const juce::Array<juce::var>& args, const juce::String& role, float& outScale)
{
    if (args.size() < 1)
        return false;
    const auto& v = args[0];
    if (!(v.isDouble() || v.isInt()))
        return false;

    const double f = static_cast<double>(v);
    // 档位表的真源是设计盒；两张表是不同长度的 std::array，类型不同，只能分支各查各的。
    // 用 double 精确比较：页面发来的就是档位表里的原值，不做容差，容差会放进表外的档位。
    bool inTable = false;
    if (role == "input")
        inTable =
            std::find(design::kInputPresets.begin(), design::kInputPresets.end(), f) != design::kInputPresets.end();
    else
        inTable =
            std::find(design::kOutputPresets.begin(), design::kOutputPresets.end(), f) != design::kOutputPresets.end();
    if (!inTable)
        return false;

    outScale = static_cast<float>(f);
    return true;
}

juce::var okResponse()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    return juce::var(o);
}

juce::var okResponseWith(const juce::String& key, const juce::var& value)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty(key, value);
    return juce::var(o);
}

juce::var failResponse(const char* reason)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", false);
    o->setProperty("reason", reason);
    return juce::var(o);
}

juce::var respondSetUiScale(UiScaleModel& model, const juce::String& role, const juce::Array<juce::var>& args,
                            bool& windowChanged)
{
    windowChanged = false;
    float scale = model.scale();
    if (!parseUiScaleArg(args, role, scale))
        return failResponse(Reason::BadScale);
    const auto outcome = model.apply(scale);
    if (!outcome.ok())
        return failResponse(outcome.reason);
    windowChanged = true;
    return okResponseWith("scale", scaleForJson(outcome.scale));
}

juce::var respondCommitUiScale(UiScaleModel& model, const std::function<void(float)>& persist)
{
    const auto outcome = model.commit();
    if (!outcome.ok())
        return failResponse(outcome.reason);
    if (persist)
        persist(outcome.scale);
    return okResponseWith("scale", scaleForJson(outcome.scale));
}

juce::var respondSetLang(const juce::Array<juce::var>& args, const std::function<void(const juce::String&)>& accepted)
{
    if (args.size() < 1 || !args[0].isString())
        return failResponse(Reason::BadLang);
    const juce::String code = args[0].toString();
    if (!isSupportedLang(code))
        return failResponse(Reason::BadLang);
    if (accepted)
        accepted(code);
    return okResponseWith("lang", code);
}

juce::String paintDeltaNote(const juce::var& payload, const char* fieldName)
{
    const auto delta = payload.getProperty(juce::Identifier(fieldName), juce::var());
    const bool usable =
        delta.isInt() || delta.isInt64() || (delta.isDouble() && std::isfinite(static_cast<double>(delta)));
    if (!usable)
        return " (no paint record)";
    // 先在双精度域夹到正负 60 秒再窄化：这是毫秒差，再大也没有诊断意义，
    // 而夹完必然落在整数可表示范围内，窄化不再触碰未定义行为
    const auto n = static_cast<int>(juce::jlimit(-60000.0, 60000.0, static_cast<double>(delta)));
    return juce::String(" (signal-firstPaint ") + (n >= 0 ? "+" : "") + juce::String(n) + " ms)";
}

} // namespace rinne::bridge
