// SPDX-License-Identifier: GPL-3.0-or-later
//
// S13 安全矩阵的格子、激励信号与判定：core 级（rinne_loop_rig）与插件级（rinne_host_tests 的 plugin_host）include
// 同一份， 两层的预期与断言因此出自同一张表。只用标准库，不依赖哪一层的试验台。
//
// 一格 = 环内链增益 × 反馈 × 信号（× 限制器开关）。共同的设置：48 kHz、块长 512、环路延迟 D = 50 ms、混合 50 %
// （g_dry = g_ret = 1）、失控保护开；激励在首次配对的淡入走完之后（kExciteAt）开始，之前静音。
// 上限取 −5 dBFS，不用默认的 −6：默认值下 +18 dB 链 × 上限恰好是 +12 dBFS（10^(18/20)·10^(−6/20) = 10^(12/20)），
// 自激被限制器压在上限时回读正好落在超大值判据的线上，冲激一列的结果由一次舍入决定（链增益偏 +0.02 dB 就从
// Quiet / Guard 变成 Huge），写不成确定的预期。−5 dBFS 让每一列离这条线至少 1 dB。这是超大值判据的边界，不是缺陷，
// 测试有意避开它；默认上限下的行为由表外几格（extraCells 里上限 −6 dBFS 的那几格，避开 +18 dB 一行）钉住。
//
// 每格的预期是四种之一，断言精确到状态：
//   Quiet    零介入：从不进 GUARD，从不跳闸；
//   Guard    进 GUARD 并真的压低了回送，到期末不跳闸；
//   Huge     TRIPPED(huge)；
//   Runaway  先进 GUARD，压到 −24 dB 之后仍在失控，TRIPPED(runaway)。
// 预期按 core 安全链的口径（防误报门、失控保护与超大值判据，见 SafetyMachine.h）写，再由实测核对：rinne_loop_rig 里的
// 隐藏用例 [.s13-survey] 把每格跑到激励后 12 s，并把链增益上下偏 0.02 / 0.1 / 0.3 dB 再跑；表里每一格在 ±0.1 dB 之内
// 结果不变，且与口径讲得通。
//
// 信号都是样本号（从激励起算）的纯函数，确定、可复现；鼓 loop 是合成的替身，不进二进制素材。
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rinne::s13
{

inline constexpr double kSampleRate = 48000.0;
inline constexpr int kBlock = 512;
inline constexpr float kDelayMs = 50.0f;
inline constexpr float kMixPercent = 50.0f;
inline constexpr float kCeilingDb = -5.0f;
// 引擎的默认上限（core 级用例 static_assert 它与 SafetyMachine.h 的默认值相同）
inline constexpr float kDefaultCeilingDb = -6.0f;
// 激励起点：首次配对的 500 ms 淡入走完之后（0.6 s）
inline constexpr std::int64_t kExciteAt = 28800;
// 粉噪表的长度：最长的一格跑到激励后 14 s
inline constexpr std::int64_t kPinkSamples = 14 * 48000;

enum class Signal : std::uint8_t
{
    kImpulse = 0, // 0 dBFS 单个冲激
    kPink = 1, // 粉噪，RMS −18 dBFS
    kDrums = 2, // 120 BPM 的合成鼓 loop，峰值约 −1 dBFS
    kSine6 = 3, // +6 dBFS 的 1 kHz 正弦（「fb 30 %、干声 +6 dBFS」一格）
    kSine12 = 4, // 峰值 +12 dBFS（留 0.01 dB）的 1 kHz 正弦（「fb 90 %、链 0 dB、干声到 +12 dBFS」一格）
    kDrumsLow = 5, // 鼓 loop 压低 12 dB（峰值约 −13 dBFS）：音箱模拟一格经 +18 dB 链后链输出在 −6…+6 dBFS
};

enum class Outcome : std::uint8_t
{
    kQuiet = 0,
    kGuard = 1,
    kHuge = 2,
    kRunaway = 3,
};

inline const char* signalName(Signal s)
{
    switch (s)
    {
    case Signal::kImpulse:
        return "impulse";
    case Signal::kPink:
        return "pink";
    case Signal::kDrums:
        return "drums";
    case Signal::kSine6:
        return "sine+6";
    case Signal::kSine12:
        return "sine+12";
    case Signal::kDrumsLow:
        return "drums-12";
    }
    return "?";
}

inline const char* outcomeName(Outcome o)
{
    switch (o)
    {
    case Outcome::kQuiet:
        return "Quiet";
    case Outcome::kGuard:
        return "Guard";
    case Outcome::kHuge:
        return "Huge";
    case Outcome::kRunaway:
        return "Runaway";
    }
    return "?";
}

struct Cell
{
    float chainDb = 0.0f;
    float feedbackPercent = 0.0f;
    Signal signal = Signal::kImpulse;
    bool limiterOn = true;
    Outcome expect = Outcome::kQuiet;
    float ceilingDb = kCeilingDb;
};

// dB → 线性：与引擎换算上限的写法相同（double 算、再取 float），上限比较因此逐位对得上
inline float dbToGain(double db)
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

// ---- 信号 ----

inline constexpr double kPi = 3.14159265358979323846;

// 确定性的哈希噪声，值在 [-1, 1)：编号和种子的纯函数
inline float hashNoise(std::uint64_t index, std::uint64_t seed)
{
    std::uint64_t z = index * 0x9e3779b97f4a7c15ull + seed * 0xbf58476d1ce4e5b9ull + 0x94d049bb133111ebull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<float>(static_cast<std::int64_t>(z >> 40) - (1 << 23)) / static_cast<float>(1 << 23);
}

// 粉噪：哈希白噪过三极点粉化滤波（经典的 Kellet 简化式），double 递推、一次算好成表，之后按样本号查表。
// 0.0732 把 RMS 定在 −18 dBFS（峰值约 −5.4 dBFS），由隐藏用例实测
inline const std::vector<float>& pinkTable()
{
    static const std::vector<float> table = [] {
        std::vector<float> t(static_cast<std::size_t>(kPinkSamples));
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        for (std::size_t i = 0; i < t.size(); ++i)
        {
            const double w = static_cast<double>(hashNoise(i, 0x5131u));
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            t[i] = static_cast<float>(0.0732 * (b0 + b1 + b2 + w * 0.1848));
        }
        return t;
    }();
    return table;
}

// 120 BPM 的鼓 loop 替身：一拍 24000 个样本。底鼓在第 1、3 拍（150 → 50 Hz 的下滑正弦，120 ms 衰减），
// 军鼓在第 2、4 拍（噪声加 190 Hz，50 ms 衰减），踩镲每个八分音符（差分噪声，12 ms 衰减）
inline float drums(std::int64_t n)
{
    constexpr std::int64_t beat = 24000;
    constexpr std::int64_t eighth = beat / 2;
    const std::int64_t inBeat = n % beat;
    const bool backbeat = (n / beat) % 2 == 1;
    const double tb = static_cast<double>(inBeat) / kSampleRate;
    double v = 0.0;
    if (!backbeat && inBeat < 9600)
    {
        const double phase = 2.0 * kPi * (50.0 * tb + 100.0 * 0.03 * (1.0 - std::exp(-tb / 0.03)));
        v += 0.8 * std::exp(-tb / 0.12) * std::sin(phase);
    }
    if (backbeat && inBeat < 7200)
    {
        const double noise = static_cast<double>(hashNoise(static_cast<std::uint64_t>(n), 0x5d1u));
        v += 0.45 * std::exp(-tb / 0.05) * (0.6 * noise + 0.4 * std::sin(2.0 * kPi * 190.0 * tb));
    }
    const std::int64_t inEighth = n % eighth;
    if (inEighth < 2400)
    {
        const double te = static_cast<double>(inEighth) / kSampleRate;
        const auto k = static_cast<std::uint64_t>(n);
        const double hiss = 0.5 * static_cast<double>(hashNoise(k, 0x4a7u) - hashNoise(k + 1, 0x4a7u));
        v += 0.15 * std::exp(-te / 0.012) * hiss;
    }
    return static_cast<float>(v);
}

// 第 n 个样本（从激励起算；n < 0 是激励之前，静音）。两个声道相同
inline float signalAt(Signal s, std::int64_t n)
{
    if (n < 0)
        return 0.0f;
    switch (s)
    {
    case Signal::kImpulse:
        return n == 0 ? 1.0f : 0.0f;
    case Signal::kPink: {
        const auto& t = pinkTable();
        return n < static_cast<std::int64_t>(t.size()) ? t[static_cast<std::size_t>(n)] : 0.0f;
    }
    case Signal::kDrums:
        return drums(n);
    case Signal::kSine6:
        return static_cast<float>(2.0 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kSampleRate));
    case Signal::kSine12:
        return static_cast<float>(3.98107170553497 * 0.999 *
                                  std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kSampleRate));
    case Signal::kDrumsLow:
        return static_cast<float>(0.25 * static_cast<double>(drums(n)));
    }
    return 0.0f;
}

