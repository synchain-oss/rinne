// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/rig.json
//
// 免 DAW 宿主的时间与走带：一张所有实例共用的 TransportMap，加上每个实例一个的 LanePlayHead。
//
// 时间模型：
//   - 游标 = 实例已经渲染过的样本数（从 0 起），也就是「这个样本送到总线的时刻」。每个实例有自己的游标：宿主可以让两端
//     各走各的（跳过一端的块、只重新 prepare 一端），时间线位置都按各自的游标从同一张图上查。
//   - TransportMap 是游标 → 时间线位置的纯函数：定位（Seek）、停（Stop）、起（Start，可同时定位）、循环区间（Cycle，
//     走到区间末尾回到开头）。事件按生效游标排好，查询只看游标，与谁在什么时候问无关，所以同一份脚本每次结果相同。
//   - 不连续点（事件、循环回绕、时间线到达某个切点）由 nextBoundary 给出。真实宿主不会让一块跨过循环点或定位点，
//     宿主本体按它把块切开：一块之内时间线恒连续，这是 Input 读环、Output 写环都默认的前提。
//
// LanePlayHead 同时是「宿主给什么」的脚本（不另设一个假播放头）：每块调用插件之前 arm 一次，定下这一块报给插件的
// 时间线位置、走带状态（来自 TransportMap），以及宿主给不给速度、给不给时间戳、给什么值（HostFacts，由脚本按块给出）。
// getPosition 只返回 arm 定下的那一份：插件在一块之内问几次都一样。
//
// 线程：只在测试线程上用（它同时扮演音频线程与消息线程）。插件在处理回调里调的 getPosition 不分配；arm 与脚本在宿主
// 调用插件之前跑，不在插件的处理回调里。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <vector>

namespace rinne::plugin_host
{

// 「没有边界」：取 int64 上限的四分之一，做加减不会溢出
inline constexpr std::int64_t kFar = std::numeric_limits<std::int64_t>::max() / 4;

enum class TransportKind
{
    Seek, // 时间线位置 := pos，走带状态不变
    Stop, // 停走带，时间线停在当前位置
    Start, // 起走带；hasPos 时同时定位到 pos
    Cycle, // 循环区间 := [pos, loopEnd)；loopEnd ≤ pos 表示关掉循环
};

struct TransportEvent
{
    std::int64_t at = 0; // 生效游标
    TransportKind kind = TransportKind::Seek;
    std::int64_t pos = 0;
    std::int64_t loopEnd = 0;
    bool hasPos = false;
};

// 某个游标上的走带状态
struct MapPoint
{
    std::int64_t t = 0; // 时间线位置（样本）
    bool playing = true;
    bool cycleOn = false;
    std::int64_t loopB = 0;
    std::int64_t loopE = 0;

    // 处在「走到 loopE 就回到 loopB」的状态：在走、循环开着、区间有效、还没越过 loopE。
    // 从区间之后起播的位置不回绕（宿主的循环只在播放头从区间里走到末尾时才起作用）
    bool wrapping() const { return playing && cycleOn && loopB < loopE && t < loopE; }
};

class TransportMap
{
public:
    explicit TransportMap(std::int64_t startT = 0, bool playing = true)
    {
        initial_.t = startT;
        initial_.playing = playing;
        rebuild();
    }

    // 按生效游标插入；同一个游标上的几个事件按加入的先后依次生效
    void add(const TransportEvent& ev)
    {
        const auto it = std::upper_bound(events_.begin(), events_.end(), ev.at,
                                         [](std::int64_t a, const TransportEvent& e) { return a < e.at; });
        events_.insert(it, ev);
        rebuild();
    }

    const std::vector<TransportEvent>& events() const { return events_; }

    MapPoint at(std::int64_t c) const { return advance(segs_[segIndex(c)], c); }

    // c 之后（严格大于 c）的第一个不连续点：下一个事件、下一次循环回绕，或时间线走到 cuts 里的某个位置（只看当前这一段
    // 线性区间，回绕之后的那一段等走到了再算）。没有就是 kFar
    std::int64_t nextBoundary(std::int64_t c, const std::vector<std::int64_t>& cuts = {}) const
    {
        const std::size_t idx = segIndex(c);
        std::int64_t best = idx + 1 < segs_.size() ? segs_[idx + 1].c0 : kFar;
        const Seg& s = segs_[idx];
        const MapPoint p = advance(s, c);
        if (!p.playing)
            return best;
        std::int64_t pieceEnd = kFar;
        if (s.p0.wrapping())
        {
            const std::int64_t first = s.c0 + (s.p0.loopE - s.p0.t);
            const std::int64_t len = s.p0.loopE - s.p0.loopB;
            pieceEnd = c < first ? first : first + ((c - first) / len + 1) * len;
        }
        best = std::min(best, pieceEnd);
        for (const std::int64_t g : cuts)
            if (g > p.t)
                best = std::min(best, c + (g - p.t));
        return best;
    }

private:
    // 一段：从游标 c0 起的线性（或循环）走法，起点状态 p0
    struct Seg
    {
        std::int64_t c0 = 0;
        MapPoint p0;
    };

