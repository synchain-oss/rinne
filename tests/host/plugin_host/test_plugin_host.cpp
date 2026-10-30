// SPDX-License-Identifier: GPL-3.0-or-later
//
// 插件级宿主本身：TransportMap 是游标的纯函数、LanePlayHead 如实报出脚本给的事实、中间链按样本号做突发与延迟，
// 两个真处理器手动选组后经测试作用域配上对，以及真的 25 Hz 定时器确实调到了 tick。
// 纯逻辑的几条在验证宿主副本与探针构建里照跑；要真处理器配对的几条在那里跳过（见 WiringTestKit.h）。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "host/plugin_host/PluginHost.h"
#include "host/plugin_host/TransportMap.h"
#include "host/wiring/WiringTestKit.h"

namespace
{

using namespace rinne::plugin_host;
using rinne::wiring_test::formalOnly;
using rinne::wiring_test::needsSharedMemory;

} // namespace

TEST_CASE("plugin_host: TransportMap is a pure function of the cursor: seek, stop, start and cycle wrap",
          "[plugin_host]")
{
    TransportMap map(1000, true);
    map.add({100, TransportKind::Seek, 5000, 0, false});
    map.add({200, TransportKind::Stop, 0, 0, false});
    map.add({300, TransportKind::Start, 8000, 0, true});
    map.add({400, TransportKind::Cycle, 8000, 8160, false});

    CHECK(map.at(0).t == 1000);
    CHECK(map.at(99).t == 1099);
    CHECK(map.at(100).t == 5000);
    CHECK(map.at(150).t == 5050);
    // 停：时间线停在停下那一刻的位置
    CHECK_FALSE(map.at(250).playing);
    CHECK(map.at(250).t == 5100);
    // 起并定位
    CHECK(map.at(300).playing);
    CHECK(map.at(300).t == 8000);
    // 循环 [8000, 8160)：游标 400 时在 8100，60 个样本后回到 8000，之后每 160 个样本回绕一次
    CHECK(map.at(400).t == 8100);
    CHECK(map.at(459).t == 8159);
    CHECK(map.at(460).t == 8000);
    CHECK(map.at(620).t == 8000);
    CHECK(map.at(700).t == 8080);

    // 不连续点：事件、回绕与切点
    CHECK(map.nextBoundary(0) == 100);
    CHECK(map.nextBoundary(100) == 200);
    CHECK(map.nextBoundary(250) == 300);
    CHECK(map.nextBoundary(400) == 460);
    CHECK(map.nextBoundary(460) == 620);
    CHECK(map.nextBoundary(0, {1050}) == 50);
    // 同一份脚本，问多少次、按什么顺序问都一样
    CHECK(map.at(700).t == 8080);
    CHECK(map.at(0).t == 1000);

    // 从循环区间之后起播的位置不回绕
    TransportMap after(9000, true);
    after.add({0, TransportKind::Cycle, 8000, 8160, false});
    CHECK(after.at(1000).t == 10000);
    CHECK(after.nextBoundary(10) == kFar);
}

TEST_CASE("plugin_host: LanePlayHead reports exactly what the block script gives, including a missing tempo or "
          "timestamp",
          "[plugin_host]")
{
    TransportMap map(0, true);
    map.add({1000, TransportKind::Stop, 0, 0, false});
    LanePlayHead lane(map, kRate);

    // 默认：速度 120、时间戳取宿主时钟
    lane.arm(256, 123456789u);
    auto position = lane.getPosition();
    REQUIRE(position.hasValue());
    CHECK(position->getIsPlaying());
    CHECK(position->getTimeInSamples() == std::int64_t{0});
    CHECK(position->getBpm() == 120.0);
    CHECK(position->getHostTimeNs() == std::uint64_t{123456789u});
    lane.advance(256);
    CHECK(lane.cursor() == 256);
    // 下一块不越过停点
    CHECK(lane.maxBlock(4096) == 1000 - 256);

    // 脚本：这一块不给速度、不给时间戳
    lane.setFacts([](std::int64_t, std::uint64_t) { return HostFacts{std::nullopt, std::nullopt}; });
    lane.arm(256, 5u);
    position = lane.getPosition();
    REQUIRE(position.hasValue());
    CHECK_FALSE(position->getBpm().hasValue());
    CHECK_FALSE(position->getPpqPosition().hasValue());
    CHECK_FALSE(position->getHostTimeNs().hasValue());
    CHECK(position->getTimeInSamples() == std::int64_t{256});

    // 脚本：按游标给速度与时间戳；一块之内问几次都一样
    lane.setFacts([](std::int64_t cursor, std::uint64_t clockNs) {
        return HostFacts{cursor < 1000 ? 90.0 : 144.0, clockNs + 7u};
    });
    lane.setCursor(1024);
    lane.arm(128, 1000u);
    for (int i = 0; i < 2; ++i)
    {
        position = lane.getPosition();
        REQUIRE(position.hasValue());
        CHECK(position->getBpm() == 144.0);
        CHECK(position->getHostTimeNs() == std::uint64_t{1007u});
        CHECK_FALSE(position->getIsPlaying());
        CHECK(position->getTimeInSamples() == std::int64_t{1000});
    }
}

