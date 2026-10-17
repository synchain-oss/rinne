// SPDX-License-Identifier: GPL-3.0-or-later
//
// 外壳阶段的核心承诺：两端对宿主的音频都是逐位直通。随机位型里带 -0.0 与各种 NaN 载荷，块长取 1 到 8192 的每一个值，
// 正常路径与旁路路径各走一遍；回调里不得有任何堆分配。验证宿主下（见 test_shell_contract.cpp）同样成立。

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>

#include "AllocationCounter.h"
#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "ShellTestKit.h"

namespace
{

using rinne::test::AllocationScope;
using rinne::test::BlockFixture;
using rinne::test::kMaxBlock;

enum class Path
{
    Normal,
    Bypassed,
};

const char* pathName(Path path)
{
    return path == Path::Normal ? "processBlock" : "processBlockBypassed";
}

template<typename Processor>
void process(Processor& processor, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, Path path)
{
    if (path == Path::Normal)
        processor.processBlock(buffer, midi);
    else
        processor.processBlockBypassed(buffer, midi);
}

// 把 1..kMaxBlock 的每个块长都走一遍；返回第一个不是逐位直通的块长，全部通过返回 0。顺带累计回调内的堆分配次数
template<typename Processor>
int sweep(Processor& processor, int numChannels, Path path, long& allocations)
{
    auto fixture = std::make_unique<BlockFixture>();
    juce::MidiBuffer midi;
    for (int n = 1; n <= kMaxBlock; ++n)
    {
        fixture->fill(numChannels, n);
        auto buffer = fixture->view(numChannels, n);
        {
            const AllocationScope scope;
            process(processor, buffer, midi, path);
            allocations += scope.allocations();
        }
        if (!fixture->unchanged(numChannels, n))
            return n;
    }
    return 0;
}

} // namespace

TEMPLATE_TEST_CASE("stereo audio passes through bit for bit at every block size", "[host][shell][passthrough]",
                   rinne::InputProcessor, rinne::OutputProcessor)
{
    TestType processor;
    processor.prepareToPlay(48000.0, kMaxBlock);

    for (const auto path : {Path::Normal, Path::Bypassed})
    {
        DYNAMIC_SECTION(pathName(path))
        {
            long allocations = 0;
            CHECK(sweep(processor, 2, path, allocations) == 0);
            CHECK(allocations == 0);
        }
    }
}

TEMPLATE_TEST_CASE("mono audio passes through bit for bit at every block size", "[host][shell][passthrough]",
                   rinne::InputProcessor, rinne::OutputProcessor)
{
    TestType processor;
    REQUIRE(
        processor.setBusesLayout(rinne::test::layoutOf(juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())));
    processor.prepareToPlay(44100.0, kMaxBlock);

    for (const auto path : {Path::Normal, Path::Bypassed})
    {
        DYNAMIC_SECTION(pathName(path))
        {
            long allocations = 0;
            CHECK(sweep(processor, 1, path, allocations) == 0);
            CHECK(allocations == 0);
        }
    }
}

TEST_CASE("fixture carries negative zero and NaN payloads in every long enough block", "[host][shell][passthrough]")
{
    // 夹具自己的自查：若它没把 -0.0 与带载荷的 NaN 放进块里，上面的直通测试就测不到这两类
    BlockFixture fixture;
    for (const int n : {14, 15, 1000, kMaxBlock})
    {
        fixture.fill(2, n);
        for (int c = 0; c < 2; ++c)
        {
            bool negativeZero = false;
            bool nanWithPayload = false;
            for (int i = 0; i < 14; ++i)
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &fixture.data[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)],
                            sizeof bits);
                negativeZero = negativeZero || bits == 0x80000000u;
                nanWithPayload = nanWithPayload || (bits & 0x7fffffffu) > 0x7f800000u;
            }
            CHECK(negativeZero);
            CHECK(nanWithPayload);
        }
    }
}

TEST_CASE("a flipped bit is detected by the comparison", "[host][shell][passthrough]")
{
    // 比较本身的反例：改动任何一位（包括 NaN 载荷的最低位、-0.0 的符号位）都必须被判成不相等
    BlockFixture fixture;
    fixture.fill(2, 64);
    REQUIRE(fixture.unchanged(2, 64));
    for (const int index : {0, 1, 4, 63})
    {
        for (const std::uint32_t flip : {1u, 0x80000000u})
        {
            BlockFixture damaged = fixture;
            auto& sample = damaged.data[1][static_cast<std::size_t>(index)];
            std::uint32_t bits = 0;
            std::memcpy(&bits, &sample, sizeof bits);
            bits ^= flip;
            std::memcpy(&sample, &bits, sizeof bits);
            CHECK_FALSE(damaged.unchanged(2, 64));
        }
    }
    // 块尾之后的越界写同样被抓到
    BlockFixture overrun = fixture;
    overrun.data[0][64] = 1.0f;
    CHECK_FALSE(overrun.unchanged(2, 64));
}
