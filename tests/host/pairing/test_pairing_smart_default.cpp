// SPDX-License-Identifier: GPL-3.0-or-later
//
// 智能默认：插入顺序（Input 先 / Output 先）、同名轨优先、两个 Input 一个 Output、同一拍里看见前一个刚建的段、
// 不做决定的拍子按扫描间隔重开只读视图、卡在初始化一半的段做决定时也按扫描间隔重开、state 读入期间不认领、8 组全满、
// 五项触发条件各缺一次、Output 等待中每秒重评、手选之后永不迁组。
// 规则以 ipc/PairingPolicy.h 与 ADR 0004 为准，这里只验接线。
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "PairingKit.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;
using rinne::RinneProcessorBase;

bool logged(const char* text)
{
    for (const juce::String& line : captured().logLines)
        if (line.contains(text))
            return true;
    return false;
}

// 宿主收到参数通知时回头 tick 一次：模拟定时器恰好落在 setState 写参数树的那一段里
class TickOnParameterChange final : public juce::AudioProcessorListener
{
public:
    explicit TickOnParameterChange(RinneProcessorBase& processor) : processor_(processor) {}
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override
    {
        processor_.instanceHost().tick();
        ++ticks;
    }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}

    int ticks = 0;

private:
    RinneProcessorBase& processor_;
};

} // namespace

TEST_CASE("pairing: an Input placed first takes the lowest empty group and the Output joins it",
          "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& input = bench.addInput();
    DirtyListener listener;
    input.addListener(&listener);
    bench.run(200);
    CHECK(groupOf(input) == 0); // 距 prepare 还不到 300 ms
    bench.run(200);
    REQUIRE(holds(input, 1));
    CHECK(provisional(input));
    CHECK(configsOf(input).group == 1);
    CHECK(configsOf(input).groupOrigin == state::GroupOrigin::Smart);
    CHECK_FALSE(settled(input)); // 认领不定型
    CHECK(listener.dirtyMarks == 0); // 也不标脏
    CHECK(logged("input: pairing: smart default chose group A (empty group)"));
    input.removeListener(&listener);

    OutputProcessor& output = bench.addOutput();
    bench.run(400);
    CHECK(holds(output, 1));
    CHECK(provisional(output));
    CHECK(configsOf(output).group == 1);
    CHECK(configsOf(output).groupOrigin == state::GroupOrigin::Smart);
    CHECK(logged("output: pairing: smart default chose group A (latest input)"));
    bench.run(100);
    CHECK(pairStateOf(input) == PairState::kConnectedUnverified);
    CHECK(pairStateOf(output) == PairState::kConnectedUnverified);
}

TEST_CASE("pairing: an Output placed first waits in an empty group and the Input joins it", "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    bench.run(400);
    REQUIRE(holds(output, 1));
    CHECK(logged("output: pairing: smart default chose group A (empty group, waiting for input)"));

    InputProcessor& input = bench.addInput();
    bench.run(400);
    CHECK(holds(input, 1));
    CHECK(holds(output, 1));
    CHECK(logged("input: pairing: smart default chose group A (waiting output)"));

    // 配上之后等待结束：后来的 Input 去别的组，Output 不跟着走
    InputProcessor& later = bench.addInput();
    bench.run(2500);
    CHECK(holds(later, 2));
    CHECK(holds(output, 1));
}

