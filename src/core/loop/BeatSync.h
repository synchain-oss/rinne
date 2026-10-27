// SPDX-License-Identifier: GPL-3.0-or-later
//
// 节拍同步：把「时值档位 + 宿主速度」换算成环路延迟 D（样本）。纯逻辑，只有头文件，不分配、不读墙钟。
//
//   D = 拍数 × 60 / BPM × sr，再取整到样本。拍数取自 engine::kDelayNoteBeats（16 档，拍数升序，只能追加，
//   那张表是档位顺序的唯一真源，这里不另存一份）。48 kHz、120 BPM、1/4（1 拍）= 24000 个样本。
//
// 速度：有效范围 [20, 999] BPM，0、负数、非有限值与超出范围的都是无效。无效时沿用本会话最后一个有效值；
// 本会话还没拿到过有效值就按 120 BPM，并置「没有速度」提示（noTempo）。按会话计：prepare 不清。
//
// 抗抖：宿主每块给的速度常带浮点噪声（119.99999），精确 D 恰在两个整数样本的正中间时，取整会在相邻两个整数之间
// 来回翻，每翻一次就是一次改 D。所以精确 D 与当前目标相差不到 1 个样本时目标不动；差到 1 个样本以上才换。
// 代价：速度缓慢漂移时，目标可以比 round(精确 D) 落后不到 1 个样本。
//
// 截断：D 的上限是 4000 ms（4 × sr 个样本），超过的被截到上限并置「被截断」提示（clipped）。截断在
// LoopTiming.h 的 chooseDelay 里做，提示位由它的 clipped 给出。
//
// 线程：音频线程每块调；全部是定长运算。
#pragma once

#include <cmath>
#include <cstdint>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"

namespace rinne::loop
{

// 档位下标落在 0..15 之内就原样用，越界（壳保证不会发生，这里只是防守）取默认档。
constexpr int noteIndexOrDefault(int note) noexcept
{
    return note >= 0 && note < engine::kDelayNoteCount ? note : engine::kDefaultDelayNote;
}

// 速度是否有效：[20, 999]。NaN 与无穷大的比较都为假，落在无效里。
inline bool isValidBpm(double bpm) noexcept
{
    return bpm >= static_cast<double>(engine::kMinHostBpm) && bpm <= static_cast<double>(engine::kMaxHostBpm);
}

// 精确的 D（样本，未取整）：拍数 × 60 / BPM × sr。bpm 必须有效（由 TempoTracker 保证）。
inline double syncDelayExact(int note, double bpm, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    return engine::kDelayNoteBeats[noteIndexOrDefault(note)] * 60.0 / bpm * sr;
}

// 速度跟踪：宿主速度 → 本会话在用的速度。
class TempoTracker
{
public:
    // 每块调一次。返回在用的速度（BPM）。
    double observe(float hostBpm) noexcept
    {
        const double bpm = static_cast<double>(hostBpm);
        if (isValidBpm(bpm))
        {
            bpm_ = bpm;
            haveHost_ = true;
        }
        return bpm_;
    }

    double bpm() const noexcept { return bpm_; }
    // 本会话还没拿到过有效的宿主速度：在用的是回退值 120，要给「没有速度」提示。
    bool fallback() const noexcept { return !haveHost_; }

private:
    double bpm_ = static_cast<double>(engine::kFallbackBpm);
    bool haveHost_ = false;
};

// 节拍同步目标的抗抖滤波：精确 D 与当前目标相差不到 1 个样本时目标不动。
class SyncTargetFilter
{
public:
    // 回到「还没有目标」：离开节拍模式、换采样率时调。
    void reset() noexcept { have_ = false; }

    // 返回新的目标（样本，取整）。档位换了一定重新取整：那是用户的动作，不是抖动。
    std::int64_t settle(double exactFrames, int note) noexcept
    {
        if (have_ && note == note_ && std::fabs(exactFrames - static_cast<double>(target_)) < 1.0)
        {
            return target_;
        }
        target_ = static_cast<std::int64_t>(std::llround(exactFrames));
        note_ = note;
        have_ = true;
        return target_;
    }

private:
    std::int64_t target_ = 0;
    int note_ = -1;
    bool have_ = false;
};

} // namespace rinne::loop
