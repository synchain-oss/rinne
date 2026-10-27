// SPDX-License-Identifier: GPL-3.0-or-later
//
// S09 穿插 0 样本调用：0 样本的调用不推进任何计数，输出与不穿插时逐位相同。
//
// 做法：同一串真实块（块长 7…1024 轮换，中途改 D 两次，一次落在 100 ms 限速窗口里被合并），跑两遍：
//   参照  只有真实周期；
//   穿插  真实周期之间随机穿插 0 样本的周期，四种形态——两端都以 0 样本被调用、只有 Input 以 0 被调用（Output 跳过）、
//         只有 Output 以 0 被调用（Input 跳过）、两端以 0 被调用但带着违约的走带与参数（时间戳倒退、走带停止、
//         离线渲染、反馈 100 %、D 300 ms）：0 样本调用必须在读走带与参数之前就返回。
// 断言：
//   - 每一个 0 样本周期前后，指纹（环路层全部计数与几何量、两端端点槽的块序号、写头、Output 的四个计数、
//     安全段、事件队列长度）逐项不变；
//   - 两遍的 Input 输出、Output 看到的轨道缓冲逐位相同，事件序列（码、参数、样本位置）逐项相同，最终指纹相同；
//   - 绝对数：读位与写头都恰为真实样本总数，两端的块序号恰为真实周期数，交叉淡化起点个数与限速窗口的预期一致。
// 调用次序取 Input 先与 Output 先各一遍。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ InputEngine 或 OutputEngine 的 0 样本提前返回删掉 → 块序号多算，指纹变；
//   ⛔ 环路层的 0 样本提前返回删掉，或限速窗口按调用次数计 → 事件的样本位置变，输出不再逐位相同。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "diag/EventQueue.h"
#include "loop/LoopEvents.h"
#include "loop/LoopStatus.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::EngineParams;

constexpr int kMaxBlock = 1024;
constexpr std::size_t kRealCycles = 140;
constexpr int kBlockPattern[] = {256, 64, 1024, 33, 512, 7, 128};

enum class Kind
{
    kReal,
    kBoth, // 两端以 0 样本被调用
    kInputOnly, // Input 以 0 样本被调用，Output 这一周期不被调用
    kOutputOnly, // Output 以 0 样本被调用，Input 这一周期不被调用
    kJunk, // 两端以 0 样本被调用，走带与参数都是违约的
};

struct Cycle
{
    Kind kind = Kind::kReal;
    int length = 0;
    EngineParams params;
    std::uint64_t hostNs = 0;
    bool playing = true;
    bool nonrealtime = false;
    float bpm = 0.0f;
};

