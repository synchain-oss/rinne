// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送链第三、四级：低切（高通）与高切（低通）。二阶状态变量滤波器，按拓扑保持变换（梯形积分）离散化，
// Q = 1/√2（Butterworth：通带无起伏，截止处 −3.01 dB）。
//   g = tan(π·fc/sr)，k = 1/Q = √2，a1 = 1/(1 + g·(g + k))，a2 = g·a1，a3 = g·a2；每个样本
//   v3 = x − s2，v1 = a1·s1 + a2·v3，v2 = s2 + a2·s1 + a3·v3，s1 ← 2·v1 − s1，s2 ← 2·v2 − s2；
//   低通取 v2，高通取 x − k·v1 − v2。
// tan 是频率预畸变，数字滤波器在 fc 处恰为 −3.01 dB。状态就是两个积分器的值，改系数时不用换算状态，截止随时间变化
// 也不会失稳（用例：10^6 次随机跳变）。状态与系数用 double：低切 20 Hz、192 kHz 时 a3 约 1e-7，float 里 s2 + a3·v3
// 的增量会被舍入吃掉。截止钳在 [1 Hz, 0.45·sr]：越靠近奈奎斯特 tan 越陡，0.45·sr 处 g ≈ 6.3，仍然稳定。
// 状态每个样本冲一次极小值，输出也冲：静音时状态几何衰减，不冲就会落进 denormal。两者分工不同：状态写回用
// flushState（非有限 → +0，其余同 flushTiny），输出只过 flushTiny（不清非有限值），见下面的输入契约。
//
// CutFilter 是参数化的一级，左右声道共用一套系数：
//   低切的参数 20–2000 Hz，≤ 20 为关；高切 1000–20000 Hz，≥ 20000 为关。关时逐位直通（滤波器不跑，样本一个位都不碰，
//   −0.0 的符号与 NaN 载荷原样）。越界的参数钳回范围，NaN 按关处理。
//   截止在 log2 域线性平滑 30 ms（按样本计数）。平滑期间每个样本按当前值重算系数（只在这 30 ms 里有 exp2 与 tan），
//   走完时用目标值本身算，与直接设在目标上的滤波器逐位相同；没有按块或按网格更新的系数，宿主怎么切块都一样。
//   开：状态清零，截止从「关」的端点（低切 20 Hz，高切 min(20000 Hz, 0.45·sr)）滑向目标，同时用 10 ms 线性交叉从干声
//   过渡到滤波输出。端点上的滤波器在音频频带里几乎直通，交叉的两头几乎一样，听不出切换。
//   关：先滑回端点，再 10 ms 交叉回干声，之后逐位直通。过渡中再改参数：平滑从当前值重新起算，交叉从当前位置折返。
//   高切被采样率截断（44.1 kHz 下 0.45·sr = 19845 Hz，参数在它之上而又不是关）时 cutoffLimited()
//   为真，供界面给中性提示。 reset() 清状态，平滑与交叉都落定到目标：起播沿复位、离开快速路径时调用。prepare()
//   换采样率后也落定。
// 输入契约：输入须已清洗（有限、|x| ≤ 16），这时状态与输出都算不出非有限值。万一出现（输入非有限，或这一步算出了
// NaN、±Inf），状态清、输出不清：
//   状态：两个状态经 flushState 写回、清成 +0，下一个样本起与刚 reset 过的滤波器逐位相同，不会被一个坏样本永久
//   卡在 NaN 上；
//   输出：这一格的非有限值原样输出（flushTiny 不清它），按设计交给 ⑥ 的有限性检查跳闸。Input 内部产生的非有限值
//   要立即跳闸，不能在本级被清成 0、让 ⑥ 看不见。
// CutFilter 的几种情形都一样往下交：滤波输出那一路是上面这条；交叉进行中干声那一路的非有限值混进输出，输出也是
// 非有限值；关态逐位直通，非有限值原样输出。三种都由 ⑥ 跳闸（用例在 test_svf_nonfinite.cpp）。
#pragma once

#include <cmath>
#include <cstdint>

#include "dsp/LinearSmoother.h"
#include "dsp/Sanitize.h"
#include "dsp/SwitchRamp.h"