// ---- 格子 ----

inline constexpr std::array<float, 5> kChainDb = {0.0f, 6.0f, 12.0f, 18.0f, 24.0f};
inline constexpr std::array<float, 4> kFeedback = {20.0f, 50.0f, 100.0f, 150.0f};
inline constexpr std::array<Signal, 3> kSignals = {Signal::kImpulse, Signal::kPink, Signal::kDrums};

// 全表的预期：[信号][链增益][反馈]。Q = Quiet，G = Guard，H = Huge。读法（口径见 SafetyMachine.h）：
//   - 链 0 dB：防误报门关着（链不放大），反馈 100 %、150 % 也只靠限制器压住，零介入；
//   - 环路增益 = 链增益 × 反馈。自激被限制器压在上限时 GR ≈ 环路增益，GR ≥ 6 dB 累计 2 s 才进 GUARD。
//     +6 dB × 150 %（9.5 dB）、+12 dB × 100 %（12 dB）三种信号都进 GUARD；+6 dB × 100 %、+12 dB × 50 % 恰在 6 dB 上，
//     只有持续不断的粉噪把 GR 推过 6 dB，冲激与鼓 loop 留在门下（零介入，自激停在上限）；
//   - 链增益 × 上限 ≥ +12 dBFS 的格子回读本身就到了超大值的线：+18、+24 dB 两列全部 TRIPPED(huge)，早于 GUARD 的 2 s；
//     +12 dB × 150 % 是 ×g（1.5）之后的 |x| 过线，也是超大值；
//   - 表内没有一格走到 TRIPPED(runaway)：上限 −5 dBFS 下，能让 GUARD 压到 −24 dB 仍 GR ≥ 6 dB 的链，回读早已过了
//     超大值的线。失控跳闸由表外的对照格覆盖（extraCells 最后一格）。
inline constexpr Outcome Q = Outcome::kQuiet;
inline constexpr Outcome G = Outcome::kGuard;
inline constexpr Outcome H = Outcome::kHuge;
inline constexpr Outcome kExpect[3][5][4] = {
    // 冲激：链 0 / +6 / +12 / +18 / +24 dB × 反馈 20 / 50 / 100 / 150 %
    {{Q, Q, Q, Q}, {Q, Q, Q, G}, {Q, Q, G, H}, {H, H, H, H}, {H, H, H, H}},
    // 粉噪
    {{Q, Q, Q, Q}, {Q, Q, G, G}, {Q, G, G, H}, {H, H, H, H}, {H, H, H, H}},
    // 鼓 loop
    {{Q, Q, Q, Q}, {Q, Q, Q, G}, {Q, Q, G, H}, {H, H, H, H}, {H, H, H, H}},
};

