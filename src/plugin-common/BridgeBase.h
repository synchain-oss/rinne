// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
//
// 两个插件共用的桥常量与助手：函数名、事件名、首帧预置键、缩放夹取与档位换算、回执构造。
// 只放两端共用的几个名字（首帧、语言、缩放）；各角色专属的函数、事件与参数 ID 在 src/{input,output}/*BridgeApi.h。
// 只依赖 juce_core 与设计盒常量，不碰任何组件，所以宿主测试可以直接编它。
// 线程：全部纯计算，任意线程可用；UiScaleModel 非线程安全，只在消息线程用。
#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "util/DesignBox.h"

namespace rinne::bridge
{
// 原生推给界面的事件名
namespace Event
{
inline constexpr const char* State = "rinne.state";
} // namespace Event

// 界面调原生的函数名（两个角色共用的几个；原生侧用 withNativeFunction 注册）
namespace Fn
{
inline constexpr const char* RequestInitialState = "requestInitialState";
inline constexpr const char* SetLang = "setLang";
inline constexpr const char* SetUiScale = "setUiScale";
inline constexpr const char* CommitUiScale = "commitUiScale";
} // namespace Fn

// withInitialisationData 的预置键：页面首帧同步可读，省一次异步往返
namespace Init
{
inline constexpr const char* Version = "version";
inline constexpr const char* Lang = "lang";
inline constexpr const char* UiScale = "uiScale";
inline constexpr const char* Role = "role"; // "input" 或 "output"
} // namespace Init

// 失败回执里的 reason。名字与 docs/UI_BRIDGE.md 一致；scale-locked 是缩放锁定时的拒绝，契约文档尚未登记
namespace Reason
{
inline constexpr const char* BadScale = "bad-scale";
inline constexpr const char* BadLang = "bad-lang";
inline constexpr const char* NoPending = "no-pending";
inline constexpr const char* ScaleLocked = "scale-locked";
} // namespace Reason

// 缩放的硬边界；可选档位的真源是设计盒的档位表
namespace plugin
{
inline constexpr float DefaultUiScale = 1.0f;
inline constexpr float MinUiScale = 0.33f;
inline constexpr float MaxUiScale = 3.0f;
} // namespace plugin

float clampUiScale(float scale);

// 百分比档位的夹取，边界就是上面那对常量乘 100，不要再写第三份边界。
// 入参用 64 位：工程文件里的缩放是 u32，先转 int 再夹会把 4294967295 折成 -1，那样落在下界是溢出撞上的，不是范围校验。
// 放在头里：纯计算，UiDefaultsStore 和宿主测试都用它，不必为一条夹取给每个目标补链接依赖。
inline int clampUiScalePercent(std::int64_t percent)
{
    const auto lo = static_cast<std::int64_t>(juce::roundToInt(plugin::MinUiScale * 100.0f));
    const auto hi = static_cast<std::int64_t>(juce::roundToInt(plugin::MaxUiScale * 100.0f));
    return static_cast<int>(juce::jlimit(lo, hi, percent));
}

// 界面语言只认这三个，其余一律不接受。落盘侧、桥侧共用这一张表，加语言时只改这里
inline bool isSupportedLang(const juce::String& code)
{
    return code == "zh" || code == "en" || code == "fr";
}

// 不认识的语言码回落到中文（页面的语言表同口径）
inline juce::String normalizeLang(const juce::String& code)
{
    return isSupportedLang(code) ? code : juce::String("zh");
}

struct DesignBoxSize
{
    int width = 0;
    int height = 0;
};

// 设计盒按档位缩放后的窗口尺寸：固定设计盒加 CSS zoom，窗口大小跟着档位走。
// role 取 "input" 或 "output"，其余值回落 Output。
inline DesignBoxSize designBoxWindowSize(const juce::String& role, float scale)
{
    int w = design::kOutputDesignW;
    int h = design::kOutputDesignH;
    if (role == "input")
    {
        w = design::kInputDesignW;
        h = design::kInputDesignH;
    }
    return {juce::roundToInt(static_cast<float>(w) * scale), juce::roundToInt(static_cast<float>(h) * scale)};
}

namespace detail
{
// 档位表升序。显示器放得下 requested 就原样用它；放不下就取放得下的最大档，一档都放不下取最小档
template<std::size_t N>
float fitToPresets(const std::array<double, N>& presets, int boxW, int boxH, float requested, int availW, int availH)
{
    const auto fits = [&](double f) {
        return juce::roundToInt(boxW * f) + design::kHostFrameW <= availW &&
               juce::roundToInt(boxH * f) + design::kHostFrameH <= availH;
    };
    if (fits(static_cast<double>(requested)))
        return requested;
    for (auto it = presets.rbegin(); it != presets.rend(); ++it)
        if (fits(*it))
            return static_cast<float>(*it);
    return static_cast<float>(presets.front());
}

// percent 恰好是某一档的百分数就返回那一档，否则返回 0
template<std::size_t N>
float snapPercentToPreset(const std::array<double, N>& presets, int percent)
{
    for (const double f : presets)
        if (juce::roundToInt(f * 100.0) == percent)
            return static_cast<float>(f);
    return 0.0f;
}
} // namespace detail

// 开窗夹取：窗口尺寸（设计盒乘档位，再加宿主给插件窗加的框）要放进显示器的可用区。
// availW、availH 是可用区尺寸，不大于 0 表示不知道，原样返回。只算本次开窗用的档位，不写回任何已保存的值。
inline float fitScaleToDisplay(const juce::String& role, float requested, int availW, int availH)
{
    if (availW <= 0 || availH <= 0)
        return requested;
    if (role == "input")
        return detail::fitToPresets(design::kInputPresets, design::kInputDesignW, design::kInputDesignH, requested,
                                    availW, availH);
    return detail::fitToPresets(design::kOutputPresets, design::kOutputDesignW, design::kOutputDesignH, requested,
                                availW, availH);
}

// 全局默认里存的百分比换成档位。0、没存过、不是本角色某一档的百分数，都当没存过，取默认档位
inline float scaleFromStoredPercent(const juce::String& role, int percent)
{
    const float snapped = role == "input" ? detail::snapPercentToPreset(design::kInputPresets, percent)
                                          : detail::snapPercentToPreset(design::kOutputPresets, percent);
    return snapped > 0.0f ? snapped : plugin::DefaultUiScale;
}

// 一次缩放操作的结果。reason 为空表示成功
struct ScaleOutcome
{
    const char* reason = nullptr;
    float scale = plugin::DefaultUiScale;
    bool ok() const noexcept { return reason == nullptr; }
};

// 窗口缩放的状态：已生效的档位、待确认的预览、锁定。窗口尺寸、落盘与回执都由宿主按这里的结果去做。
//   - 页面选档（setUiScale）只是预览：档位马上生效，但要等确认（commitUiScale）才算数；
//   - 确认成功才返回要落盘的档位，开窗时按显示器夹取的档位不会被写回；
//   - 锁定时窗口钉在 100%，选档与确认都被拒，已保存的档位不动。
class UiScaleModel
{
public:
    UiScaleModel(const juce::String& role, float saved) noexcept
        : role_(role), requested_(clampUiScale(saved)), applied_(requested_)
    {
    }

