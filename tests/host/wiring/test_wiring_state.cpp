// SPDX-License-Identifier: GPL-3.0-or-later
//
// 正式构建的 state 与定型：两个处理器的 get/setState 都经胶水；定型之前成功读入的 state 全量应用（组号交给会合，意图是
// 工程恢复；tail_mode 交给外壳），之后只取参数；哪些界面入口定型、哪些不定型；program 名与 tail。
// 删除式验证点记在本卡的验证记录里：用例旁标 ⛔ 的那一处改坏，用例必须变红。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "WiringTestKit.h"
#include "ipc/Rendezvous.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace
{

using namespace rinne::wiring_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;
using rinne::RinneProcessorBase;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// 记下处理器发给宿主的通知：参数手势的开始与结束、非参数改动（标脏）
class HostListener final : public juce::AudioProcessorListener
{
public:
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++parameterChanges; }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged && details.programChanged)
            ++dirtyMarks;
    }
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++gestureBegins; }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override { ++gestureEnds; }

    int parameterChanges = 0;
    int dirtyMarks = 0;
    int gestureBegins = 0;
    int gestureEnds = 0;
};

void setParam(state::StateDocument& document, const char* id, float value)
{
    document.params.set(id, value);
}

} // namespace

TEST_CASE("a project loaded after a default preset or an old state wins: group, limiter, ceiling and tail",
          "[host][wiring][state]")
{
    formalOnly();
    Rig rig;
    auto input = rig.makeInput();
    REQUIRE_FALSE(input->stateGlue()->isSettled());

    // 宿主先下发默认预设（或早先存的旧 state），再下发工程：两次都在定型之前，以最后一份为准
    loadState(*input, stateBytes(state::Role::Input, [](state::StateDocument& d) {
        d.configs.group = 2;
        d.configs.tailMode = state::TailMode::Infinite;
        setParam(d, "limiter", 0.0f);
        setParam(d, "ceiling", -3.0f);
    }));
    CHECK(input->instanceHost().group() == 2);
    CHECK(paramValue(*input, "limiter") == 0.0f);

    loadState(*input, stateBytes(state::Role::Input, [](state::StateDocument& d) {
        d.configs.group = 4;
        d.configs.tailMode = state::TailMode::ThirtySeconds;
        setParam(d, "limiter", 1.0f);
        setParam(d, "ceiling", -12.0f);
        setParam(d, "feedback", 30.0f);
    }));
    // ⛔ 全量应用之后不把设置交出去（loadHostState 不调 restoreConfigs）→ 组号与 tail 停在上一份
    CHECK(input->instanceHost().group() == 4);
    CHECK(input->stateGlue()->configs().group == 4);
    CHECK(paramValue(*input, "limiter") == 1.0f);
    CHECK(paramValue(*input, "ceiling") == -12.0f);
    CHECK(paramValue(*input, "feedback") == 30.0f);
    CHECK(input->getTailMode() == RinneProcessorBase::TailMode::ThirtySeconds);
    CHECK(input->getTailLengthSeconds() == 30.0);
    CHECK_FALSE(input->stateGlue()->isSettled()); // 宿主下发的 state 不定型

    // 存出来的就是这一份（经胶水，不是外壳另写的）
    juce::MemoryBlock saved;
    input->getStateInformation(saved);
    state::StateDocument decoded;
    const auto result = state::loadState(static_cast<const std::uint8_t*>(saved.getData()), saved.getSize(),
                                         state::Role::Input, decoded);
    REQUIRE(result.status == state::StateLoadStatus::Ok);
    CHECK(decoded.configs.group == 4);
    CHECK(decoded.configs.tailMode == state::TailMode::ThirtySeconds);

    // Output 同样：工程的组号与 tail 生效
    auto output = rig.makeOutput();
    loadState(*output, stateBytes(state::Role::Output, [](state::StateDocument& d) {
        d.configs.group = 5;
        d.configs.tailMode = state::TailMode::ThirtySeconds;
    }));
    CHECK(output->instanceHost().group() == 5);
    CHECK(output->getTailLengthSeconds() == 30.0);
    loadState(*output, stateBytes(state::Role::Output, [](state::StateDocument& d) {
        d.configs.group = 6;
        d.configs.tailMode = state::TailMode::Infinite;
    }));
    CHECK(output->instanceHost().group() == 6);
    CHECK(output->getTailLengthSeconds() == kInfinity);
}