// 表外的格子：原文另列的不误报三格、限制器关的联锁、默认上限的几格、失控跳闸的对照
inline const std::vector<Cell>& extraCells()
{
    static const std::vector<Cell> cells = {
        // 反馈 ≤ 90 %、链 0 dB、干声到 +12 dBFS：防误报门关着，限制器压得再狠也零介入
        Cell{0.0f, 90.0f, Signal::kSine12, true, Outcome::kQuiet},
        // 反馈 30 %、干声 +6 dBFS
        Cell{0.0f, 30.0f, Signal::kSine6, true, Outcome::kQuiet},
        // 音箱模拟式的链：+18 dB、链输出 −6…+6 dBFS、反馈 10 %（环路增益 < 1，限制器不动）
        Cell{18.0f, 10.0f, Signal::kDrumsLow, true, Outcome::kQuiet},
        // 限制器关 × 反馈 150 % × 链 0 dB：有效反馈按 100 % 封顶，软削波把回送压在 1.0 以下，零介入
        Cell{0.0f, 150.0f, Signal::kImpulse, false, Outcome::kQuiet},
        Cell{0.0f, 150.0f, Signal::kPink, false, Outcome::kQuiet},
        Cell{0.0f, 150.0f, Signal::kDrums, false, Outcome::kQuiet},
        // 默认上限 −6 dBFS：每格在链增益 ±0.3 dB 之内结果不变（[.s13-survey]）。不取 +18 dB 一行（见文件头）
        Cell{0.0f, 100.0f, Signal::kPink, true, Outcome::kQuiet, kDefaultCeilingDb},
        Cell{6.0f, 150.0f, Signal::kPink, true, Outcome::kGuard, kDefaultCeilingDb},
        Cell{12.0f, 50.0f, Signal::kPink, true, Outcome::kGuard, kDefaultCeilingDb},
        // 超阈样本稀疏（粉噪的峰），凑满 100 ms 的那个样本离串首约 132 ms：跳闸时刻要按样本号精确预测（见 CellJudge）
        Cell{12.0f, 150.0f, Signal::kPink, true, Outcome::kHuge, kDefaultCeilingDb},
        Cell{24.0f, 20.0f, Signal::kImpulse, true, Outcome::kHuge, kDefaultCeilingDb},
        // 失控跳闸的对照（与 core 安全链单测「压到 −24 dB 仍在失控」同一类）：上限 −24 dBFS，链增益比上限高出的部分
        // 让 GUARD 压到底之后 GR 仍 ≥ 6 dB，回读又到不了超大值的线
        Cell{32.0f, 100.0f, Signal::kImpulse, true, Outcome::kRunaway, -24.0f},
    };
    return cells;
}

