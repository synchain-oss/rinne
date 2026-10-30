// SPDX-License-Identifier: GPL-3.0-or-later
//
// S23、S24 的插件级复验。
//
// S23 停用复燃：两端一起被宿主挂起（不再调两端的回调）之后恢复。core
// 级（tests/loop_rig/scenarios/s23_suspend_clear.cpp）钉的是引擎一侧：请求清空之后清、不请求就不清；「挂起超过 250 ms
// 才请求」是壳的判断，在这里钉：宿主不调两端的回调，逻辑时钟照走（走的途中两端的定时器照常 tick），挂起前后各是一块
// 256 帧。壳量的是两次 Input 回调的间隔，判据是间隔 > 250 ms + 上一块的时长，
// 且这段时间里环的写头走过的样本数不到间隔的一半。
//   挂起 200 ms、250 ms  不清：旧音频照常回来（250 ms 恰在边界上，边界属于不清）；
//   挂起 260 ms、300 ms、2 s  清：回送先是挂起前最后一个回送样本的 5 ms 衰减保持，之后静音到 T + D，再是新音频。
// 环路：反馈 0、混合 50 %、delay_ms 100（D = 4800），块长恒为 256，或 32…512 的带种子随机（挂起前最后一块恒为 256，
// 保证边界上的间隔不受块长取整影响）。信号源是编号的纯函数噪声，挂起时刻 T（宿主已渲染的样本数，
// 也是恢复的第一个样本）之前一个种子、之后另一个种子：旧音频就是 T 之前的种子。读头的内容按 Input 的块逐块记下（与
// core 级同一个观测点）。断言：
//   - 清：壳请求清空恰 1 次、引擎回报因挂起清空恰 1 次；T 起 5 ms 内回送的幅度不超过挂起前最后一个回送样本；到 T + D
//     之前静音；T + D + 20 ms 起逐位对得回新音频（1e-6），T + D + 5 ms 起每个样本都不为 0；回送与旧音频在 T 起 3 s
//     里的归一化互相关小于 −40 dB；
//   - 不清：两个计数都是 0；T 到 T + D 之前回送逐位等于旧音频，T + D 起等于新音频；与旧音频在 [T, T + D)
//     里的互相关大于 −0.1 dB；
//   - 两种情况：挂起前后重同步次数不变、没有补 0；Output 逐位不碰宿主缓冲，两端都不写块尾之后。
//
// S24 单端切片的另一半是工程重载：宿主重开工程时 Input 被销毁、重建，用存下的 state 全量恢复，Output 一直留着。core
// 级（tests/loop_rig/scenarios/s24_single_end_slicing.cpp）钉的是只切一端的块；这里钉重载：
//   - state 里的参数（feedback、mix、delay_ms、ceiling）与组号还原；
//   - 新 Input 配上之后（配上的那一块起 J），回送在 [J, J + D) 里全是 0：它不读重载之前环里留着的内容（不变量 2）；
//     反馈 0、信号源是噪声（重载之前一个种子、之后另一个）时，J + D + 20 ms 起回送逐位等于 J 之后写进环的音频，
//     与重载之前音频的互相关小于 −40 dB；
//   - 反馈 50 %、信号源是两个声道各一个整周期（D 内的周期数是整数）的余弦时，回送的增益从首个有效读样本起 500 ms
//     不超过线性淡入乘稳态峰值（不变量 4），淡入走完之后回到稳态的 0.9 以上。
// 环路：delay_ms 200（D = 9600，两个余弦在 D 内各是整周期；参数不是默认值，才证得出是 state 还原的），块长恒为 256 或
// 64…512 的带种子随机。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 引擎收到清空请求后不清 → S23 挂起 260 ms、300 ms、2 s 三档变红：回送里仍有旧音频，静音段不静音；
//   ⛔ 壳的挂起阈值 250 ms 改小（150 ms）→ S23 挂起 200 ms、250 ms 两档变红：不该清的清了；
//   ⛔ 壳的挂起阈值 250 ms 改大（400 ms）→ S23 挂起 260 ms、300 ms 两档变红：该清的没清；
//   ⛔ 壳的判据「间隔 > 250 ms + 上一块时长」改成 >= → S23 挂起 250 ms 一档变红：边界属于不清；
//   ⛔ 重载时不还原 state → S24 的参数与组号检查变红，新 Input 配不上；
//   ⛔ 新配对的读头有效起点与本地延迟线锚点都提前一个 D（两层防护同时失守）→ S24 的 [J, J + D) 不再全零，
//      旧音频漏进回送。只去掉其中一层仍是绿的：两层互为冗余。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/DspTestKit.h"
#include "host/plugin_host/PluginHost.h"
#include "host/plugin_host/PluginScenarioKit.h"
#include "loop/LoopStatus.h"
#include "loop_rig/scenarios/s14_tap_trace.h"

