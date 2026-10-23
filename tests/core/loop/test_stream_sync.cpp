// SPDX-License-Identifier: GPL-3.0-or-later
//
// StreamSync 的判据：断档、严格模式的对齐、容忍模式、重同步的淡变、两端停用清空、
// 同采样率重 prepare、reset 只记诊断、起播沿的淡出与复位、宿主违约、读头槽。
//
// 这里直接驱动 StreamSync（判据那一半），不经过 LoopSession：判据是纯逻辑，
// 与环、延迟线的搬运无关。判据全部按样本计，与宿主怎么切块无关。
//
// 用例编号：L2-01…L2-11。标 ⛔ 的是删除式验证点。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "loop/LocalLine.h"
#include "loop/StreamSync.h"
#include "loop/Taps.h"

namespace
{

using namespace rinne::loop;

constexpr double kSr = 48000.0;

// 一个把输入变成输出的驱动器：包住 StreamSync、LocalLine 与 TapSet，省掉每条用例的样板。
//
// 用法：driver.feed(writeHead, blockLength) 推进一块；driver.block() 给出这一块的 SyncOutcome。
class Driver
{
public:
    Driver()
    {
        line_.allocate(kSr);
        sync_.prepare(kSr);
    }

    // 推进一块：writeHead 是这一块看到的写头 W，n 是块长。
    // 有数据的上界取 max(已拉到的位置, W)：模拟 LoopSession 在判据之后照常把环拉到 W。
    SyncOutcome feed(i64 writeHead, int n, bool paired = true)
    {
        SyncInputs in;
        in.paired = paired;
        in.writeHead = writeHead;
        in.dataEnd = std::max(line_.pulled(), writeHead);
        in.processed = processed_;
        in.declaredBlock = declaredBlock_;
        in.peerMaxBlock = peerMaxBlock_;
        in.blockLength = n;
        SyncOutcome outcome = sync_.process(in, line_, taps_);
        processed_ += n;
        advanceTaps(taps_, n);
        return outcome;
    }

    // 推进一块并带上一批事实（起播沿、换写方、停用…）。in.dataEnd 为 0 时按 feed 的口径补上。
    SyncOutcome feedWith(SyncInputs& in, i64 writeHead, int n)
    {
        in.writeHead = writeHead;
        if (in.dataEnd == 0)
        {
            in.dataEnd = std::max(line_.pulled(), writeHead);
        }
        in.processed = processed_;
        in.declaredBlock = declaredBlock_;
        in.peerMaxBlock = peerMaxBlock_;
        in.blockLength = n;
        SyncOutcome outcome = sync_.process(in, line_, taps_);
        processed_ += n;
        advanceTaps(taps_, n);
        return outcome;
    }

    LocalLine& line() { return line_; }
    TapSet& taps() { return taps_; }
    StreamSync& sync() { return sync_; }
    i64 processed() const { return processed_; }

    void setDeclaredBlock(std::int64_t n) { declaredBlock_ = n; }
    void setPeerMaxBlock(std::int64_t n) { peerMaxBlock_ = n; }

private:
    LocalLine line_;
    TapSet taps_;
    StreamSync sync_;
    i64 processed_ = 0;
    std::int64_t declaredBlock_ = 512;
    std::int64_t peerMaxBlock_ = 0;
};

// 事件出口的收集器：把事件码记下来，供用例断言。
struct EventLog
{
    std::vector<std::uint16_t> codes;

    static void sink(void* context, std::uint16_t code, std::uint8_t, std::uint8_t, std::int64_t) noexcept
    {
        static_cast<EventLog*>(context)->codes.push_back(code);
    }

