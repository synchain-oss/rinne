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
//   - 比较用的转换与写方发布采样率的转换是同一个函数（publishedSampleRate，四舍五入），小数采样率
//     （44055.944 Hz）两端一致；一端 44055.944、另一端 44056，发布值相同，也算一致；
//   - 不一致的第一块不拉环：写方这时已经按新速率在写，旧读头的淡出只读之前已经拉进来的帧；
//   - 写方改了又改回、本端一块都没跑：采样率一致，仍是一次格式变化重同步，不走不一致路径；
//   - 对端槽（错序证据的块序号与时间戳、对端报的违约）的读取与采样率比较无关，不一致的块照常读。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本文件必须变红。
//   ⛔ 不比较采样率（paired 只看写权）→ 不一致期间回送照读，本文件的大部分用例变红；
//   ⛔ 不一致时直接把回送截成 0 而不走淡出 → 淡出那一例的阶跃判据变红；
//   ⛔ 采样率比较写成「环头值大于本端才算不一致」→ 反向（本端更高）那一例变红；
//   ⛔ publishedSampleRate 改回截断 → 一端小数、另一端取整的两例变红（单元用例的逐值也变红）；
//   ⛔ LoopSession::prepare 里本端采样率改回 static_cast 截断 → 两端同为 44055.944 的一例变红；
//   ⛔ 不一致的第一块仍把环拉到当前写头 → 「第一块不拉环」一例变红（淡出读到了新速率的帧）；
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
    explicit Run(double rate = 48000.0, std::int64_t declaredBlock = 1024)
        : rig(rate, declaredBlock), params(timeParams(kDelay, rate))
    {
    }

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
    // 写方以 rate（Hz，可以是小数）重新 prepare：发布值经 publishedSampleRate，与生产里写方发布的转换相同
    void republishRate(double rate) { republish(publishedSampleRate(rate)); }
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

TEST_CASE("采样率不一致 小数采样率：两端同为 44055.944 Hz，发布与比较是同一个转换，不判不一致，回送照常")
{
    // 44.1 kHz 的拉降是 44055.944 Hz：写方发布的整数是 44056。本端若按截断算成 44055，两边永远对不上，回送恒为 0。
    // ⛔ 删除式验证点：LoopSession::prepare 里本端采样率改回 static_cast 截断 → 本用例变红。
    Run run(44055.944);
    constantSignal(run.rig);
    run.steps(60);
    const LoopStatusFields s = run.rig.status();
    CHECK(s.peerSampleRate == 44056u);
    CHECK(s.rateMismatch == 0u);
    CHECK(s.paired == 1u);
    CHECK(run.rig.trace.back() == kLevel); // 回送照常
    // 其余小数部分过半的采样率同理（试验台用同一个转换发布）
    for (const double rate : {44100.7, 47999.6, 88199.9})
    {
        Run other(rate);
        constantSignal(other.rig);
        other.steps(60);
        INFO("rate = " << rate);
        CHECK(other.rig.status().rateMismatch == 0u);
        CHECK(other.rig.status().paired == 1u);
        CHECK(other.rig.status().peerSampleRate == publishedSampleRate(rate));
        CHECK(other.rig.trace.back() == kLevel);
    }
}

TEST_CASE("采样率不一致 一端 44055.944、另一端 44056：发布值相同，算一致")
{
    // 口径：比较的是两边各自取整之后发布的整数，不是原始的小数采样率。
    // ⛔ 删除式验证点：publishedSampleRate 改回截断 → 44055.944 取成 44055，与另一端的 44056 对不上，本用例变红。
    SECTION("本端 44055.944，写方发布 44056")
    {
        Run run(44055.944);
        constantSignal(run.rig);
        run.steps(30);
        run.republish(44056u); // 值没变：不抬 epoch，也不重同步
        run.steps(30);
        CHECK(run.rig.status().rateMismatch == 0u);
        CHECK(run.rig.status().paired == 1u);
        CHECK(run.rig.status().resyncCount == 0u);
        CHECK(run.rig.trace.back() == kLevel);
    }
    SECTION("本端 44056，写方以 44055.944 发布")
    {
        Run run(44056.0);
        constantSignal(run.rig);
        run.steps(30);
        run.republishRate(44055.944); // 发布值是 44056，与原来相同
        run.steps(30);
        CHECK(run.rig.status().rateMismatch == 0u);
        CHECK(run.rig.status().paired == 1u);
        CHECK(run.rig.status().peerSampleRate == 44056u);
        CHECK(run.rig.status().resyncCount == 0u);
        CHECK(run.rig.trace.back() == kLevel);
    }
}