inline std::vector<Cell> allCells()
{
    std::vector<Cell> cells;
    for (std::size_t s = 0; s < kSignals.size(); ++s)
        for (std::size_t c = 0; c < kChainDb.size(); ++c)
            for (std::size_t f = 0; f < kFeedback.size(); ++f)
                cells.push_back(Cell{kChainDb[c], kFeedback[f], kSignals[s], true, kExpect[s][c][f]});
    const auto& extra = extraCells();
    cells.insert(cells.end(), extra.begin(), extra.end());
    return cells;
}

// ---- 判定（两层共用）----

inline constexpr std::int64_t kHugeRunSamples = 4800; // 100 ms
inline constexpr std::int64_t kHugeGapSamples = 2400; // 50 ms：相隔不超过它仍算同一串
inline constexpr std::int64_t kPlus24Deadline = 2 * 48000; // +24 dB 一列从激励起 2 s 之内跳闸
inline constexpr std::int64_t kRunawayMinSamples = 5 * 48000; // 进 GUARD 到失控跳闸至少 5 s（降满 24 dB 4 s，再 1 s）

// 安全事件码与跳闸原因（SafetyEvents.h 里的数值；判定只认数值，不依赖哪一层的头文件）
inline constexpr std::uint16_t kEventTripped = 0x0201;
inline constexpr std::uint16_t kEventGuardEngaged = 0x0205;
inline constexpr std::uint8_t kTripHuge = 3;
inline constexpr std::uint8_t kTripRunaway = 4;