    static MapPoint advance(const Seg& s, std::int64_t c)
    {
        MapPoint p = s.p0;
        const std::int64_t d = c - s.c0;
        if (!p.playing || d <= 0)
            return p;
        if (s.p0.wrapping())
        {
            const std::int64_t first = s.p0.loopE - s.p0.t;
            p.t = d < first ? s.p0.t + d : s.p0.loopB + (d - first) % (s.p0.loopE - s.p0.loopB);
            return p;
        }
        p.t = s.p0.t + d;
        return p;
    }

    // 最后一个 c0 ≤ c 的段。第 0 段的 c0 恒为 0，负的游标按第 0 段算
    std::size_t segIndex(std::int64_t c) const
    {
        const auto it =
            std::upper_bound(segs_.begin(), segs_.end(), c, [](std::int64_t a, const Seg& s) { return a < s.c0; });
        return it == segs_.begin() ? 0 : static_cast<std::size_t>(std::distance(segs_.begin(), it) - 1);
    }

    void rebuild()
    {
        segs_.clear();
        segs_.push_back(Seg{0, initial_});
        for (const TransportEvent& ev : events_)
        {
            MapPoint p = advance(segs_.back(), ev.at);
            switch (ev.kind)
            {
            case TransportKind::Seek:
                p.t = ev.pos;
                break;
            case TransportKind::Stop:
                p.playing = false;
                break;
            case TransportKind::Start:
                p.playing = true;
                if (ev.hasPos)
                    p.t = ev.pos;
                break;
            case TransportKind::Cycle:
                p.cycleOn = ev.loopEnd > ev.pos;
                p.loopB = ev.pos;
                p.loopE = ev.loopEnd;
                break;
            }
            segs_.push_back(Seg{ev.at, p});
        }
    }

    MapPoint initial_;
    std::vector<TransportEvent> events_;
    std::vector<Seg> segs_;
};

// 宿主这一块给不给速度、给不给时间戳。空 = 这一项宿主没给（getPosition 里不设）
struct HostFacts
{
    std::optional<double> bpm = 120.0;
    std::optional<std::uint64_t> hostNs;
};

// 一块定下来的位置与事实
struct BlockPos
{
    std::int64_t cursor = 0;
    int n = 0;
    MapPoint point; // 时间线 = map(cursor)
    HostFacts facts;
};

class LanePlayHead final : public juce::AudioPlayHead
{
public:
    // 按块给事实：参数是这一块的游标与宿主时钟（纳秒）；空脚本 = 速度 120、时间戳取宿主时钟
    using FactScript = std::function<HostFacts(std::int64_t cursor, std::uint64_t clockNs)>;

    LanePlayHead(const TransportMap& map, double sampleRate) : map_(map), sampleRate_(sampleRate) {}

    std::int64_t cursor() const { return cursor_; }
    void setCursor(std::int64_t c) { cursor_ = c; }
    void setFacts(FactScript script) { facts_ = std::move(script); }

    // 从当前游标起、不越过任何不连续点的最大块长（≤ limit，至少 1）
    int maxBlock(int limit, const std::vector<std::int64_t>& cuts = {}) const
    {
        const std::int64_t room = map_.nextBoundary(cursor_, cuts) - cursor_;
        return static_cast<int>(std::max<std::int64_t>(1, std::min<std::int64_t>(limit, room)));
    }

    // 这一块要调用插件了：定下位置与事实，交给之后的 getPosition
    const BlockPos& arm(int n, std::uint64_t clockNs)
    {
        armed_.cursor = cursor_;
        armed_.n = n;
        armed_.point = map_.at(cursor_);
        if (facts_)
        {
            armed_.facts = facts_(cursor_, clockNs);
        }
        else
        {
            armed_.facts = HostFacts{};
            armed_.facts.hostNs = clockNs;
        }
        return armed_;
    }

    const BlockPos& armed() const { return armed_; }

    void advance(int n) { cursor_ += n; }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        const MapPoint& pt = armed_.point;
        p.setTimeInSamples(pt.t);
        p.setTimeInSeconds(static_cast<double>(pt.t) / sampleRate_);
        p.setIsPlaying(pt.playing);
        if (armed_.facts.bpm.has_value())
        {
            const double bpm = *armed_.facts.bpm;
            p.setBpm(bpm);
            p.setPpqPosition(static_cast<double>(pt.t) / sampleRate_ * (bpm / 60.0));
            const bool looping = pt.cycleOn && pt.loopB < pt.loopE;
            p.setIsLooping(looping);
            if (looping)
            {
                LoopPoints lp;
                lp.ppqStart = static_cast<double>(pt.loopB) / sampleRate_ * (bpm / 60.0);
                lp.ppqEnd = static_cast<double>(pt.loopE) / sampleRate_ * (bpm / 60.0);
                p.setLoopPoints(lp);
            }
        }
        if (armed_.facts.hostNs.has_value())
            p.setHostTimeNs(*armed_.facts.hostNs);
        return p;
    }

private:
    const TransportMap& map_;
    double sampleRate_;
    std::int64_t cursor_ = 0;
    FactScript facts_;
    BlockPos armed_;
};

} // namespace rinne::plugin_host
