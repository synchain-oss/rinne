// SPDX-License-Identifier: GPL-3.0-or-later
//
// 节拍同步：时值表、D 的换算与取整、速度回退、截断与抗抖。
//
//   - 时值表（16 档）逐档与期望的分数一致，且严格升序：档位含义冻结，只能追加。
//   - D = 拍数 × 60 / BPM × sr，四舍五入到样本（不是向下取整）。
//   - 速度有效范围 [20, 999]；无效时沿用本会话最后一个有效值，从没有过才回退 120 并置「没有速度」提示。
//   - 要的 D 超过 4000 ms 时截到上限并置「被截断」提示。
//   - 宿主速度的浮点噪声不会让目标在相邻两个整数样本之间来回翻（那会变成一连串改 D）。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "TimingKit.h"
#include "loop/BeatSync.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;
namespace engine = rinne::engine;

// 一个只读环路状态的会话（不接环：D 与提示位照样发布）
struct Bare
{
    LoopSession session;
    std::vector<float> left = std::vector<float>(4096, 0.0f);
    std::vector<float> right = std::vector<float>(4096, 0.0f);
    std::vector<LoggedEvent> events;
    explicit Bare(double sr = 48000.0)
    {
        session.prepare(sr);
        session.setEventSink(LoopEventSink(
            [](void* context, std::uint16_t code, std::uint8_t a, std::uint8_t b, std::int64_t sample) noexcept {
                static_cast<Bare*>(context)->events.push_back(LoggedEvent{code, a, b, sample});
            },
            this));
    }
    LoopStatusFields feed(const EngineParams& params, float bpm, int n = 64)
    {
        TransportInfo transport;
        transport.bpm = bpm;
        BlockContext ctx(params, transport);
        ctx.declaredBlock = 1024;
        ctx.observedBlock = 1024;
        ctx.blockLength = n;
        ctx.returnLeft = left.data();
        ctx.returnRight = right.data();
        session.process(ctx);
        return session.statusFields();
    }
    std::size_t count(LoopEventCode code) const
    {
        std::size_t n = 0;
        for (const LoggedEvent& e : events)
        {
            n += e.code == static_cast<std::uint16_t>(code) ? 1u : 0u;
        }
        return n;
    }
    std::size_t hintEvents() const { return count(LoopEventCode::kDelayHints); }
};

EngineParams syncParams(int note)
{
    EngineParams params;
    params.delayMode = DelayMode::kSync;
    params.delayNote = static_cast<std::uint8_t>(note);
    return params;
}

} // namespace

TEST_CASE("节拍同步 时值表 16 档逐档与期望的分数一致，严格升序，默认档是 1/4", "[timing][beat]")
{
    // ⛔ 删除式验证点：表里改一个拍数、调换两档的次序或少一档 → 本用例变红（档位含义冻结，只能追加）。
    struct Row
    {
        int num;
        int den;
    };
    const Row golden[engine::kDelayNoteCount] = {
        {1, 16}, {1, 12}, {1, 8}, {1, 6}, {1, 4}, {1, 3}, {3, 8}, {1, 2},
        {2, 3},  {3, 4},  {1, 1}, {4, 3}, {3, 2}, {2, 1}, {3, 1}, {4, 1},
    };
    static_assert(engine::kDelayNoteCount == 16, "时值表是 16 档");
    for (int i = 0; i < engine::kDelayNoteCount; ++i)
    {
        INFO("档位 " << i);
        CHECK(engine::kDelayNoteBeats[i] == static_cast<double>(golden[i].num) / static_cast<double>(golden[i].den));
        if (i > 0)
        {
            CHECK(engine::kDelayNoteBeats[i] > engine::kDelayNoteBeats[i - 1]);
        }
    }
    CHECK(engine::kDefaultDelayNote == 10);
    CHECK(engine::kDelayNoteBeats[engine::kDefaultDelayNote] == 1.0); // 1/4 = 1 拍
}

