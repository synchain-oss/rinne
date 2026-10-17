// SPDX-License-Identifier: GPL-3.0-or-later
//
// 外壳对宿主的其余约定：延迟恒报 0、tail 的取值、总线布局、0 样本的回调不推进任何计数、验证宿主的识别。
// 测试对象是两端共用的基类行为，两端各跑一遍，免得哪一端悄悄改了。

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <memory>

#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "ShellTestKit.h"

namespace
{

using rinne::RinneProcessorBase;
using rinne::test::BlockFixture;
using rinne::test::kMaxBlock;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// 按指定的包装层类型构造处理器：JUCE 把「下一个要建的插件」的类型记在线程局部变量里，构造时读走
template<typename Processor>
std::unique_ptr<Processor> makeAs(juce::AudioProcessor::WrapperType type)
{
    juce::AudioProcessor::setTypeOfNextNewPlugin(type);
    auto processor = std::make_unique<Processor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    return processor;
}

// JUCE 按宿主可执行文件的名字认验证宿主；这是它认的几个名字（各平台只认其中一部分，见 tests/host/suite.cmake）
bool hostNameLooksLikeValidator()
{
    const auto exe = juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName();
    for (const char* name : {"pluginval", "auvaltool", "com.apple.audio.InfoHelper", "VST_Scanner"})
        if (exe.containsIgnoreCase(name))
            return true;
    return false;
}

} // namespace

TEMPLATE_TEST_CASE("latency is always zero", "[host][shell][latency]", rinne::InputProcessor, rinne::OutputProcessor)
{
    TestType processor;
    CHECK(processor.getLatencySamples() == 0);
    for (const double rate : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0})
    {
        for (const int block : {1, 32, 480, kMaxBlock})
        {
            processor.prepareToPlay(rate, block);
            CHECK(processor.getLatencySamples() == 0);
        }
    }

    auto fixture = std::make_unique<BlockFixture>();
    juce::MidiBuffer midi;
    fixture->fill(2, 512);
    auto buffer = fixture->view(2, 512);
    processor.processBlock(buffer, midi);
    processor.processBlockBypassed(buffer, midi);
    CHECK(processor.getLatencySamples() == 0);
    processor.releaseResources();
    CHECK(processor.getLatencySamples() == 0);
}

TEMPLATE_TEST_CASE("tail length follows the tail mode and the wrapper type", "[host][shell][tail]",
                   rinne::InputProcessor, rinne::OutputProcessor)
{
    using Mode = RinneProcessorBase::TailMode;

    SECTION("VST3 and unwrapped: automatic means infinite")
    {
        for (const auto type : {juce::AudioProcessor::wrapperType_Undefined, juce::AudioProcessor::wrapperType_VST3})
        {
            auto processor = makeAs<TestType>(type);
            CHECK(processor->getTailMode() == Mode::Auto);
            CHECK(processor->getTailLengthSeconds() == kInfinity);
        }
    }

    SECTION("AU: automatic means 30 seconds, never infinite")
    {
        for (const auto type :
             {juce::AudioProcessor::wrapperType_AudioUnit, juce::AudioProcessor::wrapperType_AudioUnitv3})
        {
            auto processor = makeAs<TestType>(type);
            CHECK(processor->getTailLengthSeconds() == 30.0);
        }
    }

    SECTION("an explicit mode wins over the wrapper type")
    {
        for (const auto type : {juce::AudioProcessor::wrapperType_VST3, juce::AudioProcessor::wrapperType_AudioUnit})
        {
            auto processor = makeAs<TestType>(type);
            processor->setTailMode(Mode::Infinite);
            CHECK(processor->getTailMode() == Mode::Infinite);
            CHECK(processor->getTailLengthSeconds() == kInfinity);
            processor->setTailMode(Mode::ThirtySeconds);
            CHECK(processor->getTailMode() == Mode::ThirtySeconds);
            CHECK(processor->getTailLengthSeconds() == 30.0);
            processor->setTailMode(Mode::Auto);
            CHECK(processor->getTailMode() == Mode::Auto);
        }
    }
}

