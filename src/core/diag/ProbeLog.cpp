// SPDX-License-Identifier: GPL-3.0-or-later

#include "diag/ProbeLog.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include "ipc/ProcessInfo.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace rinne::diag
{

namespace
{

// 契约规则表里的全部 (case, variant) 组合，与 LOG-FORMAT.md 的规则表逐项对拍（tests/core/probe）
constexpr const char kCaseKeys[] =
    " CB-01:steady CB-02:toggle CB-03:guard-both CB-03:guard-in CB-03:guard-out"
    " CB-04:dop CB-04:export-offline CB-04:export-rt CB-04:freeze CB-04:rip"
    " CB-05:bypass-in CB-05:bypass-out CB-05:deactivate CB-05:disable-track CB-06:smart"
    " CB-07:dup CB-07:fxchain CB-07:preset CB-07:rename CB-07:reopen CB-07:undo"
    " CB-08:misplaced-order CB-08:misplaced-post CB-08:misplaced-track CB-09:mono CB-10:bufchange"
    " CB-11:constrain CB-12:twoproj LU-01:- LU-02:au-au LU-02:au-vst3 LU-02:vst3-vst3"
    " LU-03:steady LU-03:toggle LU-04:bounce LU-04:export-offline LU-04:export-rt LU-04:freeze"
    " LU-05:bypass-in LU-05:bypass-out LU-05:deactivate LU-06:rename LU-07:coreaudio LU-08:- LU-09:reopen ";

constexpr std::uint64_t kMask41 = (std::uint64_t{1} << 41) - 1;
constexpr std::uint64_t kMask21 = (std::uint64_t{1} << 21) - 1;
constexpr std::uint64_t kMask32 = 0xFFFFFFFFu;

std::uint64_t packSigned41(std::int64_t v) noexcept
{
    const std::int64_t bound = kProbeMaxPos - 1; // 41 位有符号装不下 +2^40，钳到 2^40 - 1
    v = std::max(-bound, std::min(bound, v));
    return static_cast<std::uint64_t>(v) & kMask41;
}

std::int64_t unpackSigned41(std::uint64_t bits) noexcept
{
    const std::uint64_t v = bits & kMask41;
    if ((v >> 40) != 0)
    {
        return static_cast<std::int64_t>(v) - (std::int64_t{1} << 41);
    }
    return static_cast<std::int64_t>(v);
}

// ---- 文本拼接 ----
void appendUInt(std::string& out, std::uint64_t v)
{
    char buf[20];
    int n = 0;
    do
    {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0)
    {
        out.push_back(buf[--n]);
    }
}

void appendInt(std::string& out, std::int64_t v)
{
    if (v < 0)
    {
        out.push_back('-');
        appendUInt(out, std::uint64_t{0} - static_cast<std::uint64_t>(v));
    }
    else
    {
        appendUInt(out, static_cast<std::uint64_t>(v));
    }
}

void appendHex(std::string& out, std::uint64_t v, int digits)
{
    static const char kDigits[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
    {
        out.push_back(kDigits[(v >> shift) & 0xFu]);
    }
}

bool isLowerHex(std::string_view s, std::size_t length) noexcept
{
    if (s.size() != length)
    {
        return false;
    }
    for (const char c : s)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

const char* guardName(ProbeGuard guard) noexcept
{
    switch (guard)
    {
    case ProbeGuard::kOff:
        return "off";
    case ProbeGuard::kLow:
        return "low";
    case ProbeGuard::kNormal:
        return "normal";
    case ProbeGuard::kHigh:
        return "high";
    case ProbeGuard::kNa:
        break;
    }
    return "na";
}

char roleLetter(ProbeRole role) noexcept
{
    return role == ProbeRole::kInput ? 'I' : 'O';
}

void appendArg(std::string& out, ProbeArgKind kind, std::uint64_t value, std::string_view text)
{
    switch (kind)
    {
    case ProbeArgKind::kNone:
        break;
    case ProbeArgKind::kUInt:
        appendUInt(out, value);
        break;
    case ProbeArgKind::kBool:
        out.push_back(value != 0 ? '1' : '0');
        break;
    case ProbeArgKind::kGroup:
        out.push_back(value < 8 ? static_cast<char>('A' + value) : '-');
        break;
    case ProbeArgKind::kPid:
        appendUInt(out, std::min<std::uint64_t>(value, kProbeMaxPid));
        break;
    case ProbeArgKind::kText:
        out += probeEncodeText(text);
        break;
    case ProbeArgKind::kColour:
        if (((value >> 33) & 1u) != 0)
        {
            if (((value >> 32) & 1u) != 0)
            {
                appendHex(out, value & kMask32, 8);
            }
            else
            {
                appendHex(out, value & 0xFFFFFFu, 6);
            }
        }
        break;
    case ProbeArgKind::kBackend:
        out += value == 0 ? "win32" : "posix";
        break;
    case ProbeArgKind::kMode:
        out += value == 0 ? "realtime" : "offline";
        break;
    }
}

#if defined(_WIN32)
std::int64_t queryCounterFrequency() noexcept
{
    LARGE_INTEGER freq{};
    if (!::QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0)
    {
        return 1;
    }
    return freq.QuadPart;
}
// 进程启动时取一次，音频线程读时钟不会碰到「首次使用」的初始化锁
const std::int64_t gCounterFrequency = queryCounterFrequency();
#endif

// 自动分配 tok4 时 findFreeProbeLogPath 向它要候选
struct Tok4Request
{
    std::uint32_t pid;
    std::uint64_t monoMs;
};
std::string nextTok4(void* context)
{
    const auto* request = static_cast<const Tok4Request*>(context);
    return allocateProbeTok4(request->pid, request->monoMs);
}

// tok4 的进程级登记：同一进程里发出去的 tok4 不重复
std::mutex gTok4Mutex;
std::bitset<65536> gTok4Used;
std::uint32_t gTok4Counter = 0;

constexpr std::uint64_t kBatchFlushBytes = std::uint64_t{1} << 20;

} // namespace

// ---- 时钟 ----
std::uint64_t probeTicksToNs(std::uint64_t ticks, std::uint64_t freq) noexcept
{
    if (freq == 0)
    {
        return 0;
    }
    return (ticks / freq) * 1000000000u + (ticks % freq) * 1000000000u / freq;
}

std::uint64_t probeMonoNs() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    if (!::QueryPerformanceCounter(&counter))
    {
        return 0;
    }
    // 静态初始化阶段（别的翻译单元的全局对象构造时）gCounterFrequency 还是 0：现查一次，不除零
    const std::int64_t freq = gCounterFrequency > 0 ? gCounterFrequency : queryCounterFrequency();
    return probeTicksToNs(static_cast<std::uint64_t>(counter.QuadPart), static_cast<std::uint64_t>(freq));
#elif defined(CLOCK_MONOTONIC_RAW)
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000u + static_cast<std::uint64_t>(ts.tv_nsec);
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

// ---- 头与测试格 ----
ProbeHeader makeProbeHeader(ProbeRole role, ProbeFormat format, ProbeHost host, std::string build)
{
    ProbeHeader header;
    header.role = role;
    header.format = format;
    header.host = host;
#if defined(_WIN32)
    header.os = ProbeOs::kWin;
#else
    header.os = ProbeOs::kMac;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    header.arch = ProbeArch::kArm64;
#else
    header.arch = ProbeArch::kX64;
#endif
    header.pid = ipc::currentProcessId();
    header.ppid = ipc::parentProcessId();
    header.build = std::move(build);
    return header;
}

std::string_view probeCaseKeys() noexcept
{
    return kCaseKeys;
}

bool isValidProbeCase(std::string_view caseId, std::string_view variant) noexcept
{
    if (caseId.empty() || variant.empty() || caseId.find_first_of(" :") != std::string_view::npos ||
        variant.find_first_of(" :") != std::string_view::npos)
    {
        return false;
    }
    // 在键表里逐个比对 "<case>:<variant>"。不拼字符串：这个函数 noexcept，不能因为 bad_alloc 把宿主带崩
    const std::string_view keys = probeCaseKeys();
    std::size_t pos = 0;
    while (pos < keys.size())
    {
        if (keys[pos] == ' ')
        {
            ++pos;
            continue;
        }
        const std::size_t end = keys.find(' ', pos);
        const std::string_view token = keys.substr(pos, end == std::string_view::npos ? keys.size() - pos : end - pos);
        if (token.size() == caseId.size() + 1 + variant.size() && token.substr(0, caseId.size()) == caseId &&
            token[caseId.size()] == ':' && token.substr(caseId.size() + 1) == variant)
        {
            return true;
        }
        pos = end == std::string_view::npos ? keys.size() : end;
    }
    return false;
}

// ---- 逐块记录的打包 ----
BlockSlot packBlock(std::uint64_t seq, const BlockFields& f) noexcept
{
    BlockSlot slot;
    slot.record.seq = seq;
    slot.record.callSeq = f.callSeq;
    slot.record.monoNs = f.monoNs;
    slot.record.hostNs = f.hostNsValid ? f.hostNs : 0;

    const std::uint64_t n = std::min<std::uint64_t>(f.n, kProbeMaxBlock);
    const std::uint64_t nDecl = std::min<std::uint64_t>(f.nDecl, kProbeMaxBlock);
    const std::uint64_t w = std::min<std::uint64_t>(f.w, static_cast<std::uint64_t>(kProbeMaxPos));
    const std::uint64_t rs = std::min<std::uint64_t>(f.rs, 255);
    const std::uint64_t d = std::min<std::uint64_t>(f.d, kProbeMaxD);

    slot.snapshot.word[0] = packSigned41(f.m) | (n << 41) | (std::uint64_t{f.playing ? 1u : 0u} << 62) |
                            (std::uint64_t{f.nonrt ? 1u : 0u} << 63);
    slot.snapshot.word[1] = packSigned41(f.off) | (nDecl << 41) | (std::uint64_t{f.hostNsValid ? 1u : 0u} << 62) |
                            (std::uint64_t{f.bypassed ? 1u : 0u} << 63);
    slot.snapshot.word[2] = w | (rs << 41);
    slot.snapshot.word[3] = d | (std::uint64_t{f.thread} << 32);
    return slot;
}

BlockFields unpackBlock(const BlockSlot& slot) noexcept
{
    BlockFields f;
    const std::uint64_t* word = slot.snapshot.word;
    f.callSeq = slot.record.callSeq;
    f.monoNs = slot.record.monoNs;
    f.hostNs = slot.record.hostNs;
    f.m = unpackSigned41(word[0]);
    f.n = static_cast<std::uint32_t>((word[0] >> 41) & kMask21);
    f.playing = ((word[0] >> 62) & 1u) != 0;
    f.nonrt = ((word[0] >> 63) & 1u) != 0;
    f.off = unpackSigned41(word[1]);
    f.nDecl = static_cast<std::uint32_t>((word[1] >> 41) & kMask21);
    f.hostNsValid = ((word[1] >> 62) & 1u) != 0;
    f.bypassed = ((word[1] >> 63) & 1u) != 0;
    f.w = word[2] & kMask41;
    f.rs = static_cast<std::uint32_t>((word[2] >> 41) & 0xFFu);
    f.d = static_cast<std::uint32_t>(word[3] & kMask32);
    f.thread = static_cast<std::uint32_t>(word[3] >> 32);
    return f;
}

std::uint32_t probeThreadHash(std::uint64_t threadToken) noexcept
{
    std::uint32_t h = 2166136261u;
    for (int i = 0; i < 8; ++i)
    {
        h ^= static_cast<std::uint32_t>((threadToken >> (8 * i)) & 0xFFu);
        h *= 16777619u;
    }
    return h;
}

// ---- 行格式化 ----
std::string probeEncodeText(std::string_view text, std::size_t maxBytes)
{
    std::size_t cut = std::min(text.size(), maxBytes);
    // 在 UTF-8 字符边界截断：被截掉的第一个字节不能是续字节（10xxxxxx），否则退回到该字符的起点
    while (cut > 0 && cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
    {
        --cut;
    }
    std::string out;
    out.reserve(cut * 3);
    static const char kHex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < cut; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '.' || c == '_' || c == '~' || c == '-';
        if (unreserved)
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xFu]);
        }
    }
    return out;
}

