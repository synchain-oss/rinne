// SPDX-License-Identifier: GPL-3.0-or-later
//
// 127 点 MLS：序列本身（长度、平衡、循环自相关只在零移位有峰）和 Ping 用的两种相关求延迟——
// 周期注入时的循环相关、一次性注入时的线性相关。延迟要逐样本准：多估或少估 1 个样本都算错。
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/Mls.h"

namespace
{

using namespace rinne::util;

// 确定性的伪随机噪声，[-1, 1)
class Noise
{
public:
    explicit Noise(std::uint32_t seed) : state_(seed) {}
    float next()
    {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(state_ >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
    }

private:
    std::uint32_t state_;
};

// 周期注入：一个周期的接收信号，序列延迟 lag 个样本（x[k] = mls[(k - lag) mod 127]）
std::vector<float> periodReceived(int lag, float gain)
{
    std::vector<float> x(static_cast<std::size_t>(kMlsLength));
    for (int k = 0; k < kMlsLength; ++k)
    {
        const int index = ((k - lag) % kMlsLength + kMlsLength) % kMlsLength;
        x[static_cast<std::size_t>(k)] = gain * static_cast<float>(kMls[static_cast<std::size_t>(index)]);
    }
    return x;
}

// 一次性注入：n 个样本的回读，序列从 lag 处开始，其余是静默
std::vector<float> burstCapture(std::size_t n, std::size_t lag, float gain)
{
    std::vector<float> x(n, 0.0f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kMlsLength) && lag + i < n; ++i)
    {
        x[lag + i] = gain * static_cast<float>(kMls[i]);
    }
    return x;
}

} // namespace

TEST_CASE("the mls sequence has 127 points and its circular autocorrelation peaks only at zero lag", "[probe][mls]")
{
    static_assert(kMlsLength == 127, "127 点");
    static_assert(mlsIsMaximal(), "编译期核对");

    int plus = 0;
    for (const std::int8_t v : kMls)
    {
        REQUIRE((v == 1 || v == -1));
        plus += v == 1 ? 1 : 0;
    }
    CHECK(plus == 64); // 最大长度序列：64 个 1、63 个 0
    CHECK(mlsAutocorrelation(0) == 127);
    for (int lag = 1; lag < kMlsLength; ++lag)
    {
        INFO("lag " << lag);
        CHECK(mlsAutocorrelation(lag) == -1);
    }
    // 周期性：移 127 回到零移位
    CHECK(mlsAutocorrelation(kMlsLength) == 127);
}

TEST_CASE("circular correlation recovers every delay of a periodic train", "[probe][mls]")
{
    for (int lag = 0; lag < kMlsLength; ++lag)
    {
        const std::vector<float> x = periodReceived(lag, 0.1f);
        const MlsPeak found = mlsCircularDelay(x.data());
        INFO("lag " << lag);
        REQUIRE(found.lag == lag);
        CHECK(found.peak == Catch::Approx(12.7).epsilon(1e-5));
        CHECK(found.runnerUp == Catch::Approx(0.1).epsilon(1e-5)); // 其余移位恒为 -1 乘增益
    }
}

TEST_CASE("circular correlation is exact to one sample on both sides of a delay", "[probe][mls]")
{
    for (const int lag : {1, 40, 63, 126})
    {
        const int below = (lag + kMlsLength - 1) % kMlsLength;
        const int above = (lag + 1) % kMlsLength;
        CHECK(mlsCircularDelay(periodReceived(below, 0.5f).data()).lag == below);
        CHECK(mlsCircularDelay(periodReceived(lag, 0.5f).data()).lag == lag);
        CHECK(mlsCircularDelay(periodReceived(above, 0.5f).data()).lag == above);
    }
}

TEST_CASE("circular correlation reads wrapped data, which a linear search of one period cannot", "[probe][mls]")
{
    // 延迟 5 的整周期，序列的头部回卷到了窗口末尾。线性相关在 127 点的窗口里只有零移位一个候选，永远答 0
    const std::vector<float> x = periodReceived(5, 1.0f);
    CHECK(mlsCircularDelay(x.data()).lag == 5);
    CHECK(mlsLinearDelay(x.data(), x.size()).lag == 0);
}

TEST_CASE("circular correlation survives additive noise", "[probe][mls]")
{
    Noise noise(12345);
    for (const int lag : {0, 17, 100})
    {
        std::vector<float> x = periodReceived(lag, 0.3f);
        for (float& v : x)
        {
            v += 0.2f * noise.next();
        }
        const MlsPeak found = mlsCircularDelay(x.data());
        REQUIRE(found.lag == lag);
        CHECK(found.peak > 2.0 * found.runnerUp); // 峰明显高于噪声造出的次高值
    }
}

TEST_CASE("linear correlation finds the absolute delay of a one-shot burst", "[probe][mls]")
{
    const std::size_t n = 2048;
    for (const std::size_t lag :
         {std::size_t{0}, std::size_t{1}, std::size_t{255}, std::size_t{897}, n - static_cast<std::size_t>(kMlsLength)})
    {
        const std::vector<float> x = burstCapture(n, lag, 0.1f);
        const MlsPeak found = mlsLinearDelay(x.data(), x.size());
        INFO("lag " << lag);
        REQUIRE(found.lag == static_cast<int>(lag));
        CHECK(found.peak == Catch::Approx(12.7).epsilon(1e-5));
        CHECK(found.peak > 4.0 * found.runnerUp); // 单次注入的旁瓣远低于主峰
    }
}

TEST_CASE("linear correlation is exact to one sample", "[probe][mls]")
{
    const std::size_t n = 1024;
    for (const std::size_t lag : {std::size_t{100}, std::size_t{511}, std::size_t{700}})
    {
        CHECK(mlsLinearDelay(burstCapture(n, lag - 1, 0.25f).data(), n).lag == static_cast<int>(lag) - 1);
        CHECK(mlsLinearDelay(burstCapture(n, lag, 0.25f).data(), n).lag == static_cast<int>(lag));
        CHECK(mlsLinearDelay(burstCapture(n, lag + 1, 0.25f).data(), n).lag == static_cast<int>(lag) + 1);
    }
}

TEST_CASE("linear correlation finds the burst in a noisy capture and ignores an inverted copy", "[probe][mls]")
{
    Noise noise(777);
    std::vector<float> x = burstCapture(4096, 1500, 0.2f);
    for (float& v : x)
    {
        v += 0.05f * noise.next();
    }
    const MlsPeak found = mlsLinearDelay(x.data(), x.size());
    CHECK(found.lag == 1500);
    CHECK(found.peak > 3.0 * found.runnerUp);

    // 反相的序列不是我们注入的那串：只认正相关峰，反相副本压不过真正的峰
    std::vector<float> y = burstCapture(4096, 900, 0.2f);
    const std::vector<float> inverted = burstCapture(4096, 2500, -0.4f);
    for (std::size_t i = 0; i < y.size(); ++i)
    {
        y[i] += inverted[i];
    }
    CHECK(mlsLinearDelay(y.data(), y.size()).lag == 900);
}

TEST_CASE("correlation refuses input that cannot hold a sequence", "[probe][mls]")
{
    const std::vector<float> shortInput(static_cast<std::size_t>(kMlsLength) - 1, 0.5f);
    CHECK(mlsLinearDelay(shortInput.data(), shortInput.size()).lag == -1);
    CHECK(mlsLinearDelay(nullptr, 4096).lag == -1);
    CHECK(mlsCircularDelay(nullptr).lag == -1);
}
