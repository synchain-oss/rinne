// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix 律：m（0…1，参数 mix 的 0–100 %）→ 干声增益 g_dry 与回送增益 g_ret，中点两路全开的线性律
//   g_dry = min(1, 2·(1 − m))，g_ret = min(1, 2·m)。
//   0 → 1 / 0；0.25（默认）→ 1 / 0.5；0.5 → 1 / 1；1 → 0 / 1，四个点在 float 里都恰好成立（只有乘 2 与 1 − m，
//   这几个 m 上都没有舍入）。m ≤ 0.5 时 1 − m ≥ 0.5（舍入单调），g_dry 恰为 1.0f：默认插上 Rinne Input
//   音量不变，干声的逐位快速路径也靠它。两条增益都随 m 单调，没有上限档、没有模式切换。
// 越界钳到 [0, 1]；NaN 按 0 处理（干声照常、回送为 0，失败只会让回送变成 0）。
// 平滑不在这里做：g_ret 交给安全链，和 feedback 乘在一起按 30 ms 线性平滑（限制器之前）；
// g_dry 只乘干声，由引擎用 LinearSmoother 按 kMixSmoothSeconds 平滑，终点精确。
#pragma once

namespace rinne::dsp
{

inline constexpr float kMixDefault = 0.25f;
inline constexpr double kMixSmoothSeconds = 0.030;

struct MixGains
{
    float dry = 1.0f;
    float ret = 0.0f;
};

// 钳到 [0, 1]；NaN 与负数（含 −0.0）都给 +0
inline float clampMix(float m) noexcept
{
    if (!(m > 0.0f))
        return 0.0f;
    return m < 1.0f ? m : 1.0f;
}

inline float mixDryGain(float m) noexcept
{
    const float g = 2.0f * (1.0f - clampMix(m));
    return g < 1.0f ? g : 1.0f;
}

inline float mixReturnGain(float m) noexcept
{
    const float g = 2.0f * clampMix(m);
    return g < 1.0f ? g : 1.0f;
}

inline MixGains mixGains(float m) noexcept
{
    return {mixDryGain(m), mixReturnGain(m)};
}

} // namespace rinne::dsp
