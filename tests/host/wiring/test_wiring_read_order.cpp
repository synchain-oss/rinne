// SPDX-License-Identifier: GPL-3.0-or-later
//
// 每块的读序：Input 处理器先做参数快照，再读序号（胶水快照里的 stateSeq、预设库的 presetSeq）。载入预设与 setState
// 都是先加序号、再写参数，所以「拿到新参数、序号却是旧的」（这一块没有淡入请求）只会在先读序号时出现。
// 这里把真的参数层、胶水与预设库各包一层，交给处理器每块用的同一个 snapshotBlock；第一次读完之后立刻插进一次消息线程
// 的写（载入工厂预设 / 宿主设置 state），也就是两次读之间正好来了一次写——确定性地复现那个交错，不靠线程赛跑。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>

#include "WiringTestKit.h"

namespace
{

using namespace rinne::wiring_test;
namespace engine = rinne::engine;

// 第一次读（不管读的是哪一样）之后跑一次写；之后的读不再插
struct Gap
{
    std::function<void()> write;
    int reads = 0;

    void afterRead()
    {
        if (reads++ == 0)
            write();
    }
};

struct ParamsThenGap
{
    const rinne::input::InputParams& real;
    Gap& gap;

    void snapshot(engine::EngineParams& out) const noexcept
    {
        real.snapshot(out);
        gap.afterRead();
    }
};

struct GlueThenGap
{
    const rinne::input::InputStateGlue& real;
    Gap& gap;

    void snapshot(engine::EngineParams& out) const noexcept
    {
        real.snapshot(out);
        gap.afterRead();
    }
};

struct PresetsThenGap
{
    const rinne::input::PresetStore& real;
    Gap& gap;

    std::uint32_t presetSeq() const noexcept
    {
        const std::uint32_t seq = real.presetSeq();
        gap.afterRead();
        return seq;
    }
};

engine::EngineParams readBlock(rinne::InputProcessor& input, Gap& gap)
{
    engine::EngineParams out;
    rinne::InputProcessor::snapshotBlock(ParamsThenGap{*input.parameterLayer(), gap},
                                         GlueThenGap{*input.stateGlue(), gap},
                                         PresetsThenGap{*input.presetStore(), gap}, out);
    return out;
}

} // namespace

TEST_CASE("each block reads the parameters before the preset sequence, so a load in between is never missed",
          "[host][wiring][order]")
{
    formalOnly();
    Rig rig;
    rig.input = rig.makeInput();
    auto& input = *rig.input;
    REQUIRE(input.presetStore()->presetSeq() == 0);
    REQUIRE(paramValue(input, "feedback") == 0.0f);

    bool loaded = false;
    Gap gap{[&] { loaded = input.presetStore()->load("f-slapback-90", input.parameterLayer()->tree()).ok(); }};
    // ⛔ snapshotBlock 先读 presetSeq、再做参数快照 → 这一块拿到 50 % 的新反馈，序号却是旧的 0
    const engine::EngineParams block = readBlock(input, gap);
    REQUIRE(loaded);
    REQUIRE(gap.reads == 3);
    // 写落在两次读之间：参数还是旧的，序号已经是新的（引擎先降到 0 再淡入，新参数下一块落在淡入里）
    CHECK(block.feedbackPercent == 0.0f);
    CHECK(block.presetSeq == 1);

    Gap none{[] {}};
    const engine::EngineParams next = readBlock(input, none);
    CHECK(next.feedbackPercent == 50.0f);
    CHECK(next.presetSeq == 1);
}

TEST_CASE("each block reads the parameters before the state sequence, so a host setState in between is never missed",
          "[host][wiring][order]")
{
    formalOnly();
    Rig rig;
    rig.input = rig.makeInput();
    auto& input = *rig.input;
    // 定型之后宿主的预设或 A/B 只改参数、stateSeq 加一（未定型的全量应用是工程恢复，不加序号）
    input.userParamGestureBegan();
    const std::uint32_t seq0 = input.stateGlue()->stateSeq();
    REQUIRE(paramValue(input, "feedback") == 0.0f);
    const auto bytes = stateBytes(state::Role::Input, [](state::StateDocument& d) { d.params.set("feedback", 30.0f); });

    Gap gap{[&] { loadState(input, bytes); }};
    // ⛔ snapshotBlock 先做胶水快照（stateSeq）、再做参数快照 → 这一块拿到 30 % 的新反馈，stateSeq 却是旧的
    const engine::EngineParams block = readBlock(input, gap);
    REQUIRE(input.stateGlue()->stateSeq() == seq0 + 1);
    REQUIRE(gap.reads == 3);
    CHECK(block.feedbackPercent == 0.0f);
    CHECK(block.stateSeq == seq0 + 1);

    Gap none{[] {}};
    const engine::EngineParams next = readBlock(input, none);
    CHECK(next.feedbackPercent == 30.0f);
    CHECK(next.stateSeq == seq0 + 1);
}