TEST_CASE("节拍同步 D = 拍数 × 60 / BPM × sr：48 kHz、120 BPM、1/4 = 24000；其余档按表", "[timing][beat]")
{
    // ⛔ 删除式验证点：公式里的 60 或 sr 写错、拍数取错档 → 本用例变红。
    CHECK(syncDelayExact(10, 120.0, 48000.0) == 24000.0);
    CHECK(syncDelayExact(7, 120.0, 48000.0) == 12000.0); // 1/8 = 半拍
    CHECK(syncDelayExact(0, 120.0, 48000.0) == 1500.0); // 1/64 = 1/16 拍
    CHECK(syncDelayExact(15, 120.0, 48000.0) == 96000.0); // 1/1 = 4 拍
    CHECK(syncDelayExact(10, 60.0, 44100.0) == 44100.0);
    CHECK(syncDelayExact(10, 150.0, 48000.0) == 19200.0);
    // 越界的档位取默认档（壳保证不会发生，这里只是防守）
    CHECK(noteIndexOrDefault(-1) == engine::kDefaultDelayNote);
    CHECK(noteIndexOrDefault(16) == engine::kDefaultDelayNote);
    CHECK(noteIndexOrDefault(0) == 0);
    CHECK(noteIndexOrDefault(15) == 15);

    Bare bare;
    CHECK(bare.feed(syncParams(10), 120.0f).delayTarget == 24000);
    CHECK(bare.feed(syncParams(10), 120.0f).delayEffective == 24000);
    // 越界的档位（uint8 里的 200）取默认档，不再取模
    Bare other;
    CHECK(other.feed(syncParams(200), 120.0f).delayTarget == 24000);
}

TEST_CASE("节拍同步 换算到样本是四舍五入，不是向下取整", "[timing][beat]")
{
    // ⛔ 删除式验证点：llround 改成向下取整 → 6631.58 取成 6631，本用例变红。
    // 44.1 kHz、133 BPM、1/8T（1/3 拍）：44100 × 20 / 133 = 6631.578…
    const double exact = syncDelayExact(5, 133.0, 44100.0);
    REQUIRE(exact > 6631.5);
    REQUIRE(exact < 6632.0);
    SyncTargetFilter filter;
    CHECK(filter.settle(exact, 5) == 6632);
    Bare bare(44100.0);
    CHECK(bare.feed(syncParams(5), 133.0f).delayTarget == 6632);
}