TEST_CASE("plugin_host: the middle chain applies gain, a fixed delay and garbage bursts by its own sample index",
          "[plugin_host]")
{
    MiddleChain chain;
    chain.setGain(2.0f);
    chain.setDelay(3);
    chain.addBurst(Garbage::kNan, 10, 12);
    chain.addBurst(Garbage::kInf, 12, 14);
    chain.addBurst(Garbage::kHuge, 14, 15);
    std::vector<float> left(16);
    std::vector<float> right(16);
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        left[i] = static_cast<float>(i + 1);
        right[i] = -static_cast<float>(i + 1);
    }
    // 两块：块中间落着突发区间的起点
    chain.process(left.data(), right.data(), 11);
    chain.process(left.data() + 11, right.data() + 11, 5);
    CHECK(chain.position() == 16);
    CHECK(left[0] == 0.0f);
    CHECK(left[2] == 0.0f);
    CHECK(left[3] == 2.0f); // 第 0 个样本 × 2，延迟 3
    CHECK(left[9] == 14.0f);
    CHECK(std::isnan(left[10]));
    CHECK(std::isnan(right[11]));
    CHECK(left[12] == std::numeric_limits<float>::infinity());
    CHECK(right[13] == -std::numeric_limits<float>::infinity());
    CHECK(left[14] == 2.4e7f); // 第 11 个样本（值 12）× 2 × 1e6
    CHECK(left[15] == 26.0f);
}

TEST_CASE("plugin_host: two real processors pair by manual group selection on the logical clock, and only within "
          "one group",
          "[plugin_host]")
{
    formalOnly();
    needsSharedMemory();
    {
        PluginHost host;
        host.source = [](std::int64_t index, float& left, float& right) {
            left = 0.25f * static_cast<float>(std::sin(static_cast<double>(index) * 0.01));
            right = -left;
        };
        host.prepare();
        REQUIRE(host.selectGroup(3));
        CHECK(host.pairUp(512));
        host.run(48000, 512);
        CHECK(host.paired());
        CHECK(host.outputEngine().writtenFrames() > 0u);
        CHECK(host.inputLane().cursor() == host.trackPosition());
        CHECK(host.outputLane().cursor() == host.trackPosition());
        CHECK(host.outputTouched == 0u);
        CHECK(host.canaryHits == 0u);
    }
    {
        // 两端不在同一组：永远配不上
        PluginHost host;
        host.prepare();
        REQUIRE(host.input().selectGroup(1));
        REQUIRE(host.output().selectGroup(2));
        host.tick();
        CHECK_FALSE(host.pairUp(512, 200));
    }
}

TEST_CASE("plugin_host: the real 25 Hz timer reaches tick; with the timer running but never pumped the rendezvous "
          "does not move",
          "[plugin_host]")
{
    formalOnly();
    needsSharedMemory();
    HostOptions options;
    options.realTimer = true;
    constexpr double waitMs = 400.0;

    // 对照：定时器起了，但没人泵：同样的墙钟时间里两端都到不了 kActive（只有 tick 能让会合往前走）
    {
        PluginHost idle(options);
        idle.prepare();
        REQUIRE(idle.selectGroup(1));
        juce::Thread::sleep(static_cast<int>(waitMs));
        CHECK_FALSE(idle.active());
        CHECK(idle.input().instanceHost().rendezvousState() != rinne::ipc::RendezvousState::kActive);
    }

    // 泵定时器：测试线程就是消息线程，到期的定时器在这里回调 timerCallback → tick。从不手动 tick
    PluginHost host(options);
    host.prepare();
    REQUIRE(host.selectGroup(1));
    const double start = juce::Time::getMillisecondCounterHiRes();
    while (!host.active() && juce::Time::getMillisecondCounterHiRes() - start < 2000.0)
    {
        juce::Timer::callPendingTimersSynchronously();
        juce::Thread::sleep(5);
    }
    // 只判「泵了就到 kActive」，不拿墙钟耗时当判据：负载高、Debug 或 sanitizer 档下泵到 kActive 可能比对照的等待长，
    // 「不泵就不动」已由上面的对照证明。实测耗时只打进 INFO 供排查
    const double toActive = juce::Time::getMillisecondCounterHiRes() - start;
    INFO("active after " << toActive << " ms of pumping");
    REQUIRE(host.active());
    for (int i = 0; i < 400 && !host.paired(); ++i)
    {
        host.cycle(480);
        juce::Timer::callPendingTimersSynchronously();
    }
    CHECK(host.paired());
}