namespace
{

using namespace rinne::plugin_host;
using rinne::wiring_test::formalOnly;
using rinne::wiring_test::needsSharedMemory;
using rinne::wiring_test::paramValue;

constexpr std::int64_t kSecond = 48000;
constexpr std::int64_t kHold = 240; // 5 ms 淡出
constexpr std::int64_t kOnset = 960; // 20 ms onset
constexpr std::int64_t kFadeIn = 24000; // 500 ms
constexpr std::int64_t kCycle = 256;

float hashNoise(std::uint64_t index, std::uint64_t key)
{
    std::uint64_t z = index * 0x9e3779b97f4a7c15ull + key * 0xbf58476d1ce4e5b9ull + 0x2545f4914f6cdd1dull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<float>(static_cast<std::int64_t>(z >> 40) - (1 << 23)) / static_cast<float>(1 << 23);
}

// 旧种子与新种子的噪声：同一个编号上两个声道、两个种子互不相关
float oldAt(std::int64_t index, int channel)
{
    return 0.25f * hashNoise(static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(61 + channel));
}

float newAt(std::int64_t index, int channel)
{
    return 0.25f * hashNoise(static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(71 + channel));
}

float sourceAt(std::int64_t index, std::int64_t turn, int channel)
{
    return index < turn ? oldAt(index, channel) : newAt(index, channel);
}

struct Blocks
{
    int low;
    int high;
    std::uint64_t seed;
};

class BlockSource
{
public:
    explicit BlockSource(Blocks blocks) : blocks_(blocks), rng_(blocks.seed) {}
    int next() { return blocks_.low + rng_.below(blocks_.high - blocks_.low + 1); }

private:
    Blocks blocks_;
    rinne::test::Rng rng_;
};

void runTo(PluginHost& host, std::int64_t target, BlockSource& blocks)
{
    while (host.trackPosition() < target)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(blocks.next(), target - host.trackPosition()));
        host.cycle(n);
    }
}

// 逐个 Input 块记下读头（回送，安全链的输入）、干声与 Input 的输出（左声道），按 Input 的样本号排
class Recorder
{
public:
    explicit Recorder(PluginHost& host) : host_(host)
    {
        host.onInputBlock = [this](const InputBlock& block) { record(block); };
    }

    ~Recorder() { host_.onInputBlock = nullptr; }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    std::vector<float> tap;
    std::vector<float> dry;
    std::vector<float> out;

private:
    void record(const InputBlock& block)
    {
        const rinne::loop::BlockResult& r = host_.inputEngine().lastBlockResult();
        const auto first = static_cast<std::size_t>(block.first);
        const auto n = static_cast<std::size_t>(block.n);
        for (auto* v : {&tap, &dry, &out})
        {
            v->resize(first + n, 0.0f);
        }
        const std::size_t length =
            r.returnLeft != nullptr ? static_cast<std::size_t>(std::min(r.returnLength, block.n)) : 0u;
        for (std::size_t i = 0; i < n; ++i)
        {
            tap[first + i] = i < length ? r.returnLeft[i] : 0.0f;
            dry[first + i] = block.dryLeft[i];
            out[first + i] = block.outLeft[i];
        }
    }

    PluginHost& host_;
};

bool allZero(const std::vector<float>& v, std::int64_t from, std::int64_t to)
{
    for (std::int64_t t = from; t < to; ++t)
    {
        if (v[static_cast<std::size_t>(t)] != 0.0f)
        {
            return false;
        }
    }
    return true;
}

// 归一化互相关（dB）：v 在 [from, to) 与 reference(t) 的
template<typename Reference>
double correlationDb(const std::vector<float>& v, std::int64_t from, std::int64_t to, const Reference& reference)
{
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (std::int64_t t = from; t < to; ++t)
    {
        const double a = v[static_cast<std::size_t>(t)];
        const double b = reference(t);
        xy += a * b;
        xx += a * a;
        yy += b * b;
    }
    if (xx <= 0.0 || yy <= 0.0)
    {
        return -300.0;
    }
    return 20.0 * std::log10(std::max(std::fabs(xy) / std::sqrt(xx * yy), 1.0e-15));
}