struct CellResult
{
    Outcome got = Outcome::kQuiet;
    bool unexpectedTrip = false; // 跳闸原因不是超大值也不是失控
    std::int64_t guardAt = -1; // 第一次进 GUARD 的样本号
    std::int64_t tripAt = -1;
    std::uint8_t tripReason = 0;
    std::int64_t hotStart = -1; // 引发跳闸的那串超阈样本的首个
    std::int64_t hugeDue = -1; // 按样本号重建：第一个让所在那一串凑满 100 ms 的超阈样本，超大值必须恰在这里跳
    std::int64_t firstHot = -1; // 激励以来第一个超阈样本
    float maxRet = 0.0f;
    bool retBounded = true;
    bool outputFinite = true;
    double guardFloorDb = 0.0; // 失控保护压到过的最低值（dB，≤ 0）
    std::int64_t samples = 0; // 跑到的样本号
};

// 逐块喂：先给这一块的安全事件，再逐样本给回送与「超阈」位，块末给失控保护当前的压低量
class CellJudge
{
public:
    CellJudge(const Cell& cell, std::int64_t exciteAt)
        : cell_(cell), exciteAt_(exciteAt), ceiling_(dbToGain(static_cast<double>(cell.ceilingDb)))
    {
    }

    void event(std::uint16_t code, std::uint8_t a, std::int64_t sample)
    {
        if (code == kEventGuardEngaged && r_.guardAt < 0)
            r_.guardAt = sample;
        if (code == kEventTripped && r_.tripAt < 0)
        {
            r_.tripAt = sample;
            r_.tripReason = a;
        }
    }

    // ret 是回送；retSlack 是它的测量误差上界（逐位得到时为 0）。限制器开时要求 |ret| ≤ 上限，关时 |ret| < 1.0；
    // 误差只往宽松的一侧算（测得的值减去误差仍超界才算越界）
    void sample(std::int64_t at, float retLeft, float retRight, float retSlack, bool hot, bool outputFinite)
    {
        if (at >= exciteAt_ && hot)
        {
            if (runStart_ < 0 || at - runLast_ > kHugeGapSamples)
                runStart_ = at;
            runLast_ = at;
            if (r_.firstHot < 0)
                r_.firstHot = at;
            // 引擎在凑满的那个超阈样本上跳。超阈样本稀疏时（相隔可到 50 ms）它可以比串首晚 100–150 ms，
            // 所以按样本号精确预测，不给一个宽松的窗口
            if (r_.hugeDue < 0 && at - runStart_ + 1 >= kHugeRunSamples)
                r_.hugeDue = at;
        }
        if (at == r_.tripAt && runStart_ >= 0 && at - runLast_ <= kHugeGapSamples)
            r_.hotStart = runStart_;
        const float ret = std::fabs(retLeft) > std::fabs(retRight) ? std::fabs(retLeft) : std::fabs(retRight);
        r_.maxRet = ret > r_.maxRet ? ret : r_.maxRet;
        const float measured = ret - retSlack;
        r_.retBounded = r_.retBounded && (cell_.limiterOn ? measured <= ceiling_ : measured < 1.0f);
        r_.outputFinite = r_.outputFinite && outputFinite;
        r_.samples = at + 1;
    }

    void endBlock(double guardDb) { r_.guardFloorDb = guardDb < r_.guardFloorDb ? guardDb : r_.guardFloorDb; }