TEST_CASE("after the user changed something, a host preset or A/B swap only changes parameters, never the group",
          "[host][wiring][state]")
{
    formalOnly();
    Rig rig;
    auto input = rig.makeInput();
    // 界面上拖了一下反馈、又手动选了组：定型
    input->userParamGestureBegan();
    hostSetParam(*input, "feedback", 20.0f);
    REQUIRE(input->selectGroup(2));
    REQUIRE(input->stateGlue()->isSettled());
    const std::uint32_t seqBefore = input->stateGlue()->stateSeq();

    // 宿主预设（A），再切到 B：参数换成读入的，组号、tail 与其余设置不动，每次 stateSeq 恰好加一（引擎据此淡入）
    loadState(*input, stateBytes(state::Role::Input, [](state::StateDocument& d) {
        d.configs.group = 6;
        d.configs.tailMode = state::TailMode::ThirtySeconds;
        setParam(d, "feedback", 55.0f);
        setParam(d, "limiter", 0.0f);
    }));
    CHECK(paramValue(*input, "feedback") == 55.0f);
    CHECK(paramValue(*input, "limiter") == 0.0f);
    CHECK(input->instanceHost().group() == 2);
    CHECK(input->stateGlue()->configs().group == 2);
    CHECK(input->getTailMode() == RinneProcessorBase::TailMode::Auto);
    CHECK(input->stateGlue()->stateSeq() == seqBefore + 1);

    loadState(*input, stateBytes(state::Role::Input, [](state::StateDocument& d) {
        d.configs.group = 7;
        setParam(d, "feedback", 10.0f);
    }));
    CHECK(paramValue(*input, "feedback") == 10.0f);
    CHECK(input->instanceHost().group() == 2);
    CHECK(input->stateGlue()->stateSeq() == seqBefore + 2);

    // 读不进来的字节：什么都不动（保持当前，不是回到默认）
    const std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    loadState(*input, garbage);
    CHECK(input->stateGlue()->lastLoadStatus() != state::StateLoadStatus::Ok);
    CHECK(paramValue(*input, "feedback") == 10.0f);
    CHECK(input->instanceHost().group() == 2);

    // Output：定型之后的 state 一样不动组号
    auto output = rig.makeOutput();
    REQUIRE(output->selectGroup(3));
    loadState(*output, stateBytes(state::Role::Output, [](state::StateDocument& d) {
        d.configs.group = 6;
        d.configs.tailMode = state::TailMode::ThirtySeconds;
    }));
    CHECK(output->instanceHost().group() == 3);
    CHECK(output->getTailMode() == RinneProcessorBase::TailMode::Auto);
}

