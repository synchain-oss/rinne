// SPDX-License-Identifier: GPL-3.0-or-later
//
// 正式构建的音频路径：一对真处理器经测试作用域的共享内存配对。外壳不切块（整块交给引擎）、超长块只出干声、
// 两端的旁路、0 样本、不变量 8（Output 逐位不碰宿主缓冲）与不变量 7（参数恒定时与切块逐位无关；反馈 0 时逐位直通）。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../shell/AllocationCounter.h"
#include "../shell/ShellTestKit.h"
#include "WiringTestKit.h"
#include "engine/Meters.h"
#include "engine/OutputEngine.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"

namespace
{

using namespace rinne::wiring_test;
namespace engine = rinne::engine;
using rinne::test::BlockFixture;

// 第一个逐位不同的样本；没有就是 -1
long firstDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = from; i < n; ++i)
        if (!sameBits(a[i], b[i]))
            return static_cast<long>(i);
    return a.size() == b.size() ? -1 : static_cast<long>(n);
}

bool tripped(const rinne::InputProcessor& input)
{
    const std::uint32_t word = input.instanceHost().inputEngine().meters().safety.load(std::memory_order_relaxed);
    return engine::PackedSafety::state(word) == engine::SafetyState::Tripped;
}

// Output 当前绑定的环：读写头、读一段（测试线程就是音频线程，块与块之间读）
struct RingPeek
{
    explicit RingPeek(const rinne::OutputProcessor& output)
        : lease(output.instanceHost().rendezvous().bindings().current())
    {
        REQUIRE(lease.ok());
    }
    std::uint64_t head() const { return ipc::LoopRing::writeHead(lease.ring()); }
    std::vector<float> left(std::int64_t from, std::uint32_t frames) const
    {
        std::vector<float> l(frames);
        std::vector<float> r(frames);
        ipc::LoopRing::read(lease.ring(), from, frames, l.data(), r.data());
        return l;
    }
    ipc::BoundBlock lease;
};

// 切块无关的用例跑多长：首个有效读样本在 D = 1000 ms 处，500 ms 淡入，再绕两圈
constexpr std::int64_t kPartitionTotal = 4 * 48000;

float constantHalf(std::uint64_t, int)
{
    return 0.5f;
}

void startRecording(Rig& rig)
{
    rig.dryLeft.clear();
    rig.outLeft.clear();
    rig.record = true;
}

} // namespace

TEST_CASE("a block longer than declared is processed whole; one longer than 32768 gives the dry signal only",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    constexpr int kDeclared = 512;
    Rig rig(kDeclared);
    rig.source = &sine;
    rig.open(1);
    hostSetParam(*rig.input, "feedback", 50.0f);
    REQUIRE(rig.pairUp());
    rig.cycles(160, kDeclared); // 回送的 500 ms 淡入走完
    REQUIRE_FALSE(tripped(*rig.input));

    // 声明值 × 3 + 1：整块交给引擎，环路层看见违约、把 D 的下限抬到块长；Output 整块写进环
    // ⛔ 外壳按声明值截块（InstanceHost::process 里 view.length = min(n, 声明值)）→ 看不见违约
    const std::uint64_t written = rig.output->instanceHost().outputEngine().writtenFrames();
    rig.cycle(3 * kDeclared + 1);
    {
        const auto& r = rig.input->instanceHost().inputEngine().lastBlockResult();
        CHECK(r.hostViolation);
        CHECK_FALSE(r.unsupportedBlock);
        CHECK(r.delayMin >= 3 * kDeclared + 1);
    }
    CHECK(rig.output->instanceHost().outputEngine().writtenFrames() == written + 3 * kDeclared + 1);
    CHECK(rig.canaryHits == 0);
    CHECK(rig.outputTouched == 0);

    // 32769：Input 只出干声，Output 不写环、置不支持块长。Input 开头的回送在 10 ms 里淡出（环路层给不超过 5 ms 的尾巴，
    // 安全链把这一块当旁路、10 ms 斜坡），之后逐位是干声
    rig.cycles(20, kDeclared);
    const std::uint64_t before = rig.output->instanceHost().outputEngine().writtenFrames();
    const std::uint32_t unsupported = rig.output->instanceHost().outputEngine().unsupportedCount();
    startRecording(rig);
    rig.cycle(kLongestBlock);
    const auto& r = rig.input->instanceHost().inputEngine().lastBlockResult();
    CHECK(r.unsupportedBlock);
    CHECK(r.returnLength <= 240);
    CHECK(firstDiff(rig.outLeft, rig.dryLeft, 480) == -1);
    CHECK(firstDiff(rig.outLeft, rig.dryLeft) != -1); // 开头确实还有回送在淡出（不是空断言）
    CHECK(rig.output->instanceHost().outputEngine().state() == engine::WriteState::kUnsupportedBlock);
    CHECK(rig.output->instanceHost().outputEngine().writtenFrames() == before);
    CHECK(rig.output->instanceHost().outputEngine().unsupportedCount() == unsupported + 1);
    CHECK(rig.canaryHits == 0);
    CHECK(rig.outputTouched == 0);
}