TEMPLATE_TEST_CASE("only mono and stereo with matching channel counts are accepted", "[host][shell][buses]",
                   rinne::InputProcessor, rinne::OutputProcessor)
{
    TestType processor;
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto surround = juce::AudioChannelSet::create5point1();
    const auto none = juce::AudioChannelSet::disabled();

    CHECK(processor.isBusesLayoutSupported(rinne::test::layoutOf(mono, mono)));
    CHECK(processor.isBusesLayoutSupported(rinne::test::layoutOf(stereo, stereo)));
    CHECK_FALSE(processor.isBusesLayoutSupported(rinne::test::layoutOf(mono, stereo)));
    CHECK_FALSE(processor.isBusesLayoutSupported(rinne::test::layoutOf(stereo, mono)));
    CHECK_FALSE(processor.isBusesLayoutSupported(rinne::test::layoutOf(surround, surround)));
    CHECK_FALSE(processor.isBusesLayoutSupported(rinne::test::layoutOf(none, none)));
    CHECK_FALSE(processor.isBusesLayoutSupported(rinne::test::layoutOf(stereo, none)));

    // 旁路参数由包装层建，处理器自己不建（否则宿主里会出现两个旁路）
    CHECK(processor.getBypassParameter() == nullptr);

    CHECK_FALSE(processor.acceptsMidi());
    CHECK_FALSE(processor.producesMidi());
    CHECK_FALSE(processor.isMidiEffect());
}

TEMPLATE_TEST_CASE("a zero-sample callback advances no counter", "[host][shell][zero-samples]", rinne::InputProcessor,
                   rinne::OutputProcessor)
{
    TestType processor;
    processor.prepareToPlay(48000.0, 512);
    juce::MidiBuffer midi;
    REQUIRE(processor.getProcessedSamples() == 0);

    auto fixture = std::make_unique<BlockFixture>();

    SECTION("before any audio")
    {
        auto none = fixture->view(2, 0);
        processor.processBlock(none, midi);
        processor.processBlockBypassed(none, midi);
        CHECK(processor.getProcessedSamples() == 0);
    }

    SECTION("between real blocks, with and without channels")
    {
        std::int64_t total = 0;
        for (const int n : {1, 64, 7, 480})
        {
            fixture->fill(2, n);
            auto buffer = fixture->view(2, n);
            processor.processBlock(buffer, midi);
            total += n;
            REQUIRE(processor.getProcessedSamples() == total);

            auto empty = fixture->view(2, 0);
            processor.processBlock(empty, midi);
            processor.processBlockBypassed(empty, midi);
            juce::AudioBuffer<float> noChannels(0, 0);
            processor.processBlock(noChannels, midi);
            processor.processBlockBypassed(noChannels, midi);
            CHECK(processor.getProcessedSamples() == total);

            // 旁路的回调同样累计
            auto again = fixture->view(2, n);
            processor.processBlockBypassed(again, midi);
            total += n;
            CHECK(processor.getProcessedSamples() == total);
        }
    }
}

TEMPLATE_TEST_CASE("validation host detection follows the host executable", "[host][shell][validation-host]",
                   rinne::InputProcessor, rinne::OutputProcessor)
{
    // 同一个测试由几个 ctest 条目跑：原名可执行文件（普通宿主），和带验证宿主名字的副本（每个平台认的名字见
    // suite.cmake）。 副本那条把 RINNE_EXPECT_VALIDATION_HOST 设成 1，没认出来或者副本根本没换名都会变红。
    TestType processor;
    const bool expectValidation = juce::SystemStats::getEnvironmentVariable("RINNE_EXPECT_VALIDATION_HOST", "") == "1";

    CHECK(processor.isValidationHost() == hostNameLooksLikeValidator());
    CHECK(processor.isValidationHost() == expectValidation);

    // 外壳阶段还没有共享内存段：验证宿主与普通宿主一样不开段，音频一样逐位直通。段接上之后在这里加「验证宿主下不建段」的断言。
    auto fixture = std::make_unique<BlockFixture>();
    juce::MidiBuffer midi;
    fixture->fill(2, 480);
    auto buffer = fixture->view(2, 480);
    processor.processBlock(buffer, midi);
    CHECK(fixture->unchanged(2, 480));
    CHECK(processor.getProcessedSamples() == 480);
}