void appendProbeMagicAndHeader(std::string& out, const ProbeHeader& h)
{
    out += "rinne-probe-log,1\n";
    out += "H,";
    out.push_back(roleLetter(h.role));
    out += h.format == ProbeFormat::kVst3 ? ",vst3," : ",au,";
    switch (h.host)
    {
    case ProbeHost::kCubase:
        out += "cubase,";
        break;
    case ProbeHost::kLuna:
        out += "luna,";
        break;
    case ProbeHost::kOther:
        out += "other,";
        break;
    }
    out += h.os == ProbeOs::kWin ? "win," : "mac,";
    out += h.arch == ProbeArch::kX64 ? "x64," : "arm64,";
    appendUInt(out, h.pid);
    out.push_back(',');
    appendUInt(out, h.ppid);
    out.push_back(',');
    out += isLowerHex(h.tok4, 4) ? h.tok4 : std::string("0000");
    out.push_back(',');
    out += isLowerHex(h.build, 8) ? h.build : std::string("00000000");
    out.push_back('\n');
}

bool appendProbeCaseRow(std::string& out, std::uint64_t tNs, const ProbeCase& c)
{
    if (!isValidProbeCase(c.caseId, c.variant) || c.buf > kProbeMaxBuf || c.sampleRate > kProbeMaxSampleRate)
    {
        return false;
    }
    out += "C,";
    appendUInt(out, tNs);
    out.push_back(',');
    out.append(c.caseId);
    out.push_back(',');
    out.append(c.variant);
    out.push_back(',');
    if (c.buf == 0)
    {
        out.push_back('-');
    }
    else
    {
        appendUInt(out, c.buf);
    }
    out.push_back(',');
    out += guardName(c.guard);
    out.push_back(',');
    appendUInt(out, c.sampleRate);
    out.push_back('\n');
    return true;
}