namespace rinne::dsp
{

inline constexpr double kSvfDamping = 1.4142135623730950488; // k = 1/Q，Q = 1/√2
inline constexpr double kSvfMinCutoffHz = 1.0;
inline constexpr double kSvfMaxCutoffRatio = 0.45;

inline constexpr float kLowCutOffHz = 20.0f;
inline constexpr float kLowCutMaxHz = 2000.0f;
inline constexpr float kHighCutMinHz = 1000.0f;
inline constexpr float kHighCutOffHz = 20000.0f;
inline constexpr double kCutoffSmoothSeconds = 0.030;
inline constexpr double kCutSwitchSeconds = 0.010;

struct SvfCoefficients
{
    double g = 0.0;
    double a1 = 1.0;
    double a2 = 0.0;
    double a3 = 0.0;
};

// 截止上限 0.45·sr；采样率无效时为 0
inline double svfMaxCutoff(double sampleRate) noexcept
{
    return sampleRate > 0.0 ? kSvfMaxCutoffRatio * sampleRate : 0.0;
}

// 钳到 [1 Hz, 0.45·sr]；NaN 给下限
inline double svfClampCutoff(double hz, double sampleRate) noexcept
{
    const double hi = svfMaxCutoff(sampleRate);
    const double lo = hz > kSvfMinCutoffHz ? hz : kSvfMinCutoffHz;
    return lo < hi ? lo : hi;
}

// 采样率无效时给 g = 0 的系数：低通输出 0，高通直通
inline SvfCoefficients svfCoefficients(double hz, double sampleRate) noexcept
{
    SvfCoefficients c;
    if (!(sampleRate > 0.0))
        return c;
    constexpr double pi = 3.14159265358979323846;
    const double g = std::tan(pi * svfClampCutoff(hz, sampleRate) / sampleRate);
    c.g = g;
    c.a1 = 1.0 / (1.0 + g * (g + kSvfDamping));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

enum class SvfResponse
{
    LowPass,
    HighPass
};

// 只给状态写回用：非有限 → +0，其余同 flushTiny。输出不用它，输出的非有限值要留给 ⑥ 跳闸（见文件头）
inline double flushState(double x) noexcept
{
    return isFiniteSample(x) ? flushTiny(x) : 0.0;
}

// 单声道的状态；系数由调用方给，左右声道可以共用一套
class SvfTpt
{
public:
    void reset() noexcept
    {
        s1_ = 0.0;
        s2_ = 0.0;
    }

    float process(float in, const SvfCoefficients& c, SvfResponse response) noexcept
    {
        const double x = in;
        const double v3 = x - s2_;
        const double v1 = c.a1 * s1_ + c.a2 * v3;
        const double v2 = s2_ + c.a2 * s1_ + c.a3 * v3;
        s1_ = flushState(2.0 * v1 - s1_);
        s2_ = flushState(2.0 * v2 - s2_);
        const double y = response == SvfResponse::HighPass ? x - kSvfDamping * v1 - v2 : v2;
        // 输出不清非有限值：交给 ⑥ 跳闸
        return flushTiny(static_cast<float>(y));
    }

private:
    double s1_ = 0.0;
    double s2_ = 0.0;
};

enum class CutKind
{
    LowCut,
    HighCut
};

class CutFilter
{
public:
    explicit CutFilter(CutKind kind) noexcept
        : kind_(kind), paramHz_(kind == CutKind::LowCut ? kLowCutOffHz : kHighCutOffHz)
    {
    }

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 0.0;
        maxHz_ = svfMaxCutoff(sampleRate_);
        const double highOff = static_cast<double>(kHighCutOffHz);
        offHz_ = kind_ == CutKind::LowCut ? static_cast<double>(kLowCutOffHz) : (highOff < maxHz_ ? highOff : maxHz_);
        cutoff_.reset(sampleRate_, kCutoffSmoothSeconds);
        switch_.prepare(sampleRate_, kCutSwitchSeconds);
        applyParam();
        reset();
    }

    // 清状态；截止平滑与开关交叉都落定到目标。没有 prepare 过时什么都不做（照样逐位直通）
    void reset() noexcept
    {
        left_.reset();
        right_.reset();
        if (!(sampleRate_ > 0.0))
            return;
        cutoff_.setCurrentAndTargetValue(static_cast<float>(std::log2(targetHz_)));
        coeffs_ = svfCoefficients(targetHz_, sampleRate_);
        switch_.setTarget(wantOn_);
        switch_.snapToTarget();
        running_ = wantOn_;
    }

    // 参数值（Hz），关也从这里给。每块调用同一个值什么都不改；prepare 之前给的值在 prepare 时生效
    void setCutoff(float hz) noexcept
    {
        paramHz_ = hz;
        if (!(sampleRate_ > 0.0))
            return;
        const double before = targetHz_;
        applyParam();
        bool restarted = false;
        if (!running_)
        {
            if (!wantOn_)
                return;
            // 从关的端点起步
            left_.reset();
            right_.reset();
            cutoff_.setCurrentAndTargetValue(static_cast<float>(std::log2(offHz_)));
            coeffs_ = svfCoefficients(offHz_, sampleRate_);
            running_ = true;
            restarted = true;
        }
        if (restarted || targetHz_ != before)
        {
            cutoff_.setTargetValue(static_cast<float>(std::log2(targetHz_)));
            // 新目标的 log2 在 float 上与当前值相同、平滑没有起步时，直接换成目标本身的系数（差别不到一个 ulp）
            if (!cutoff_.isSmoothing())
                coeffs_ = svfCoefficients(targetHz_, sampleRate_);
        }
        if (wantOn_)
            switch_.setTarget(true);
    }

    // 就地处理一对样本
    void process(float& left, float& right) noexcept
    {
        if (!running_)
            return;
        advance();
        const SvfResponse r = response();
        const float wl = left_.process(left, coeffs_, r);
        const float wr = right_.process(right, coeffs_, r);
        left = switch_.mix(left, wl);
        right = switch_.mix(right, wr);
        settle();
    }

    // 单声道：只用左声道的状态。同一个实例只用一种调用方式
    float process(float x) noexcept
    {
        if (!running_)
            return x;
        advance();
        const float y = switch_.mix(x, left_.process(x, coeffs_, response()));
        settle();
        return y;
    }

    CutKind kind() const noexcept { return kind_; }
    // 逐位直通（关，且交叉已走完）
    bool isBypassed() const noexcept { return !running_; }
    // 参数是否为开（过渡可能还没走完）
    bool isOn() const noexcept { return wantOn_; }
    bool isSmoothing() const noexcept { return running_ && (cutoff_.isSmoothing() || !switch_.isSettled()); }
    // 高切被采样率截断在 0.45·sr
    bool cutoffLimited() const noexcept { return limited_; }
    // 截止的目标（Hz，已钳）；关时是端点频率
    double targetCutoffHz() const noexcept { return targetHz_; }
    const SvfCoefficients& coefficients() const noexcept { return coeffs_; }

private:
    SvfResponse response() const noexcept
    {
        return kind_ == CutKind::LowCut ? SvfResponse::HighPass : SvfResponse::LowPass;
    }

    void applyParam() noexcept
    {
        const float p = paramHz_;
        double hz = 0.0;
        if (kind_ == CutKind::LowCut)
        {
            wantOn_ = p > kLowCutOffHz; // NaN 为关
            hz = static_cast<double>(p < kLowCutMaxHz ? p : kLowCutMaxHz);
        }
        else
        {
            wantOn_ = p < kHighCutOffHz; // NaN 为关
            hz = static_cast<double>(p > kHighCutMinHz ? p : kHighCutMinHz);
        }
        limited_ = wantOn_ && hz > maxHz_;
        targetHz_ = !wantOn_ ? offHz_ : (limited_ ? maxHz_ : hz);
    }

    // 每个样本先走平滑与交叉，再用新系数滤波
    void advance() noexcept
    {
        if (cutoff_.isSmoothing())
        {
            const float l = cutoff_.getNextValue();
            const double hz = cutoff_.isSmoothing() ? std::exp2(static_cast<double>(l)) : targetHz_;
            coeffs_ = svfCoefficients(hz, sampleRate_);
        }
        // 关：滑回端点之后才交叉回干声
        if (!wantOn_ && !cutoff_.isSmoothing())
            switch_.setTarget(false);
        switch_.step();
    }

    void settle() noexcept
    {
        if (!wantOn_ && switch_.isFullyOff())
            running_ = false;
    }

    CutKind kind_;
    float paramHz_;
    double sampleRate_ = 0.0;
    double maxHz_ = 0.0;
    double offHz_ = 0.0;
    double targetHz_ = 0.0;
    bool wantOn_ = false;
    bool limited_ = false;
    bool running_ = false;
    LinearSmoother cutoff_;
    SwitchRamp switch_;
    SvfCoefficients coeffs_;
    SvfTpt left_;
    SvfTpt right_;
};

} // namespace rinne::dsp