    bool has(std::uint16_t code) const
    {
        for (const auto c : codes)
        {
            if (c == code)
            {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE("loop sync L2-01 断档：R 前进 2·N_max − 1 不判断档，= 2·N_max 判断档")
{
    // ⛔ 删除式验证点：把判据改成按块计 → 本用例变红。
    Driver driver;
    driver.setDeclaredBlock(512);
    driver.setPeerMaxBlock(0);

    const std::int64_t limit = stallThreshold(512, 0); // 1024

    // 推进量恰好 2·N_max − 1：还不判断档。
    driver.feed(0, static_cast<int>(limit - 1));
    REQUIRE_FALSE(driver.sync().state() == SyncState::kStalled);

    // 再推 1 个样本：累计正好 2·N_max，判断档。
    const SyncOutcome outcome = driver.feed(0, 1);
    REQUIRE(outcome.stalled);
    REQUIRE(driver.sync().state() == SyncState::kStalled);
}

TEST_CASE("loop sync L2-01b 断档之后写头恢复推进，判据解除")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    driver.feed(0, 1024);
    REQUIRE(driver.sync().state() == SyncState::kStalled);

    // 写头恢复推进：解除断档并重建读头。
    const SyncOutcome outcome = driver.feed(512, 512);
    REQUIRE_FALSE(outcome.stalled);
    REQUIRE(driver.sync().state() != SyncState::kStalled);
}

TEST_CASE("loop sync L2-02 严格模式：Δ = W − R（扣掉 off）连续两次不为 0 才重同步；块长与写头增量怎么变都不算")
{
    // ⛔ 删除式验证点：把判据改回「写头每块的增量与上一块比」→ 第一段（块长 512 / 480 交替）就重同步，本用例变红。
    Driver driver;
    driver.setDeclaredBlock(512);

    // 正确摆放：写方与读方处理同一串样本，每块看到的 W 恰好等于本块的块首读位 R（偏移锚在 0）。
    // 块长 512、480 交替：写头每块的增量跟着交替，但 m = W − (R + off) 恒为 0。
    for (int block = 0; block < 40; ++block)
    {
        const int n = block % 2 == 0 ? 512 : 480;
        const SyncOutcome outcome = driver.feed(driver.processed(), n);
        REQUIRE_FALSE(outcome.resynced);
    }
    REQUIRE(driver.sync().resyncCount() == 0);
    REQUIRE(driver.sync().misalignedChecks() == 0);

    // Output 晚到一块：这一块写头没动（不算一次检查），下一块补上（m 回到 0）。不重同步。
    const i64 frozen = driver.processed();
    REQUIRE_FALSE(driver.feed(frozen, 512).resynced);
    REQUIRE_FALSE(driver.feed(frozen, 512).resynced);
    REQUIRE_FALSE(driver.feed(driver.processed(), 512).resynced);
    REQUIRE(driver.sync().resyncCount() == 0);

    // m 只有一次不为 0、下一次回到 0：吸收，不重同步。
    REQUIRE_FALSE(driver.feed(driver.processed() - 128, 512).resynced);
    REQUIRE(driver.sync().misalignedChecks() == 1);
    REQUIRE_FALSE(driver.feed(driver.processed(), 512).resynced);
    REQUIRE(driver.sync().misalignedChecks() == 0);
    REQUIRE(driver.sync().resyncCount() == 0);

    // 真错位：写方丢了一块（那一块写头没动，不算检查），之后照常前进，但 W 从此比 R + off 少 512：
    // m 连续两次检查都不为 0 → 第二次检查时重同步（原因：对齐）。
    REQUIRE_FALSE(driver.feed(driver.processed() - 512, 512).resynced); // 写头没动
    REQUIRE_FALSE(driver.feed(driver.processed() - 512, 512).resynced); // 第一次检查：m = −512
    REQUIRE(driver.sync().misalignedChecks() == 1);
    const SyncOutcome outcome = driver.feed(driver.processed() - 512, 512); // 第二次检查
    REQUIRE(outcome.resynced);
    REQUIRE(outcome.reason == SyncReason::kShift);
    REQUIRE(driver.sync().resyncCount() == 1);
    // 重同步之后偏移重新锚定：同样的落后量下 m 回到 0，不再重同步。
    for (int block = 0; block < 8; ++block)
    {
        REQUIRE_FALSE(driver.feed(driver.processed() - 512, 512).resynced);
    }
    REQUIRE(driver.sync().resyncCount() == 1);
}

TEST_CASE("loop sync L2-03 10 s 内第 3 次重同步 → 转容忍模式并置「时序不稳定」")
{
    // ⛔ 删除式验证点：去掉容忍模式 → 本用例变红。
    Driver driver;
    driver.setDeclaredBlock(512);

    EventLog log;
    driver.sync().setEventSink(LoopEventSink(&EventLog::sink, &log));

    // 漂移跳变（写头猛冲）触发重同步。四次跳变：前三次落在 10 s 窗口内。
    driver.feed(0, 512);
    driver.feed(1, 512);
    driver.feed(2, 10000);
    driver.feed(3, 10000);
    driver.feed(4, 10000);
    driver.feed(5, 10000);

    REQUIRE(driver.sync().resyncCount() >= 3);
    REQUIRE(driver.sync().tolerant());
    REQUIRE(driver.sync().unstable());
    REQUIRE(log.has(static_cast<std::uint16_t>(LoopEventCode::kTolerantMode)));
    REQUIRE(log.has(static_cast<std::uint16_t>(LoopEventCode::kUnstable)));
}

TEST_CASE("loop sync L2-03b 超出 10 s 的重同步不转容忍模式")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    // 同样的三次跳变，但每两次之间隔 > 10 s 的样本数，所以第 3 次不在窗口内。
    driver.feed(0, 512);
    driver.feed(1, 512);
    driver.feed(2, 480001);
    driver.feed(3, 480001);
    driver.feed(4, 480001);
    driver.feed(5, 480001);

    REQUIRE(driver.sync().resyncCount() >= 3);
    REQUIRE_FALSE(driver.sync().tolerant());
}

TEST_CASE("loop sync L2-04 重同步：旧读头 5 ms 淡出、新读头 20 ms onset")
{
    // ⛔ 删除式验证点：去掉淡出 → 本用例变红（出现阶跃）。
    Driver driver;
    driver.setDeclaredBlock(512);

    // 读头（满增益）。
    Tap& tap = driver.taps().at(0);
    tap.active = true;
    tap.offset = 0;
    tap.delay = 100;
    tap.validFrom = 0;
    tap.validTo = 1000000;

    // 触发一次重同步（漂移跳变：写头一下子领先超过门限）。块要小：5 ms 的淡出是 240 个样本，块太大淡出就在块末走完了。
    driver.feed(0, 64);
    driver.feed(64, 64);
    const SyncOutcome outcome = driver.feed(128 + 20000, 64);
    REQUIRE(outcome.resynced);
    REQUIRE(outcome.reason == SyncReason::kJump);
    REQUIRE(tap.env.fadeOutSteps == 240);

    // 旧读头开始淡出：包络在下降（不再是 1）。
    REQUIRE(tap.env.fadingOut());
    REQUIRE(TapSet::currentEnvelope(tap) < 1.0f);
    REQUIRE(TapSet::currentEnvelope(tap) > 0.0f);

    // 淡出是单调下降的：每推进一步包络都不增。
    float previous = TapSet::currentEnvelope(tap);
    for (int i = 0; i < 20; ++i)
    {
        advanceTaps(driver.taps(), 1);
        const float now = TapSet::currentEnvelope(tap);
        REQUIRE(now <= previous);
        previous = now;
    }
}

TEST_CASE("loop sync L2-05 两端停用 > 250 ms 清空本地延迟线；恰好 250 ms 不清")
{
    // ⛔ 删除式验证点：去掉停用 250 ms 清空 → 本用例变红（停用后复燃）。
    Driver driver;
    driver.setDeclaredBlock(512);

    // 先拉一点数据进来。
    driver.line().pull(0, 100, [&](std::int64_t, float& l, float& r) {
        l = 1.0f;
        r = 1.0f;
    });
    REQUIRE(driver.line().pulled() == 100);

    SyncInputs in;
    in.paired = false;
    in.inactive = true;

    // 恰好 250 ms（12000 个样本，48 kHz）：不清。
    driver.feedWith(in, 0, 12000);
    REQUIRE_FALSE(driver.sync().state() == SyncState::kStalled);
    REQUIRE(driver.line().pulled() == 100);

    // 超过 250 ms：清空（pulled 归零）。
    driver.feedWith(in, 0, 1);
    REQUIRE(driver.line().pulled() == 0);
}

TEST_CASE("loop sync 两端停用：一段停用只清一次、只发一条提示码；回到活跃之后再停用超过 250 ms 再清一次")
{
    // ⛔ 删除式验证点：去掉「一段停用只清一次」的锁存 → 超过 250 ms 之后每一块都清一遍、都发一条提示码，本用例变红。
    Driver driver;
    driver.setDeclaredBlock(512);
    EventLog log;
    driver.sync().setEventSink(LoopEventSink(&EventLog::sink, &log));
    const auto clearedCount = [&] {
        return std::count(log.codes.begin(), log.codes.end(), static_cast<std::uint16_t>(LoopEventCode::kLineCleared));
    };

    SyncInputs in;
    in.paired = false;
    in.inactive = true;

    // 第一段停用：跨过 250 ms 的那一块清一次，之后继续停用的 40 块不再清。
    driver.feedWith(in, 0, 12000);
    CHECK(clearedCount() == 0);
    CHECK(driver.feedWith(in, 0, 512).lineCleared);
    CHECK(clearedCount() == 1);
    for (int block = 0; block < 40; ++block)
    {
        CHECK_FALSE(driver.feedWith(in, 0, 512).lineCleared);
    }
    CHECK(clearedCount() == 1);

    // 回到活跃：计时与锁存都归零。再停用，超过 250 ms 之后又清一次（也只一次）。
    SyncInputs active;
    active.paired = true;
    driver.feedWith(active, 100, 512);
    CHECK(clearedCount() == 1);
    driver.feedWith(in, 100, 12000);
    CHECK(clearedCount() == 1);
    CHECK(driver.feedWith(in, 100, 512).lineCleared);
    for (int block = 0; block < 10; ++block)
    {
        CHECK_FALSE(driver.feedWith(in, 100, 512).lineCleared);
    }
    CHECK(clearedCount() == 2);
}

TEST_CASE("loop sync L2-06 同采样率重新 prepare：W、R、读头、延迟线内容逐位不变")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    driver.line().pull(0, 100, [&](std::int64_t, float& l, float& r) {
        l = 0.25f;
        r = -0.25f;
    });

    Tap& tap = driver.taps().at(0);
    tap.active = true;
    tap.offset = 17;
    tap.delay = 4800;
    tap.validFrom = 3;
    tap.validTo = 999;

    driver.feed(100, 512);

    const i64 pulledBefore = driver.line().pulled();
    const i64 offsetBefore = driver.sync().offset();
    const i64 writeHeadBefore = driver.sync().writeHead();
    const std::int64_t resyncBefore = driver.sync().resyncCount();

    // 同采样率重新 prepare：什么都不动。
    driver.sync().prepare(kSr);

    REQUIRE(driver.line().pulled() == pulledBefore);
    REQUIRE(driver.sync().offset() == offsetBefore);
    REQUIRE(driver.sync().writeHead() == writeHeadBefore);
    REQUIRE(driver.sync().resyncCount() == resyncBefore);

    // 读头逐位不变。
    REQUIRE(driver.taps().at(0).offset == 17);
    REQUIRE(driver.taps().at(0).delay == 4800);
    REQUIRE(driver.taps().at(0).validFrom == 3);
    REQUIRE(driver.taps().at(0).validTo == 999);

    // 延迟线的内容逐位不变。
    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(driver.line().read(10, l, r));
    REQUIRE(l == 0.25f);
    REQUIRE(r == -0.25f);
}

TEST_CASE("loop sync L2-07 reset() 只记诊断：调用前后音频输出逐位相同")
{
    // ⛔ 删除式验证点：让 reset 清延迟线 → 本用例变红。
    Driver driver;
    driver.setDeclaredBlock(512);

    driver.line().pull(0, 100, [&](std::int64_t, float& l, float& r) {
        l = 0.5f;
        r = 0.5f;
    });

    const std::uint32_t before = driver.sync().resetCount();
    driver.sync().noteReset();
    REQUIRE(driver.sync().resetCount() == before + 1);

    // 延迟线的内容逐位不变：reset 没有清它。
    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(driver.line().read(10, l, r));
    REQUIRE(l == 0.5f);
    REQUIRE(r == 0.5f);
    REQUIRE(driver.line().pulled() == 100);
}

TEST_CASE("loop sync L2-08 起播沿：s0 起 5 ms 淡出旧回送；reset_on_play 关时不起作用")
{
    // ⛔ 删除式验证点：去掉起播淡出 → 本用例变红（咔嗒）。
    Driver driver;
    driver.setDeclaredBlock(512);

    Tap& tap = driver.taps().at(0);
    tap.active = true;
    tap.offset = 0;
    tap.delay = 100;
    tap.validFrom = 0;
    tap.validTo = 1000000;

    driver.feed(0, 64);

    SyncInputs in;
    in.paired = true;
    in.playEdge = true;
    in.resetOnPlay = true;

    // 起播沿这一块写方推进到了 W = 512（本地延迟线还只拉到 0）。
    const SyncOutcome outcome = driver.feedWith(in, 512, 64);
    REQUIRE(outcome.fadeStarted);
    REQUIRE(outcome.resynced);
    REQUIRE(outcome.reason == SyncReason::kPlay);
    // 旧读头开始淡出，淡出长度 T = min(5 ms, D_eff) = 240 帧。
    REQUIRE(tap.env.fadingOut());
    REQUIRE(tap.env.fadeOutSteps == 240);
    // ⛔ 删除式验证点：旧读头的读上界取「上一块拉到的位置」→ 淡出读到多少随宿主切块变 → 本用例变红。
    // 上界取当前的 W：淡出要读的那 T 帧一定在 W 之前，这一块拉环之后都在。
    REQUIRE(tap.validTo == 512);
    // 新的对齐锚在 W − R；复位点是绝对样本号：沿的块首 R = 64，加 T。
    REQUIRE(driver.sync().offset() == 512 - 64);
    REQUIRE(outcome.resetAtSample == 64 + 240);

    // reset_on_play 关时：不起作用（读头不淡出）。
    Driver other;
    Tap& otherTap = other.taps().at(0);
    otherTap.active = true;
    otherTap.offset = 0;
    otherTap.delay = 100;
    otherTap.validFrom = 0;
    otherTap.validTo = 1000000;
    other.feed(0, 64);

    SyncInputs off;
    off.paired = true;
    off.playEdge = true;
    off.resetOnPlay = false;
    other.feedWith(off, 0, 64);
    REQUIRE_FALSE(otherTap.env.fadingOut());
}

TEST_CASE("loop sync L2-09 宿主违约（n > N_prep）：补 0 且 D_min 立即抬升")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    SyncOutcome outcome = driver.feed(0, 1024);
    REQUIRE(outcome.hostViolation);
    REQUIRE(outcome.overrunSamples == 1024 - 512);