    // 窗口当前该用的档位
    float scale() const noexcept { return locked_ ? plugin::DefaultUiScale : applied_; }
    // 构造时要求的档位（已保存的那一份），夹取不改它
    float requested() const noexcept { return requested_; }
    bool pending() const noexcept { return pending_; }
    bool locked() const noexcept { return locked_; }

    // 开窗：按显示器可用区夹取。不产生待确认的预览。返回夹取后的档位
    float fitToDisplay(int availW, int availH)
    {
        applied_ = clampUiScale(fitScaleToDisplay(role_, requested_, availW, availH));
        pending_ = false;
        return scale();
    }

    // 锁定或解锁。锁定会丢掉待确认的预览；解锁后回到锁定前生效的档位
    void setLocked(bool shouldLock) noexcept
    {
        locked_ = shouldLock;
        if (locked_)
            pending_ = false;
    }

    // 页面选档（档位已经过档位表校验）。锁定时拒绝，其余成功并进入待确认
    ScaleOutcome apply(float next) noexcept
    {
        if (locked_)
            return {Reason::ScaleLocked, scale()};
        applied_ = clampUiScale(next);
        pending_ = true;
        return {nullptr, applied_};
    }

    // 页面确认保留。成功时 scale 是该落盘的档位；锁定或没有待确认的预览时拒绝，调用方不得落盘
    ScaleOutcome commit() noexcept
    {
        if (locked_)
            return {Reason::ScaleLocked, scale()};
        if (!pending_)
            return {Reason::NoPending, scale()};
        pending_ = false;
        return {nullptr, applied_};
    }

private:
    juce::String role_;
    float requested_;
    float applied_;
    bool pending_ = false;
    bool locked_ = false;
};

// 缩放档位换成要落盘的百分比（0.65 变 65）。与 scaleFromStoredPercent 的换算互为逆运算
inline int scaleToPercent(float scale)
{
    return juce::roundToInt(scale * 100.0f);
}

// 档位发给页面时的取值：单精度的 0.65 直接转双精度会变成 0.6499999761...，页面按档位表做相等比较会认不出来。
// 四位小数足够区分所有档位
inline double scaleForJson(float scale)
{
    return std::round(static_cast<double>(scale) * 10000.0) / 10000.0;
}

// 首帧预置与状态快照共用的基础信息
struct UiSeed
{
    juce::String role; // "input" 或 "output"
    juce::String version;
    juce::String lang; // zh、en、fr
    float uiScale = plugin::DefaultUiScale;
};

// 状态快照里来自全局默认存储的几项（本机设置，跨工程）
struct UiFlags
{
    bool langChosen = false; // 用户是否显式选过语言；为假时页面显示语言选择卡
    int rulesAck = 0; // 已确认的规则页版本，0 表示没确认过
    bool guideSeen = false; // 本角色的引导是否已看
    bool diagLog = false; // 诊断日志开关
};

// 返回 (键, 值) 列表，键取 Init::*，顺序与宿主装配首帧预置的顺序一致。
// 快照由同一份列表构造，首帧预置与快照的键值不会各写各的
std::vector<std::pair<juce::String, juce::var>> buildUiSeedPairs(const UiSeed& seed);

// 状态快照的共用部分（rinne.state 的共用字段）；各角色在此之上补自己的字段，帧序号由发送方加
juce::var buildUiSnapshot(const UiSeed& seed, const UiFlags& flags);

// setUiScale 的参数校验：至少一个参数、是数值、且是该角色档位表里的一档。
// 档位表里是 double，用 double 比较，免得 0.33 这类值被精度误判。命中返回 true 并写出档位，否则返回 false
bool parseUiScaleArg(const juce::Array<juce::var>& args, const juce::String& role, float& outScale);

// 严格的布尔取值：只接受 bool 和整数（非零为真）。字符串一律不接受：
// juce::var 的布尔转换对非空字符串给真，"false" 会被当成 true
inline bool strictBool(const juce::var& v, bool& out)
{
    if (v.isBool())
    {
        out = static_cast<bool>(v);
        return true;
    }
    if (v.isInt() || v.isInt64())
    {
        out = static_cast<juce::int64>(v) != 0;
        return true;
    }
    return false;
}

// 回执：{ ok: true }
juce::var okResponse();
// {ok: true, key: value}
juce::var okResponseWith(const juce::String& key, const juce::var& value);
// {ok: false, reason}
juce::var failResponse(const char* reason);

// 桥函数的回执逻辑（纯逻辑）：宿主只负责在成功后把动作落到窗口与存储上。

// setUiScale：参数不在档位表回 bad-scale；锁定时回 scale-locked；成功回 {ok, scale} 并让 windowChanged 为真
juce::var respondSetUiScale(UiScaleModel& model, const juce::String& role, const juce::Array<juce::var>& args,
                            bool& windowChanged);

// commitUiScale：成功时先调用 persist 落盘再回 {ok, scale}；没有待确认的预览回 no-pending，锁定回 scale-locked，
// 这两种情况都不调用 persist
juce::var respondCommitUiScale(UiScaleModel& model, const std::function<void(float)>& persist);

// setLang：参数必须是字符串且在白名单内（zh、en、fr），否则回 bad-lang 且不调用 accepted；成功回 {ok, lang}
juce::var respondSetLang(const juce::Array<juce::var>& args, const std::function<void(const juce::String&)>& accepted);

// 首帧信号载荷里的诊断字段，整理成日志后缀。字段缺失、不是数、或是无穷与非数时给 " (no paint record)"；
// 否则给 " (signal-firstPaint +N ms)"，N 夹在正负 60 秒内。
// 载荷跨了页面到原生的边界，所以先校验再用：非有限的双精度直接转整数是未定义行为
juce::String paintDeltaNote(const juce::var& payload, const char* fieldName);

} // namespace rinne::bridge