void appendProbeBlockRow(std::string& out, ProbeRole role, const BlockSlot& slot)
{
    const BlockFields f = unpackBlock(slot);
    out += "B,";
    appendUInt(out, slot.record.seq);
    out.push_back(',');
    appendUInt(out, f.callSeq);
    out.push_back(',');
    out.push_back(roleLetter(role));
    out.push_back(',');
    appendUInt(out, f.n);
    out.push_back(',');
    appendUInt(out, f.nDecl);
    out.push_back(',');
    appendUInt(out, f.hostNs);
    out.push_back(',');
    out.push_back(f.hostNsValid ? '1' : '0');
    out.push_back(',');
    out.push_back(f.playing ? '1' : '0');
    out.push_back(',');
    out.push_back(f.nonrt ? '1' : '0');
    out.push_back(',');
    out.push_back(f.bypassed ? '1' : '0');
    out.push_back(',');
    appendUInt(out, f.monoNs);
    out.push_back(',');
    appendHex(out, f.thread, 8);
    out.push_back(',');
    appendInt(out, f.m);
    out.push_back(',');
    appendUInt(out, f.w);
    out.push_back(',');
    appendInt(out, f.off);
    out.push_back(',');
    appendUInt(out, f.rs);
    out.push_back(',');
    appendUInt(out, f.d);
    out.push_back('\n');
}

