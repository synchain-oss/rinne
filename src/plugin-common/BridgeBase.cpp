// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "BridgeBase.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

#include "diag/DiagReport.h"

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

juce::var respondNotReady()
{
    return failResponse(Reason::NotReady);
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

bool jsBoolArg(const juce::Array<juce::var>& args, bool& out)
{
    if (args.size() < 1 || !args[0].isBool())
        return false;
    out = static_cast<bool>(args[0]);
    return true;
}

bool parseRulesAckArg(const juce::Array<juce::var>& args, int& outVersion)
{
    if (args.size() < 1)
        return false;
    const auto& v = args[0];
    constexpr auto kMax = static_cast<juce::int64>(std::numeric_limits<int>::max());
    if (v.isInt() || v.isInt64())
    {
        const auto n = static_cast<juce::int64>(v);
        if (n < 0 || n > kMax)
            return false;
        outVersion = static_cast<int>(n);
        return true;
    }
    if (v.isDouble())
    {
        // 先判有限与范围再窄化：非有限或越界的双精度转整数是未定义行为
        const double d = static_cast<double>(v);
        if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(kMax) || std::floor(d) != d)
            return false;
        outVersion = static_cast<int>(d);
        return true;
    }
    return false;
}

bool parseGroupArg(const juce::Array<juce::var>& args, int& outGroup)
{
    if (args.size() < 1 || !args[0].isString())
        return false;
    const juce::String g = args[0].toString();
    if (g.length() != 1 || g[0] < 'A' || g[0] > 'H')
        return false;
    outGroup = static_cast<int>(g[0] - 'A') + 1;
    return true;
}

juce::String groupLetter(std::uint32_t group)
{
    if (group < 1u || group > 8u)
        return {};
    return juce::String::charToString(static_cast<juce::juce_wchar>('A' + static_cast<int>(group) - 1));
}

namespace
{
// 不分大小写比较 ASCII 串
bool equalsAsciiNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

bool isPathChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

// 路径里有没有 . 或 .. 段
bool hasDotSegment(std::string_view path)
{
    std::size_t start = 0;
    while (start <= path.size())
    {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto segment = path.substr(start, end - start);
        if (segment == "." || segment == "..")
            return true;
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return false;
}
} // namespace

bool isAllowedExternalUrl(const juce::String& url)
{
    // 白名单里的地址都很短；上限只挡掉异常长的输入
    constexpr std::size_t kMaxLength = 2048;
    const std::string raw = url.toStdString();
    if (raw.size() > kMaxLength)
        return false;
    constexpr std::string_view kScheme = "https://";
    const std::string_view text(raw);
    if (!equalsAsciiNoCase(text.substr(0, kScheme.size()), kScheme))
        return false;
    const std::string_view rest = text.substr(kScheme.size());
    const auto slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    const std::string_view path = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
    if (!std::all_of(path.begin(), path.end(), isPathChar) || hasDotSegment(path))
        return false;
    if (equalsAsciiNoCase(authority, "synchain.ca"))
        return true;
    if (!equalsAsciiNoCase(authority, "github.com"))
        return false;
    constexpr std::string_view kRepo = "/synchain-oss/rinne";
    return path == kRepo ||
           (path.size() > kRepo.size() && path.substr(0, kRepo.size()) == kRepo && path[kRepo.size()] == '/');
}

juce::var respondOpenExternal(const juce::Array<juce::var>& args,
                              const std::function<bool(const juce::String&)>& launch)
{
    if (args.size() < 1 || !args[0].isString() || !isAllowedExternalUrl(args[0].toString()))
        return failResponse(Reason::NotAllowed);
    if (!launch || !launch(args[0].toString()))
        return failResponse(Reason::Unavailable);
    return okResponse();
}

namespace
{
juce::var copyFailure(const char* reason)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", false);
    o->setProperty("copied", false);
    o->setProperty("reason", reason);
    return juce::var(o);
}
} // namespace

juce::var respondCopyDiagnostics(const juce::Array<juce::var>& args, const juce::String& text,
                                 const std::function<bool(const juce::String&)>& write)
{
    const bool commit = args.size() >= 1 && args[0].isBool() && static_cast<bool>(args[0]);
    if (text.isEmpty())
        return copyFailure(Reason::NotReady);
    if (commit && (!write || !write(text)))
        return copyFailure(Reason::Clipboard);
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty("copied", commit);
    o->setProperty("text", text);
    return juce::var(o);
}

juce::String uiDiagLine(const juce::String& webview, float scale, const juce::String& lang)
{
    std::string label = diag::sanitizeText(webview.toStdString());
    std::replace(label.begin(), label.end(), ' ', '-');
    if (label.empty())
        label = "unknown";
    // 两位小数的定点：先夹到合法档位范围，再按百分数取整
    const int percent = juce::roundToInt(clampUiScale(scale) * 100.0f);
    const juce::String hundredths = juce::String(percent % 100).paddedLeft('0', 2);
    return "ui: webview=" + juce::String(label) + " scale=" + juce::String(percent / 100) + "." + hundredths +
           " lang=" + normalizeLang(lang);
}

juce::String withUiDiagLine(const std::string& coreText, const juce::String& uiLine)
{
    if (coreText.empty())
        return {};
    juce::String text = juce::String::fromUTF8(coreText.data(), static_cast<int>(coreText.size()));
    if (!text.endsWithChar('\n'))
        text << "\n";
    return text + uiLine;
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