    // D 的下限立刻抬到违约的块长。
    REQUIRE(delayMinFrames(512, 1024) == 1024);
}

TEST_CASE("loop sync L2-10 容忍模式下断档判据仍然生效")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    EventLog log;
    driver.sync().setEventSink(LoopEventSink(&EventLog::sink, &log));

    // 先转容忍模式：10 s 内三次漂移跳变。
    driver.feed(0, 512);
    driver.feed(1, 512);
    driver.feed(2, 10000);
    driver.feed(3, 10000);
    driver.feed(4, 10000);
    driver.feed(5, 10000);
    REQUIRE(driver.sync().tolerant());

    // 容忍模式下断档判据仍然生效：写头不动，R 前进 2·N_max。
    const SyncOutcome outcome = driver.feed(5, 1024);
    REQUIRE(outcome.stalled);
    REQUIRE(driver.sync().state() == SyncState::kStalled);
}

TEST_CASE("loop sync L2-11 读头槽：3 槽满时丢包络最小的旧读头，不硬丢")
{
    TapSet taps;
    REQUIRE(taps.allocate() == 0);
    REQUIRE(taps.allocate() == 1);
    REQUIRE(taps.allocate() == 2);
    // 满了：allocate 返回 -1（调用方按「丢一个包络最小的」处理）。
    REQUIRE(taps.allocate() == -1);
    REQUIRE(taps.count() == 3);

    // 让槽 1 的包络最小（正在淡出、且快走完了）。
    taps.at(0).env.fadeOutSteps = 240;
    taps.at(0).env.fadeOutStep = 0;
    taps.at(1).env.fadeOutSteps = 240;
    taps.at(1).env.fadeOutStep = 230; // 包络最小
    taps.at(2).env.fadeOutSteps = 240;
    taps.at(2).env.fadeOutStep = 100;

    const int dropped = taps.dropSmallestEnvelope();
    REQUIRE(dropped == 1);
    // 丢的是包络最小的那个，不是硬丢第一个。
    REQUIRE(taps.count() == 2);
    REQUIRE_FALSE(taps.at(1).active);
    REQUIRE(taps.at(0).active);
    REQUIRE(taps.at(2).active);
}