void appendProbeSummaryRow(std::string& out, std::uint64_t tNs, const ProbeCounters& c)
{
    out += "S,";
    appendUInt(out, tNs);
    out.push_back(',');
    appendUInt(out, c.blocks);
    out.push_back(',');
    appendUInt(out, c.dropped);
    out.push_back(',');
    appendUInt(out, c.maxN);
    out.push_back(',');
    appendUInt(out, c.resync);
    out.push_back(',');
    appendUInt(out, c.trips);
    out.push_back('\n');
}

void appendProbeCloseRow(std::string& out, std::uint64_t tNs, std::uint64_t blocksTotal, std::uint64_t droppedTotal)
{
    out += "Z,";
    appendUInt(out, tNs);
    out.push_back(',');
    appendUInt(out, blocksTotal);
    out.push_back(',');
    appendUInt(out, droppedTotal);
    out.push_back('\n');
}

bool appendProbeEventRow(std::string& out, std::uint64_t tNs, ProbeEventCode code, const ProbeEventArgs& args)
{
    const ProbeEventInfo* info = probeEventInfo(code);
    if (info == nullptr)
    {
        return false;
    }
    out += "E,";
    appendUInt(out, tNs);
    out.push_back(',');
    out += info->name;
    out.push_back(',');
    appendArg(out, info->a, args.a, args.text);
    out.push_back(',');
    appendArg(out, info->b, args.b, std::string_view());
    out.push_back('\n');
    return true;
}

