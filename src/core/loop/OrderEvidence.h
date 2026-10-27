// SPDX-License-Identifier: GPL-3.0-or-later
//
// 错序证据：两端各自用宿主时间戳判「Output 是不是在 Input 前面被处理」（口径见 ADR-0009）。
//
// 原理：同一个宿主周期里两端拿到的是同一个时间戳，每端在块首读对端槽里发布的时间戳与块序号：
//   对端的时间戳不小于本块的 → 对端本周期已经处理过（或更晚的块）；
//   对端的更小               → 对端还停在上一周期，本周期里是本端先被处理。
// Input 看 Output 的槽：Output 的不小于本块的 = Output 在前（错序）。
// Output 看 Input 的槽：Input 的更小 = Output 在前（错序）。两个方向对同一个现场给出同一个结论。
//
// 判据：
//   - 每块判一次。连续 3 次「Output 在前」定为 kOutputFirst；连续 3 次「Input 在前」定为 kCorrect（清除错序）。
//   - 判不了的块清零连续计数，但不改已有的结论：宿主没给时间戳、对端没发布过时间戳、对端自上次检查起没有处理
//     新块（槽里还是旧值，不是证据）、本端或对端的时间戳没有前进（回退或原地不动）、本端的时间戳比按样本推进的
//     预期多跳了很多。
//   - 时间戳回退与跳变是真宿主的行为：Cubase 提前处理开关切换时，时间戳会回退数秒再跳回来，两端取到同一串值。
//     这种块不投票、不重置结论，只计数（glitches），诊断看得见。
//   - 1 s（按样本计，不读墙钟）没有有效检查，结论回到 unknown。
//
// 线程：只在音频线程上用，没有原子量；发布给消息线程的是 OrderSnapshot 打成的一个 64 位整数（见 pack），
// 由调用方存进一个 lock-free 原子量，结论、票数、有效检查数因此出自同一块。
// 0 样本的调用 observe 直接返回，不推进任何计数（调用方一般在更早处就返回了）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rinne::loop
{

// 结论。数值是状态快照、桥与诊断的一部分，只能追加。
enum class OrderVerdict : std::uint8_t
{
    kUnknown = 0, // 无法判定（宿主没给时间戳、没有对端证据，或有效检查中断超过 1 s）
    kCorrect = 1, // 顺序正确（连续 3 次判 Input 在前）
    kOutputFirst = 2, // Output 在 Input 之前（琥珀提示）
};

// 当前这串连续票的方向。
enum class OrderDirection : std::uint8_t
{
    kNone = 0, // 没有连续票（刚清零，或上一块判不了）
    kOutputFirst = 1,
    kInputFirst = 2,
};

// 本端是哪一端：决定「对端的时间戳不小于本块的」到底意味着谁在前。
enum class OrderSide : std::uint8_t
{
    kInput,
    kOutput,
};

// 状态快照：纯值。
struct OrderSnapshot
{
    OrderVerdict verdict = OrderVerdict::kUnknown;
    OrderDirection direction = OrderDirection::kNone;
    std::uint32_t votes = 0; // 当前这串连续票的个数（封顶在定案所需的票数）
    std::uint32_t checks = 0; // 累计的有效检查数（投过票的块）
    std::uint32_t glitches = 0; // 累计因时间戳没有前进或跳变而没投票的块数
    bool hostNsValid = false; // 本端上一块的宿主时间戳是否有效

    // 结论、方向、票数、有效位、有效检查数打成一个 64 位整数，存进一个原子量就是一份自洽的快照。
    // glitches 另存（它只是诊断计数，不要求与其余字段出自同一块）。
    constexpr std::uint64_t pack() const noexcept
    {
        return static_cast<std::uint64_t>(verdict) | (static_cast<std::uint64_t>(direction) << 8) |
               (static_cast<std::uint64_t>(votes & 0xFFu) << 16) |
               (static_cast<std::uint64_t>(hostNsValid ? 1u : 0u) << 24) | (static_cast<std::uint64_t>(checks) << 32);
    }

    static constexpr OrderSnapshot unpack(std::uint64_t word, std::uint32_t glitches) noexcept
    {
        OrderSnapshot s;
        s.verdict = static_cast<OrderVerdict>(word & 0xFFu);
        s.direction = static_cast<OrderDirection>((word >> 8) & 0xFFu);
        s.votes = static_cast<std::uint32_t>((word >> 16) & 0xFFu);
        s.hostNsValid = ((word >> 24) & 1u) != 0u;
        s.checks = static_cast<std::uint32_t>(word >> 32);
        s.glitches = glitches;
        return s;
    }
};

class OrderEvidence
{
public:
    // 连续这么多次同方向的检查才定案。
    static constexpr std::uint32_t kVotesToJudge = 3;
    // 没有有效检查超过这么久（秒，按样本计）回到 unknown。
    static constexpr double kExpirySeconds = 1.0;
    // 本端时间戳比「按上一块块长推进的预期」多出这么多（秒）就算跳变。
    static constexpr double kJumpSlackSeconds = 0.25;

    // 非实时线程。换采样率时整份状态重来；同采样率重新 prepare 只重算常数，证据保持连续。
    void prepare(double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const bool changed = sr != sampleRate_;
        sampleRate_ = sr;
        expirySamples_ = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(kExpirySeconds * sr)));
        nsPerSample_ = 1.0e9 / sr;
        jumpSlackNs_ = static_cast<std::uint64_t>(kJumpSlackSeconds * 1.0e9);
        if (changed)
        {
            reset();
        }
    }

    // 整份状态清零（保留 prepare 算好的常数）。
    void reset() noexcept
    {
        verdict_ = OrderVerdict::kUnknown;
        direction_ = OrderDirection::kNone;
        votes_ = 0;
        checks_ = 0;
        glitches_ = 0;
        hostNsValid_ = false;
        sinceCheck_ = 0;
        havePrevOwn_ = false;
        prevOwnNs_ = 0;
        prevN_ = 0;
        havePeerSeq_ = false;
        lastPeerSeq_ = 0;
        havePrevPeer_ = false;
        prevPeerNs_ = 0;
    }

    // 音频线程，每个 n > 0 的块一次。
    //   side         本端是 Input 还是 Output
    //   n            本块样本数（失效计时按它累加）
    //   ownValid     本端宿主给了时间戳；ownNs 是它（0 也当没给）
    //   peerPresent  读到了对端槽；peerSeq 是对端槽的块序号，peerNs 是槽里的时间戳（0 = 对端没给）
    void observe(OrderSide side, std::int64_t n, bool ownValid, std::uint64_t ownNs, bool peerPresent,
                 std::uint64_t peerSeq, std::uint64_t peerNs) noexcept
    {
        if (n <= 0)
        {
            return;
        }
        sinceCheck_ = std::min<std::int64_t>(sinceCheck_ + n, kSinceCap);

        const bool own = ownValid && ownNs != 0;
        hostNsValid_ = own;
        bool comparable = own;
        bool glitch = false;

        // 本端的时间戳必须往前走，而且不能比按样本推进的预期多跳很多：回退、原地不动、大跳变都不可信。
        // 无论这一块能不能比较，都以它为下一块的基准（回退之后的下一块要和回退后的值比，不是和回退前的）。
        if (own)
        {
            if (havePrevOwn_)
            {
                if (ownNs <= prevOwnNs_)
                {
                    glitch = true;
                }
                else if (ownNs - prevOwnNs_ > expectedNs(prevN_) + jumpSlackNs_)
                {
                    glitch = true;
                }
            }
            prevOwnNs_ = ownNs;
            prevN_ = n;
            havePrevOwn_ = true;
        }
        else
        {
            havePrevOwn_ = false;
        }

        // 对端：槽里的块序号没动 = 对端自上次检查起没有处理新块，里面是旧值，不是证据；
        // 新块的时间戳没有比它自己的上一块大 = 对端回退，同样不可信。
        bool peerFresh = false;
        if (peerPresent)
        {
            peerFresh = !havePeerSeq_ || peerSeq != lastPeerSeq_;
            havePeerSeq_ = true;
            lastPeerSeq_ = peerSeq;
            if (peerFresh)
            {
                if (peerNs != 0)
                {
                    if (havePrevPeer_ && peerNs <= prevPeerNs_)
                    {
                        glitch = true;
                    }
                    prevPeerNs_ = peerNs;
                    havePrevPeer_ = true;
                }
                else
                {
                    havePrevPeer_ = false;
                }
            }
        }
        else
        {
            havePeerSeq_ = false;
            havePrevPeer_ = false;
        }
        if (!peerPresent || peerNs == 0 || !peerFresh)
        {
            comparable = false;
        }

        if (glitch)
        {
            ++glitches_;
            comparable = false;
        }

        if (comparable)
        {
            // 对端的时间戳不小于本块的：Input 看到 Output 的这样 = Output 先处理；Output 看到 Input 的这样 = Input
            // 先处理。
            const bool peerNotBehind = peerNs >= ownNs;
            vote(side == OrderSide::kInput ? peerNotBehind : !peerNotBehind);
            return;
        }

        // 判不了：清零连续计数，结论不动；超过 1 s 没有有效检查才回到 unknown。
        votes_ = 0;
        direction_ = OrderDirection::kNone;
        if (sinceCheck_ >= expirySamples_)
        {
            verdict_ = OrderVerdict::kUnknown;
        }
    }

    OrderSnapshot snapshot() const noexcept
    {
        OrderSnapshot s;
        s.verdict = verdict_;
        s.direction = direction_;
        s.votes = votes_;
        s.checks = checks_;
        s.glitches = glitches_;
        s.hostNsValid = hostNsValid_;
        return s;
    }