    // 这一格可以停了：跑满了，或者跳闸之后又跑够了
    bool done() const
    {
        const std::int64_t horizon = horizonFor(cell_);
        if (r_.samples >= exciteAt_ + horizon)
            return true;
        return r_.tripAt >= 0 && r_.samples >= r_.tripAt + (cell_.expect == Outcome::kHuge ? 2400 : 1);
    }

    CellResult finish() const
    {
        CellResult out = r_;
        if (out.tripAt >= 0)
        {
            if (out.tripReason == kTripHuge)
                out.got = Outcome::kHuge;
            else if (out.tripReason == kTripRunaway)
                out.got = Outcome::kRunaway;
            else
                out.unexpectedTrip = true;
        }
        else if (out.guardAt >= 0)
        {
            out.got = Outcome::kGuard;
        }
        return out;
    }

    // 一格跑多久（激励之后的样本数）：Quiet 6 s；Guard 12 s（看它到期末也不跳闸）；Huge 最多 4 s（跳闸后再跑 50 ms）；
    // Runaway 最多 14 s（跳闸即停）
    static std::int64_t horizonFor(const Cell& cell)
    {
        switch (cell.expect)
        {
        case Outcome::kQuiet:
            return 6 * 48000;
        case Outcome::kGuard:
            return 12 * 48000;
        case Outcome::kHuge:
            return 4 * 48000;
        case Outcome::kRunaway:
            return 14 * 48000;
        }
        return 6 * 48000;
    }

private:
    Cell cell_;
    std::int64_t exciteAt_;
    float ceiling_;
    CellResult r_;
    std::int64_t runStart_ = -1;
    std::int64_t runLast_ = -1;
};

struct Check
{
    const char* what;
    bool pass;
};

// 一格的全部断言（两层同一份）
inline std::vector<Check> verdict(const Cell& cell, const CellResult& r, std::int64_t exciteAt)
{
    std::vector<Check> checks;
    checks.push_back({"outcome is exactly the one in the table", r.got == cell.expect && !r.unexpectedTrip});
    checks.push_back({cell.limiterOn ? "|ret| <= ceiling all the time" : "|ret| < 1.0 all the time", r.retBounded});
    checks.push_back({"input output stays finite", r.outputFinite});
    switch (cell.expect)
    {
    case Outcome::kQuiet:
        checks.push_back({"zero intervention: never GUARD, never tripped", r.guardAt < 0 && r.tripAt < 0});
        break;
    case Outcome::kGuard:
        checks.push_back({"GUARD engaged and actually lowered the return", r.guardAt >= 0 && r.guardFloorDb < 0.0});
        checks.push_back({"no trip to the end of the run", r.tripAt < 0});
        break;
    case Outcome::kHuge: {
        const std::int64_t span = r.tripAt >= 0 && r.hotStart >= 0 ? r.tripAt - r.hotStart + 1 : -1;
        checks.push_back({"TRIPPED(huge) on exactly the hot sample that fills 100 ms of its run (gaps <= 50 ms)",
                          r.tripAt >= 0 && r.tripAt == r.hugeDue});
        checks.push_back({"the tripping run spans 100 ms .. 100 ms + one gap",
                          span >= kHugeRunSamples && span <= kHugeRunSamples + kHugeGapSamples});
        break;
    }
    case Outcome::kRunaway:
        checks.push_back({"GUARD engaged before the runaway trip", r.guardAt >= 0 && r.tripAt > r.guardAt});
        checks.push_back({"runaway trips only after GUARD bottomed out at -24 dB (>= 5 s after engaging)",
                          r.tripAt - r.guardAt >= kRunawayMinSamples && r.guardFloorDb <= -24.0});
        break;
    }
    if (cell.chainDb >= 24.0f && cell.expect == Outcome::kHuge)
        checks.push_back({"+24 dB column trips within 2 s of the excitation",
                          r.tripAt >= 0 && r.tripAt - exciteAt <= kPlus24Deadline});
    return checks;
}

} // namespace rinne::s13