TEST_CASE("pairing: an Output on a same-named track goes to that Input before the latest one", "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& vox = bench.addInput();
    setTrack(vox, juce::String("Lead Vox"));
    bench.run(400);
    REQUIRE(holds(vox, 1));
    InputProcessor& gtr = bench.addInput();
    setTrack(gtr, juce::String("Rhythm Gtr"));
    bench.run(400);
    REQUIRE(holds(gtr, 2)); // 没有在等的 Output：编号最小的空组

    SECTION("the same name wins over the latest claim")
    {
        OutputProcessor& output = bench.addOutput();
        setTrack(output, juce::String("Lead Vox"));
        bench.run(400);
        CHECK(holds(output, 1));
        CHECK(logged("output: pairing: smart default chose group A (same-name input)"));
    }
    SECTION("a name that matches nothing falls back to the latest Input")
    {
        OutputProcessor& output = bench.addOutput();
        setTrack(output, juce::String("Bass"));
        bench.run(400);
        CHECK(holds(output, 2));
    }
    SECTION("no name at all falls back to the latest Input")
    {
        OutputProcessor& output = bench.addOutput();
        bench.run(400);
        CHECK(holds(output, 2));
    }
}

TEST_CASE("pairing: two Inputs deciding in the same tick never pick the same group", "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& first = bench.addInput();
    InputProcessor& second = bench.addInput();
    bench.run(400);
    CHECK(holds(first, 1));
    CHECK(holds(second, 2));

    // 两个 Input 在同一拍认领（claim_ms 相同）：并列取编号小的组
    OutputProcessor& output = bench.addOutput();
    bench.run(400);
    CHECK(holds(output, 1));
    CHECK(holds(first, 1));
    CHECK(holds(second, 2));
}

// 前一个实例这一拍刚建了会合段、认领了组，后一个实例同一拍做决定时必须看见它：要做决定的那一拍强制重开只读视图，
// 不能沿用上一次「段不存在」的结论。两个实例重开视图的节拍错开（建的时间不同），做决定却落在同一拍
TEST_CASE("pairing: an instance deciding in the same tick sees a segment created earlier in that tick",
          "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& first = bench.addInput(false);
    bench.run(100);
    InputProcessor& second = bench.addInput(false);
    bench.run(200);
    first.prepareToPlay(kRate, kBlock);
    second.prepareToPlay(kRate, kBlock);
    bench.run(400);
    CHECK(holds(first, 1));
    CHECK(holds(second, 2));
}

// 只读视图打不开（这里是段还不存在）时，不做决定的拍子按观察者的扫描间隔（250 ms）再试，不是每拍都试：
// 从没 prepare 过的实例 1 s 里 25 拍，只该试 4 次左右。做决定的拍子强制再试，见上一条用例
TEST_CASE("pairing: while no smart decision is due an unopenable view is retried at the scan interval, not every tick",
          "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    CountingBackend backend;
    Bench bench;
    rinne::InstanceHostConfig config = bench.config();
    config.backend = &backend;
    InputProcessor& input = bench.add(std::make_unique<InputProcessor>(config), false);
    bench.run(1000);
    CHECK(backend.readOnlyOpens >= 2);
    CHECK(backend.readOnlyOpens <= 5);
    CHECK(groupOf(input) == 0);

    // prepare 之后五项一齐就决定，照常认领（段由它自己的会合建）
    input.prepareToPlay(kRate, kBlock);
    bench.run(400);
    CHECK(holds(input, 1));
}

// 别的实例建了会合段、初始化到一半就停住（段头一直没发布）：快照一直不能用，五项齐了之后每一拍都要做决定、都做不成。
// 强制重开只对「段不存在」生效，这时照样按扫描间隔：1 s 里 25 拍只开 4 次左右，不是每拍一次打开、校验、解映射
TEST_CASE("pairing: a rendezvous segment stuck mid-initialisation is reopened at the scan interval while a decision is "
          "due",
          "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    CountingBackend backend;
    Bench bench;
    rinne::InstanceHostConfig config = bench.config();
    config.backend = &backend;

    // 与实例的会合同一个作用域的会合段：只建、不初始化（magic 一直是 0）。比 Bench 后建、先拆
    struct StuckSegment
    {
        ipc::PlatformSegmentBackend backend;
        ipc::SegmentView view;
        ~StuckSegment()
        {
            if (view.base != nullptr)
                backend.unmap(view);
        }
    } stuck;
    std::string name;
    REQUIRE(ipc::Naming(config.scope).name(ipc::SegmentId::rendezvous(), name) == ipc::NameStatus::kOk);
    REQUIRE(stuck.backend.createOrOpen(name, ipc::kRvBytes, stuck.view) == ipc::InitResult::kOk);

    InputProcessor& input = bench.add(std::make_unique<InputProcessor>(config), true);
    bench.run(400); // 过了 300 ms、实时块也够了：从这里起每一拍都要做决定
    const int before = backend.readOnlyOpens;
    bench.run(1000);
    CHECK(backend.readOnlyOpens - before >= 3);
    CHECK(backend.readOnlyOpens - before <= 5);
    CHECK(groupOf(input) == 0); // 快照一直不能用：不决定、不认领
}

