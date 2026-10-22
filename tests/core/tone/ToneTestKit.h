// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音色级测试共用的小工具：锁相测幅（在整数个周期上与正余弦求相关，稳态正弦的幅度没有泄漏误差）、预畸变二阶
// Butterworth 的理论幅频、正弦相邻样本差的上界（判咔嗒用）。
#pragma once

#include <cmath>
#include <cstdint>

#include "core/dsp/DspTestKit.h"

namespace rinne::test
{

// 先走 settle 个样本让瞬态过去，再在 n 个样本上测增益（线性）。n·freq/sampleRate 必须是整数：整周期上正余弦正交，
// 测出来的就是这个频率上的稳态幅度，与相位无关
template<typename Process>
double lockInGain(Process&& process, double freq, double sampleRate, std::int64_t settle, std::int64_t n,
                  double amplitude = 0.5)
{
    double sumSin = 0.0;
    double sumCos = 0.0;
    for (std::int64_t i = 0; i < settle + n; ++i)
    {
        const double phase = 2.0 * kPi * freq * static_cast<double>(i) / sampleRate;
        const double s = std::sin(phase);
        const double y = static_cast<double>(process(static_cast<float>(amplitude * s)));
        if (i >= settle)
        {
            sumSin += y * s;
            sumCos += y * std::cos(phase);
        }
    }
    return 2.0 / static_cast<double>(n) * std::sqrt(sumSin * sumSin + sumCos * sumCos) / amplitude;
}

// 双线性变换（截止预畸变）后的二阶 Butterworth：Ω = tan(πf/sr) / tan(πfc/sr)，低通 1/√(1 + Ω⁴)，高通 Ω²/√(1 + Ω⁴)
inline double butterworthGain(bool highPass, double freq, double cutoff, double sampleRate)
{
    const double w = std::tan(kPi * freq / sampleRate) / std::tan(kPi * cutoff / sampleRate);
    const double w4 = w * w * w * w;
    return highPass ? std::sqrt(w4 / (1.0 + w4)) : std::sqrt(1.0 / (1.0 + w4));
}

// 幅度 a、频率 f 的正弦相邻两个样本之差的上界 2a·sin(πf/sr)。线性时不变、增益不超过 1 的滤波器在稳态下也不超过它；
// 硬切换（旁路、系数跳变）在不过零的相位上会远远超过
inline double sineStepBound(double amplitude, double freq, double sampleRate)
{
    return 2.0 * amplitude * std::sin(kPi * freq / sampleRate);
}

} // namespace rinne::test