// ---- 文件名与 tok4 ----
std::string probeLogFileName(ProbeRole role, std::uint32_t pid, std::string_view tok4)
{
    std::string name = "probe-";
    name.push_back(roleLetter(role));
    name.push_back('-');
    appendUInt(name, pid);
    name.push_back('-');
    name.append(tok4);
    name += ".csv";
    return name;
}

std::string deriveProbeTok4(std::uint32_t pid, std::uint64_t monoMs, std::uint32_t counter)
{
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    for (const std::uint64_t v : {std::uint64_t{pid}, monoMs, std::uint64_t{counter}})
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        // splitmix64 的终结混合
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
    }
    std::string out;
    appendHex(out, (h ^ (h >> 16) ^ (h >> 32) ^ (h >> 48)) & 0xFFFFu, 4);
    return out;
}

std::string allocateProbeTok4(std::uint32_t pid, std::uint64_t monoMs)
{
    std::lock_guard<std::mutex> lock(gTok4Mutex);
    if (gTok4Used.all())
    {
        gTok4Used.reset();
    }
    const std::string candidate = deriveProbeTok4(pid, monoMs, gTok4Counter++);
    std::size_t index = 0;
    for (const char c : candidate)
    {
        index = index * 16 + static_cast<std::size_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    while (gTok4Used.test(index))
    {
        index = (index + 1) & 0xFFFFu; // 顺延到下一个没发过的
    }
    gTok4Used.set(index);
    std::string out;
    appendHex(out, index, 4);
    return out;
}

bool findFreeProbeLogPath(const std::string& directoryUtf8, ProbeRole role, std::uint32_t pid, int attempts,
                          ProbeTok4Source source, void* sourceContext, std::string& tok4, std::string& pathUtf8)
{
    namespace fs = std::filesystem;
    if (source == nullptr)
    {
        return false;
    }
    const fs::path dir = fs::u8path(directoryUtf8);
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        std::string candidate = source(sourceContext);
        const fs::path path = dir / fs::u8path(probeLogFileName(role, pid, candidate));
        std::error_code ec;
        if (!fs::exists(path, ec) && !ec)
        {
            tok4 = std::move(candidate);
            pathUtf8 = path.u8string();
            return true;
        }
    }
    return false;
}

// ---- ProbeLog ----
ProbeLog::ProbeLog(ProbeLogOptions options)
    : options_(std::move(options)), role_(options_.header.role), blocks_(options_.blockRingSlots),
      audioEvents_(options_.audioEventSlots)
{
    pending_.reserve(std::min<std::size_t>(options_.pendingLimit, 256));
    openFile();
    nextSummaryNs_ = now() + options_.summaryIntervalNs;
    if (open_ && options_.startThread)
    {
        thread_ = std::thread([this] { threadMain(); });
    }
}

