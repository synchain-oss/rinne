// SPDX-License-Identifier: GPL-3.0-or-later
//
// 外壳测试共用的夹具：确定性的随机位型（含 -0.0、各种 NaN 载荷、无穷、非规格化数）、位级比较、
// 不分配的缓冲视图。两端处理器用同一份。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rinne::test
{

inline constexpr int kMaxBlock = 8192;

// 位型即数据：直接按 IEEE-754 位写，不经过任何浮点运算，NaN 的载荷与符号才不会被硬件改掉
inline constexpr std::array<std::uint32_t, 14> kSpecialBits{
    0x80000000u, // -0.0
    0x00000000u, // +0.0
    0x7fc00000u, // 静默 NaN
    0xffc00000u, // 负的静默 NaN
    0x7fc00001u, // 带载荷的静默 NaN
    0xffc12345u, // 负号加载荷
    0x7fa00000u, // 信号 NaN
    0xffbfffffu, // 负的信号 NaN，载荷全 1
    0x7fffffffu, // 载荷全 1 的 NaN
    0x7f800000u, // +inf
    0xff800000u, // -inf
    0x00000001u, // 最小的非规格化数
    0x807fffffu, // 最大的负非规格化数
    0x3f800000u, // 1.0
};

// 3 个最大块那么长的随机位型；每个块从不同偏移取，对齐与内容都不重复
inline const std::vector<std::uint32_t>& patternBits()
{
    static const std::vector<std::uint32_t> bits = [] {
        std::vector<std::uint32_t> v(static_cast<std::size_t>(3 * kMaxBlock));
        std::uint64_t s = 0x9e3779b97f4a7c15ull;
        for (auto& w : v)
        {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            w = static_cast<std::uint32_t>((s * 2685821657736338717ull) >> 32);
            // 约八分之一的样本换成特殊位型，其余是任意 32 位（含大量 NaN 与非规格化数）
            if ((w & 7u) == 0u)
                w = kSpecialBits[(w >> 8) % kSpecialBits.size()];
        }
        return v;
    }();
    return bits;
}

// 一个块的缓冲：每个声道 kMaxBlock 个样本，加一份拷贝作对照。全部预先分配，处理时不再分配
struct BlockFixture
{
    static constexpr int kCanary = 64; // 块尾之后多查的样本数，抓越界写
    static constexpr std::uint32_t kCanaryBits = 0xa5a5a5a5u;
    using Channel = std::array<float, kMaxBlock + kCanary>;

    std::array<Channel, 2> data{};
    std::array<Channel, 2> expected{};

    static void put(Channel& ch, int index, std::uint32_t bits)
    {
        std::memcpy(&ch[static_cast<std::size_t>(index)], &bits, sizeof bits);
    }

    // 填 numChannels 个声道、numSamples 个样本；每个声道开头 14 个样本轮换放特殊位型，保证每个块（包括 1
    // 个样本的块）都带
    void fill(int numChannels, int numSamples)
    {
        const auto& bits = patternBits();
        const auto offset = static_cast<std::size_t>((numSamples * 7) % kMaxBlock);
        for (int c = 0; c < numChannels; ++c)
        {
            auto& ch = data[static_cast<std::size_t>(c)];
            for (int i = 0; i < numSamples; ++i)
                put(ch, i, bits[offset + static_cast<std::size_t>(c) * kMaxBlock + static_cast<std::size_t>(i)]);
            for (int i = 0; i < numSamples && i < static_cast<int>(kSpecialBits.size()); ++i)
                put(ch, i, kSpecialBits[static_cast<std::size_t>(i + numSamples + c) % kSpecialBits.size()]);
            for (int i = numSamples; i < numSamples + kCanary; ++i)
                put(ch, i, kCanaryBits);
            expected[static_cast<std::size_t>(c)] = ch;
        }
    }

    // 逐位比较前 numSamples + kCanary 个样本（NaN 不能用 == 比）
    bool unchanged(int numChannels, int numSamples) const
    {
        const auto bytes = static_cast<std::size_t>(numSamples + kCanary) * sizeof(float);
        for (int c = 0; c < numChannels; ++c)
            if (std::memcmp(data[static_cast<std::size_t>(c)].data(), expected[static_cast<std::size_t>(c)].data(),
                            bytes) != 0)
                return false;
        return true;
    }

    // 不拷贝、不分配的缓冲视图：JUCE 的 AudioBuffer 对外部指针的构造，声道数 ≤ 2 时用对象内的预留空间
    juce::AudioBuffer<float> view(int numChannels, int numSamples)
    {
        std::array<float*, 2> ptrs{data[0].data(), data[1].data()};
        return juce::AudioBuffer<float>(ptrs.data(), numChannels, numSamples);
    }
};

inline juce::AudioProcessor::BusesLayout layoutOf(const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(in);
    layout.outputBuses.add(out);
    return layout;
}

} // namespace rinne::test
