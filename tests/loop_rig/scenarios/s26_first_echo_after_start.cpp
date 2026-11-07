// SPDX-License-Identifier: GPL-3.0-or-later
//
// S26 起播之后的第一个回声：宿主在静音的开头不送块（或送块但报不在走带），第一个在走的块恰好就是声音开始的那一块。
// 起播沿因此落在声音所在的块首；沿上的复位不能把这块里的声音的回声压低或吞掉。
//
// 在 LUNA 3.1 的 AU 上观察到：导出开始之后第一个冲激的第一个回声被压低 13–21 dB，之后每圈照常 ×0.5；
// 同一个冲激离块首越近压得越多。导出的数据反推出来：被压的倍数恰为 (冲激在块内的偏移 − 239) / 960，
// 也就是起播沿新读头的 20 ms onset（从写头 + 5 ms 起按位置走）在这个冲激上走到的那一步。
// 宿主在静音的开头不调插件（或者调了但报不在走带），第一个在走的块就是有声音的那一块，
// 起播沿落在它的块首，5 ms 的排除窗口与 20 ms 的 onset 正好压在声音的开头。
//
// 做法：环路延迟 D = 4800（100 ms），中间链是恒等，混合 50 %、反馈 50 %（每圈 ×0.5），
// 块长 512（另有 2048 一路）。每一路先在走带的状态下跑够淡入（约 1.07 s，全是静音），
// 再报停 640 ms（超过去抖阈值，确认停走带），然后：
//   开头不调  宿主在静音的开头一块都不调，下一次调用就是有声音的那一块（报在走带），这一块就是起播沿；
//   开头报停  宿主在静音的开头照常调用，但报不在走带，到有声音的那一块才报在走带
//             （140 块，与 LUNA 的导出同一个块号）；
//   实时挂起  开头不调，而且壳量出两端一起挂起超过 250 ms，在这一块之前请求清空本地延迟线
//             （实时导出走这一条）；
//   对照      起播沿在导出的第一块，静音的开头照常走带（Cubase 的调用方式），冲激在 140 块之后。
// 冲激 0.9 落在起播之后那一块的块内偏移 320（LUNA 导出的 echo-in）、448（LUNA 导出的 test-signal），
// 以及 100（落在 5 ms 的排除窗口里），块长 2048 的那一路偏移 320；第二个冲激在第一个之后 50000 个样本。
//
// 另有一路「DC 尾巴」：走带时有回声在环里转，之后把反馈调到 0（环里随后是数字静音，回送链的 DC 阻断里还留着衰减尾巴），
// 停走带，开头不调，沿上把反馈调回 50 %，冲激在沿后 320。
//
// 断言：
//   - 第一个回声（冲激 + D）恰为 0.9 × 0.5，左右声道都是；第二个冲激的回声约为 0.45；
//   - 各路与对照按冲激对齐，从起播那一块的块首起 2 s 逐位相同（宿主怎么调用、实时还是离线、块长多少都一样）；
//   - 每路输出与参照安全链按文档接线重算的结果逐位相同；起播重同步恰 2 次（开场一次、导出一次）；
//   - DC 尾巴那一路：复位点就是沿所在块的块首 s0，[s0, s0 + 5 ms) 与一圈之后的 [s0 + D, s0 + D + 5 ms) 输出恰为 0，
//     冲激的第一个回声恰为 0.45。
//
// 这些都是安静的起播沿（沿上没有旧回送要淡出、写头之前 20 ms 是数字静音，口径见 LoopSession::quietPlayEdge）。
// 沿前有回声在响的起播沿照旧（5 ms 淡出、W + 5 ms 起读、20 ms onset），由 S03、S05b 钉住。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 安静判据恒为假（起播沿一律照旧）→ 第一个回声回到 −21.48 dB（偏移 320）、−13.24 dB（448）与 0（100）；
//   ⛔ 安静的沿仍在 s0 + 5 ms 复位 DSP → DC 尾巴那一路 [s0, s0 + 5 ms) 不为 0；
//   ⛔ 安静的沿上新读头仍带 20 ms onset → 第一个回声被压低。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::SyncReason;

constexpr std::int64_t kDelay = 4800;
constexpr std::size_t kWarmCycles = 100; // 走带、静音：配对与 500 ms 淡入走完
constexpr std::size_t kStopCycles = 60; // 报停 640 ms：确认停走带
constexpr std::size_t kLeadCycles = 140; // 导出开头的静音（LUNA 的 echo-in 导出里，冲激所在的块号）
constexpr std::int64_t kSecondAfter = 50000; // 第二个冲激在第一个之后
constexpr std::int64_t kCompare = 2 * 48000; // 比较窗口
constexpr float kImpulse = 0.9f;

enum class Calls
{
    kSkipLead, // 开头一块都不调
    kStoppedLead, // 开头照常调，报不在走带
    kSkipLeadSuspended, // 开头不调，壳请求清空（实时挂起）
    kControl, // 起播沿在导出的第一块，开头照常走带
};