TEST_CASE("a bypassed Output passes the host audio and fades the ring data to zero in 10 ms while the head moves on",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &constantHalf;
    rig.open(1);
    REQUIRE(rig.pairUp());
    rig.cycles(20);

    // ⛔ Output 的旁路回调不把旁路交给引擎（processBlockBypassed 传 false）→ 环里还是 0.5
    const std::uint64_t w0 = RingPeek(*rig.output).head();
    for (int i = 0; i < 10; ++i)
        rig.cycle(kBlock, false, true);
    const std::uint64_t w1 = RingPeek(*rig.output).head();
    CHECK(w1 == w0 + 10 * kBlock);
    CHECK(rig.outputTouched == 0);
    CHECK(rig.output->instanceHost().outputEngine().state() == engine::WriteState::kBypassedZeros);

    constexpr float kMaxStep = 0.5f / 480.0f * 1.01f;
    {
        const auto ring = RingPeek(*rig.output).left(static_cast<std::int64_t>(w0) - 16, 16 + 10 * kBlock);
        for (std::size_t i = 0; i < 16; ++i)
            CHECK(ring[i] == 0.5f);
        float previous = 0.5f;
        float largestStep = 0.0f;
        for (std::size_t i = 16; i < 16 + 480; ++i)
        {
            CHECK(ring[i] <= previous);
            largestStep = std::max(largestStep, previous - ring[i]);
            previous = ring[i];
        }
        CHECK(largestStep <= kMaxStep);
        CHECK(ring[16] > 0.0f); // 斜坡，不是阶跃
        for (std::size_t i = 16 + 481; i < ring.size(); ++i)
            CHECK(ring[i] == 0.0f);
    }

    // 解除旁路：同样 10 ms 淡回
    for (int i = 0; i < 10; ++i)
        rig.cycle();
    const auto ring = RingPeek(*rig.output).left(static_cast<std::int64_t>(w1), 10 * kBlock);
    float previous = 0.0f;
    float largestStep = 0.0f;
    for (std::size_t i = 0; i < 480; ++i)
    {
        CHECK(ring[i] >= previous);
        largestStep = std::max(largestStep, ring[i] - previous);
        previous = ring[i];
    }
    CHECK(largestStep <= kMaxStep);
    for (std::size_t i = 481; i < ring.size(); ++i)
        CHECK(ring[i] == 0.5f);
    CHECK(rig.outputTouched == 0);
}

TEST_CASE("a bypassed Input passes the dry signal bit for bit after the 10 ms fade and keeps its delay line",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    rig.open(1);
    hostSetParam(*rig.input, "feedback", 50.0f);
    REQUIRE(rig.pairUp());
    rig.cycles(150);
    REQUIRE_FALSE(tripped(*rig.input));

    // ⛔ Input 的旁路回调不把旁路交给引擎（processBlockBypassed 传 false）→ 旁路块里还有回送
    startRecording(rig);
    for (int i = 0; i < 30; ++i)
        rig.cycle(kBlock, true, false);
    CHECK(firstDiff(rig.outLeft, rig.dryLeft) != -1); // 开头 10 ms 回送在淡出
    CHECK(firstDiff(rig.outLeft, rig.dryLeft, 480) == -1);
    CHECK_FALSE(tripped(*rig.input));

    // 解除旁路：本地延迟线没清，回送在 10 ms 的斜坡里就回来（清过的话要等一整个 D = 100 ms）
    startRecording(rig);
    for (int i = 0; i < 3; ++i)
        rig.cycle();
    CHECK(firstDiff(rig.outLeft, rig.dryLeft) != -1);
    CHECK(rig.input->instanceHost().counts().suspendClears == 0);
}

