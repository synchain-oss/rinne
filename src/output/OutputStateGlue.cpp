// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的 state 胶水，规则见 OutputStateGlue.h；分流、定型与保存的顺序在 core 的 state/StateGlueCommon.h。

#include "OutputStateGlue.h"

#include <vector>

namespace rinne::output
{

OutputStateGlue::OutputStateGlue(juce::AudioProcessor& processor)
    : processor_(processor), document_(state::makeDefaultDocument(state::Role::Output))
{
    std::lock_guard<std::mutex> lock(mutex_);
    publishConfigsLocked();
}

OutputStateGlue::SetStateResult OutputStateGlue::setState(const void* data, int sizeInBytes)
{
    std::lock_guard<std::mutex> serial(loadMutex_);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t size = bytes != nullptr && sizeInBytes > 0 ? static_cast<std::size_t>(sizeInBytes) : 0u;

    SetStateResult result;
    std::shared_ptr<const std::function<void()>> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state::StateDocument loaded;
        const state::glue::LoadStep step =
            state::glue::loadForApply(bytes, size, state::Role::Output, settlement_.settled(), writeBack_, loaded);
        state::glue::mergeLoaded(document_, std::move(loaded), step.mode);
        if (step.mode == state::glue::ApplyMode::Full)
        {
            publishConfigsLocked();
            result.applied = document_.configs;
        }
        result.status = step.status;
        result.mode = step.mode;
        lastLoad_.store(static_cast<std::uint8_t>(step.status), std::memory_order_relaxed);
        holding_.store(writeBack_.holding(), std::memory_order_relaxed);
        hook = confirmClaim_;
    }
    callHook(hook);
    return result;
}

void OutputStateGlue::getState(juce::MemoryBlock& destData)
{
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<const std::function<void()>> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const state::glue::SaveResult saved = state::glue::bytesForSave(document_, writeBack_, nullptr, 0u, bytes);
        if (!saved.ok)
            encodeFailures_.fetch_add(1u, std::memory_order_relaxed);
        hook = confirmClaim_;
    }
    destData.replaceAll(bytes.data(), bytes.size()); // 编码失败时 bytes 为空，交出空块
    callHook(hook);
}

bool OutputStateGlue::markUserAction()
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

bool OutputStateGlue::isSettled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return settlement_.settled();
}

void OutputStateGlue::markStateDirty()
{
    processor_.updateHostDisplay(
        juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true).withProgramChanged(true));
}

state::Configs OutputStateGlue::configs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return document_.configs;
}

void OutputStateGlue::setConfirmClaimHook(std::function<void()> hook)
{
    auto shared = hook ? std::make_shared<const std::function<void()>>(std::move(hook)) : nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    confirmClaim_ = std::move(shared);
}

std::optional<state::StateLoadStatus> OutputStateGlue::lastLoadStatus() const noexcept
{
    const std::uint8_t value = lastLoad_.load(std::memory_order_relaxed);
    if (value == kNoLoad)
        return std::nullopt;
    return static_cast<state::StateLoadStatus>(value);
}

state::TailMode OutputStateGlue::tailMode() const noexcept
{
    return static_cast<state::TailMode>(tailMode_.load(std::memory_order_relaxed));
}

void OutputStateGlue::publishConfigsLocked() noexcept
{
    // 不认识的数值（只可能来自 editConfigs 的强制转换）按自动处理，与编码时的纠正一致
    const state::TailMode mode = document_.configs.tailMode;
    const bool known =
        mode == state::TailMode::Auto || mode == state::TailMode::Infinite || mode == state::TailMode::ThirtySeconds;
    tailMode_.store(static_cast<std::uint8_t>(known ? mode : state::TailMode::Auto), std::memory_order_relaxed);
}

void OutputStateGlue::callHook(const std::shared_ptr<const std::function<void()>>& hook)
{
    if (hook != nullptr && *hook)
        (*hook)();
}

} // namespace rinne::output