TEST_CASE("采样率不一致 比较用的转换与写方发布采样率的转换相同（四舍五入）")
{
    // 写方与本端用同一个转换：44100.7 发布成 44101，本端也必须当作 44101，否则永远配不上
    Run run(44100.7);
    constantSignal(run.rig);
    run.steps(60);
    CHECK(run.rig.status().peerSampleRate == 44101u);
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

TEST_CASE("采样率不一致 第一块不拉环：写方已按新速率写下的帧不进旧读头的淡出（D 接近块长）")
{
    // 写方在两个 Input 块之间换了采样率并写下一整块：这块帧是新速率的。Input 在下一块看到不一致时，写方没有停写，
    // 不能像「对端刚走」那样把环拉到当前写头；旧读头的淡出只读之前拉进来的帧。
    // D 取块长 + 44：淡出要读的位置离写头只有 300 帧，拉了环就会读到新速率的那 256 帧（特征值 kMarker）。
    // ⛔ 删除式验证点：不一致的第一块仍把环拉到当前写头 → 淡出读到 kMarker，本用例变红。
    constexpr std::int64_t kNear = 300;
    constexpr float kMarker = 0.9f;
    constexpr std::int64_t kSwitch = 100 * kBlock; // 写方换采样率时的写头
    constexpr std::int64_t kRecover = kSwitch + 40 * kBlock; // 改回一致时的写头
    Run run(48000.0, kBlock);
    run.params = timeParams(kNear);
    run.rig.setSignal([=](std::int64_t p) {
        if (p < kSwitch)
        {
            return kLevel;
        }
        return p < kRecover ? kMarker : noiseAt(p);
    });
    run.steps(100);
    REQUIRE(run.rig.trace.back() == kLevel);
    REQUIRE(run.rig.status().delayEffective == kNear);

    // Input 读完这一块之后，写方换采样率，再写下一整块（新速率的帧，值是 kMarker）
    run.last = run.rig.feed(kBlock, run.params);
    run.republish(96000);
    run.rig.write(kBlock);
    REQUIRE(run.rig.status().paired == 1u); // 上一块还配着，不一致要到下一块才被看到
    const std::size_t at = run.now();

    run.step(kBlock); // 看到不一致的第一块
    CHECK(run.rig.status().rateMismatch == 1u);
    CHECK(run.rig.status().paired == 0u);
    run.steps(10);

    // 淡出读的全是不一致之前的帧：没有一个样本超过原来的电平，更不会出现 kMarker 的幅度
    float peak = 0.0f;
    for (std::size_t t = at; t < run.now(); ++t)
    {
        peak = std::max(peak, std::fabs(run.rig.trace[t]));
    }
    CHECK(peak <= kLevel);
    CHECK(run.rig.trace[at] == kLevel); // 淡出从满增益起步
    // 能用的帧只有 D − 块长 = 44 个：淡出缩短到恰好用完的那一点走到 0，之后一直是 0；不阶跃
    constexpr std::int64_t kAvailable = kNear - kBlock;
    CHECK(run.rig.trace[at + static_cast<std::size_t>(kAvailable)] == 0.0f);
    CHECK(allZeroFrom(run.rig.trace, at + static_cast<std::size_t>(kAvailable), run.now()));
    CHECK(maxStep(std::vector<float>(run.rig.trace.begin() + static_cast<std::ptrdiff_t>(at) - 1,
                                     run.rig.trace.end())) <= kLevel / static_cast<float>(kAvailable) + 1.0e-6f);

    // 改回一致：新配对，从当前写头起读，不读不一致期间写下的 kMarker；D 之后读到的是改回之后写下的帧
    run.steps(static_cast<int>((kRecover - static_cast<std::int64_t>(run.now())) / kBlock));
    REQUIRE(static_cast<std::int64_t>(run.now()) == kRecover);
    run.republish(48000);
    run.steps(40);
    CHECK(run.rig.status().paired == 1u);
    CHECK(run.rig.status().rateMismatch == 0u);
    CHECK(allZeroFrom(run.rig.trace, static_cast<std::size_t>(kRecover), static_cast<std::size_t>(kRecover + kNear)));
    for (std::size_t t = static_cast<std::size_t>(kRecover + kNear + kOnset); t < run.now(); ++t)
    {
        REQUIRE(run.rig.trace[t] == noiseAt(static_cast<std::int64_t>(t) - kNear));
    }
}

TEST_CASE("采样率不一致 写方改了又改回、本端一块都没跑：采样率一致，仍是一次格式变化重同步，不走不一致路径")
{
    // 环头 epoch 只在采样率改变时抬。写方在两个 Input 块之间 48 → 96 → 48 kHz，Input 看到的采样率与本端一致、
    // epoch 变了：这是一次格式变化重同步（旧读头 5 ms 淡出，新读头从当前写头起读），不是不一致，回送不被置 0。
    // 单端换采样率之后，生产里只有这一种情形还走得到这条重同步；单端停在另一个采样率上走的是上面的不一致路径。
    Run run;
    constantSignal(run.rig);
    run.steps(60);
    REQUIRE(run.rig.trace.back() == kLevel);
    REQUIRE(run.rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kFormat)] == 0u);
    const std::size_t at = run.now();

    run.republish(96000);
    run.republish(48000);
    run.steps(60);

    const LoopStatusFields s = run.rig.status();
    CHECK(s.rateMismatch == 0u);
    CHECK(s.paired == 1u);
    CHECK(s.resyncByReason[static_cast<std::size_t>(SyncReason::kFormat)] == 1u);
    CHECK(s.resyncCount == 1u);

    // 旧读头 5 ms（240 个样本）线性淡出，之后到新读头的第一个样本（D 之后）是 0；再 20 ms onset 回到满电平
    constexpr std::int64_t kResyncFade = 240;
    CHECK(run.rig.trace[at] == kLevel);
    CHECK(run.rig.trace[at + kResyncFade] == 0.0f);
    CHECK(allZeroFrom(run.rig.trace, at + kResyncFade, at + static_cast<std::size_t>(kDelay)));
    CHECK(maxStep(std::vector<float>(run.rig.trace.begin() + static_cast<std::ptrdiff_t>(at) - 1,
                                     run.rig.trace.end())) <= kLevel / static_cast<float>(kResyncFade) + 1.0e-6f);
    CHECK(run.rig.trace.back() == kLevel);
}