TEST_CASE("zero-sample callbacks advance nothing, and one that reaches the instance host does not hide a suspend",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    rig.open(1);
    REQUIRE(rig.pairUp());
    rig.cycles(30);

    const std::int64_t processed = rig.input->getProcessedSamples();
    const std::uint64_t written = rig.output->instanceHost().outputEngine().writtenFrames();
    const std::uint64_t head = RingPeek(*rig.output).head();
    juce::MidiBuffer midi;
    auto fixture = std::make_unique<BlockFixture>();
    auto empty = fixture->view(2, 0);
    juce::AudioBuffer<float> noChannels(0, 0);
    for (rinne::RinneProcessorBase* p : {static_cast<rinne::RinneProcessorBase*>(rig.input.get()),
                                         static_cast<rinne::RinneProcessorBase*>(rig.output.get())})
    {
        p->processBlock(empty, midi);
        p->processBlockBypassed(empty, midi);
        p->processBlock(noChannels, midi);
        p->processBlockBypassed(noChannels, midi);
    }
    CHECK(rig.input->getProcessedSamples() == processed);
    CHECK(rig.output->getProcessedSamples() == processed);
    CHECK(rig.output->instanceHost().outputEngine().writtenFrames() == written);
    CHECK(RingPeek(*rig.output).head() == head);

    // 宿主把两端一起挂起 300 ms，中间来了一次 0 样本的调用（直接进 InstanceHost，不经外壳的 beginBlock）：
    // 它不算一块，挂起照样判出来、清一次
    // ⛔ InstanceHost::process 不挡 0 样本 → 挂起账从这一次记起，300 ms 被切成两段 150 ms，判不出挂起
    REQUIRE(rig.input->instanceHost().counts().suspendFlushes == 0);
    rig.advance(150'000'000);
    rig.input->instanceHost().process(empty, engine::EngineParams{}, false);
    rig.output->instanceHost().process(empty, engine::EngineParams{}, false);
    rig.advance(150'000'000);
    rig.cycles(10);
    CHECK(rig.input->instanceHost().counts().suspendFlushes == 1);
    CHECK(rig.input->instanceHost().counts().suspendClears == 1);
}