TEST_CASE("loop sync 0 样本的调用立刻返回，不推进任何计数")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    const SyncOutcome outcome = driver.feed(0, 0);
    REQUIRE_FALSE(outcome.resynced);
    REQUIRE_FALSE(outcome.stalled);
    REQUIRE(driver.sync().resyncCount() == 0);
    REQUIRE(driver.processed() == 0);
}

TEST_CASE("loop sync 没有对端：只判断为未配对，不做判据")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    const SyncOutcome outcome = driver.feed(0, 512, false);
    REQUIRE_FALSE(outcome.resynced);
    REQUIRE(driver.sync().state() == SyncState::kUnpaired);
}

TEST_CASE("loop sync 容忍模式只数对齐重同步：10 s 内换写方 4 次不转容忍模式")
{
    // ⛔ 删除式验证点：把换写方、改采样率、断档恢复、起播沿也算进容忍模式 → 本用例变红。
    // 那些是宿主或用户的动作，不是「时序不稳定」的证据：反复换写方不该让环路进容忍模式、亮「时序不稳定」。
    Driver driver;
    driver.setDeclaredBlock(512);

    driver.feed(0, 512);
    for (int i = 1; i <= 4; ++i)
    {
        SyncInputs in;
        in.paired = true;
        in.writerChanged = true;
        const SyncOutcome outcome = driver.feedWith(in, 512 * i, 512);
        REQUIRE(outcome.resynced);
        REQUIRE(outcome.reason == SyncReason::kWriter);
    }
    REQUIRE(driver.sync().resyncCount() == 4);
    REQUIRE_FALSE(driver.sync().tolerant());
    REQUIRE_FALSE(driver.sync().unstable());
}