// ---- S23 ----

constexpr std::int64_t kS23Delay = 4800;
constexpr std::int64_t kSuspendAt = 3 * kSecond;

struct SuspendTrace
{
    std::vector<float> tap;
    std::uint32_t flushes = 0; // 壳请求清空
    std::uint32_t clears = 0; // 引擎回报因挂起清空
    std::uint32_t resyncs = 0; // 挂起前后新增的重同步
    std::uint64_t overrun = 0;
    std::int64_t delay = 0;
    std::size_t outputTouched = 0;
    std::size_t canaryHits = 0;
};

SuspendTrace runSuspend(std::uint64_t gapMs, bool randomBlocks)
{
    SuspendTrace trace;
    HostOptions options;
    options.inputBlock = 512;
    options.outputBlock = 512;
    PluginHost host(options);
    Recorder rec(host);
    setParamExact(host, "feedback", 0.0f);
    setParamExact(host, "mix", 50.0f);
    setParam(host, "delay_ms", 100.0f);
    host.source = [](std::int64_t index, float& left, float& right) {
        left = sourceAt(index, kSuspendAt, 0);
        right = sourceAt(index, kSuspendAt, 1);
    };
    host.prepare();
    REQUIRE(host.selectGroup(1));
    REQUIRE(host.pairUp(256));

    BlockSource blocks(randomBlocks ? Blocks{32, 512, 0x5a110023u} : Blocks{256, 256, 1});
    // 挂起前最后一块恒为 256
    runTo(host, kSuspendAt - kCycle, blocks);
    host.cycle(kCycle);
    REQUIRE(host.trackPosition() == kSuspendAt);
    const auto before = host.inputEngine().loopStatus();
    REQUIRE(before.delayEffective == kS23Delay);
    REQUIRE(host.input().instanceHost().counts().suspendFlushes == 0u);

    host.advance(gapMs * 1'000'000ull);
    runTo(host, kSuspendAt + 4 * kSecond, blocks);

    const auto after = host.inputEngine().loopStatus();
    const auto counts = host.input().instanceHost().counts();
    trace.tap = rec.tap;
    trace.flushes = counts.suspendFlushes;
    trace.clears = counts.suspendClears;
    trace.resyncs = after.resyncCount - before.resyncCount;
    trace.overrun = after.overrun - before.overrun;
    trace.delay = after.delayEffective;
    trace.outputTouched = host.outputTouched;
    trace.canaryHits = host.canaryHits;
    return trace;
}

void checkCommon(const SuspendTrace& t)
{
    CHECK(t.resyncs == 0u);
    CHECK(t.overrun == 0u);
    CHECK(t.delay == kS23Delay);
    CHECK(t.outputTouched == 0u);
    CHECK(t.canaryHits == 0u);
    REQUIRE(t.tap.size() >= static_cast<std::size_t>(kSuspendAt + 3 * kSecond));
}

void checkCleared(const SuspendTrace& t)
{
    const std::int64_t from = kSuspendAt;
    const auto total = static_cast<std::int64_t>(t.tap.size());
    CHECK(t.flushes == 1u);
    CHECK(t.clears == 1u);
    // 前 5 ms：挂起之前最后一个回送样本的衰减保持
    const float last = std::fabs(t.tap[static_cast<std::size_t>(from - 1)]);
    bool held = true;
    for (std::int64_t at = from; at < from + kHold; ++at)
    {
        held = held && std::fabs(t.tap[static_cast<std::size_t>(at)]) <= last * (1.0f + 1.0e-6f) + 1.0e-7f;
    }
    CHECK(held);
    CHECK(allZero(t.tap, from + kHold + 2, from + kS23Delay));
    bool exact = true;
    bool alive = true;
    bool bounded = true;
    for (std::int64_t at = from + kS23Delay; at < total; ++at)
    {
        const float want = newAt(at - kS23Delay, 0);
        const float got = t.tap[static_cast<std::size_t>(at)];
        if (at >= from + kS23Delay + kOnset)
        {
            exact = exact && std::fabs(got - want) <= 1.0e-6f;
        }
        if (at >= from + kS23Delay + kHold)
        {
            alive = alive && got != 0.0f;
        }
        if (got != 0.0f)
        {
            bounded = bounded && (got > 0.0f) == (want > 0.0f) && std::fabs(got) <= std::fabs(want) * (1.0f + 1.0e-6f);
        }
    }
    CHECK(exact);
    CHECK(alive);
    CHECK(bounded);
    CHECK(correlationDb(t.tap, from, std::min(total, from + 3 * kSecond),
                        [](std::int64_t at) { return oldAt(at - kS23Delay, 0); }) < -40.0);
}