std::uint64_t mix(std::uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

// 真实周期 r 的 D（毫秒）：r = 40 起第一次改（前沿，立刻淡），r = 44 起第二次改（落在第一次的 100 ms 窗口里，被合并，
// 窗口到期时再淡一次），r = 80 起第三次改（此时第二次的窗口已到期）
float delayMsAt(std::size_t r)
{
    if (r < 40)
    {
        return 100.0f;
    }
    if (r < 44)
    {
        return 150.0f;
    }
    return r < 80 ? 125.0f : 200.0f;
}

std::vector<Cycle> makePlan(bool withZeros)
{
    std::vector<Cycle> plan;
    std::uint64_t position = 0;
    const auto addZeros = [&](std::size_t count, std::uint64_t salt) {
        for (std::size_t j = 0; j < count; ++j)
        {
            Cycle c;
            c.kind = static_cast<Kind>(1 + (mix(salt + j * 7919u) >> 5) % 4);
            c.length = 0;
            if (c.kind == Kind::kJunk)
            {
                c.params = loopParams(100.0f, 100.0f, 300.0f);
                c.hostNs = 5; // 比之前任何一个真实周期的时间戳都早
                c.playing = false;
                c.nonrealtime = true;
                c.bpm = 300.0f;
            }
            else
            {
                c.params = loopParams(50.0f, 50.0f, 100.0f);
                c.hostNs = 1000000u;
            }
            plan.push_back(c);
        }
    };
    for (std::size_t r = 0; r < kRealCycles; ++r)
    {
        if (withZeros)
        {
            addZeros(static_cast<std::size_t>((mix(r + 1) >> 3) % 3), r * 131u + 17u);
        }
        Cycle c;
        c.length = kBlockPattern[r % std::size(kBlockPattern)];
        c.params = loopParams(50.0f, 50.0f, delayMsAt(r));
        c.hostNs = 1000000u + static_cast<std::uint64_t>(static_cast<double>(position) * 1.0e9 / kSr);
        plan.push_back(c);
        position += static_cast<std::uint64_t>(c.length);
    }
    if (withZeros)
    {
        addZeros(4, 99991u);
    }
    return plan;
}

using Fingerprint = std::vector<std::pair<const char*, std::int64_t>>;

// 两端的全部计数：环路层状态、端点槽的块序号、写头、Output 的计数、安全段。事件队列长度由调用方另记
Fingerprint fingerprint(EngineHost& host)
{
    Fingerprint f;
    const auto add = [&f](const char* name, std::int64_t value) { f.emplace_back(name, value); };
    const rinne::loop::LoopStatusFields s = host.input().loopStatus();
    std::uint32_t userBits = 0;
    std::memcpy(&userBits, &s.delayUserMs, sizeof(userBits));
    add("delayUserMs", userBits);
    add("delayEffective", s.delayEffective);
    add("delayMin", s.delayMin);
    add("delayMinReason", s.delayMinReason);
    add("maxBlockSeen", s.maxBlockSeen);
    add("delayMode", s.delayMode);
    add("bpmCenti", s.bpmCenti);
    add("bpmSource", s.bpmSource);
    add("delayTarget", s.delayTarget);
    add("delayHints", s.delayHints);
    add("delayXfadeLeft", s.delayXfadeLeft);
    add("tapsActive", s.tapsActive);
    add("tapDrop", s.tapDrop);
    add("resyncCount", s.resyncCount);
    for (const std::uint32_t v : s.resyncByReason)
    {
        add("resyncByReason", v);
    }
    add("overflow", static_cast<std::int64_t>(s.overflow));
    add("torn", static_cast<std::int64_t>(s.torn));
    add("overrun", static_cast<std::int64_t>(s.overrun));
    add("wCasFail", s.wCasFail);
    add("unstable", s.unstable);
    add("offsetDriftBlocks", s.offsetDriftBlocks);
    add("frozenMax", s.frozenMax);
    add("stalled", s.stalled);
    add("tolerant", s.tolerant);
    add("paired", s.paired);
    add("drift", s.drift);
    add("statusWriteHead", s.writeHead);
    add("offset", s.offset);
    add("orderVerdict", s.orderVerdict);
    add("orderVotes", s.orderVotes);
    add("hostNsValid", s.hostNsValid);
    add("inputBlockSeq", static_cast<std::int64_t>(host.slot(rinne::ipc::Role::kInput)->block_seq.load()));
    add("outputBlockSeq", static_cast<std::int64_t>(host.slot(rinne::ipc::Role::kOutput)->block_seq.load()));
    add("writeHead", static_cast<std::int64_t>(host.writeHead()));
    add("outputWritten", static_cast<std::int64_t>(host.outputWrittenFrames()));
    add("outputState", static_cast<std::int64_t>(host.outputState()));
    add("outputNonFinite", host.outputNonFiniteCount());
    add("outputClamp", host.outputClampCount());
    add("outputConflict", host.outputConflictCount());
    add("outputUnsupported", host.outputUnsupportedCount());
    add("lastBlockStart", host.inputLastBlock().blockStart);
    const rinne::engine::SafetyDiag d = host.inputSafetyDiag();
    add("safetyState", d.state);
    add("safetyTrips", d.trips);
    add("safetyNonfiniteRing", d.nonfiniteRing);
    add("safetyClampRing", d.clampRing);
    return f;
}

// 第一处不同的名字；全同是 nullptr
const char* firstChange(const Fingerprint& a, const Fingerprint& b)
{
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        if (a[i].second != b[i].second)
        {
            return a[i].first;
        }
    }
    return a.size() == b.size() ? nullptr : "(size)";
}

