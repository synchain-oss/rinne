// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两端采样率不一致：写方在环头里发布的采样率与本端不同，环里的帧按另一个速率解释，读了就是变调的声音。
// 环路层把它当作没有写方：配对位为假，读头 10 ms 淡出（走「没有对端」的路径，不另开安全转移），回送为 0，
// 干声照常；恢复一致（任一端改回）后按新配对处理，不读不一致期间留在环里的任何帧。
//
//   - 一致时什么都不变；不一致时 paired 为 0、rateMismatch 为 1，peerSampleRate 是环头里的值；
//   - 回送在 10 ms 内线性淡到 0，逐样本的跳变不超过淡出的斜率，之后一直是 0、逐样本无效；
//   - 改回一致：新配对，从当前写头起读，D 之前一直是 0；之后读到的正是 D 个样本之前写下的那一帧；
//   - 本端自己换采样率（prepare）造成的不一致同样处理；开机就不一致的也一样；不一致超过 250 ms 也一样；
//   - 比较用的转换与 Output 发布采样率的转换相同（取整）。
//   - 对端槽（错序证据的块序号与时间戳、对端报的违约）的读取与采样率比较无关，不一致的块照常读。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本文件必须变红。
//   ⛔ 不比较采样率（paired 只看写权）→ 不一致期间回送照读，本文件的大部分用例变红；
//   ⛔ 不一致时直接把回送截成 0 而不走淡出 → 淡出那一例的阶跃判据变红；
//   ⛔ 采样率比较写成「环头值大于本端才算不一致」→ 反向（本端更高）那一例变红；
//   ⛔ 本端采样率取整改成四舍五入 → 非整数采样率那一例变红；
//   ⛔ 不一致期间仍算有对端（恢复一致不按新配对）→ 恢复时记一次格式变化重同步，「改回一致」一例变红。
//   ⛔ 不一致的块不读对端槽 → 错序证据没有新块可判、对端在不一致期间报的违约不计，「对端槽照常读」一例变红。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "TimingKit.h"
#include "loop/ExportLatch.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;

constexpr int kBlock = 256;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kFadeOut = 480; // 没有对端：读头 10 ms 淡出
constexpr std::int64_t kOnset = 960; // 新读头的 20 ms onset
constexpr float kLevel = 0.5f;

// 常数信号：读头读到 0 或凭空消失都是一处明显的阶跃，淡变的斜率则是已知的小量
void constantSignal(Rig& rig)
{
    rig.setSignal([](std::int64_t) { return kLevel; });
}

struct Run
{
    explicit Run(double rate = 48000.0) : rig(rate, 1024), params(timeParams(kDelay, rate)) {}

    BlockResult step(int n = kBlock)
    {
        last = rig.step(n, params);
        return last;
    }
    void steps(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            step();
        }
    }
    // 写方（第一个 Output）以 rate 重新发布自己
    void republish(std::uint32_t rate) { rinne::ipc::LoopRing::publishWriter(rig.ring(), 1, rate, 2); }
    std::size_t now() const { return rig.trace.size(); }

    Rig rig;
    EngineParams params;
    BlockResult last;
};

bool allZeroFrom(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    for (std::size_t i = from; i < to && i < trace.size(); ++i)
    {
        if (trace[i] != 0.0f)
        {
            return false;
        }
    }
    return from <= trace.size();
}

} // namespace

TEST_CASE("采样率不一致 一致时什么都不变")
{
    Run run;
    constantSignal(run.rig);
    run.steps(60);
    const LoopStatusFields s = run.rig.status();
    CHECK(s.paired == 1u);
    CHECK(s.rateMismatch == 0u);
    CHECK(s.peerSampleRate == 48000u);
    CHECK(run.rig.trace.back() == kLevel);
}