void checkKept(const SuspendTrace& t)
{
    const std::int64_t from = kSuspendAt;
    const auto total = static_cast<std::int64_t>(t.tap.size());
    CHECK(t.flushes == 0u);
    CHECK(t.clears == 0u);
    bool old = true;
    for (std::int64_t at = from; at < from + kS23Delay; ++at)
    {
        old = old && std::fabs(t.tap[static_cast<std::size_t>(at)] - oldAt(at - kS23Delay, 0)) <= 1.0e-6f;
    }
    CHECK(old);
    bool fresh = true;
    for (std::int64_t at = from + kS23Delay; at < total; ++at)
    {
        fresh = fresh && std::fabs(t.tap[static_cast<std::size_t>(at)] - newAt(at - kS23Delay, 0)) <= 1.0e-6f;
    }
    CHECK(fresh);
    CHECK(correlationDb(t.tap, from, from + kS23Delay, [](std::int64_t at) { return oldAt(at - kS23Delay, 0); }) >
          -0.1);
}

// ---- S24 工程重载 ----

constexpr std::int64_t kReloadAt = 3 * kSecond;
constexpr float kReloadDelayMs = 200.0f; // D = 9600，220 Hz 与 330 Hz 在 D 内各是 44、66 个整周期

struct ReloadTrace
{
    std::vector<float> tap;
    std::vector<float> dry;
    std::vector<float> out;
    std::vector<float> paramsBefore;
    std::vector<float> paramsAfter;
    std::uint32_t groupAfter = 0;
    std::int64_t delay = 0;
    std::int64_t joinAt = -1; // 新 Input 配上的那一块的第一个样本
    std::size_t outputTouched = 0;
    std::size_t canaryHits = 0;
};

ReloadTrace runReload(float feedback, bool tones, bool randomBlocks)
{
    ReloadTrace trace;
    HostOptions options;
    options.inputBlock = 512;
    options.outputBlock = 512;
    PluginHost host(options);
    Recorder rec(host);
    setParamExact(host, "feedback", feedback);
    setParamExact(host, "mix", 50.0f);
    setParam(host, "delay_ms", kReloadDelayMs);
    setParam(host, "ceiling", -3.0f);
    const char* const ids[] = {"feedback", "mix", "delay_ms", "ceiling"};
    for (const char* id : ids)
    {
        trace.paramsBefore.push_back(paramValue(host.input(), id));
    }
    if (tones)
    {
        useTones(host);
    }
    else
    {
        host.source = [](std::int64_t index, float& left, float& right) {
            left = sourceAt(index, kReloadAt, 0);
            right = sourceAt(index, kReloadAt, 1);
        };
    }
    host.prepare();
    REQUIRE(host.selectGroup(1));
    REQUIRE(host.pairUp(256));
    trace.delay = host.inputEngine().loopStatus().delayEffective;

    BlockSource blocks(randomBlocks ? Blocks{64, 512, 0x24a11ull} : Blocks{256, 256, 1});
    runTo(host, kReloadAt, blocks);
    REQUIRE(host.trackPosition() == kReloadAt);

    host.reloadInput();
    for (const char* id : ids)
    {
        trace.paramsAfter.push_back(paramValue(host.input(), id));
    }
    trace.groupAfter = host.input().instanceHost().group();

    for (int i = 0; i < 2000 && trace.joinAt < 0; ++i)
    {
        const std::int64_t at = host.trackPosition();
        host.cycle(blocks.next());
        if (host.paired())
        {
            trace.joinAt = at;
        }
    }
    REQUIRE(trace.joinAt >= 0);
    runTo(host, trace.joinAt + trace.delay + kFadeIn + 3 * kSecond, blocks);
    trace.tap = rec.tap;
    trace.dry = rec.dry;
    trace.out = rec.out;
    trace.outputTouched = host.outputTouched;
    trace.canaryHits = host.canaryHits;
    return trace;
}

