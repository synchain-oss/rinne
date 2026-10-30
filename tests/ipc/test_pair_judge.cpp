// SPDX-License-Identifier: GPL-3.0-or-later
//
// 进程外配对自测（tests/oop/rinne_auhost 的 --pair 模式）的判据，在所有平台上自测：宿主只能在 mac 上跑，
// 但判据是纯函数（tests/oop/PairJudge.h），这里用手摆的槽与环快照把每一条判据的绿与红各跑一遍，
// 再把宿主拼 POSIX 段名的函数与产品的 composeSegmentName 逐个对拍，防两边的命名漂移。
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../oop/PairJudge.h"
#include "ipc/Naming.h"

namespace
{

using namespace rinne;
using rinne::oop::GroupSnapshot;
using rinne::oop::PairCheck;
using rinne::oop::Verdict;

constexpr ipc::u32 kHostPid = 100;
constexpr ipc::u32 kPluginPid = 4242;
constexpr std::uint64_t kBlocks = 200;

struct Pair
{
    GroupSnapshot early;
    GroupSnapshot late;
    PairCheck check;
};

// 一对健康的配对：同组、同进程（不是宿主）、窗口 200 块（约 2.1 秒），心跳与块计数照常走，环的写头跟着前进
Pair healthyPair()
{
    Pair p;
    p.check.hostPid = kHostPid;
    p.check.hostBlocks = kBlocks;
    const auto fill = [](oop::SlotSnapshot& s, std::uint64_t heartbeat, std::uint64_t seq, ipc::u32 safety) {
        s.ctl = ipc::makeCtl(7, ipc::kCtlPhaseActive);
        s.pid = kPluginPid;
        s.heartbeatMs = heartbeat;
        s.blockSeq = seq;
        s.safetyState = safety;
        s.rtFlags = 0;
    };
    p.early.group = 3;
    fill(p.early.input, 10000, 100, ipc::kSafetyFadeIn);
    fill(p.early.output, 10000, 100, ipc::kSafetyRun);
    p.early.ringMapped = true;
    p.early.ringMagic = ipc::kRingMagic;
    p.early.writeHead = 51200;
    p.late = p.early;
    fill(p.late.input, 12133, 100 + kBlocks, ipc::kSafetyRun);
    fill(p.late.output, 12133, 100 + kBlocks, ipc::kSafetyRun);
    p.late.writeHead = 51200 + kBlocks * 512;
    return p;
}

bool mentions(const Verdict& v, const std::string& text)
{
    for (const std::string& f : v.failures)
    {
        if (f.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string joined(const Verdict& v)
{
    std::string text;
    for (const std::string& f : v.failures)
    {
        text += f + " | ";
    }
    return text;
}

std::vector<float> tone(double hz, double amplitude, std::size_t count, double rate = 48000.0)
{
    std::vector<float> samples(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        samples[i] =
            static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) / rate));
    }
    return samples;
}

} // namespace

TEST_CASE("the host's POSIX segment names match the product's composeSegmentName", "[pair-judge][naming]")
{
    const ipc::u32 uids[] = {0u, 501u, 0xdeadbeefu, 0x000001f5u};
    for (const ipc::PrefixFamily& family : ipc::kPrefixFamilies)
    {
        for (const ipc::u32 uid : uids)
        {
            for (ipc::u32 group = 0; group <= ipc::kGroupCount; ++group)
            {
                std::string product;
                REQUIRE(ipc::composeSegmentName(ipc::NamePlatform::kPosix, family, ipc::NameScope::formal(), uid,
                                                ipc::SegmentId{group}, product) == ipc::NameStatus::kOk);
                INFO(family.posix << " uid " << uid << " group " << group);
                CHECK(oop::posixSegmentName(family.posix, uid, group) == product);
            }
        }
    }
}