TEST_CASE("采样率不一致 Output 改了采样率：回送 10 ms 内线性淡到 0，之后一直是 0")
{
    // ⛔ 删除式验证点：不比较采样率 → 回送不淡出，本用例变红；直接截成 0 → 阶跃判据变红。
    Run run;
    constantSignal(run.rig);
    run.steps(60);
    REQUIRE(run.rig.trace.back() == kLevel);
    const std::size_t at = run.now();

    run.republish(96000); // Output 以 96 kHz 重新 prepare 并发布
    run.steps(40);

    const LoopStatusFields s = run.rig.status();
    CHECK(s.paired == 0u);
    CHECK(s.rateMismatch == 1u);
    CHECK(s.peerSampleRate == 96000u);
    CHECK(s.tapsActive == 0u);
    CHECK_FALSE(run.last.paired);

    // 淡出：从不一致被看到的那一块块首起线性降到 0，最陡的一步是 kLevel / 480
    CHECK(run.rig.trace[at] == kLevel); // 淡出从满增益起步
    CHECK(run.rig.trace[at + 1] < kLevel);
    CHECK(std::fabs(run.rig.trace[at + kFadeOut / 2] - kLevel * 0.5f) < 0.01f); // 线性：中点是一半
    CHECK(run.rig.trace[at + kFadeOut] == 0.0f);
    CHECK(maxStep(std::vector<float>(run.rig.trace.begin() + static_cast<std::ptrdiff_t>(at) - 1,
                                     run.rig.trace.end())) <= kLevel / static_cast<float>(kFadeOut) + 1.0e-6f);
    // 之后一直是 0，逐样本无效
    CHECK(allZeroFrom(run.rig.trace, at + kFadeOut, run.now()));
    CHECK(run.last.nonZeroReturn == 0u);
    for (int i = 0; i < kBlock; ++i)
    {
        CHECK(run.last.returnValid[i] == 0u);
    }
}

TEST_CASE("采样率不一致 本端比环头高也算不一致（两个方向都判）")
{
    // ⛔ 删除式验证点：比较写成「环头值大于本端才算不一致」→ 本用例变红。
    Run run;
    constantSignal(run.rig);
    run.steps(60);
    run.republish(44100);
    run.steps(10);
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(run.rig.status().paired == 0u);
    CHECK(run.rig.trace.back() == 0.0f);
}

TEST_CASE("采样率不一致 改回一致：新配对，D 之前一直是 0，不读不一致期间留在环里的帧")
{
    // ⛔ 删除式验证点：不一致期间仍算有对端（恢复时不按新配对）→ 恢复记一次格式变化重同步，本用例变红。
    Run run;
    // 信号按位置编码，读回来能对上第几帧；不一致期间写下的帧（位置 ≥ 60 块）也是这个信号，所以泄漏会被看见
    run.rig.setSignal([](std::int64_t p) { return noiseAt(p); });
    run.steps(60);
    run.republish(96000);
    run.steps(20); // 不一致期间，Output 仍在往环里写（按另一个速率）
    REQUIRE(run.rig.status().rateMismatch == 1u);
    const std::size_t recover = run.now();

    run.republish(48000);
    run.steps(40);
    const LoopStatusFields s = run.rig.status();
    CHECK(s.paired == 1u);
    CHECK(s.rateMismatch == 0u);
    CHECK(s.peerSampleRate == 48000u);
    CHECK(run.rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kFormat)] == 0u); // 不是格式重同步

    // 新配对从当前写头起读：恢复之后 D 个样本里一直是 0
    CHECK(allZeroFrom(run.rig.trace, recover, recover + static_cast<std::size_t>(kDelay)));
    // 之后是 20 ms onset，再往后读到的正是 D 个样本之前写下的那一帧（环位置 = 输出下标 − D）
    for (std::size_t t = recover + static_cast<std::size_t>(kDelay + kOnset); t < run.now(); ++t)
    {
        REQUIRE(run.rig.trace[t] == noiseAt(static_cast<std::int64_t>(t) - kDelay));
    }
}

TEST_CASE("采样率不一致 本端换采样率（prepare）造成的不一致同样处理")
{
    Run run;
    constantSignal(run.rig);
    run.steps(60);
    REQUIRE(run.rig.trace.back() == kLevel);

    // 本端以 96 kHz 重新 prepare，环头里仍是 48 kHz
    REQUIRE(run.rig.session().prepare(96000.0));
    run.steps(20);
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(run.rig.status().paired == 0u);
    CHECK(run.rig.status().peerSampleRate == 48000u);
    CHECK(run.rig.trace.back() == 0.0f);
    CHECK(run.last.nonZeroReturn == 0u);

    // Output 也以 96 kHz 发布：一致了，按新配对
    run.republish(96000);
    run.steps(60);
    CHECK(run.rig.status().rateMismatch == 0u);
    CHECK(run.rig.status().paired == 1u);
    CHECK(run.rig.status().peerSampleRate == 96000u);
}