ProbeLog::~ProbeLog()
{
    close();
}

std::uint64_t ProbeLog::now() const noexcept
{
    return options_.clock != nullptr ? options_.clock(options_.clockContext) : probeMonoNs();
}

void ProbeLog::openFile()
{
    namespace fs = std::filesystem;
    if (options_.directory.empty())
    {
        return;
    }
    std::error_code ec;
    const fs::path dir = fs::u8path(options_.directory);
    fs::create_directories(dir, ec);
    if (ec)
    {
        return;
    }
    ProbeHeader& header = options_.header;
    fs::path path;
    if (isLowerHex(header.tok4, 4))
    {
        path = dir / fs::u8path(probeLogFileName(header.role, header.pid, header.tok4));
    }
    else
    {
        // 自动分配：已有同名文件（别的进程留下的同 pid 同 tok4）就换下一个，最多试 256 次。
        // 256 个全被占了就不开日志（open_ 保持假）：绝不拿 trunc 去冲别人的文件
        Tok4Request request{header.pid, now() / 1000000u};
        std::string tok4;
        std::string pathUtf8;
        if (!findFreeProbeLogPath(options_.directory, header.role, header.pid, 256, &nextTok4, &request, tok4,
                                  pathUtf8))
        {
            return;
        }
        header.tok4 = std::move(tok4);
        path = fs::u8path(pathUtf8);
    }
    // 二进制模式：Windows 的文本模式会把 LF 写成 CRLF，格式只许 LF
    file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!file_)
    {
        return;
    }
    std::string head;
    appendProbeMagicAndHeader(head, header);
    file_.write(head.data(), static_cast<std::streamsize>(head.size()));
    file_.flush();
    if (!file_)
    {
        return;
    }
    bytesWritten_ = head.size();
    tok4_ = header.tok4;
    path_ = path.u8string();
    open_ = true;
}