// 工程 state 读入的过程中（胶水已经换上设置、正在写参数树）来了一拍：这一拍不做智能认领，否则智能组会写进刚换上的
// 设置，而交给会合的是工程里的组号，两边不一致
TEST_CASE("pairing: no smart claim lands while a project state is being loaded", "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& input = bench.addInput();
    bench.run(280); // 最后一拍在 280 ms
    bench.step();
    bench.step(); // 300 ms：五项已齐，下一拍还没到
    REQUIRE(groupOf(input) == 0);

    TickOnParameterChange ticker(input);
    input.addListener(&ticker);
    restore(input, rinne::wiring_test::stateBytes(state::Role::Input, [](state::StateDocument& d) {
                d.configs.group = 3;
                d.configs.groupOrigin = state::GroupOrigin::User;
                d.params.set("feedback", 30.0f);
            }));
    input.removeListener(&ticker);
    REQUIRE(ticker.ticks > 0);
    bench.run(200);
    CHECK(holds(input, 3));
    CHECK(configsOf(input).group == 3);
    CHECK(configsOf(input).groupOrigin == state::GroupOrigin::User);
}

TEST_CASE("pairing: with all eight groups taken nothing is claimed and the state is unassigned",
          "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;

    SECTION("a ninth Input")
    {
        for (int group = 1; group <= 8; ++group)
            REQUIRE(bench.addInput().selectGroup(group));
        bench.run(100);
        InputProcessor& ninth = bench.addInput();
        bench.run(1000);
        CHECK(groupOf(ninth) == 0);
        CHECK(stateOf(ninth) == ipc::RendezvousState::kUnassigned);
        CHECK(pairStateOf(ninth) == PairState::kUnassigned);
        CHECK(configsOf(ninth).group == 0);
        CHECK(configsOf(ninth).groupOrigin == state::GroupOrigin::Undecided);
        CHECK(logged("input: pairing: smart default found no free group"));
    }
    SECTION("a ninth Output")
    {
        for (int group = 1; group <= 8; ++group)
            REQUIRE(bench.addOutput().selectGroup(group));
        bench.run(100);
        OutputProcessor& ninth = bench.addOutput();
        bench.run(1000);
        CHECK(groupOf(ninth) == 0);
        CHECK(pairStateOf(ninth) == PairState::kUnassigned);
        CHECK(configsOf(ninth).group == 0);
    }
}

