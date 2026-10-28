// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的 state 胶水，规则见 InputStateGlue.h；分流、定型与保存的顺序在 core 的 state/StateGlueCommon.h。

#include "InputStateGlue.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rinne::input
{

namespace
{

// 参数表的一行里胶水要用的几项：ID、范围与默认值（明文值，与 state 里存的一致）
struct StateParamSpec
{
    const char* id;
    float lo;
    float hi;
    float def;
};

constexpr StateParamSpec kStateSpecs[] = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) \
    {#id, static_cast<float>(lo), static_cast<float>(hi), static_cast<float>(def)},
#include "ParamTable.h"
};

static_assert(sizeof(kStateSpecs) / sizeof(kStateSpecs[0]) == kParamCount, "kParamCount 要和参数表的行数一致");

// state 的 TailMode 与引擎的 TailMode 数值逐个相同（两边都是线格式的取值），这里按数值搬运
static_assert(static_cast<int>(state::TailMode::Auto) == static_cast<int>(engine::TailMode::kAuto));
static_assert(static_cast<int>(state::TailMode::Infinite) == static_cast<int>(engine::TailMode::kInfinite));
static_assert(static_cast<int>(state::TailMode::ThirtySeconds) == static_cast<int>(engine::TailMode::kThirtySeconds));

engine::TailMode toEngineTailMode(state::TailMode mode) noexcept
{
    switch (mode)
    {
    case state::TailMode::Infinite:
        return engine::TailMode::kInfinite;
    case state::TailMode::ThirtySeconds:
        return engine::TailMode::kThirtySeconds;
    case state::TailMode::Auto:
        break;
    }
    return engine::TailMode::kAuto; // 不认识的数值（只可能来自 editConfigs 的强制转换）按自动处理，与编码时的纠正一致
}

// 参数层的规则：缺失与非有限值取默认值，其余夹进范围。夹值放在换算之前：JUCE 的换算遇到越界值会断言。
float sanitized(const StateParamSpec& spec, const state::ParamValue* entry) noexcept
{
    if (entry == nullptr || !std::isfinite(entry->value))
        return spec.def;
    return std::clamp(entry->value, spec.lo, spec.hi);
}

} // namespace

InputStateGlue::InputStateGlue(juce::AudioProcessorValueTreeState& tree)
    : tree_(tree), document_(state::makeDefaultDocument(state::Role::Input))
{
    bool bound = true;
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        parameters_[i] = tree_.getParameter(kStateSpecs[i].id);
        bound = bound && parameters_[i] != nullptr;
    }
    // 参数树和参数表对不上是接线错误；取不到的参数在存取时按默认值处理，不会访问空指针
    jassert(bound);
    juce::ignoreUnused(bound);
    std::lock_guard<std::mutex> lock(mutex_);
    publishConfigsLocked();
}

InputStateGlue::SetStateResult InputStateGlue::setState(const void* data, int sizeInBytes)
{
    std::lock_guard<std::mutex> serial(loadMutex_);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t size = bytes != nullptr && sizeInBytes > 0 ? static_cast<std::size_t>(sizeInBytes) : 0u;

    ParamValues values{};
    bool applyParams = false;
    SetStateResult result;
    std::shared_ptr<const std::function<void()>> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state::StateDocument loaded;
        const state::glue::LoadStep step =
            state::glue::loadForApply(bytes, size, state::Role::Input, settlement_.settled(), writeBack_, loaded);
        if (step.mode != state::glue::ApplyMode::None)
        {
            values = paramValuesFrom(loaded.params);
            applying_ = landedValues(values);
            applyParams = true;
        }
        state::glue::mergeLoaded(document_, std::move(loaded), step.mode);
        if (step.mode == state::glue::ApplyMode::Full)
        {
            publishConfigsLocked();
            result.applied = document_.configs;
        }
        else if (step.mode == state::glue::ApplyMode::ParamsOnly)
        {
            stateSeq_.fetch_add(1u, std::memory_order_relaxed);
        }
        result.status = step.status;
        result.mode = step.mode;
        lastLoad_.store(static_cast<std::uint8_t>(step.status), std::memory_order_relaxed);
        holding_.store(writeBack_.holding(), std::memory_order_relaxed);
        hook = confirmClaim_;
    }
    if (applyParams)
    {
        // 写完或中途抛出都清掉，之后的 getState 回到读参数树
        struct ClearApplying
        {
            InputStateGlue& glue;
            ~ClearApplying()
            {
                std::lock_guard<std::mutex> lock(glue.mutex_);
                glue.applying_.reset();
            }
        } clear{*this};
        writeParams(values);
    }
    callHook(hook);
    return result;
}