void checkReloadCommon(const ReloadTrace& t)
{
    REQUIRE(t.paramsBefore.size() == t.paramsAfter.size());
    for (std::size_t i = 0; i < t.paramsBefore.size(); ++i)
    {
        INFO("parameter " << i);
        CHECK(t.paramsAfter[i] == t.paramsBefore[i]);
    }
    CHECK(t.groupAfter == 1u);
    CHECK(t.delay == 9600);
    CHECK(t.outputTouched == 0u);
    CHECK(t.canaryHits == 0u);
}

} // namespace

TEST_CASE("plugin_host s23: the shell requests a flush only when both ends were suspended for over 250 ms",
          "[plugin_host][s23]")
{
    formalOnly();
    needsSharedMemory();
    struct Gap
    {
        std::uint64_t ms;
        bool clears;
    };
    for (const bool randomBlocks : {false, true})
    {
        for (const Gap gap : {Gap{200, false}, Gap{250, false}, Gap{260, true}, Gap{300, true}, Gap{2000, true}})
        {
            INFO("suspended " << gap.ms << " ms, " << (randomBlocks ? "random blocks" : "blocks of 256"));
            const SuspendTrace t = runSuspend(gap.ms, randomBlocks);
            checkCommon(t);
            if (gap.clears)
            {
                checkCleared(t);
            }
            else
            {
                checkKept(t);
            }
        }
    }
}

TEST_CASE(
    "plugin_host s24: after a project reload the new Input restores its state and never reads the old ring content",
    "[plugin_host][s24]")
{
    formalOnly();
    needsSharedMemory();
    for (const bool randomBlocks : {false, true})
    {
        INFO((randomBlocks ? "random blocks" : "blocks of 256"));
        {
            INFO("feedback 0, noise");
            const ReloadTrace t = runReload(0.0f, false, randomBlocks);
            checkReloadCommon(t);
            const std::int64_t j = t.joinAt;
            const std::int64_t d = t.delay;
            // 分辨力：重载之前环里确有旧音频，回送逐位对得回
            bool primed = true;
            for (std::int64_t at = kReloadAt - kSecond; at < kReloadAt; ++at)
            {
                primed = primed && std::fabs(t.tap[static_cast<std::size_t>(at)] - oldAt(at - d, 0)) <= 1.0e-6f;
            }
            CHECK(primed);
            CHECK(allZero(t.tap, j, j + d));
            bool exact = true;
            bool alive = true;
            for (std::int64_t at = j + d + kHold; at < static_cast<std::int64_t>(t.tap.size()); ++at)
            {
                const float got = t.tap[static_cast<std::size_t>(at)];
                alive = alive && got != 0.0f;
                if (at >= j + d + kOnset)
                {
                    exact = exact && std::fabs(got - newAt(at - d, 0)) <= 1.0e-6f;
                }
            }
            CHECK(exact);
            CHECK(alive);
            CHECK(correlationDb(t.tap, j, j + 3 * kSecond, [d](std::int64_t at) { return oldAt(at - d, 0); }) < -40.0);
        }
        {
            INFO("feedback 50 %, tones");
            const ReloadTrace t = runReload(50.0f, true, randomBlocks);
            checkReloadCommon(t);
            const std::int64_t j = t.joinAt;
            const std::int64_t d = t.delay;
            const std::int64_t f = j + d; // 首个有效读样本
            auto injection = [&t](std::int64_t at) {
                return std::fabs(t.out[static_cast<std::size_t>(at)] - t.dry[static_cast<std::size_t>(at)]);
            };
            float steady = 0.0f;
            for (std::int64_t at = kReloadAt - kSecond / 2; at < kReloadAt; ++at)
            {
                steady = std::max(steady, injection(at));
            }
            CHECK(steady > 0.05f);
            float silent = 0.0f;
            for (std::int64_t at = j; at < f; ++at)
            {
                silent = std::max(silent, injection(at));
            }
            CHECK(silent <= 1.0e-6f);
            bool shaped = true;
            for (std::int64_t at = f; at < f + kFadeIn; ++at)
            {
                const float ramp = static_cast<float>(at - f) / static_cast<float>(kFadeIn);
                shaped = shaped && injection(at) <= ramp * steady * 1.05f + 2.0e-3f;
            }
            CHECK(shaped);
            float settled = 0.0f;
            for (std::int64_t at = f + kFadeIn + 2 * kSecond; at < f + kFadeIn + 2 * kSecond + kSecond / 4; ++at)
            {
                settled = std::max(settled, injection(at));
            }
            CHECK(settled >= 0.9f * steady);
        }
    }
}
