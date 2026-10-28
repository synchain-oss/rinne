// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的 state 胶水：宿主的 setStateInformation / getStateInformation 经它读写 RNST state（契约
// docs/contracts/STATE_SCHEMA.md）。它把读入的参数写进参数树、把设置交给处理器与引擎，并按「定型」规则决定应用多少。
// 加载结果的分流、定型与保存的顺序和 Output 共用 core 的 state/StateGlueCommon.h，这里只做 Input 自己的映射。
//
// setState（宿主调 setStateInformation 的那个线程：消息线程或任何非实时线程）：loadState → 写回保留 → 分流。
//   · 未定型、成功读入（Ok / Migrated）：全量应用。PRMS 里认识的 11 个参数写进参数树：缺的取参数表默认值，
//     非有限值取默认值，其余夹进范围；不认识的 id 原样留在文档里，下次保存原样写回。CFGS 全部生效（组号、组来源、
//     reset_on_play、tail_mode、delay_unit、fb_unit、guard_on、ui_scale、ui_lang、preset_name、preset_dirty）。
//     以最后一份为准，不加任何命令序号：这是工程恢复。
//   · 已定型、成功读入：只改参数（规则同上），设置一律不动（含组号），stateSeq 恰好加一，引擎据此按 SetState 淡入。
//     先加序号、再写参数：淡入先开始，参数落在淡入里。
//   · RejectedNewer：不应用。写回保留接管原字节，保存时原样交回，直到下一份成功读入的 state 取代它；保留期间参数与
//     设置的改动都不进 state，界面要常驻说明（读 holdingNewerState）。
//   · Empty / Corrupt / IgnoredProbe：什么都不动，保持当前（不是回到默认）。Corrupt 的提示由界面按 lastLoadStatus 给。
//   · limiter 与 ceiling 是参数，随工程保存，加载时按 state 里的值，不强制复位。
//   · 不依赖 setState 与 prepareToPlay 的先后：参数层不读采样率，setState 在 prepare 之前来，参数照样生效。
// getState：正在保留原字节时原样交出；否则把参数树里 11 个参数的当前值写进文档、再裁剪、再编码。为压进上限丢掉的
//   未知 id 条数累加进 droppedUnknownParams（处理器汇进诊断文本）；编码失败交出空块，并累加 encodeFailures。
//   setState 正在锁外写参数树时（宿主收到参数通知可能回头调 getState），交出这次 setState 要写的值，不读写到一半的
//   参数树：否则交出的是新旧参数混在一起的一份，旧值还会被 set 回文档。
//
// 定型（markUserAction）：只由界面上真实的用户点击或拖动触发，一次性，不可撤销。桥层处理下面「算定型」的调用时
// 调用它；页面自己发起的调用不算（桥调用带一个「用户发起」标志，由页面在真实的点击或拖动处理里置位）。
//   算定型：beginParamGesture / setParam / endParamGesture（界面发起的参数改动）；
//           setLimiter、setGroup、moveToFreeGroup；resolveTakeover（accept = true）；commitUiScale；
//           setGuardOn、setResetOnPlay、setTailMode；loadPreset、savePreset。
//   不算：  requestInitialState；setLang、setRulesAck、setGuideSeen、setDiagLog（写本机的全局设置，不进工程）；
//           setUiScale（10 秒内可能回退，等 commitUiScale 才算）；resolveTakeover（accept = false）；
//           deletePreset（只动文件，不动本实例）；dismissHint（不进工程）；copyDiagnostics、openExternal、showLicense；
//           panic、resumeLoop（安全动作，不是设置）；宿主下发的参数与自动化，以及任何认领。
//
// 认领转正（setConfirmClaimHook）：每次 getState、每次 setState 处理完（不论结果）、定型的那一刻，各调一次注入的
//   回调。转正的策略不在这里，由配对层实现、接线时注入。回调在胶水的锁外调用。
//
// markStateDirty：用户改了不是参数的设置（改组、修复配对、保存预设等）之后由界面调用，告诉宿主工程有改动。
//   同时带 nonParameterStateChanged（VST3 据此标脏）与 programChanged（AU 只认这一个：据此发 ClassInfo 与
//   PresentPreset）；program 数保持 1。AU 宿主会因此重读一次当前 program 名，所以处理器的 getProgramName(0) 要返回
//   固定的非空名字，接线时在处理器里做。参数改动不走这里，宿主有参数自己的通知。
//
// 线程：setState、getState、markUserAction、editConfigs 可在任何非实时线程上调用，内部有锁；宿主不会并发调两次
//   setState，胶水仍把整个 setState 串起来。参数树的写入在锁外做（宿主收到参数通知时可能回头调 getState，
//   这期间交出什么见上面 getState 一条）。
//   音频线程只读这里的原子量（snapshot，或 stateSeq、guardOn、resetOnPlay、tailMode），不加锁、不分配。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "InputParams.h"
#include "engine/EngineParams.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateGlueCommon.h"
#include "state/StateMigration.h"