void ProbeLog::onBlock(const BlockFields& fields) noexcept
{
    if (closed_.load(std::memory_order_relaxed))
    {
        return;
    }
    // 单写者：读后写不会丢更新。与 B 行的 n 同一个上界，S 行的 max_n 因此永远在契约范围内
    const std::uint64_t n = std::min<std::uint64_t>(fields.n, kProbeMaxBlock);
    if (n > maxN_.load(std::memory_order_relaxed))
    {
        maxN_.store(n, std::memory_order_relaxed);
    }
    if (fields.rs != 0)
    {
        resync_.fetch_add(fields.rs, std::memory_order_relaxed);
    }
    if (!recording_.load(std::memory_order_acquire))
    {
        return;
    }
    // 块在 beginCase 之前就开始了（beginCase 落在这块中途）：它的 mono_ns 早于 C.t_ns，记进来会成孤块或被算到上一格。
    // 当窗口外处理：不占 seq，不计丢弃。recording_ 由假变真时，caseStartNs_ 在它之前写、release 发布，上面 acquire
    // 读到真就一定看得到新起点。不经 endCase 连开两格时 recording_ 一直为真，这里可能还读到旧起点，落在两个 C 行之间
    // 开始的块会被记下；它的 mono_ns 不早于上一个 C 行，分析器按时间戳把它归进上一格，不违反格式契约
    if (fields.monoNs < caseStartNs_.load(std::memory_order_relaxed))
    {
        return;
    }
    const std::uint64_t seq = nextSeq_++; // 丢了也占一个序号：文件里的缺口就是丢弃的位置
    if (blocks_.full())
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // 先加后推：落盘线程弹出之后的 fetch_sub（dropBlock）一定排在这次加之后，blocksPushed_ 不会下溢。
    // 生产者只有这一个线程、环刚才没满，tryPush 不会失败；万一失败就把计数还回去
    blocksPushed_.fetch_add(1, std::memory_order_relaxed);
    if (!blocks_.tryPush(packBlock(seq, fields)))
    {
        blocksPushed_.fetch_sub(1, std::memory_order_relaxed);
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ProbeLog::onAudioEvent(ProbeEventCode code, std::uint64_t a, std::uint64_t b, std::uint64_t monoNs) noexcept
{
    if (closed_.load(std::memory_order_relaxed))
    {
        return;
    }
    AudioEventSlot slot;
    slot.tNs = monoNs;
    slot.a = a;
    slot.b = b;
    slot.code = static_cast<std::uint64_t>(code);
    if (!audioEvents_.tryPush(slot))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t ProbeLog::snapshotSlot(ProbeEventCode code) noexcept
{
    switch (code)
    {
    case ProbeEventCode::kPrepare:
        return 0;
    case ProbeEventCode::kBackend:
        return 1;
    case ProbeEventCode::kPaired:
        return 2;
    case ProbeEventCode::kShmDenied:
        return 3;
    case ProbeEventCode::kSetState:
        return 4;
    case ProbeEventCode::kTrackProps:
        return 5;
    default:
        break;
    }
    return kSnapshotKinds;
}

void ProbeLog::enqueueLocked(std::string&& line)
{
    if (pending_.size() >= options_.pendingLimit)
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pending_.push_back(std::move(line));
}

bool ProbeLog::beginCase(const ProbeCase& c)
{
    if (!open_ || closed_.load(std::memory_order_acquire))
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // 先取时间戳、写 C 行，最后才打开块记录：C.t_ns 因此不会晚于该窗口第一块的 mono_ns
    const std::uint64_t t0 = now();
    std::string row;
    if (!appendProbeCaseRow(row, t0, c))
    {
        return false;
    }
    std::vector<std::string> rows;
    rows.reserve(1 + kSnapshotKinds);
    rows.push_back(std::move(row));

    // 状态快照：配对、后端、状态恢复这类事件往往发生在选 case 之前，补写一遍让每一格自含。
    // 原事件在发生时已经写过，这里是额外的一份，t_ns 是补写的时间
    static constexpr ProbeEventCode kOrder[kSnapshotKinds] = {ProbeEventCode::kPrepare,  ProbeEventCode::kBackend,
                                                              ProbeEventCode::kPaired,   ProbeEventCode::kShmDenied,
                                                              ProbeEventCode::kSetState, ProbeEventCode::kTrackProps};
    for (std::size_t i = 0; i < kSnapshotKinds; ++i)
    {
        const StoredEvent& stored = stored_[i];
        // track_props 从未送达也要写，a 与 b 都为空
        if (!stored.seen && kOrder[i] != ProbeEventCode::kTrackProps)
        {
            continue;
        }
        std::string line;
        if (appendProbeEventRow(line, now(), kOrder[i], ProbeEventArgs{stored.a, stored.b, stored.text}))
        {
            rows.push_back(std::move(line));
        }
    }
    // C 行加全部快照要整体放得下：丢了 C 行还照常开窗口，这一格的块就会被分析器归进上一格。放不下什么都不改
    if (pending_.size() + rows.size() > options_.pendingLimit)
    {
        return false;
    }
    for (std::string& line : rows)
    {
        pending_.push_back(std::move(line));
    }
    caseStartNs_.store(t0, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
    return true;
}

void ProbeLog::endCase()
{
    recording_.store(false, std::memory_order_release);
}

void ProbeLog::logEvent(ProbeEventCode code, const ProbeEventArgs& args)
{
    if (!open_ || closed_.load(std::memory_order_acquire))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string row;
    if (!appendProbeEventRow(row, now(), code, args))
    {
        return;
    }
    const std::size_t slot = snapshotSlot(code);
    if (slot < kSnapshotKinds)
    {
        StoredEvent& stored = stored_[slot];
        stored.seen = true;
        stored.a = args.a;
        stored.b = args.b;
        stored.text.assign(args.text.data(), args.text.size());
    }
    enqueueLocked(std::move(row));
}

ProbeCounters ProbeLog::counters() const noexcept
{
    ProbeCounters c;
    c.blocks = blocksPushed_.load(std::memory_order_relaxed);
    c.dropped = dropped_.load(std::memory_order_relaxed);
    c.maxN = maxN_.load(std::memory_order_relaxed);
    c.resync = resync_.load(std::memory_order_relaxed);
    c.trips = trips_.load(std::memory_order_relaxed);
    return c;
}

void ProbeLog::dropBlock() noexcept
{
    // 已经进了环、却没能写进文件的 B 行：从「记录块数」挪到「丢弃数」，文件里留下 seq 缺口
    blocksPushed_.fetch_sub(1, std::memory_order_relaxed);
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

void ProbeLog::writeOut()
{
    if (out_.empty())
    {
        return;
    }
    if (!broken_)
    {
        file_.write(out_.data(), static_cast<std::streamsize>(out_.size()));
        file_.flush();
        if (file_)
        {
            bytesWritten_ += out_.size();
        }
        else
        {
            broken_ = true;
        }
    }
    out_.clear();
}

void ProbeLog::pump()
{
    std::lock_guard<std::mutex> lock(pumpMutex_);
    pumpLocked(false);
}

void ProbeLog::pumpLocked(bool final)
{
    if (!open_ || finished_)
    {
        return;
    }
    // 1. 其他线程排队的行（C、事件及快照），顺序就是入队顺序
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scratch_.swap(pending_);
    }
    for (const std::string& line : scratch_)
    {
        out_ += line;
    }
    scratch_.clear();

    // 2. 音频线程自己发的事件
    AudioEventSlot event;
    while (audioEvents_.tryPop(event))
    {
        appendProbeEventRow(out_, event.tNs, static_cast<ProbeEventCode>(event.code),
                            ProbeEventArgs{event.a, event.b, std::string_view()});
    }

    // 3. 逐块记录。文件到上限（或写坏了）之后不再写 B 行，按丢弃计数
    BlockSlot slot;
    while (blocks_.tryPop(slot))
    {
        if (broken_ || bytesWritten_ + out_.size() >= options_.maxFileBytes)
        {
            dropBlock();
            continue;
        }
        appendProbeBlockRow(out_, role_, slot);
        if (out_.size() >= kBatchFlushBytes)
        {
            writeOut();
        }
    }

    // 4. 1 Hz 摘要；收尾时不写这一条，由 Z 行接着
    const std::uint64_t t = now();
    if (!final && t >= nextSummaryNs_)
    {
        appendProbeSummaryRow(out_, t, counters());
        nextSummaryNs_ = t + options_.summaryIntervalNs;
    }
    if (final)
    {
        const ProbeCounters c = counters();
        appendProbeCloseRow(out_, t, c.blocks, c.dropped);
    }
    writeOut();
    if (final)
    {
        file_.close();
        finished_ = true;
    }
}

void ProbeLog::threadMain()
{
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stopRequested_; });
            if (stopRequested_)
            {
                break;
            }
        }
        try
        {
            pump();
        }
        catch (...)
        {
            // 内存不够之类：不再写文件，数据按丢弃计数，线程继续活着等收尾
            std::lock_guard<std::mutex> lock(pumpMutex_);
            broken_ = true;
        }
    }
    try
    {
        std::lock_guard<std::mutex> lock(pumpMutex_);
        pumpLocked(true);
    }
    catch (...)
    {
    }
}

void ProbeLog::close()
{
    std::lock_guard<std::mutex> closeLock(closeMutex_);
    if (closed_.load(std::memory_order_acquire))
    {
        return;
    }
    recording_.store(false, std::memory_order_release);
    closed_.store(true, std::memory_order_release);
    if (thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            stopRequested_ = true;
        }
        wake_.notify_all();
        thread_.join();
        return;
    }
    try
    {
        std::lock_guard<std::mutex> lock(pumpMutex_);
        pumpLocked(true);
    }
    catch (...)
    {
    }
}

} // namespace rinne::diag