void InputStateGlue::getState(juce::MemoryBlock& destData)
{
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<const std::function<void()>> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 在锁里判：applying_ 在 setState 开始写之前置上、写完之后才清掉，没置上时参数树不会正被 setState 写到一半
        const ParamValues current = applying_.has_value() ? *applying_ : currentParamValues();
        std::array<state::glue::KnownParam, kParamCount> known{};
        for (std::size_t i = 0; i < kParamCount; ++i)
            known[i] = {kStateSpecs[i].id, current[i]};
        const state::glue::SaveResult saved =
            state::glue::bytesForSave(document_, writeBack_, known.data(), known.size(), bytes);
        if (saved.dropped != 0u)
            dropped_.fetch_add(static_cast<std::uint32_t>(saved.dropped), std::memory_order_relaxed);
        if (!saved.ok)
            encodeFailures_.fetch_add(1u, std::memory_order_relaxed);
        hook = confirmClaim_;
    }
    destData.replaceAll(bytes.data(), bytes.size()); // 编码失败时 bytes 为空，交出空块
    callHook(hook);
}

bool InputStateGlue::markUserAction()
{
    std::shared_ptr<const std::function<void()>> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!settlement_.markUserAction())
            return false;
        hook = confirmClaim_;
    }
    callHook(hook);
    return true;
}

bool InputStateGlue::isSettled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return settlement_.settled();
}

void InputStateGlue::markStateDirty()
{
    tree_.processor.updateHostDisplay(
        juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true).withProgramChanged(true));
}

state::Configs InputStateGlue::configs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return document_.configs;
}

void InputStateGlue::setConfirmClaimHook(std::function<void()> hook)
{
    auto shared = hook ? std::make_shared<const std::function<void()>>(std::move(hook)) : nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    confirmClaim_ = std::move(shared);
}

std::optional<state::StateLoadStatus> InputStateGlue::lastLoadStatus() const noexcept
{
    const std::uint8_t value = lastLoad_.load(std::memory_order_relaxed);
    if (value == kNoLoad)
        return std::nullopt;
    return static_cast<state::StateLoadStatus>(value);
}

engine::TailMode InputStateGlue::tailMode() const noexcept
{
    return static_cast<engine::TailMode>(tailMode_.load(std::memory_order_relaxed));
}

void InputStateGlue::snapshot(engine::EngineParams& out) const noexcept
{
    out.stateSeq = stateSeq();
    out.guardOn = guardOn();
    out.resetOnPlay = resetOnPlay();
    out.tailMode = tailMode();
}

InputStateGlue::ParamValues InputStateGlue::paramValuesFrom(const state::Params& params) const noexcept
{
    ParamValues values{};
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        const state::ParamValue* entry = nullptr;
        for (const auto& candidate : params.entries)
        {
            if (candidate.id == kStateSpecs[i].id)
            {
                entry = &candidate;
                break;
            }
        }
        values[i] = sanitized(kStateSpecs[i], entry);
    }
    return values;
}

// 参数当前的明文值：从参数自己的归一化值换回来，落在步进格上，与宿主、引擎看到的是同一个值
InputStateGlue::ParamValues InputStateGlue::currentParamValues() const
{
    ParamValues values{};
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        const auto* parameter = parameters_[i];
        values[i] = parameter != nullptr ? parameter->convertFrom0to1(parameter->getValue()) : kStateSpecs[i].def;
    }
    return values;
}

// writeParams 写完之后参数树里的明文值：落在步进格上，与 writeParams 判「没变」用的是同一个换算
InputStateGlue::ParamValues InputStateGlue::landedValues(const ParamValues& values) const
{
    ParamValues landed{};
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        const auto* parameter = parameters_[i];
        landed[i] =
            parameter != nullptr ? parameter->convertFrom0to1(parameter->convertTo0to1(values[i])) : kStateSpecs[i].def;
    }
    return landed;
}

// 没变的参数不写，免得给宿主多发一串参数通知
void InputStateGlue::writeParams(const ParamValues& values)
{
    for (std::size_t i = 0; i < kParamCount; ++i)
    {
        auto* parameter = parameters_[i];
        if (parameter == nullptr)
            continue;
        const float target = parameter->convertTo0to1(values[i]);
        if (juce::exactlyEqual(parameter->convertFrom0to1(parameter->getValue()), parameter->convertFrom0to1(target)))
            continue;
        parameter->setValueNotifyingHost(target);
    }
}

void InputStateGlue::publishConfigsLocked() noexcept
{
    const state::Configs& configs = document_.configs;
    guardOn_.store(configs.guardOn, std::memory_order_relaxed);
    resetOnPlay_.store(configs.resetOnPlay, std::memory_order_relaxed);
    tailMode_.store(static_cast<std::uint8_t>(toEngineTailMode(configs.tailMode)), std::memory_order_relaxed);
}

void InputStateGlue::callHook(const std::shared_ptr<const std::function<void()>>& hook)
{
    if (hook != nullptr && *hook)
        (*hook)();
}

} // namespace rinne::input
