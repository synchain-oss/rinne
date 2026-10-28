// SPDX-License-Identifier: GPL-3.0-or-later
//
// 正式构建的命令序号：Panic 与恢复是界面动作，各加一个计数，块首交给引擎；基线让重载工程不重放旧命令；
// 恢复的 1.5 s 冷却由安全链判，冷却里的点击被丢掉，外壳不排队。载入工厂预设由预设库加序号，处理器每块填进引擎参数。
// 只用 Input（安全链在 Input），不需要配对。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "WiringTestKit.h"
#include "engine/Meters.h"
#include "engine/SafetyEvents.h"

namespace
{

using namespace rinne::wiring_test;
namespace engine = rinne::engine;

engine::SafetyState safetyState(const rinne::InputProcessor& input)
{
    const std::uint32_t word = input.instanceHost().inputEngine().meters().safety.load(std::memory_order_relaxed);
    return engine::PackedSafety::state(word);
}

// 跑 count 块（10 ms 一块），每 40 ms tick 一次（引擎事件在 tick 里排空、计数）
void run(Rig& rig, int count)
{
    rig.cycles(count);
    rig.tick();
}

} // namespace

TEST_CASE("a panic pressed before the first audio block is executed on the first block", "[host][wiring][commands]")
{
    formalOnly();
    Rig rig;
    rig.source = &sine;
    rig.input = rig.makeInput();

    // ⛔ prepare 时把当时的计数取作基线（InstanceHost::prepare 里调 setCommandBaseline）→ 这次 Panic 被吞掉
    rig.input->panic(); // 宿主还没 prepare、一块音频都没有
    rig.input->prepareToPlay(kRate, kBlock);
    run(rig, 1);
    CHECK(rig.input->instanceHost().counts().panicsExecuted == 1);
    CHECK(safetyState(*rig.input) == engine::SafetyState::Tripped);
    CHECK_FALSE(rig.input->canResumeLoop()); // 冷却里
    CHECK_FALSE(rig.input->stateGlue()->isSettled());

    // prepare 之后、下一块之前按的同样不丢（先恢复，再按一次）
    run(rig, 160);
    REQUIRE(rig.input->canResumeLoop());
    rig.input->resumeLoop();
    run(rig, 4);
    REQUIRE(rig.input->instanceHost().counts().recoversExecuted == 1);
    rig.input->releaseResources();
    rig.input->prepareToPlay(kRate, kBlock);
    rig.input->panic();
    run(rig, 1);
    CHECK(rig.input->instanceHost().counts().panicsExecuted == 2);
}

TEST_CASE("reloading the project on the same instance does not replay old commands", "[host][wiring][commands]")
{
    formalOnly();
    Rig rig;
    rig.source = &sine;
    rig.input = rig.makeInput();
    rig.input->prepareToPlay(kRate, kBlock);
    run(rig, 10);
    rig.input->panic();
    run(rig, 160); // 冷却 1.5 s 过去
    rig.input->resumeLoop();
    run(rig, 20);
    REQUIRE(rig.input->instanceHost().counts().panicsExecuted == 1);
    REQUIRE(rig.input->instanceHost().counts().recoversExecuted == 1);
    REQUIRE(safetyState(*rig.input) != engine::SafetyState::Tripped);

    // 宿主重载工程：停用 → 设 state → 再 prepare（同一个实例）。旧的 Panic 与恢复都不再执行
    // ⛔ prepare 时把引擎的命令基线清成 0（InstanceHost::prepare 里 setCommandBaseline({})）→ 旧 Panic 重放
    rig.input->releaseResources();
    loadState(*rig.input, stateBytes(state::Role::Input, [](state::StateDocument& d) { d.configs.group = 0; }));
    rig.input->prepareToPlay(kRate, kBlock);
    run(rig, 100);
    CHECK(rig.input->instanceHost().counts().panicsExecuted == 1);
    CHECK(rig.input->instanceHost().counts().recoversExecuted == 1);
    CHECK(safetyState(*rig.input) != engine::SafetyState::Tripped);

    // 换采样率重新 prepare 也一样
    rig.input->releaseResources();
    rig.input->prepareToPlay(96000.0, kBlock);
    run(rig, 20);
    CHECK(rig.input->instanceHost().counts().panicsExecuted == 1);
    CHECK(safetyState(*rig.input) != engine::SafetyState::Tripped);
}