TEST_CASE("invariant 8: the Output never changes the host buffer, negative zero and NaN payloads included",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig(rinne::test::kMaxBlock);
    rig.source = &sine;
    rig.open(1);
    REQUIRE(rig.pairUp());

    // ⛔ Output 旁路时清空宿主缓冲（processBlockBypassed 里 buffer.clear()）→ 这里变红
    auto fixture = std::make_unique<BlockFixture>();
    juce::MidiBuffer midi;
    const std::uint64_t written = rig.output->instanceHost().outputEngine().writtenFrames();
    const std::uint32_t cleaned = rig.output->instanceHost().outputEngine().nonFiniteCount();
    std::uint64_t frames = 0;
    for (const int channels : {2, 1})
    {
        for (const int n : {1, 7, 64, 480, 4096, rinne::test::kMaxBlock})
        {
            for (const bool bypassed : {false, true})
            {
                fixture->fill(channels, n);
                auto buffer = fixture->view(channels, n);
                if (bypassed)
                    rig.output->processBlockBypassed(buffer, midi);
                else
                    rig.output->processBlock(buffer, midi);
                CHECK(fixture->unchanged(channels, n));
                frames += static_cast<std::uint64_t>(n);
                rig.advance(static_cast<std::uint64_t>(n) * 1'000'000'000ull / 48000ull);
            }
        }
    }
    // 真的在写环（不是没拿到环就直接返回）：帧数对得上，写进环之前清洗掉了缓冲里的非有限值
    CHECK(rig.output->instanceHost().outputEngine().writtenFrames() == written + frames);
    CHECK(rig.output->instanceHost().outputEngine().nonFiniteCount() > cleaned);
}

TEST_CASE("invariant 7: with feedback at 0 a paired Input passes its input through bit for bit",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig(rinne::test::kMaxBlock);
    rig.source = &sine;
    rig.open(1);
    REQUIRE(rig.pairUp());
    rig.cycles(150);
    REQUIRE(paramValue(*rig.input, "feedback") == 0.0f); // 默认值

    auto fixture = std::make_unique<BlockFixture>();
    juce::MidiBuffer midi;
    for (const int n : {1, 7, 64, 480, 4096, rinne::test::kMaxBlock})
    {
        fixture->fill(2, n);
        auto buffer = fixture->view(2, n);
        rig.input->processBlock(buffer, midi);
        CHECK(fixture->unchanged(2, n));
        rig.output->processBlock(buffer, midi);
        rig.advance(static_cast<std::uint64_t>(n) * 1'000'000'000ull / 48000ull);
    }
    CHECK(rig.paired());
    CHECK_FALSE(tripped(*rig.input));
}

TEST_CASE("invariant 7: with constant parameters the Input output does not depend on how the host splits blocks",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    // D = 1000 ms（大于最大块长 32768，块长不抬 D 的下限），三种切法声明的最大块长都是 32768。两端先配好对再出声，
    // 之后参数不动：回送从首个有效读样本起淡入、绕几圈，逐位比三份输出
    // ⛔ 外壳只处理每块的前 8192 帧（InstanceHost::process 里 view.length = min(n, 8192)）→ 第三种切法变红
    const auto runWith = [](const std::vector<int>& lengths) {
        Rig rig(32768);
        rig.source = [](std::uint64_t t, int channel) { return sine(t, channel) + (t % 997 == 0 ? 0.2f : 0.0f); };
        rig.input = rig.makeInput();
        rig.output = rig.makeOutput();
        hostSetParam(*rig.input, "feedback", 80.0f);
        hostSetParam(*rig.input, "mix", 50.0f);
        hostSetParam(*rig.input, "delay_ms", 1000.0f);
        rig.open(1);
        for (int i = 0; i < 50 && !rig.ringsBound(); ++i)
            rig.advance(4 * kBlockNs);
        REQUIRE(rig.ringsBound());
        startRecording(rig);
        std::int64_t done = 0;
        for (std::size_t k = 0; done < kPartitionTotal; ++k)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(lengths[k % lengths.size()], kPartitionTotal - done));
            rig.cycle(n);
            done += n;
        }
        CHECK(firstDiff(rig.outLeft, rig.dryLeft) != -1); // 回送确实在响
        CHECK(rig.input->instanceHost().inputEngine().loopStatus().resyncCount == 0u);
        CHECK_FALSE(tripped(*rig.input));
        CHECK(rig.canaryHits == 0);
        CHECK(rig.outputTouched == 0);
        return rig.outLeft;
    };
    const auto a = runWith({64});
    const auto b = runWith({480});
    const auto c = runWith({1, 7, 300, 4096, 8193, 32768, 513, 2048});
    REQUIRE(a.size() == static_cast<std::size_t>(kPartitionTotal));
    CHECK(firstDiff(a, b) == -1);
    CHECK(firstDiff(a, c) == -1);
}

TEST_CASE("the wired audio callbacks neither allocate nor free while paired, bypassed or past the declared length",
          "[host][wiring][audio]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig(kBlock);
    rig.source = &sine;
    rig.open(1);
    hostSetParam(*rig.input, "feedback", 50.0f);
    REQUIRE(rig.pairUp());
    rig.cycles(60);

    std::vector<float> left(4 * kBlock, 0.1f);
    std::vector<float> right(4 * kBlock, -0.1f);
    float* channels[2] = {left.data(), right.data()};
    juce::MidiBuffer midi;
    // 计数在作用域里读、断言在作用域外：断言本身（报告器记结果）会分配
    long allocations = 0;
    long frees = 0;
    for (int i = 0; i < 60; ++i)
    {
        const int n = i % 10 == 9 ? 3 * kBlock + 1 : kBlock; // 隔几块来一块超过声明值的
        juce::AudioBuffer<float> buffer(channels, 2, n);
        const rinne::test::AllocationScope scope;
        if (i % 7 == 3)
        {
            rig.input->processBlockBypassed(buffer, midi);
            rig.output->processBlockBypassed(buffer, midi);
        }
        else
        {
            rig.input->processBlock(buffer, midi);
            rig.output->processBlock(buffer, midi);
        }
        allocations += scope.allocations();
        frees += scope.frees();
    }
    CHECK(allocations == 0);
    CHECK(frees == 0);
    CHECK(rig.paired());
}