TEST_CASE("节拍同步 速度有效范围 [20, 999]：边界与非有限值", "[timing][beat]")
{
    // ⛔ 删除式验证点：范围写成开区间或放宽 → 边界两侧的判定反过来，本用例变红。
    CHECK_FALSE(isValidBpm(19.99));
    CHECK(isValidBpm(20.0));
    CHECK(isValidBpm(120.0));
    CHECK(isValidBpm(999.0));
    CHECK_FALSE(isValidBpm(999.01));
    CHECK_FALSE(isValidBpm(0.0));
    CHECK_FALSE(isValidBpm(-120.0));
    CHECK_FALSE(isValidBpm(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(isValidBpm(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(isValidBpm(-std::numeric_limits<double>::infinity()));
}

TEST_CASE("节拍同步 速度跟踪：无效时沿用最后一个有效值，从没有过才回退 120 并置没有速度", "[timing][beat]")
{
    // ⛔ 删除式验证点：无效速度一律回退 120（不沿用最后一个有效值）→ 第三步 D 跳回 120 BPM 的值，本用例变红；
    //    没有速度的提示在拿到有效速度之后不撤 → 本用例变红。
    TempoTracker tempo;
    CHECK(tempo.fallback());
    CHECK(tempo.observe(0.0f) == 120.0);
    CHECK(tempo.fallback());
    CHECK(tempo.observe(19.99f) == 120.0);
    CHECK(tempo.observe(std::numeric_limits<float>::quiet_NaN()) == 120.0);
    CHECK(tempo.fallback());
    CHECK(tempo.observe(150.0f) == 150.0);
    CHECK_FALSE(tempo.fallback());
    CHECK(tempo.observe(0.0f) == 150.0);
    CHECK(tempo.observe(1000.0f) == 150.0);
    CHECK(tempo.observe(std::numeric_limits<float>::infinity()) == 150.0);
    CHECK_FALSE(tempo.fallback());
    CHECK(tempo.observe(20.0f) == 20.0);
    CHECK(tempo.observe(999.0f) == 999.0);

    // 会话里：第一块没有速度 -> 120 BPM + 提示；拿到 150 -> 提示撤；之后又没有 -> 沿用 150，提示不再出现。
    Bare bare;
    LoopStatusFields fields = bare.feed(syncParams(10), 0.0f);
    CHECK(fields.delayTarget == 24000);
    CHECK((fields.delayHints & kDelayHintNoTempo) != 0u);
    CHECK(fields.bpmSource == static_cast<std::uint8_t>(BpmSource::kFallback));
    fields = bare.feed(syncParams(10), 150.0f);
    CHECK(fields.delayTarget == 19200);
    CHECK((fields.delayHints & kDelayHintNoTempo) == 0u);
    CHECK(fields.bpmSource == static_cast<std::uint8_t>(BpmSource::kHost));
    fields = bare.feed(syncParams(10), 0.0f);
    CHECK(fields.delayTarget == 19200);
    CHECK((fields.delayHints & kDelayHintNoTempo) == 0u);
    fields = bare.feed(syncParams(10), 5.0f);
    CHECK(fields.delayTarget == 19200);
    // 提示位只在变化时发事件：置一次、撤一次
    CHECK(bare.hintEvents() == 2u);
}

TEST_CASE("节拍同步 没有速度的提示只属于节拍模式：时间模式下宿主不给速度不置提示", "[timing][beat]")
{
    // ⛔ 删除式验证点：提示位不看模式 → 时间模式下也置没有速度，本用例变红。
    Bare bare;
    EngineParams params;
    params.delayMs = 100.0f;
    const LoopStatusFields fields = bare.feed(params, 0.0f);
    CHECK(fields.delayHints == 0u);
    CHECK(fields.delayTarget == 4800);
    CHECK(bare.hintEvents() == 0u);
}

TEST_CASE("节拍同步 超过 4000 ms 截到上限并置被截断提示；回到范围内撤掉", "[timing][beat]")
{
    // ⛔ 删除式验证点：不截断（D 超出本地延迟线的容量）→ 目标大于 192000，本用例变红；
    //    截断了不置提示 -> 本用例变红。
    Bare bare;
    // 20 BPM、1/1（4 拍）= 12 s -> 截到 4 s = 192000
    LoopStatusFields fields = bare.feed(syncParams(15), 20.0f);
    CHECK(fields.delayTarget == 192000);
    CHECK((fields.delayHints & kDelayHintClipped) != 0u);
    CHECK((fields.delayHints & kDelayHintNoTempo) == 0u);
    // 正好 4 s（60 BPM 的 4 拍）不算截断
    fields = bare.feed(syncParams(15), 60.0f);
    CHECK(fields.delayTarget == 192000);
    CHECK((fields.delayHints & kDelayHintClipped) == 0u);
    // 再来一次超限：置；回到范围内：撤。每次变化一条事件：置、撤、置、撤。
    fields = bare.feed(syncParams(15), 20.0f);
    CHECK((fields.delayHints & kDelayHintClipped) != 0u);
    fields = bare.feed(syncParams(10), 120.0f);
    CHECK(fields.delayTarget == 24000);
    CHECK(fields.delayHints == 0u);
    CHECK(bare.hintEvents() == 4u);

    // 时间模式：4000 ms 恰在上限，不算截断
    EngineParams time;
    time.delayMs = 4000.0f;
    Bare t;
    fields = t.feed(time, 120.0f);
    CHECK(fields.delayTarget == 192000);
    CHECK(fields.delayHints == 0u);
}

TEST_CASE("节拍同步 D_min 抬升事件只在 D 被抬到下限时发；被上限截断走提示位，不发它", "[timing][beat]")
{
    // ⛔ 删除式验证点：D_min 抬升事件的条件放宽成「被改过」（含被上限截断）→ 截断那一条也发 D_min 抬升，本用例变红；
    //    条件收成永不发 -> 抬升那一条没有事件，本用例变红。
    // 被上限截断：20 BPM、4 拍 = 12 s -> 4 s。要的比实际大，但不是下限的事。
    Bare clipped;
    LoopStatusFields fields = clipped.feed(syncParams(15), 20.0f);
    CHECK(fields.delayTarget == 192000);
    CHECK((fields.delayHints & kDelayHintClipped) != 0u);
    CHECK(clipped.count(LoopEventCode::kDMinRaised) == 0u);

    // 被下限抬起：5 ms 远小于声明块长 1024 -> 抬到下限；同一个目标不重复发，再往下改也还是同一个目标
    Bare raised;
    EngineParams tiny;
    tiny.delayMs = 5.0f;
    fields = raised.feed(tiny, 120.0f);
    CHECK(fields.delayTarget == 1024);
    CHECK(fields.delayTarget == fields.delayMin);
    CHECK(raised.count(LoopEventCode::kDMinRaised) == 1u);
    CHECK(raised.count(LoopEventCode::kDelayHints) == 0u);
    raised.feed(tiny, 120.0f);
    tiny.delayMs = 3.0f;
    raised.feed(tiny, 120.0f);
    CHECK(raised.count(LoopEventCode::kDMinRaised) == 1u);
    // 离开下限、再被抬起：又是一次
    tiny.delayMs = 100.0f;
    CHECK(raised.feed(tiny, 120.0f).delayTarget == 4800);
    tiny.delayMs = 5.0f;
    raised.feed(tiny, 120.0f);
    CHECK(raised.count(LoopEventCode::kDMinRaised) == 2u);
}

TEST_CASE("节拍同步 宿主速度的浮点噪声不让目标在相邻两个样本间来回翻：0 次改 D", "[timing][beat]")
{
    // ⛔ 删除式验证点：去掉抗抖（每块都拿取整后的精确值当目标）→ 精确 D 恰在两个整数之间时目标逐块翻转，
    //    第二块起就是一次改 D，本用例变红。
    // 44.1 kHz、1 拍：D = 2646000 / BPM。取 BPM 使精确 D 落在 22000.5 上：float 里相邻的两个速度值（差 1 个 ulp，
    // 约 0.0014 个样本）恰好把 22000.5 夹在中间，一个取整成 22000、一个取整成 22001。
    const auto centre = static_cast<float>(2646000.0 / 22000.5);
    const float neighbours[3] = {std::nextafter(centre, 0.0f), centre, std::nextafter(centre, 1000.0f)};
    float up = centre;
    float down = centre;
    for (const float a : neighbours)
    {
        for (const float b : neighbours)
        {
            if (std::llround(syncDelayExact(10, static_cast<double>(a), 44100.0)) !=
                std::llround(syncDelayExact(10, static_cast<double>(b), 44100.0)))
            {
                up = a;
                down = b;
            }
        }
    }
    REQUIRE(std::llround(syncDelayExact(10, static_cast<double>(up), 44100.0)) !=
            std::llround(syncDelayExact(10, static_cast<double>(down), 44100.0)));

    Rig rig(44100.0, 1024);
    const EngineParams params = syncParams(10);
    // 先稳定一会儿再抖：暖机 D 之后，逐块在两个值之间交替
    TransportInfo transport;
    transport.bpm = up;
    rig.run(22000 + 4096, {256}, params, transport);
    for (int block = 0; block < 400; ++block)
    {
        transport.bpm = (block % 2 == 0) ? down : up;
        rig.step(256, params, transport);
    }
    CHECK(rig.xfadeStarts().empty());
    CHECK(rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kDChange)] == 0u);
    CHECK(rig.status().tapDrop == 0u);
}

TEST_CASE("节拍同步 抗抖只吃不到 1 个样本的差：差到 1 个样本以上就换目标", "[timing][beat]")
{
    // ⛔ 删除式验证点：抗抖门限写大（例如 3 个样本）→ 差 1.5 个样本的变化被吃掉，本用例变红。
    SyncTargetFilter filter;
    CHECK(filter.settle(1000.0, 4) == 1000);
    CHECK(filter.settle(1000.4, 4) == 1000); // 不到 1
    CHECK(filter.settle(999.6, 4) == 1000);
    CHECK(filter.settle(1000.99, 4) == 1000);
    CHECK(filter.settle(1001.0, 4) == 1001); // 差恰好 1：换
    CHECK(filter.settle(1001.4, 4) == 1001);
    CHECK(filter.settle(999.0, 4) == 999); // 向下差 2：换
    CHECK(filter.settle(1000.5, 4) == 1001); // 差 1.5：换，取整 llround
    // 换档位一定重新取整（那是用户的动作），哪怕恰好差不到 1：不换档时 1001.6 会被吃掉（目标留在 1001）
    CHECK(filter.settle(1001.6, 5) == 1002);
    filter.reset();
    CHECK(filter.settle(500.4, 4) == 500); // 复位之后从头取整
}
