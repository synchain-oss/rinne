// SPDX-License-Identifier: GPL-3.0-or-later
//
// 诊断文本的拼装。自由文本先按片段清洗再写出：任何片段带路径分隔符、冒号、@、~、% 的，整串不要，其余字符只留
// ASCII 白名单，所以宿主字符串里的路径、盘符和带账号特征的内容进不了剪贴板；不带分隔符的裸名字清洗不出来，
// 靠调用方只交名称与版本。数字一律走整数十进制，时间拆成
// 整数秒与三位毫秒，浮点只参与取整前的乘法，不用 printf 的浮点格式，所以输出与区域设置无关、逐字节可复现。
#include "diag/DiagReport.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rinne::diag
{

namespace
{

struct EventInfo
{
    const char* name;
    std::uint8_t params;
};

// 事件码的名字与参数个数放在同一张表里：新增事件码时长度断言先拦住，名字与参数不会各写一份而对不上
constexpr EventInfo kEventInfo[] = {
    {"construct", 0},     {"prepare", 2},    {"release", 0},    {"set_state", 1},   {"group_set", 2},
    {"smart_default", 2}, {"pair_state", 1}, {"peer_lost", 0},  {"conflict", 1},    {"sr_mismatch", 2},
    {"resync", 1},        {"trip", 1},       {"recover", 0},    {"panic", 0},       {"loop_on", 0},
    {"loop_off", 0},      {"bypass_on", 0},  {"bypass_off", 0}, {"nonrealtime", 1},
};
static_assert(sizeof(kEventInfo) / sizeof(kEventInfo[0]) == kEventCodeCount, "事件名字表长度必须等于 kEventCodeCount");

bool isWhitespace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool isAsciiAlnum(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isAllowedTextChar(char c) noexcept
{
    return isAsciiAlnum(c) || c == '.' || c == '_' || c == '+' || c == '-' || c == '(' || c == ')';
}

bool isRejectedTextChar(char c) noexcept
{
    return c == '/' || c == '\\' || c == ':' || c == '@' || c == '~' || c == '%';
}

std::uint8_t eventParamCount(EventCode code) noexcept
{
    const std::size_t index = static_cast<std::size_t>(code);
    if (index >= kEventCodeCount)
    {
        return 0; // 越界码没有参数，名字由 eventName 写成 unknown
    }
    return kEventInfo[index].params;
}

void appendSanitized(std::string& out, std::string_view raw)
{
    const std::string cleaned = sanitizeText(raw);
    if (cleaned.empty())
    {
        out += "none"; // key=value 里不写空值，也避免连续空格带来歧义
    }
    else
    {
        out += cleaned;
    }
}

void appendBool(std::string& out, bool value)
{
    out += (value ? '1' : '0');
}

const char* originName(DiagOrigin origin) noexcept
{
    switch (origin)
    {
    case DiagOrigin::kNone:
        return "none";
    case DiagOrigin::kSmart:
        return "smart";
    case DiagOrigin::kRestore:
        return "restore";
    case DiagOrigin::kManual:
        return "manual";
    }
    return "none";
}

const char* relationName(DiagTrackRelation relation) noexcept
{
    switch (relation)
    {
    case DiagTrackRelation::kUnknown:
        return "unknown";
    case DiagTrackRelation::kSame:
        return "same";
    case DiagTrackRelation::kDifferent:
        return "different";
    }
    return "unknown";
}

const char* roleName(DiagRole role) noexcept
{
    switch (role)
    {
    case DiagRole::kInput:
        return "input";
    case DiagRole::kOutput:
        return "output";
    }
    return "unknown";
}

const char* delayModeName(DiagDelayMode mode) noexcept
{
    switch (mode)
    {
    case DiagDelayMode::kTime:
        return "ms";
    case DiagDelayMode::kSync:
        return "sync";
    }
    return "ms";
}

const char* bpmSourceName(DiagBpmSource source) noexcept
{
    switch (source)
    {
    case DiagBpmSource::kNone:
        return "none";
    case DiagBpmSource::kHost:
        return "host";
    case DiagBpmSource::kManual:
        return "manual";
    }
    return "none";
}

const char* orderVerdictName(DiagOrderVerdict verdict) noexcept
{
    switch (verdict)
    {
    case DiagOrderVerdict::kUnknown:
        return "unknown";
    case DiagOrderVerdict::kCorrect:
        return "correct";
    case DiagOrderVerdict::kWrong:
        return "wrong";
    }
    return "unknown";
}

const char* loadStatusName(DiagLoadStatus status) noexcept
{
    switch (status)
    {
    case DiagLoadStatus::kNone:
        return "none";
    case DiagLoadStatus::kOk:
        return "ok";
    case DiagLoadStatus::kMigrated:
        return "migrated";
    case DiagLoadStatus::kEmpty:
        return "empty";
    case DiagLoadStatus::kIgnoredProbe:
        return "ignored_probe";
    case DiagLoadStatus::kRejectedNewer:
        return "rejected_newer";
    case DiagLoadStatus::kCorrupt:
        return "corrupt";
    }
    return "none";
}

std::string packedVersion(std::uint32_t version)
{
    return std::to_string(version >> 16u) + "." + std::to_string((version >> 8u) & 0xffu) + "." +
           std::to_string(version & 0xffu);
}

void appendEventTime(std::string& out, std::uint64_t tMs)
{
    out += std::to_string(tMs / 1000u);
    out += '.';
    const std::string millis = std::to_string(static_cast<std::uint32_t>(tMs % 1000u));
    constexpr std::size_t kMillisWidth = 3;
    out.append(kMillisWidth - millis.size(), '0');
    out += millis;
}

void appendEvent(std::string& out, const DiagEvent& event)
{
    out += "  t+";
    appendEventTime(out, event.tMs);
    out += ' ';
    out += eventName(event.code);
    const std::uint8_t params = eventParamCount(event.code);
    if (params >= 1)
    {
        out += ' ';
        out += std::to_string(event.a);
    }
    if (params >= 2)
    {
        out += '/';
        out += std::to_string(event.b);
    }
    out += '\n';
}

} // namespace

std::string sanitizeText(std::string_view raw, std::size_t maxLen)
{
    if (maxLen == 0u)
    {
        return {};
    }
    std::string joined;
    std::size_t pos = 0;
    while (pos < raw.size())
    {
        while (pos < raw.size() && isWhitespace(raw[pos]))
        {
            ++pos;
        }
        if (pos >= raw.size())
        {
            break;
        }
        const std::size_t start = pos;
        while (pos < raw.size() && !isWhitespace(raw[pos]))
        {
            ++pos;
        }
        const std::string_view fragment = raw.substr(start, pos - start);
        bool rejected = false;
        for (const char c : fragment)
        {
            if (isRejectedTextChar(c))
            {
                rejected = true;
                break;
            }
        }
        if (rejected)
        {
            // 含路径或账号特征：整串不要。路径里的目录名可以带空格（用户目录名常是「名 姓」），它前面的词也可能是
            // 目录名的一部分，只丢这一段或这一段之后的内容，都会把 "Jane" 或 "Doe" 留在输出里
            return {};
        }
        // 拼到 maxLen 就不再往里加字符，临时串因此有上界。但后面的片段仍要看有没有路径或账号特征：只看已经
        // 拼好的部分就收工，会把后面那段路径漏过去，让带路径的串拼出前半个词
        if (joined.size() >= maxLen)
        {
            continue;
        }
        if (!joined.empty())
        {
            joined.push_back(' ');
        }
        for (const char c : fragment)
        {
            if (joined.size() >= maxLen)
            {
                break;
            }
            joined.push_back(isAllowedTextChar(c) ? c : '_');
        }
    }
    if (joined.size() > maxLen)
    {
        joined.resize(maxLen);
    }
    // 截断可能留下尾部空格：先去首尾，再交出结果
    std::size_t first = 0;
    while (first < joined.size() && joined[first] == ' ')
    {
        ++first;
    }
    std::size_t last = joined.size();
    while (last > first && joined[last - 1] == ' ')
    {
        --last;
    }
    return joined.substr(first, last - first);
}

std::string formatFixed(double value, int decimals)
{
    const int digits = decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals);
    const double magnitude = std::fabs(value);
    if (!std::isfinite(value) || magnitude > 1e15)
    {
        return "nan";
    }
    static constexpr std::uint64_t kScale[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    const std::uint64_t scale = kScale[static_cast<std::size_t>(digits)];
    const std::uint64_t whole = static_cast<std::uint64_t>(magnitude);
    const double fraction = magnitude - static_cast<double>(whole);
    // 整数化小数部分：+0.5 后截断就是正数的四舍五入，恰好半格时远离零
    std::uint64_t fractionUnits = static_cast<std::uint64_t>(fraction * static_cast<double>(scale) + 0.5);
    std::uint64_t wholeUnits = whole;
    if (fractionUnits >= scale)
    {
        fractionUnits -= scale;
        ++wholeUnits;
    }
    const bool negative = value < 0.0 && (wholeUnits != 0u || fractionUnits != 0u);
    std::string out;
    if (negative)
    {
        out.push_back('-');
    }
    out += std::to_string(wholeUnits);
    if (digits > 0)
    {
        out.push_back('.');
        const std::string fractionText = std::to_string(fractionUnits);
        out.append(static_cast<std::size_t>(digits) - fractionText.size(), '0');
        out += fractionText;
    }
    return out;
}

std::string formatHex8(std::uint32_t value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(8, '0');
    for (std::size_t i = 0; i < 8u; ++i)
    {
        out[7 - i] = kHex[static_cast<std::size_t>(value & 0xfu)];
        value >>= 4u;
    }
    return out;
}

std::string groupLetter(std::uint32_t group)
{
    if (group == 0u || group > 8u)
    {
        return "-";
    }
    return std::string(1, static_cast<char>('A' + (group - 1u)));
}

const char* eventName(EventCode code) noexcept
{
    const std::size_t index = static_cast<std::size_t>(code);
    if (index >= kEventCodeCount)
    {
        return "unknown";
    }
    return kEventInfo[index].name;
}

void DiagEventLog::push(std::uint64_t tMs, EventCode code, std::uint32_t a, std::uint32_t b) noexcept
{
    const std::size_t slot = static_cast<std::size_t>(count_ % kEventCapacity);
    ring_[slot] = DiagEvent{tMs, code, a, b};
    ++count_;
}

DiagEvent DiagEventLog::at(std::size_t i) const noexcept
{
    if (i >= size())
    {
        return {};
    }
    std::size_t index = i;
    if (count_ >= kEventCapacity)
    {
        const std::size_t oldest = static_cast<std::size_t>(count_ % kEventCapacity);
        index = (oldest + i) % kEventCapacity;
    }
    return ring_[index];
}

std::string buildDiagReport(const DiagInputs& in)
{
    std::string out;
    out.reserve(4096);
    out += "rinne-diag v1\n";

    out += "product=";
    appendSanitized(out, in.product);
    out += " version=";
    appendSanitized(out, in.version);
    out += " build=";
    appendSanitized(out, in.build);
    out += " format=";
    appendSanitized(out, in.format);
    out += " host=";
    appendSanitized(out, in.host);
    out += " os=";
    appendSanitized(out, in.os);
    out += " arch=";
    appendSanitized(out, in.arch);
    out += '\n';

    out += "backend=";
    appendSanitized(out, in.backend);
    out += " ipc_abi=";
    out += std::to_string(in.ipcAbi);
    out += " prefix=";
    appendSanitized(out, in.prefix);
    out += " group=";
    out += groupLetter(in.group);
    out += " origin=";
    out += originName(in.origin);
    out += " pid=";
    out += std::to_string(in.pid);
    out += " pair=";
    appendSanitized(out, in.pairState);
    out += '\n';

    out += "track: relation=";
    out += relationName(in.peer.present ? in.peer.name : DiagTrackRelation::kUnknown);
    out += " hash=";
    if (in.trackHash == 0u)
    {
        out += "none";
    }
    else
    {
        out += formatHex8(in.trackHash);
    }
    out += '\n';

    out += "peer: ";
    if (!in.peer.present)
    {
        out += "none\n";
    }
    else
    {
        out += "role=";
        out += roleName(in.peer.role);
        out += " state=";
        out += (in.peer.active ? "active" : "dormant");
        out += " same_pid=";
        appendBool(out, in.peer.samePid);
        out += " version=";
        out += packedVersion(in.peer.version);
        out += " sr=";
        out += std::to_string(in.peer.sampleRate);
        out += " max_block=";
        out += std::to_string(in.peer.maxBlock);
        out += " ch=";
        out += std::to_string(in.peer.channels);
        out += " name=";
        out += relationName(in.peer.name);
        out += " hb_age_ms=";
        out += std::to_string(in.peer.heartbeatAgeMs);
        out += '\n';
    }

    out += "prepare: count=";
    out += std::to_string(in.prepare.count);
    out += " sr=";
    out += std::to_string(in.prepare.sampleRate);
    out += " max=";
    out += std::to_string(in.prepare.maxBlock);
    out += " nonrealtime_seen=";
    appendBool(out, in.prepare.nonrealtimeSeen);
    out += " setstate_before_prepare=";
    appendBool(out, in.prepare.setStateBeforePrepare);
    out += " reset_calls=";
    out += std::to_string(in.prepare.resetCalls);
    out += '\n';

    out += "blocks: total=";
    out += std::to_string(in.blocks.blocksTotal);
    out += " zero_len=";
    out += std::to_string(in.blocks.zeroLen);
    out += " over_max=";
    out += std::to_string(in.blocks.overMax);
    out += " bypassed=";
    out += std::to_string(in.blocks.bypassed);
    out += " max_n_seen=";
    out += std::to_string(in.prepare.maxNSeen);
    out += '\n';

    out += "nsamples: min=";
    out += std::to_string(in.blocks.nMin);
    out += " max=";
    out += std::to_string(in.blocks.nMax);
    out += " hist=";
    for (std::size_t i = 0; i < kNsamplesBuckets; ++i)
    {
        if (i != 0u)
        {
            out += ',';
        }
        out += std::to_string(in.blocks.hist[i]);
    }
    out += '\n';

    if (in.role == DiagRole::kInput)
    {
        out += "loop: d_user_ms=";
        out += formatFixed(in.loop.dUserMs, 3);
        out += " d_eff=";
        out += std::to_string(in.loop.dEff);
        out += " d_min=";
        out += std::to_string(in.loop.dMin);
        out += " mode=";
        out += delayModeName(in.loop.mode);
        out += " bpm=";
        out += formatFixed(in.loop.bpm, 1);
        out += " bpm_src=";
        out += bpmSourceName(in.loop.bpmSource);
        out += " taps_active=";
        out += std::to_string(in.loop.tapsActive);
        out += " tap_drop=";
        out += std::to_string(in.loop.tapDrop);
        out += '\n';

        out += "sync: resync=";
        out += std::to_string(in.sync.resync);
        out += " overflow=";
        out += std::to_string(in.sync.overflow);
        out += " torn=";
        out += std::to_string(in.sync.torn);
        out += " overrun=";
        out += std::to_string(in.sync.overrun);
        out += " w_cas_fail=";
        out += std::to_string(in.sync.wCasFail);
        out += " unstable=";
        out += std::to_string(in.sync.unstable);
        out += " off_drift_blocks=";
        out += std::to_string(in.sync.offDriftBlocks);
        out += " frozen_max=";
        out += std::to_string(in.sync.frozenMax);
        out += '\n';

        out += "order: verdict=";
        out += orderVerdictName(in.order.verdict);
        out += " votes=";
        out += formatFixed(in.order.votes, 2);
        out += " host_ns=";
        out += (in.order.hostNsValid ? "valid" : "invalid");
        out += '\n';

        out += "safety: state=";
        appendSanitized(out, in.safety.state);
        out += " trips=";
        out += std::to_string(in.safety.trips);
        out += " tail_peak_max_db=";
        out += formatFixed(in.safety.tailPeakMaxDb, 1);
        out += " limiter_gr_max_db=";
        out += formatFixed(in.safety.limiterGrMaxDb, 1);
        out += " nonfinite_ring=";
        out += std::to_string(in.safety.nonfiniteRing);
        out += " clamp_ring=";
        out += std::to_string(in.safety.clampRing);
        out += '\n';
    }

    out += "state: last_load=";
    out += loadStatusName(in.state.lastLoad);
    if (in.role == DiagRole::kInput)
    {
        out += " dropped_unknown=";
        out += std::to_string(in.state.droppedUnknownParams);
    }
    out += " encode_failures=";
    out += std::to_string(in.state.encodeFailures);
    out += " holding_newer=";
    appendBool(out, in.state.holdingNewer);
    out += '\n';

    const std::uint64_t eventCount = in.events != nullptr ? static_cast<std::uint64_t>(in.events->size()) : 0u;
    const std::uint64_t eventTotal = in.events != nullptr ? in.events->total() : 0u;
    out += "events(last ";
    out += std::to_string(eventCount);
    out += " of ";
    out += std::to_string(eventTotal);
    out += "):\n";
    if (in.events != nullptr)
    {
        for (std::size_t i = 0; i < in.events->size(); ++i)
        {
            appendEvent(out, in.events->at(i));
        }
    }
    return out;
}

} // namespace rinne::diag
