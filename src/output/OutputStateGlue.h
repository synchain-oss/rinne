// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的 state 胶水：宿主的 setStateInformation / getStateInformation 经它读写 RNST state（契约
// docs/contracts/STATE_SCHEMA.md）。Output 没有参数，state 里只有 CFGS 的五个键（group、group_origin、tail_mode、
// ui_scale、ui_lang），配置的唯一真源在 Input。加载结果的分流、定型与保存的顺序和 Input 共用 core 的
// state/StateGlueCommon.h；两个壳互不 include。
//
// setState（宿主调 setStateInformation 的那个线程：消息线程或任何非实时线程）：loadState → 写回保留 → 分流。
//   · 未定型、成功读入（Ok / Migrated）：全量应用，CFGS 全部生效，未知键与未知块原样留着、
//     下次保存写回。以最后一份为准。
//   · 已定型、成功读入：什么都不改（Output 没有参数可取）。写回保留照样按结果更新，回调照样调用。
//   · RejectedNewer：不应用，写回保留接管原字节，保存时原样交回，直到下一份成功读入的 state 取代它。
//     保留期间记下那份 state 头里的 abi（heldNewerAbi），界面据此提示「工程来自更新版本」。
//   · Empty / Corrupt / IgnoredProbe：什么都不动，保持当前（不是回到默认）。
// getState：正在保留原字节时原样交出；否则编码当前的文档。编码失败交出空块，并累加 encodeFailures。
//
// 定型（markUserAction）：只由界面上真实的用户点击或拖动触发，一次性，不可撤销；页面自己发起的调用不算。
//   算定型：setGroup；resolveTakeover（accept = true）；repairPair；setTailMode；commitUiScale。
//   不算：  requestInitialState；setLang、setRulesAck、setGuideSeen、setDiagLog（写本机的全局设置，不进工程）；
//           setUiScale（10 秒内可能回退，等 commitUiScale 才算）；resolveTakeover（accept = false）；
//           copyDiagnostics、openExternal、showLicense；任何认领。
//
// 认领转正（setConfirmClaimHook）：每次 getState、每次 setState 处理完（不论结果）、定型的那一刻，各调一次注入的
//   回调，在胶水的锁外调用。策略由配对层实现、接线时注入。
//
// markStateDirty：用户改了设置（改组、修复配对等）之后由界面调用。同时带 nonParameterStateChanged（VST3）与
//   programChanged（AU 只认这一个）；program 数保持 1，处理器的 getProgramName(0) 要返回固定的非空名字（接线时做）。
//
// 线程：setState、getState、markUserAction、editConfigs 可在任何非实时线程上调用，内部有锁；音频线程只读 tailMode。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateGlueCommon.h"
#include "state/StateMigration.h"

namespace rinne::output
{

class OutputStateGlue
{
public:
    // processor 要比胶水活得久：markStateDirty 经它通知宿主。
    explicit OutputStateGlue(juce::AudioProcessor& processor);

    // ---- 宿主 ----
    // setState 这一次的结果，都在锁里定下。接线方（处理器）直接用它：是不是全量应用看 mode，交给会合与外壳的组号与
    // tail 取 applied，不另读 isSettled、不另判，也不在返回之后再读 configs()（那时别的线程可能已经定型或改过设置）。
    struct SetStateResult
    {
        state::StateLoadStatus status = state::StateLoadStatus::Empty; // 这一次的读入结果（同时写进 lastLoadStatus）
        state::glue::ApplyMode mode = state::glue::ApplyMode::None; // 这一次实际的应用方式（applyModeFor 的口径）
        std::optional<state::Configs> applied; // 只在 mode 是 Full 时有：这一次换上的那份设置的副本
    };
    SetStateResult setState(const void* data, int sizeInBytes);
    void getState(juce::MemoryBlock& destData);

    // ---- 界面（桥层） ----
    // 用户动作：第一次调用让实例定型并返回 true，之后返回 false。清单见文件头。
    bool markUserAction();
    bool isSettled() const;
    void markStateDirty();

    // 当前的设置（一份拷贝）。
    state::Configs configs() const;

    // 改设置：在锁里对设置的一份拷贝调用 edit(state::Configs&)，做完整份换上，并刷新 tailMode。不定型、不标脏。
    // unknown 是读入时保留的未知键（含只有 Input 才认识的键），原样写回；edit 里不要往 unknown 放这一端认识的键，
    // 否则这份文档编不出来，getState 交出空块并计一次 encodeFailures。
    template<typename Edit>
    void editConfigs(Edit&& edit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state::Configs next = document_.configs;
        std::forward<Edit>(edit)(next);
        document_.configs = std::move(next);
        publishConfigsLocked();
    }

    // 认领转正的回调；传空的 std::function 就是不接。接线时在宿主第一次调用 setState / getState 之前设好。
    void setConfirmClaimHook(std::function<void()> hook);

    // ---- 状态位（任何线程，无锁） ----
    std::optional<state::StateLoadStatus> lastLoadStatus() const noexcept;
    bool holdingNewerState() const noexcept { return holding_.load(std::memory_order_relaxed); }
    // 正在保留的那份更新版本 state 头里的 abi；没有保留时为 0
    std::uint32_t heldNewerAbi() const noexcept { return heldAbi_.load(std::memory_order_relaxed); }
    std::uint32_t encodeFailures() const noexcept { return encodeFailures_.load(std::memory_order_relaxed); }

    // ---- 音频线程（无锁） ----
    state::TailMode tailMode() const noexcept;

private:
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "音频线程读尾长模式要求 std::atomic<uint8_t> 无锁");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    void publishConfigsLocked() noexcept;
    static void callHook(const std::shared_ptr<const std::function<void()>>& hook);

    static constexpr std::uint8_t kNoLoad = 0xffu; // lastLoad_ 的初值：还没有过 setState

    juce::AudioProcessor& processor_;

    std::mutex loadMutex_; // 串起整个 setState
    mutable std::mutex mutex_; // 管下面四项；持锁时不调用宿主或回调
    state::StateDocument document_;
    state::RawWriteBack writeBack_;
    state::glue::Settlement settlement_;
    std::shared_ptr<const std::function<void()>> confirmClaim_;

    std::atomic<std::uint8_t> tailMode_{0};
    std::atomic<std::uint8_t> lastLoad_{kNoLoad};
    std::atomic<bool> holding_{false};
    std::atomic<std::uint32_t> heldAbi_{0};
    std::atomic<std::uint32_t> encodeFailures_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputStateGlue)
};

} // namespace rinne::output