TEST_CASE("the host reads the rendezvous slots and finds the first group with both ends active", "[pair-judge]")
{
    auto rv = std::make_unique<ipc::RvSegment>();
    ipc::GroupBlock& block = rv->groups[2]; // 组 3
    const auto set = [](ipc::RoleSlot& s, ipc::u32 phase, ipc::u32 pid, std::uint64_t hb, std::uint64_t seq) {
        s.ctl.store(ipc::makeCtl(9, phase), std::memory_order_relaxed);
        s.pid.store(pid, std::memory_order_relaxed);
        s.heartbeat_ms.store(hb, std::memory_order_relaxed);
        s.block_seq.store(seq, std::memory_order_relaxed);
        s.safety_state.store(ipc::kSafetyRun, std::memory_order_relaxed);
        s.rt_flags.store(0x20, std::memory_order_relaxed);
    };
    set(block.input, ipc::kCtlPhaseActive, 11, 1234, 56);
    set(block.output, ipc::kCtlPhaseDormant, 22, 4321, 65);

    const GroupSnapshot g = oop::snapshotGroup(*rv, 3);
    CHECK(g.group == 3);
    CHECK(g.input.pid == 11);
    CHECK(g.input.heartbeatMs == 1234);
    CHECK(g.input.blockSeq == 56);
    CHECK(g.input.safetyState == ipc::kSafetyRun);
    CHECK(g.input.rtFlags == 0x20);
    CHECK(g.input.active());
    CHECK(g.output.pid == 22);
    CHECK_FALSE(g.output.active());
    CHECK_FALSE(g.bothActive());
    CHECK(oop::snapshotGroup(*rv, 0).group == 0);
    CHECK(oop::snapshotGroup(*rv, 9).group == 0);

    std::array<GroupSnapshot, ipc::kGroupCount> groups{};
    for (ipc::u32 i = 0; i < ipc::kGroupCount; ++i)
    {
        groups[i] = oop::snapshotGroup(*rv, i + 1);
    }
    CHECK(oop::findPairedGroup(groups) == 0); // Output 是休眠，不算配上

    set(block.output, ipc::kCtlPhaseActive, 22, 4321, 65);
    for (ipc::u32 i = 0; i < ipc::kGroupCount; ++i)
    {
        groups[i] = oop::snapshotGroup(*rv, i + 1);
    }
    CHECK(oop::findPairedGroup(groups) == 3);

    auto ring = std::make_unique<ipc::RingSegment>();
    ring->header.magic.store(ipc::kRingMagic, std::memory_order_relaxed);
    ring->header.write_head.store(777, std::memory_order_relaxed);
    GroupSnapshot with = groups[2];
    oop::attachRing(with, ring->header);
    CHECK(with.ringMapped);
    CHECK(with.ringMagic == ipc::kRingMagic);
    CHECK(with.writeHead == 777);
}

TEST_CASE("pairing verdict: a healthy pair passes", "[pair-judge]")
{
    const Pair p = healthyPair();
    const Verdict v = oop::judgePairing(p.early, p.late, p.check);
    INFO(joined(v));
    CHECK(v.ok());
    // 允许 Input 还在淡入
    Pair fading = healthyPair();
    fading.late.input.safetyState = ipc::kSafetyFadeIn;
    CHECK(oop::judgePairing(fading.early, fading.late, fading.check).ok());
    // 安全状态字的高位是跳闸原因，没有跳闸时为 0；这里带上原因位，低位仍是 Run 也不影响状态判断
    Pair withTripBits = healthyPair();
    withTripBits.late.input.safetyState = ipc::kSafetyRun | (ipc::kTripPanic << ipc::kTripShift);
    CHECK(oop::judgePairing(withTripBits.early, withTripBits.late, withTripBits.check).ok());
    CHECK(oop::describeGroup(p.late).find("组 3") == 0);
}