private:
    // 累加的上限：够装下任何一个合法的失效门槛，又不会溢出。
    static constexpr std::int64_t kSinceCap = static_cast<std::int64_t>(1) << 40;

    std::uint64_t expectedNs(std::int64_t samples) const noexcept
    {
        return static_cast<std::uint64_t>(static_cast<double>(samples) * nsPerSample_);
    }

    void vote(bool outputFirst) noexcept
    {
        sinceCheck_ = 0;
        ++checks_;
        const OrderDirection direction = outputFirst ? OrderDirection::kOutputFirst : OrderDirection::kInputFirst;
        if (direction == direction_)
        {
            votes_ = std::min(votes_ + 1u, kVotesToJudge);
        }
        else
        {
            direction_ = direction;
            votes_ = 1;
        }
        if (votes_ >= kVotesToJudge)
        {
            verdict_ = outputFirst ? OrderVerdict::kOutputFirst : OrderVerdict::kCorrect;
        }
    }

    double sampleRate_ = 0.0;
    std::int64_t expirySamples_ = 48000;
    double nsPerSample_ = 1.0e9 / 48000.0;
    std::uint64_t jumpSlackNs_ = 250000000u;

    OrderVerdict verdict_ = OrderVerdict::kUnknown;
    OrderDirection direction_ = OrderDirection::kNone;
    std::uint32_t votes_ = 0;
    std::uint32_t checks_ = 0;
    std::uint32_t glitches_ = 0;
    bool hostNsValid_ = false;
    std::int64_t sinceCheck_ = 0; // 距上一次有效检查过了多少样本

    bool havePrevOwn_ = false;
    std::uint64_t prevOwnNs_ = 0;
    std::int64_t prevN_ = 0;
    bool havePeerSeq_ = false;
    std::uint64_t lastPeerSeq_ = 0;
    bool havePrevPeer_ = false;
    std::uint64_t prevPeerNs_ = 0;
};

} // namespace rinne::loop