TEST_CASE("a recover clicked during the cooldown is dropped, not queued", "[host][wiring][commands]")
{
    formalOnly();
    Rig rig;
    rig.source = &sine;
    rig.input = rig.makeInput();
    rig.input->prepareToPlay(kRate, kBlock);
    run(rig, 10);
    rig.input->panic();
    run(rig, 2);
    REQUIRE(safetyState(*rig.input) == engine::SafetyState::Tripped);

    // ⛔ 现在能不能点恢复不看冷却位（canRecover 只看跳闸）→ 冷却里报可点
    CHECK_FALSE(rig.input->canResumeLoop());
    rig.input->resumeLoop(); // 冷却里点：被安全链丢掉
    run(rig, 200); // 冷却早就过了
    CHECK(rig.input->instanceHost().counts().recoversExecuted == 0);
    CHECK(safetyState(*rig.input) == engine::SafetyState::Tripped);
    CHECK(rig.input->canResumeLoop());

    // 冷却之后再点一次才恢复，而且只恢复一次
    rig.input->resumeLoop();
    run(rig, 20);
    CHECK(rig.input->instanceHost().counts().recoversExecuted == 1);
    CHECK(safetyState(*rig.input) != engine::SafetyState::Tripped);
    CHECK_FALSE(rig.input->canResumeLoop());
    CHECK(rig.input->instanceHost().counts().panicsExecuted == 1);

    // Output 端没有安全链：两个动作什么都不做
    auto output = rig.makeOutput();
    output->instanceHost().panic();
    output->instanceHost().recover();
    CHECK_FALSE(output->instanceHost().canRecover());
}

TEST_CASE("loading a factory preset asks the engine for one preset fade-in, and a reload does not replay it",
          "[host][wiring][commands]")
{
    formalOnly();
    Rig rig;
    rig.source = &sine;
    rig.input = rig.makeInput();
    rig.input->prepareToPlay(kRate, kBlock);
    run(rig, 10);
    constexpr auto preset = static_cast<std::size_t>(engine::FadeReason::Preset);
    const auto before = rig.input->instanceHost().counts();

    auto* store = rig.input->presetStore();
    REQUIRE(store != nullptr);
    REQUIRE(store->factoryPresetsValid());
    const auto loaded = store->load("f-slapback-90", rig.input->parameterLayer()->tree());
    REQUIRE(loaded.ok());
    CHECK(store->presetSeq() == 1);
    CHECK(paramValue(*rig.input, "feedback") == 50.0f);
    CHECK(paramValue(*rig.input, "delay_ms") == 90.0f);

    // ⛔ 处理器不把 presetSeq() 填进引擎参数（恒 0）→ 引擎看不到载入，不淡入
    run(rig, 2);
    CHECK(rig.input->instanceHost().counts().fadeIns[preset] - before.fadeIns[preset] == 1);

    // 同一个实例停用再 prepare：序号没变，不再淡入一次
    rig.input->releaseResources();
    rig.input->prepareToPlay(kRate, kBlock);
    run(rig, 20);
    CHECK(rig.input->instanceHost().counts().fadeIns[preset] - before.fadeIns[preset] == 1);

    // 找不到的预设什么都不改：序号不变，引擎也不淡入
    CHECK_FALSE(store->load("f-no-such-preset", rig.input->parameterLayer()->tree()).ok());
    run(rig, 2);
    CHECK(store->presetSeq() == 1);
    CHECK(rig.input->instanceHost().counts().fadeIns[preset] - before.fadeIns[preset] == 1);
}