TEST_CASE("采样率不一致 开机就不一致：从没配上，回送一直是 0，改一致后才配上")
{
    Run run;
    constantSignal(run.rig);
    run.republish(96000); // 第一块之前就不一致
    run.steps(30);
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(run.rig.status().paired == 0u);
    CHECK(run.rig.status().tapsActive == 0u);
    CHECK(allZeroFrom(run.rig.trace, 0, run.now()));

    run.republish(48000);
    run.steps(60);
    CHECK(run.rig.status().paired == 1u);
    CHECK(run.rig.trace.back() == kLevel);
}

TEST_CASE("采样率不一致 超过 250 ms 的不一致：停用清空之后恢复，仍是新配对，什么旧数据都不读")
{
    Run run;
    run.rig.setSignal([](std::int64_t p) { return noiseAt(p); });
    run.steps(60);
    run.republish(96000);
    run.steps(60); // 60 × 256 = 15360 样本，超过 250 ms（12000）
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(allZeroFrom(run.rig.trace, 60 * kBlock + kFadeOut, run.now()));
    const std::size_t recover = run.now();

    run.republish(48000);
    run.steps(60);
    CHECK(run.rig.status().paired == 1u);
    CHECK(allZeroFrom(run.rig.trace, recover, recover + static_cast<std::size_t>(kDelay)));
    for (std::size_t t = recover + static_cast<std::size_t>(kDelay + kOnset); t < run.now(); ++t)
    {
        REQUIRE(run.rig.trace[t] == noiseAt(static_cast<std::int64_t>(t) - kDelay));
    }
}

TEST_CASE("采样率不一致 比较用的转换与 Output 发布采样率的转换相同（取整）")
{
    // Output 发布的是 static_cast<uint32_t>(采样率)：44100.7 发布成 44100，本端也必须当作 44100，否则永远配不上
    Run run(44100.7);
    constantSignal(run.rig);
    // Rig 构造时按 static_cast<uint32_t>(44100.7) 发布
    run.steps(60);
    CHECK(run.rig.status().peerSampleRate == 44100u);
    CHECK(run.rig.status().rateMismatch == 0u);
    CHECK(run.rig.status().paired == 1u);
}

TEST_CASE("采样率不一致 对端槽照常读：错序证据照常出票，对端报的违约照常计一次，恢复一致后不重复计")
{
    // 采样率不一致只改配对位；对端槽（块序号、宿主时间戳、违约标志）的读取与环头无关，每个块都读。
    // ⛔ 删除式验证点：不一致的块不读对端槽 → 槽里的值停在旧的，错序证据没有新块可判，票数清零，结论定不下来；
    //    对端在不一致期间报的违约也不会被计到，本用例变红。
    Run run;
    run.rig.enablePeerSlot();
    TransportInfo transport;
    transport.nonrealtime = true;
    transport.hostNsValid = true;
    std::uint64_t ns = 5000000;
    constexpr std::uint64_t kStepNs = 256ull * 1000000000ull / 48000ull;
    // 一个宿主周期：Output 先发布这个周期的时间戳（Output 在前的一票），Input 再读；环里的帧仍是 Input 先读、写方后写
    const auto cycle = [&] {
        transport.hostNs = ns;
        run.rig.publishPeer(transport);
        run.last = run.rig.feed(kBlock, run.params, transport);
        run.rig.write(kBlock);
        ns += kStepNs;
    };

    cycle(); // 第一次读到对端槽，只记块序号
    cycle(); // 第一票
    CHECK(run.rig.status().orderVotes == 1u);

    run.republish(96000);
    cycle(); // 不一致的第一块：第二票
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(run.rig.status().orderVotes == 2u);
    CHECK(run.rig.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
    cycle(); // 第三票，在不一致期间定案
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(run.rig.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kOutputFirst));
    CHECK(run.rig.status().exportEvents == 0u);

    // 对端在不一致期间的一个新块带违约位：计一次（该块是离线块），原因只有对端违约
    run.rig.setPeerOverMax(true);
    cycle();
    run.rig.setPeerOverMax(false);
    CHECK(run.rig.status().exportEvents == 1u);
    CHECK(run.rig.status().exportCauses == rinne::loop::kExportCausePeerOverMax);
    cycle();
    CHECK(run.rig.status().exportEvents == 1u);

    // 改回一致：新配对不算导出不一致，对端那一次也不会再计
    run.republish(48000);
    for (int i = 0; i < 4; ++i)
    {
        cycle();
    }
    CHECK(run.rig.status().rateMismatch == 0u);
    CHECK(run.rig.status().exportEvents == 1u);
}