TEST_CASE("pairing: each trigger condition held back once keeps the instance out of every group",
          "[host][pairing][smart][trigger]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;

    SECTION("never prepared")
    {
        InputProcessor& input = bench.addInput(false);
        bench.run(1000);
        CHECK(groupOf(input) == 0);
        input.prepareToPlay(kRate, kBlock); // 五项齐了
        bench.run(400);
        CHECK(holds(input, 1));
    }
    SECTION("fewer than eight realtime blocks")
    {
        InputProcessor& input = bench.addInput();
        bench.run(70); // 7 块
        bench.setRunning(input, false);
        bench.idle(1000);
        CHECK(groupOf(input) == 0);
        bench.setRunning(input, true);
        bench.run(10); // 第 8 块
        bench.run(40); // 下一拍
        CHECK(holds(input, 1));
    }
    SECTION("offline blocks do not count")
    {
        InputProcessor& input = bench.addInput();
        input.setNonRealtime(true);
        bench.run(1000);
        CHECK(groupOf(input) == 0);
        input.setNonRealtime(false);
        bench.run(200);
        CHECK(holds(input, 1));
    }
    SECTION("less than 300 ms since the first prepare")
    {
        InputProcessor& input = bench.addInput();
        bench.run(280); // 28 块，最后一拍在 280 ms
        CHECK(groupOf(input) == 0);
        bench.run(40); // 320 ms 的那一拍
        CHECK(holds(input, 1));
    }
    SECTION("a decided group in the project state")
    {
        SECTION("the user had left every group")
        {
            InputProcessor& input = bench.addInput();
            restore(input, groupState(state::Role::Input, 0, state::GroupOrigin::User));
            bench.run(1000);
            CHECK(groupOf(input) == 0);
            CHECK(stateOf(input) == ipc::RendezvousState::kUnassigned);
        }
        SECTION("the project names a group")
        {
            InputProcessor& input = bench.addInput();
            restore(input, groupState(state::Role::Input, 3, state::GroupOrigin::Smart));
            bench.run(1000);
            CHECK(holds(input, 3));
            CHECK_FALSE(provisional(input)); // 工程恢复的认领不是 provisional
        }
        SECTION("an undecided project state does not hold it back")
        {
            InputProcessor& input = bench.addInput();
            restore(input, groupState(state::Role::Input, 0, state::GroupOrigin::Undecided));
            bench.run(400);
            CHECK(holds(input, 1));
        }
    }
    SECTION("a manual pick in this session")
    {
        InputProcessor& input = bench.addInput();
        REQUIRE(input.selectGroup(0));
        bench.run(1000);
        CHECK(groupOf(input) == 0);
    }
}

TEST_CASE("pairing: a waiting Output re-evaluates once a second and stops once paired", "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    REQUIRE(runUntil(bench, [&] { return groupOf(output) != 0; }));
    const std::uint64_t decidedMs = bench.nowMs();
    REQUIRE(holds(output, 1));

    // 用户把一个 Input 手选到 C：Output 要到下一次重评才过去，不是下一拍
    InputProcessor& input = bench.addInput();
    REQUIRE(input.selectGroup(3));
    while (bench.nowMs() < decidedMs + 960)
    {
        bench.step();
        REQUIRE(groupOf(output) == 1);
    }
    bench.run(100);
    CHECK(holds(output, 3));
    CHECK(configsOf(output).group == 3);
    CHECK(configsOf(output).groupOrigin == state::GroupOrigin::Smart);
    CHECK(logged("output: pairing: re-evaluated, moved to group C (latest input)"));

    // 配上之后不再重评
    InputProcessor& later = bench.addInput();
    REQUIRE(later.selectGroup(5));
    bench.run(3000);
    CHECK(holds(output, 3));
}

TEST_CASE("pairing: after a manual pick the instance never moves on its own", "[host][pairing][smart]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;

    SECTION("picked before any smart default")
    {
        OutputProcessor& output = bench.addOutput();
        REQUIRE(output.selectGroup(5));
        InputProcessor& input = bench.addInput();
        REQUIRE(input.selectGroup(2));
        bench.run(3000);
        CHECK(holds(output, 5));
        CHECK(holds(input, 2));
        CHECK(configsOf(output).groupOrigin == state::GroupOrigin::User);
    }
    SECTION("picked while waiting after a smart default")
    {
        OutputProcessor& output = bench.addOutput();
        REQUIRE(runUntil(bench, [&] { return groupOf(output) != 0; }));
        REQUIRE(output.selectGroup(4));
        InputProcessor& input = bench.addInput();
        REQUIRE(input.selectGroup(2));
        bench.run(3000);
        CHECK(holds(output, 4));
        CHECK(holds(input, 2));
    }
}
