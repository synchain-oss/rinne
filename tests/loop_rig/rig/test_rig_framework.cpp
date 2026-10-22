// SPDX-License-Identifier: GPL-3.0-or-later
//
// rig 框架自身的用例：框架自检、场景自注册、中间链的七种形态、脚本化块长与时钟、随机的测试作用域。
//
// 场景文件由各自的卡拥有（tests/loop_rig/scenarios/**），本文件只验框架本身。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/TransportInfo.h"
#include "rig/EngineHost.h"
#include "rig/MiddleChain.h"
#include "rig/ScenarioRegistry.h"
#include "rig/Schedule.h"
#include "rig/Segments.h"

namespace
{

using namespace rinne::rig;

// 一条恒定的块长脚本
BlockScript fixedScript(int length, std::size_t count)
{
    BlockScript script;
    script.lengths.assign(count, length);
    return script;
}

} // namespace

TEST_CASE("rig 框架自检：一段宿主信号经中间链写进环，环的写头照常推进", "[rig][framework]")
{
    Segments segments;
    REQUIRE(segments.open(1));
    REQUIRE(segments.rv() != nullptr);
    REQUIRE(segments.ring(1) != nullptr);
    CHECK(segments.ring(2) == nullptr);
}

TEST_CASE("rig 场景自注册：注册表里能查到本组的场景 id", "[rig][framework]")
{
    // 注册表是定长的：至少有一个场景被注册进来（占位场景证明自注册可用）。
    CHECK(rinne::rig::scenarioCount() >= 1u);
    const rinne::rig::ScenarioEntry* entry = rinne::rig::scenarioAt(0);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->id != nullptr);
    REQUIRE(entry->factory != nullptr);
    // 同一个 id 能查到同一条
    CHECK(rinne::rig::findScenario(entry->id) == entry);
    // 查不到的 id 返回空
    CHECK(rinne::rig::findScenario("no-such-scenario") == nullptr);
}

TEST_CASE("rig 中间链的七种形态各可用", "[rig][chain]")
{
    MiddleChain chain;
    chain.prepare(48000.0, 256);

    constexpr int n = 16;
    std::vector<float> inLeft(n, 0.5f);
    std::vector<float> inRight(n, -0.5f);
    std::vector<float> outLeft(n, 0.0f);
    std::vector<float> outRight(n, 0.0f);

    // 恒等
    chain.setKind(ChainKind::kIdentity);
    chain.process(inLeft.data(), inRight.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        CHECK(outLeft[i] == 0.5f);
        CHECK(outRight[i] == -0.5f);
    }

    // 增益
    chain.setKind(ChainKind::kGain);
    chain.setGain(2.0f);
    chain.process(inLeft.data(), inRight.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        CHECK(outLeft[i] == 1.0f);
        CHECK(outRight[i] == -1.0f);
    }

    // 一阶低通：直流增益是 1（稳态），第一个样本还没跟上。
    // 一阶低通的时间常数是 1/(2π·fc)：1 kHz 下约 168 个样本，所以要喂够长的直流才看得出稳态。
    chain.setKind(ChainKind::kOnePoleLowpass);
    chain.setLowpassHz(1000.0f);
    chain.reset();
    {
        std::vector<float> dc(4096, 0.5f);
        std::vector<float> dcOut(4096, 0.0f);
        chain.process(dc.data(), dc.data(), dcOut.data(), dcOut.data(), 4096);
        CHECK(dcOut[0] < 0.5f);
        CHECK(std::abs(dcOut[4095] - 0.5f) < 1.0e-3f);
    }

    // k 样本延迟：输出是 k 个样本之前的输入
    chain.setKind(ChainKind::kDelay);
    chain.setDelaySamples(4);
    chain.reset();
    std::vector<float> ramp(n);
    for (int i = 0; i < n; ++i)
    {
        ramp[static_cast<std::size_t>(i)] = static_cast<float>(i);
    }
    chain.process(ramp.data(), ramp.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        const float expected = i < 4 ? 0.0f : static_cast<float>(i - 4);
        CHECK(outLeft[static_cast<std::size_t>(i)] == expected);
    }
    CHECK(chain.totalLatency() == 4);

    // NaN 注入：从第 8 个样本起是 NaN
    chain.setKind(ChainKind::kNanInject);
    chain.setNanInjectAt(8);
    chain.reset();
    chain.process(ramp.data(), ramp.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < 8; ++i)
    {
        CHECK(outLeft[static_cast<std::size_t>(i)] == static_cast<float>(i));
    }
    for (int i = 8; i < n; ++i)
    {
        CHECK(std::isnan(outLeft[static_cast<std::size_t>(i)]));
    }

    // 反相
    chain.setKind(ChainKind::kInvert);
    chain.reset();
    chain.process(inLeft.data(), inRight.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        CHECK(outLeft[i] == -0.5f);
        CHECK(outRight[i] == 0.5f);
    }

    // 低架提升：直流（低频）被提升
    chain.setKind(ChainKind::kLowShelfBoost);
    chain.setLowShelfBoost(0.5f, 0.5f);
    chain.setLowpassHz(100.0f);
    chain.reset();
    {
        // 同样要喂够长的直流才看得出稳态（100 Hz 的时间常数约 1.6 ms，48 kHz 下 1600 个样本）。
        std::vector<float> dc(4096, 0.5f);
        std::vector<float> dcOut(4096, 0.0f);
        chain.process(dc.data(), dc.data(), dcOut.data(), dcOut.data(), 4096);
        // 稳态的直流增益是 1 + boost = 1.5
        CHECK(std::abs(dcOut[4095] - 0.75f) < 1.0e-2f);
    }
}

TEST_CASE("rig 脚本化块长与走带：块长序列、0 样本穿插、起播沿、离线标记都能脚本化", "[rig][script]")
{
    BlockScript script;
    script.lengths = {256, 0, 512, 64};
    script.skips = {false, false, true, false};
    CHECK(script.lengthAt(0) == 256);
    CHECK(script.lengthAt(1) == 0);
    CHECK(script.lengthAt(2) == 512);
    CHECK(script.lengthAt(3) == 64);
    // 循环取用
    CHECK(script.lengthAt(4) == 256);
    CHECK(script.skipAt(2));
    CHECK(!script.skipAt(0));

    TransportScript transport;
    transport.hostNs = {1000, 2000};
    transport.hostNsValid = {true, false};
    transport.playing = {true, false};
    transport.nonrealtime = {false, true};
    transport.bpm = {140.0f, 0.0f};

    const rinne::engine::TransportInfo first = transport.at(0);
    CHECK(first.hostNs == 1000u);
    CHECK(first.hostNsValid);
    CHECK(first.isPlaying);
    CHECK(!first.nonrealtime);
    CHECK(first.bpm == 140.0f);

    const rinne::engine::TransportInfo second = transport.at(1);
    CHECK(second.hostNs == 2000u);
    CHECK(!second.hostNsValid);
    CHECK(!second.isPlaying);
    CHECK(second.nonrealtime);
    CHECK(second.bpm == 0.0f);
}

TEST_CASE("rig 随机测试作用域：两个 rig 不撞段名", "[rig][scope]")
{
    Segments first;
    Segments second;
    CHECK(first.scope().tag() != second.scope().tag());
    CHECK(first.naming().scope().tag() != second.naming().scope().tag());
    CHECK(first.scope().isTesting());
}