struct Outcome
{
    std::vector<float> outLeft;
    std::vector<float> outRight;
    std::vector<float> fedLeft;
    std::vector<float> fedRight;
    std::vector<rinne::diag::Event> events;
    Fingerprint last;
    std::size_t zeroCycles = 0;
    std::size_t zeroByKind[5]{};
    std::size_t zeroChanged = 0; // 0 样本周期前后指纹不同的次数
    std::string firstChanged;
    std::int64_t realSamples = 0;
    std::size_t realCycles = 0;
    std::vector<std::int64_t> xfadeStarts;
};

Outcome runPlan(const std::vector<Cycle>& plan, CallOrder order)
{
    Outcome outcome;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    host->setCallOrder(order);
    host->setSource([](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.3f * hashNoise(first + static_cast<std::uint64_t>(i), 1);
            right[i] = 0.3f * hashNoise(first + static_cast<std::uint64_t>(i), 2);
        }
    });

    BlockScript input;
    BlockScript output;
    ParamsScript params;
    TransportScript transport;
    for (const Cycle& c : plan)
    {
        input.lengths.push_back(c.length);
        output.lengths.push_back(c.length);
        input.skips.push_back(c.kind == Kind::kOutputOnly);
        output.skips.push_back(c.kind == Kind::kInputOnly);
        params.snapshots.push_back(c.params);
        transport.hostNs.push_back(c.hostNs);
        transport.hostNsValid.push_back(true);
        transport.playing.push_back(c.playing);
        transport.nonrealtime.push_back(c.nonrealtime);
        transport.bpm.push_back(c.bpm);
    }
    host->setBlockScripts(input, output);
    host->setParamsScript(params);
    host->setTransportScript(transport);

    const auto drain = [&] { host->drainEvents([&](const rinne::diag::Event& e) { outcome.events.push_back(e); }); };
    for (std::size_t i = 0; i < plan.size(); ++i)
    {
        const std::size_t eventsBefore = outcome.events.size();
        const Fingerprint before = fingerprint(*host);
        host->runCycle(i);
        drain();
        const Cycle& c = plan[i];
        if (c.kind == Kind::kReal)
        {
            const auto n = static_cast<std::ptrdiff_t>(c.length);
            outcome.outLeft.insert(outcome.outLeft.end(), host->inputOutputLeft().begin(),
                                   host->inputOutputLeft().begin() + n);
            outcome.outRight.insert(outcome.outRight.end(), host->inputOutputRight().begin(),
                                    host->inputOutputRight().begin() + n);
            outcome.fedLeft.insert(outcome.fedLeft.end(), host->outputFedLeft().begin(),
                                   host->outputFedLeft().begin() + n);
            outcome.fedRight.insert(outcome.fedRight.end(), host->outputFedRight().begin(),
                                    host->outputFedRight().begin() + n);
            outcome.realSamples += c.length;
            ++outcome.realCycles;
            continue;
        }
        ++outcome.zeroCycles;
        ++outcome.zeroByKind[static_cast<std::size_t>(c.kind)];
        const char* changed = firstChange(before, fingerprint(*host));
        if (changed == nullptr && outcome.events.size() != eventsBefore)
        {
            changed = "(events)";
        }
        if (changed != nullptr)
        {
            if (outcome.zeroChanged == 0)
            {
                outcome.firstChanged = std::string(changed) + " @cycle " + std::to_string(i);
            }
            ++outcome.zeroChanged;
        }
    }
    outcome.last = fingerprint(*host);
    for (const rinne::diag::Event& e : outcome.events)
    {
        if (e.code == static_cast<std::uint16_t>(rinne::loop::LoopEventCode::kResync) &&
            e.a == static_cast<std::uint8_t>(rinne::loop::ResyncReason::kDChange))
        {
            outcome.xfadeStarts.push_back(e.sample);
        }
    }
    return outcome;
}

std::int64_t valueOf(const Fingerprint& f, const char* name)
{
    for (const auto& item : f)
    {
        if (std::strcmp(item.first, name) == 0)
        {
            return item.second;
        }
    }
    return -1;
}

class ZeroSampleScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s09_zero_sample"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        leg(v, CallOrder::kInputFirst);
        leg(v, CallOrder::kOutputFirst);
        return v.ok();
    }

private:
    void leg(Verdict& v, CallOrder order)
    {
        INFO("order = " << (order == CallOrder::kInputFirst ? "input first" : "output first"));
        const Outcome base = runPlan(makePlan(false), order);
        const Outcome zeros = runPlan(makePlan(true), order);

        // 穿插是真的：每一种形态都出现了足够多次
        RIG_CHECK(v, zeros.zeroCycles >= 120);
        for (std::size_t k = 1; k <= 4; ++k)
        {
            INFO("zero kind " << k);
            RIG_CHECK(v, zeros.zeroByKind[k] >= 25);
        }
        RIG_EQ(v, base.zeroCycles, std::size_t{0});

        // 每个 0 样本周期前后，全部计数与事件队列长度不变
        INFO("first change: " << zeros.firstChanged);
        RIG_EQ(v, zeros.zeroChanged, std::size_t{0});

        // 两遍的输出逐位相同，事件序列逐项相同，最终指纹相同
        RIG_EQ(v, zeros.outLeft.size(), base.outLeft.size());
        RIG_CHECK(v, sameBits(zeros.outLeft, base.outLeft, 0, base.outLeft.size()));
        RIG_CHECK(v, sameBits(zeros.outRight, base.outRight, 0, base.outRight.size()));
        RIG_CHECK(v, sameBits(zeros.fedLeft, base.fedLeft, 0, base.fedLeft.size()));
        RIG_CHECK(v, sameBits(zeros.fedRight, base.fedRight, 0, base.fedRight.size()));
        RIG_EQ(v, zeros.events.size(), base.events.size());
        bool sameEvents = zeros.events.size() == base.events.size();
        for (std::size_t i = 0; sameEvents && i < base.events.size(); ++i)
        {
            sameEvents = zeros.events[i].code == base.events[i].code && zeros.events[i].a == base.events[i].a &&
                         zeros.events[i].b == base.events[i].b && zeros.events[i].sample == base.events[i].sample;
        }
        RIG_CHECK(v, sameEvents);
        const char* finalDiff = firstChange(zeros.last, base.last);
        INFO("final fingerprint differs at " << (finalDiff != nullptr ? finalDiff : "-"));
        RIG_CHECK(v, finalDiff == nullptr);

        // 绝对数：读位、写头都恰为真实样本总数，块序号恰为真实周期数
        const std::int64_t total = zeros.realSamples;
        RIG_EQ(v, total, base.realSamples);
        const auto cycles = static_cast<std::int64_t>(kRealCycles);
        RIG_EQ(v, valueOf(zeros.last, "writeHead"), total);
        RIG_EQ(v, valueOf(zeros.last, "outputWritten"), total);
        RIG_EQ(v, valueOf(zeros.last, "inputBlockSeq"), cycles);
        RIG_EQ(v, valueOf(zeros.last, "outputBlockSeq"), cycles);
        RIG_EQ(v, valueOf(zeros.last, "lastBlockStart") + kBlockPattern[(kRealCycles - 1) % std::size(kBlockPattern)],
               total);

        // 改 D 的流程确实走了一遍：三次交叉淡化（前沿一次、窗口到期补一次、第三次改动一次），终点是 200 ms
        RIG_EQ(v, zeros.xfadeStarts.size(), std::size_t{3});
        RIG_CHECK(v, zeros.xfadeStarts == base.xfadeStarts);
        RIG_EQ(v, valueOf(zeros.last, "delayEffective"), std::int64_t{9600});
        RIG_EQ(v, valueOf(zeros.last, "delayXfadeLeft"), std::int64_t{0});
    }
};

} // namespace

RINNE_RIG_SCENARIO("s09_zero_sample")
{
    return new ZeroSampleScenario();
}

TEST_CASE("s09: zero-sample calls interleaved on both ends change no counter and no output bit", "[loop_rig][s09]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s09_zero_sample"));
}