TEST_CASE("loop sync 对端没了：活跃读头 10 ms 淡出，读位照常前进")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    // 延迟线里有足够的数据（对端走之前写下的），淡出能走满 10 ms。
    driver.line().pull(0, 2000, [&](std::int64_t, float& l, float& r) {
        l = 0.5f;
        r = 0.5f;
    });

    Tap& tap = driver.taps().at(0);
    tap.active = true;
    tap.delay = 100;
    tap.validFrom = 0;
    tap.validTo = 1000000;

    driver.feed(0, 64);
    const i64 before = driver.sync().processed();

    SyncInputs in;
    in.paired = false;
    const SyncOutcome outcome = driver.feedWith(in, 0, 64);
    REQUIRE(outcome.fadeStarted);
    REQUIRE(tap.env.fadingOut());
    REQUIRE(tap.env.fadeOutSteps == 480); // 10 ms @ 48 kHz
    REQUIRE(tap.validTo == 2000); // 读上界 = 本块拉环之后有数据的上界
    // 读位照常前进：不推进的话淡出期间读头会反复读同一段。
    REQUIRE(driver.sync().processed() == before + 64);
    REQUIRE(driver.sync().state() == SyncState::kUnpaired);
}

TEST_CASE("loop sync 淡出的读上界与长度：取本块拉环之后的上界；数据不够一个淡出时缩短到恰好用完，不以阶跃收尾")
{
    // ⛔ 删除式验证点：读上界改回「上一块拉到的位置」→ 第一段 validTo 停在上一块的位置、淡出被截短，本用例变红。
    // ⛔ 删除式验证点：去掉「按剩下的样本缩短淡出」→ 第二段的淡出长度仍是名义值（读到一半撞上界就是阶跃），本用例变红。
    // 几何：块长 512，D = 600（只比上一块的块长多 88，不到一个 5 ms 淡出）。第一块 R = 0、W = 488，偏移锚在 488；
    // 上一块拉到 488。第二块 R = 512、W = 1000（m = 0），读头从 R + off − D = 400 起读：离上一块拉到的位置只剩 88
    // 个样本， 离本块的 W 还有 600 个。
    const auto setUp = [](Driver& driver) -> Tap& {
        driver.setDeclaredBlock(512);
        driver.line().pull(0, 488, [&](std::int64_t, float& l, float& r) {
            l = 0.5f;
            r = 0.5f;
        });
        driver.feed(488, 512);
        REQUIRE(driver.sync().offset() == 488);
        Tap& tap = driver.taps().at(0);
        tap.active = true;
        tap.offset = driver.sync().offset();
        tap.delay = 600;
        tap.validFrom = 0;
        tap.validTo = 1000000;
        return tap;
    };

    // 直接调判据（不经 Driver：Driver 每块末推进包络，淡出在 512 的块里就走完释放了，这里要看挂上那一刻的读头）。
    {
        // 环还在（换写方：同一个环上的接力）：读上界是本块的 W，淡出走满 5 ms（240 个样本）。
        Driver driver;
        Tap& tap = setUp(driver);
        SyncInputs in;
        in.paired = true;
        in.writerChanged = true;
        in.writeHead = 1000;
        in.dataEnd = 1000; // LoopSession 的口径：本块照常把环拉到 W
        in.processed = driver.processed();
        in.declaredBlock = 512;
        in.blockLength = 512;
        const SyncOutcome outcome = driver.sync().process(in, driver.line(), driver.taps());
        REQUIRE(outcome.resynced);
        REQUIRE(outcome.reason == SyncReason::kWriter);
        REQUIRE(tap.validTo == 1000);
        REQUIRE(tap.env.fadeOutSteps == 240);
    }
    {
        // 环没了（对端连段一起走了）：只剩上一块拉到的 488，读头只剩 88 个样本。淡出缩短到 88，恰好在数据用完时走到 0。
        // 直接调判据（不经 Driver：Driver 每块末推进包络，这里要看挂上那一刻的包络）。
        Driver driver;
        Tap& tap = setUp(driver);
        LocalLine& line = driver.line();
        TapSet& taps = driver.taps();
        StreamSync& sync = driver.sync();
        SyncInputs in;
        in.paired = false;
        in.writeHead = 0;
        in.dataEnd = line.pulled();
        in.processed = driver.processed();
        in.declaredBlock = 512;
        in.blockLength = 512;
        const SyncOutcome outcome = sync.process(in, line, taps);
        REQUIRE(outcome.fadeStarted);
        REQUIRE(tap.validTo == 488);
        REQUIRE(tap.env.fadeOutSteps == 88);
        // 包络在第 88 个样本（读位置 488，第一个读不到的位置）恰好是 0：之后补的 0 与它相接，没有阶跃。
        REQUIRE(TapSet::currentEnvelope(tap) == 1.0f);
        advanceTaps(taps, 87);
        REQUIRE(TapSet::currentEnvelope(tap) > 0.0f);
        REQUIRE(TapSet::currentEnvelope(tap) <= 1.0f / 88.0f + 1.0e-6f);
        advanceTaps(taps, 1);
        REQUIRE_FALSE(tap.active); // 走完即释放（包络为 0）
    }
}