struct Spec
{
    const char* name;
    Calls calls;
    int block;
    std::int64_t offset; // 冲激在起播之后那一块里的偏移（对照：在第 kLeadCycles 块里的偏移）
    bool offline;
};

struct Run
{
    std::vector<float> left;
    std::vector<float> right;
    std::int64_t impulseAt = 0; // 第一个冲激（插件看到的绝对样本号）
    std::int64_t edgeBlockAt = 0; // 冲激所在那一块的块首
    std::uint32_t playResyncs = 0;
    std::vector<std::int64_t> resetPoints;
    bool matchesReference = false;
};

Run runOne(const Spec& spec)
{
    Run run;
    const std::size_t lead = spec.calls == Calls::kStoppedLead || spec.calls == Calls::kControl ? kLeadCycles : 0;
    const std::size_t before = kWarmCycles + kStopCycles + lead;
    const std::int64_t block = spec.block;
    run.edgeBlockAt = static_cast<std::int64_t>(before) * block;
    run.impulseAt = run.edgeBlockAt + spec.offset;
    const std::int64_t second = run.impulseAt + kSecondAfter;
    const std::size_t total = before + static_cast<std::size_t>((kSecondAfter + kCompare + 2 * kDelay) / block + 2);

    auto host = makeHost(kSr, spec.block);
    REQUIRE(host != nullptr);
    host->setBlockScripts(fixedBlocks(spec.block), fixedBlocks(spec.block));
    TransportScript transport;
    transport.playing.assign(total, true);
    for (std::size_t i = kWarmCycles; i < kWarmCycles + kStopCycles; ++i)
    {
        transport.playing[i] = false;
    }
    if (spec.calls == Calls::kStoppedLead)
    {
        for (std::size_t i = kWarmCycles + kStopCycles; i < before; ++i)
        {
            transport.playing[i] = false;
        }
    }
    transport.nonrealtime = {spec.offline};
    host->setTransportScript(transport);

    const auto first = static_cast<std::uint64_t>(run.impulseAt);
    const auto again = static_cast<std::uint64_t>(second);
    Recorder rec(*host, [first, again](std::uint64_t start, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t at = start + static_cast<std::uint64_t>(i);
            const float v = at == first || at == again ? kImpulse : 0.0f;
            left[i] = v;
            right[i] = v;
        }
    });
    const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);
    for (std::size_t i = 0; i < total; ++i)
    {
        if (spec.calls == Calls::kSkipLeadSuspended && i == before)
        {
            host->input().requestFlushAfterSuspend();
        }
        rec.cycle(i, p);
    }
    run.left = rec.outLeft;
    run.right = rec.outRight;
    run.playResyncs = host->inputStatus().resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    run.resetPoints = rec.resetPoints;
    run.matchesReference = rec.matchesReference();
    return run;
}

// 安静的沿上 DSP 在 s0 复位：沿前回送链里还留着 DC 阻断的衰减尾巴（反馈调到 0 之后，环里已经是数字静音，
// 但 DC 阻断的状态还没衰减到冲成 0 的下限），沿上把反馈调回 50 %。安静的沿把 DSP 在 s0 复位，[s0, s0 + T) 的回送恰为
// 0， 写进沿后新流 [W, W + T) 的只有干声（静音），一圈之后读回来也是 0；沿后的冲激照常满幅回来。 ⛔ 安静的沿仍在 s0 + T
// 复位 → 尾巴乘上正在爬升的反馈增益漏进 [s0, s0 + T)，本段变红。
struct DcTailRun
{
    std::vector<float> left;
    std::vector<float> right;
    std::int64_t edgeAt = 0;
    std::int64_t impulseAt = 0;
    std::uint32_t playResyncs = 0;
    std::vector<std::int64_t> resetPoints;
    bool matchesReference = false;
};

DcTailRun runDcTail()
{
    constexpr int kBlock = 512;
    constexpr std::size_t kFeedbackOff = 60; // 第 60 块起反馈 0：回声不再生，环里随后是数字静音
    const std::size_t edge = kWarmCycles + kStopCycles; // 开头不调：停走带之后的下一块就是起播沿
    DcTailRun run;
    run.edgeAt = static_cast<std::int64_t>(edge) * kBlock;
    run.impulseAt = run.edgeAt + 320;
    const std::size_t total = edge + static_cast<std::size_t>((3 * kDelay) / kBlock + 2);

    auto host = makeHost(kSr, kBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
    TransportScript transport;
    transport.playing.assign(total, true);
    for (std::size_t i = kWarmCycles; i < edge; ++i)
    {
        transport.playing[i] = false;
    }
    transport.nonrealtime = {true};
    host->setTransportScript(transport);

    const auto early = static_cast<std::uint64_t>(10000); // 走带时的冲激：回声在环里转几圈
    const auto late = static_cast<std::uint64_t>(run.impulseAt);
    Recorder rec(*host, [early, late](std::uint64_t start, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t at = start + static_cast<std::uint64_t>(i);
            const float v = at == early || at == late ? kImpulse : 0.0f;
            left[i] = v;
            right[i] = v;
        }
    });
    const rinne::engine::EngineParams on = loopParams(50.0f, 50.0f, 100.0f);
    const rinne::engine::EngineParams off = loopParams(0.0f, 50.0f, 100.0f);
    for (std::size_t i = 0; i < total; ++i)
    {
        rec.cycle(i, i >= kFeedbackOff && i < edge ? off : on);
    }
    run.left = rec.outLeft;
    run.right = rec.outRight;
    run.playResyncs = host->inputStatus().resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    run.resetPoints = rec.resetPoints;
    run.matchesReference = rec.matchesReference();
    return run;
}

class FirstEchoScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s26_first_echo_after_start"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        const std::vector<Spec> specs = {
            {"control", Calls::kControl, 512, 320, false},
            {"skip-lead-320", Calls::kSkipLead, 512, 320, true},
            {"stopped-lead-320", Calls::kStoppedLead, 512, 320, true},
            {"suspended-320", Calls::kSkipLeadSuspended, 512, 320, false},
            {"skip-lead-448", Calls::kSkipLead, 512, 448, true},
            {"skip-lead-100", Calls::kSkipLead, 512, 100, false},
            {"skip-lead-2048", Calls::kSkipLead, 2048, 320, true},
        };
        const float want = kImpulse * 0.5f;
        std::vector<Run> runs;
        for (const Spec& spec : specs)
        {
            runs.push_back(runOne(spec));
        }
        const Run& control = runs[0];
        for (std::size_t r = 0; r < runs.size(); ++r)
        {
            const Spec& spec = specs[r];
            const Run& run = runs[r];
            INFO("run " << spec.name);
            const auto echo = static_cast<std::size_t>(run.impulseAt + kDelay);
            const auto echo2 = static_cast<std::size_t>(run.impulseAt + kSecondAfter + kDelay);
            REQUIRE(run.left.size() > echo2);
            INFO("first echo L = " << run.left[echo] << ", R = " << run.right[echo] << " (want " << want << ", "
                                   << 20.0 * std::log10(std::fabs(run.left[echo]) / want) << " dB)");
            RIG_EQ(v, run.left[echo], want);
            RIG_EQ(v, run.right[echo], want);
            RIG_CHECK(v, std::fabs(run.left[echo2] - want) < 0.005f);
            RIG_EQ(v, run.playResyncs, 2u); // 开场第一块一次，导出起播一次
            RIG_CHECK(v, run.matchesReference);
            if (r > 0)
            {
                // 按冲激对齐：从起播那一块的块首起逐位相同
                const auto from = static_cast<std::size_t>(run.edgeBlockAt);
                const auto shift = static_cast<std::ptrdiff_t>(control.impulseAt - run.impulseAt);
                std::vector<float> l(run.left.begin() + static_cast<std::ptrdiff_t>(from),
                                     run.left.begin() + static_cast<std::ptrdiff_t>(from + kCompare));
                std::vector<float> c(control.left.begin() + static_cast<std::ptrdiff_t>(from) + shift,
                                     control.left.begin() + static_cast<std::ptrdiff_t>(from + kCompare) + shift);
                RIG_CHECK(v, sameBits(l, c, 0, l.size()));
            }
        }

        // 沿前 DSP 里还有 DC 尾巴：安静的沿在 s0 复位
        const DcTailRun tail = runDcTail();
        INFO("run dc-tail");
        RIG_EQ(v, tail.playResyncs, 2u);
        RIG_CHECK(v, tail.matchesReference);
        RIG_CHECK(v,
                  std::find(tail.resetPoints.begin(), tail.resetPoints.end(), tail.edgeAt) != tail.resetPoints.end());
        bool silent = true; // [s0, s0 + T) 与一圈之后的 [s0 + D, s0 + D + T) 恰为 0（干声在那里也是 0）
        for (std::int64_t k = 0; k < 240; ++k)
        {
            for (const std::int64_t at : {tail.edgeAt + k, tail.edgeAt + kDelay + k})
            {
                const auto i = static_cast<std::size_t>(at);
                silent = silent && tail.left[i] == 0.0f && tail.right[i] == 0.0f;
            }
        }
        RIG_CHECK(v, silent);
        const auto tailEcho = static_cast<std::size_t>(tail.impulseAt + kDelay);
        INFO("dc-tail first echo L = " << tail.left[tailEcho]);
        RIG_EQ(v, tail.left[tailEcho], want);
        RIG_EQ(v, tail.right[tailEcho], want);
        return v.ok();
    }
};

} // namespace

RINNE_RIG_SCENARIO("s26_first_echo_after_start")
{
    return new FirstEchoScenario();
}

TEST_CASE("s26: the first echo after a play start keeps its full level when the host skips the silent lead-in",
          "[loop_rig][s26]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s26_first_echo_after_start"));
}