TEST_CASE("pairing verdict: each broken criterion turns it red with its own message", "[pair-judge]")
{
    const auto expectRed = [](const char* what, const std::string& message, auto&& mutate) {
        Pair p = healthyPair();
        mutate(p);
        const Verdict v = oop::judgePairing(p.early, p.late, p.check);
        INFO(what << " -> " << joined(v));
        CHECK_FALSE(v.ok());
        CHECK(mentions(v, message));
    };

    expectRed("Output 不再是 Active", "没有同时保持 Active",
              [](Pair& p) { p.late.output.ctl = ipc::makeCtl(7, ipc::kCtlPhaseDormant); });
    expectRed("Input 在窗口开头就不是 Active", "没有同时保持 Active",
              [](Pair& p) { p.early.input.ctl = ipc::makeCtl(7, ipc::kCtlPhaseClaiming); });
    expectRed("两次取样不是同一组", "不是同一个组", [](Pair& p) { p.late.group = 4; });
    expectRed("窗口太短", "观察窗口只有", [](Pair& p) { p.check.hostBlocks = 50; });
    expectRed("两端在不同进程", "pid 不同", [](Pair& p) {
        p.late.output.pid = kPluginPid + 1;
        p.early.output.pid = kPluginPid + 1;
    });
    expectRed("槽里没有 pid", "没有 pid", [](Pair& p) {
        p.late.input.pid = 0;
        p.early.input.pid = 0;
    });
    expectRed("插件其实在宿主进程里", "就是宿主自己", [](Pair& p) {
        p.late.input.pid = kHostPid;
        p.late.output.pid = kHostPid;
        p.early.input.pid = kHostPid;
        p.early.output.pid = kHostPid;
    });
    expectRed("Input 的 pid 在窗口里变了", "pid 在观察窗口里变了", [](Pair& p) { p.late.input.pid = kPluginPid + 7; });
    expectRed("Output 的槽被重新认领", "重新认领",
              [](Pair& p) { p.late.output.ctl = ipc::makeCtl(8, ipc::kCtlPhaseActive); });
    expectRed("Input 的心跳停了", "Input 的心跳",
              [](Pair& p) { p.late.input.heartbeatMs = p.early.input.heartbeatMs + 100; });
    expectRed("Output 的心跳停了", "Output 的心跳",
              [](Pair& p) { p.late.output.heartbeatMs = p.early.output.heartbeatMs; });
    expectRed("Input 的块计数不涨", "Input 的块计数",
              [](Pair& p) { p.late.input.blockSeq = p.early.input.blockSeq + 10; });
    expectRed("Output 的块计数不涨", "Output 的块计数",
              [](Pair& p) { p.late.output.blockSeq = p.early.output.blockSeq; });
    expectRed("Input 跳闸", "运行标志", [](Pair& p) { p.late.input.rtFlags = ipc::kRtTripped; });
    expectRed("Output 冲突", "运行标志", [](Pair& p) { p.late.output.rtFlags = ipc::kRtConflict; });
    expectRed("Input 安全状态是未配对", "安全状态", [](Pair& p) { p.late.input.safetyState = ipc::kSafetyUnpaired; });
    expectRed("Input 安全状态是停摆", "安全状态", [](Pair& p) { p.late.input.safetyState = ipc::kSafetyStalled; });
    expectRed("Input 安全状态是跳闸", "安全状态",
              [](Pair& p) { p.late.input.safetyState = ipc::kSafetyTripped | (ipc::kTripNan << ipc::kTripShift); });
    expectRed("数据段没映射", "数据段没有映射到", [](Pair& p) { p.late.ringMapped = false; });
    expectRed("数据段 magic 不对", "magic 不对", [](Pair& p) { p.late.ringMagic = 0; });
    expectRed("写头不前进", "写头", [](Pair& p) { p.late.writeHead = p.early.writeHead + 100; });
}