TEST_CASE("loop sync 已经在淡出的读头不被重新起算（重新起算是一处阶跃）")
{
    Driver driver;
    driver.setDeclaredBlock(512);

    // 延迟线里有足够的数据，淡出能走满 5 ms。
    driver.line().pull(0, 2000, [&](std::int64_t, float& l, float& r) {
        l = 0.5f;
        r = 0.5f;
    });

    Tap& tap = driver.taps().at(0);
    tap.active = true;
    tap.delay = 100;
    tap.validFrom = 0;
    tap.validTo = 1000000;

    // 先因断档开始淡出，走一半。块要小：5 ms 的淡出是 240 个样本，块太大淡出就在块末走完了。
    for (int i = 0; i < 16; ++i)
    {
        driver.feed(0, 64); // 第 16 块累计 1024 = 2·N_max → 断档，挂上 240 帧淡出
    }
    REQUIRE(driver.sync().state() == SyncState::kStalled);
    REQUIRE(tap.env.fadingOut());
    advanceTaps(driver.taps(), 100);
    const float midway = TapSet::currentEnvelope(tap);
    REQUIRE(midway < 1.0f);

    // 写头恢复（断档恢复重同步）：已在淡出的读头照它自己的进度走，包络不跳回 1。
    driver.feed(256, 64);
    REQUIRE(TapSet::currentEnvelope(tap) <= midway);
}