namespace rinne::input
{

class InputStateGlue
{
public:
    // tree 是 InputParams 建好的参数树，它与它的处理器都要比胶水活得久。参数按参数表的 ID 在树里取，取不到是接线错误。
    explicit InputStateGlue(juce::AudioProcessorValueTreeState& tree);

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

    // 改设置：在锁里对设置的一份拷贝调用 edit(state::Configs&)，做完整份换上，并刷新给引擎的三个开关。
    // 不定型、不标脏：调用方按需另调 markUserAction 与 markStateDirty。unknown 是读入时保留的未知键，原样写回；
    // edit 里不要往 unknown 放这一端认识的键，否则这份文档编不出来，getState 交出空块并计一次 encodeFailures。
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
    // 最近一次 setState 的结果，还没有过 setState 时为空。界面据此提示「state 未能恢复」或「工程由更新的版本保存」。
    std::optional<state::StateLoadStatus> lastLoadStatus() const noexcept;
    // 正在原样回写更高版本的 state：参数与设置的改动都不进 state，界面要常驻说明。
    bool holdingNewerState() const noexcept { return holding_.load(std::memory_order_relaxed); }
    // 保存时为压进上限丢掉的未知参数 id，累计条数。
    std::uint32_t droppedUnknownParams() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    // 保存时编码失败（交出空块）的累计次数。
    std::uint32_t encodeFailures() const noexcept { return encodeFailures_.load(std::memory_order_relaxed); }

    // ---- 音频线程（无锁） ----
    std::uint32_t stateSeq() const noexcept { return stateSeq_.load(std::memory_order_relaxed); }
    bool guardOn() const noexcept { return guardOn_.load(std::memory_order_relaxed); }
    bool resetOnPlay() const noexcept { return resetOnPlay_.load(std::memory_order_relaxed); }
    engine::TailMode tailMode() const noexcept;
    // 每块一次：把 stateSeq 与三个工程开关写进 out，其余字段不动（参数由 InputParams::snapshot 写）。
    void snapshot(engine::EngineParams& out) const noexcept;

private:
    using ParamValues = std::array<float, kParamCount>;

    static_assert(std::atomic<bool>::is_always_lock_free, "音频线程读工程开关要求 std::atomic<bool> 无锁");
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "音频线程读尾长模式要求 std::atomic<uint8_t> 无锁");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "音频线程读命令序号要求 std::atomic<uint32_t> 无锁");

    ParamValues paramValuesFrom(const state::Params& params) const noexcept;
    ParamValues currentParamValues() const;
    ParamValues landedValues(const ParamValues& values) const;
    void writeParams(const ParamValues& values);
    void publishConfigsLocked() noexcept;
    static void callHook(const std::shared_ptr<const std::function<void()>>& hook);

    static constexpr std::uint8_t kNoLoad = 0xffu; // lastLoad_ 的初值：还没有过 setState

    juce::AudioProcessorValueTreeState& tree_;
    std::array<juce::RangedAudioParameter*, kParamCount> parameters_{};

    std::mutex loadMutex_; // 串起整个 setState：两次 setState 的参数写入不会交错
    mutable std::mutex mutex_; // 管下面五项；持锁时不写参数树、不调用宿主或回调（读参数的当前值只是原子读，可以在锁里）
    state::StateDocument document_;
    state::RawWriteBack writeBack_;
    state::glue::Settlement settlement_;
    std::shared_ptr<const std::function<void()>> confirmClaim_;
    std::optional<ParamValues> applying_; // setState 在锁外写参数树期间：写完后参数树里会是的值

    std::atomic<std::uint32_t> stateSeq_{0};
    std::atomic<bool> guardOn_{true};
    std::atomic<bool> resetOnPlay_{true};
    std::atomic<std::uint8_t> tailMode_{0};
    std::atomic<std::uint8_t> lastLoad_{kNoLoad};
    std::atomic<bool> holding_{false};
    std::atomic<std::uint32_t> dropped_{0};
    std::atomic<std::uint32_t> encodeFailures_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputStateGlue)
};

} // namespace rinne::input