TEST_CASE("a project's group reaches the rendezvous as a restore: it takes over a dormant slot, a manual pick does not",
          "[host][wiring][state]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;

    // 上一次打开工程留下的实例：组 2 的 Output 槽被它持有、休眠了（宿主停用了它）
    auto sleeper = rig.makeOutput();
    rig.extraTicks.push_back(sleeper.get());
    sleeper->prepareToPlay(kRate, kBlock);
    REQUIRE(sleeper->selectGroup(2));
    rig.tick();
    REQUIRE(sleeper->instanceHost().rendezvousState() == ipc::RendezvousState::kActive);
    sleeper->releaseResources();
    REQUIRE(sleeper->instanceHost().rendezvousState() == ipc::RendezvousState::kDormant);
    rig.advance(2'500'000'000ull);

    // 重新载入工程的新实例：state 里的组号经会合认领，意图是工程恢复，能拿休眠槽
    // ⛔ 组号交给会合时用手动选组的意图（kManual）→ 拿不到休眠槽，停在冲突里
    rig.output = rig.makeOutput();
    loadState(*rig.output, stateBytes(state::Role::Output, [](state::StateDocument& d) { d.configs.group = 2; }));
    CHECK(rig.output->instanceHost().group() == 2);
    rig.output->prepareToPlay(kRate, kBlock);
    for (int i = 0; i < 150 && rig.output->instanceHost().rendezvousState() != ipc::RendezvousState::kActive; ++i)
        rig.advance(4 * kBlockNs);
    CHECK(rig.output->instanceHost().rendezvousState() == ipc::RendezvousState::kActive);
    CHECK(sleeper->instanceHost().rendezvousState() != ipc::RendezvousState::kActive);

    // 对照：同样的休眠槽，界面上手动选进去的实例拿不到（要用户确认接管，那是界面卡的事）
    auto sleeper3 = rig.makeOutput();
    rig.extraTicks.push_back(sleeper3.get());
    sleeper3->prepareToPlay(kRate, kBlock);
    REQUIRE(sleeper3->selectGroup(3));
    rig.tick();
    REQUIRE(sleeper3->instanceHost().rendezvousState() == ipc::RendezvousState::kActive);
    sleeper3->releaseResources();
    rig.advance(2'500'000'000ull);
    auto picked = rig.makeOutput();
    rig.extraTicks.push_back(picked.get());
    picked->prepareToPlay(kRate, kBlock);
    REQUIRE(picked->selectGroup(3));
    for (int i = 0; i < 150; ++i)
        rig.advance(4 * kBlockNs);
    CHECK(picked->instanceHost().rendezvousState() == ipc::RendezvousState::kConflict);
    CHECK(sleeper3->instanceHost().rendezvousState() == ipc::RendezvousState::kDormant);

    rig.extraTicks.clear();
}

TEST_CASE("the loop switch, a manual group pick and a user gesture settle; panic, recover and automation do not",
          "[host][wiring][state]")
{
    formalOnly();
    Rig rig;
    HostListener listener;

    SECTION("host automation, panic and recover leave the instance unsettled")
    {
        auto input = rig.makeInput();
        input->prepareToPlay(kRate, kBlock);
        hostSetParam(*input, "feedback", 40.0f);
        hostSetParam(*input, "loop_on", 0.0f);
        input->panic();
        input->resumeLoop();
        rig.input = std::move(input);
        rig.cycles(5);
        CHECK_FALSE(rig.input->stateGlue()->isSettled());
    }

    SECTION("the loop switch is one user gesture on loop_on and settles")
    {
        auto input = rig.makeInput();
        input->addListener(&listener);
        REQUIRE(input->isLoopOn());
        // ⛔ 环路开关不定型（setLoopOnFromUi 不调 markUserAction）→ 这里变红
        input->setLoopOnFromUi(false);
        CHECK(input->stateGlue()->isSettled());
        CHECK_FALSE(input->isLoopOn());
        CHECK(paramValue(*input, "loop_on") == 0.0f);
        CHECK(listener.gestureBegins == 1);
        CHECK(listener.gestureEnds == 1);
        CHECK(listener.parameterChanges == 1);
        input->setLoopOnFromUi(true);
        CHECK(input->isLoopOn());
        input->removeListener(&listener);
    }

    SECTION("a manual group pick settles, reaches the rendezvous and marks the project dirty")
    {
        auto input = rig.makeInput();
        input->addListener(&listener);
        // ⛔ 手动选组不标脏（pickGroup 不调 markStateDirty）→ dirtyMarks 为 0
        REQUIRE(input->selectGroup(3));
        CHECK(input->stateGlue()->isSettled());
        CHECK(input->instanceHost().group() == 3);
        CHECK(input->stateGlue()->configs().group == 3);
        CHECK(input->stateGlue()->configs().groupOrigin == state::GroupOrigin::User);
        CHECK(listener.dirtyMarks == 1);
        input->removeListener(&listener);

        auto output = rig.makeOutput();
        output->addListener(&listener);
        REQUIRE(output->selectGroup(0)); // 离组也是一次选组
        CHECK(output->stateGlue()->isSettled());
        CHECK(output->stateGlue()->configs().groupOrigin == state::GroupOrigin::User);
        CHECK(listener.dirtyMarks == 2);
        output->removeListener(&listener);
    }

    SECTION("out-of-range groups change nothing")
    {
        auto input = rig.makeInput();
        auto output = rig.makeOutput();
        CHECK_FALSE(input->selectGroup(9));
        CHECK_FALSE(input->selectGroup(-1));
        CHECK_FALSE(output->selectGroup(9));
        CHECK_FALSE(input->stateGlue()->isSettled());
        CHECK_FALSE(output->stateGlue()->isSettled());
        CHECK(input->instanceHost().group() == 0);
    }

    SECTION("a user parameter gesture settles by itself")
    {
        auto input = rig.makeInput();
        input->userParamGestureBegan();
        CHECK(input->stateGlue()->isSettled());
    }
}

TEST_CASE("the program name is fixed and not empty, and there is one program", "[host][wiring][state]")
{
    // 两种构建都跑：标脏带 programChanged，AU 宿主会重读 program 名
    Rig rig;
    auto input = rig.makeInput();
    auto output = rig.makeOutput();
    for (RinneProcessorBase* p :
         {static_cast<RinneProcessorBase*>(input.get()), static_cast<RinneProcessorBase*>(output.get())})
    {
        CHECK(p->getNumPrograms() == 1);
        CHECK(p->getCurrentProgram() == 0);
        CHECK(p->getProgramName(0).isNotEmpty());
        CHECK(p->getProgramName(0) == p->getName());
        p->changeProgramName(0, "renamed");
        CHECK(p->getProgramName(0) == p->getName());
    }
}

TEST_CASE("before settling, each loaded project sets the tail mode the host is told", "[host][wiring][state]")
{
    formalOnly();
    Rig rig;
    auto input = rig.makeInput();
    loadState(*input, stateBytes(state::Role::Input,
                                 [](state::StateDocument& d) { d.configs.tailMode = state::TailMode::Infinite; }));
    CHECK(input->getTailMode() == RinneProcessorBase::TailMode::Infinite);
    CHECK(input->getTailLengthSeconds() == kInfinity);
    loadState(*input, stateBytes(state::Role::Input,
                                 [](state::StateDocument& d) { d.configs.tailMode = state::TailMode::Auto; }));
    CHECK(input->getTailMode() == RinneProcessorBase::TailMode::Auto);
}

TEST_CASE("the group and tail handed on are the ones this setState applied, even if settings change right after",
          "[host][wiring][state]")
{
    formalOnly();
    Rig rig;
    auto input = rig.makeInput();
    auto output = rig.makeOutput();
    // 认领转正的回调在 setState 放开锁之后、返回之前调用：借它在两步之间改一次设置，模拟别的线程恰好在这时改了设置
    int inputCalls = 0;
    input->stateGlue()->setConfirmClaimHook([&] {
        if (inputCalls++ == 0)
            input->stateGlue()->editConfigs([](state::Configs& c) {
                c.group = 7;
                c.tailMode = state::TailMode::Auto;
            });
    });
    int outputCalls = 0;
    output->stateGlue()->setConfirmClaimHook([&] {
        if (outputCalls++ == 0)
            output->stateGlue()->editConfigs([](state::Configs& c) { c.group = 6; });
    });

    // ⛔ 壳在 setState 返回之后再读 configs()（不用胶水交回的这一次的副本）→ 会合拿到 7 与 6，tail 成了自动
    loadState(*input, stateBytes(state::Role::Input, [](state::StateDocument& d) {
        d.configs.group = 4;
        d.configs.tailMode = state::TailMode::Infinite;
    }));
    loadState(*output, stateBytes(state::Role::Output, [](state::StateDocument& d) { d.configs.group = 5; }));
    REQUIRE(inputCalls >= 1);
    REQUIRE(outputCalls >= 1);
    REQUIRE(input->stateGlue()->configs().group == 7); // 两步之间确实改过
    REQUIRE(output->stateGlue()->configs().group == 6);
    CHECK(input->instanceHost().group() == 4);
    CHECK(input->getTailMode() == RinneProcessorBase::TailMode::Infinite);
    CHECK(output->instanceHost().group() == 5);
}