TEST_CASE("tone verdict: Goertzel finds each tone and the judge wants the peer's tone in the Input output",
          "[pair-judge]")
{
    constexpr std::size_t kSecond = 48000;
    const std::vector<float> dry = tone(oop::kInputToneHz, oop::kToneAmplitude, kSecond);
    const std::vector<float> ret = tone(oop::kOutputToneHz, 0.125, kSecond);
    std::vector<float> mixed(kSecond);
    for (std::size_t i = 0; i < kSecond; ++i)
    {
        mixed[i] = dry[i] + ret[i];
    }

    CHECK(oop::goertzelAmplitude(dry.data(), kSecond, oop::kInputToneHz, 48000.0) == Catch::Approx(0.25).margin(0.001));
    CHECK(oop::goertzelAmplitude(dry.data(), kSecond, oop::kOutputToneHz, 48000.0) < 0.001);
    CHECK(oop::goertzelAmplitude(mixed.data(), kSecond, oop::kInputToneHz, 48000.0) ==
          Catch::Approx(0.25).margin(0.001));
    CHECK(oop::goertzelAmplitude(mixed.data(), kSecond, oop::kOutputToneHz, 48000.0) ==
          Catch::Approx(0.125).margin(0.001));
    CHECK(oop::goertzelAmplitude(nullptr, 10, 440.0, 48000.0) == 0.0);
    CHECK(oop::goertzelAmplitude(dry.data(), 0, 440.0, 48000.0) == 0.0);

    const double mixedDry = oop::goertzelAmplitude(mixed.data(), kSecond, oop::kInputToneHz, 48000.0);
    const double mixedRet = oop::goertzelAmplitude(mixed.data(), kSecond, oop::kOutputToneHz, 48000.0);
    CHECK(oop::judgeTones(mixedDry, mixedRet).ok());

    // 只有干声：没有配对，回送没过来
    const double aloneDry = oop::goertzelAmplitude(dry.data(), kSecond, oop::kInputToneHz, 48000.0);
    const double aloneRet = oop::goertzelAmplitude(dry.data(), kSecond, oop::kOutputToneHz, 48000.0);
    const Verdict alone = oop::judgeTones(aloneDry, aloneRet);
    CHECK_FALSE(alone.ok());
    CHECK(mentions(alone, "880 Hz"));
    CHECK_FALSE(mentions(alone, "干声"));

    // 干声没了：直通坏了
    const Verdict silent = oop::judgeTones(0.0, 0.1);
    CHECK_FALSE(silent.ok());
    CHECK(mentions(silent, "干声"));

    // NaN 不能被当成「够大」放过
    CHECK_FALSE(oop::judgeTones(std::nan(""), std::nan("")).ok());
}

TEST_CASE("Output pass-through verdict and plugin image path detection", "[pair-judge]")
{
    CHECK(oop::judgePassthrough(0.25).ok());
    CHECK(oop::judgePassthrough(0.26).ok());
    CHECK_FALSE(oop::judgePassthrough(0.0).ok());
    CHECK_FALSE(oop::judgePassthrough(0.125).ok());
    CHECK_FALSE(oop::judgePassthrough(std::nan("")).ok());
    CHECK(mentions(oop::judgePassthrough(0.5), "直通"));

    CHECK(oop::isRinnePluginImagePath(
        "/Users/runner/Library/Audio/Plug-Ins/Components/Rinne Input.component/Contents/MacOS/Rinne Input"));
    CHECK_FALSE(oop::isRinnePluginImagePath("/Library/Audio/Plug-Ins/Components/Other.component/Contents/MacOS/Other"));
    CHECK_FALSE(oop::isRinnePluginImagePath("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox"));
    CHECK_FALSE(oop::isRinnePluginImagePath("/Users/runner/work/rinne/build-oop/rinne_auhost"));
    CHECK_FALSE(oop::isRinnePluginImagePath("/Users/runner/work/Rinne/build-oop/rinne_auhost"));
    CHECK_FALSE(oop::isRinnePluginImagePath(""));
}
