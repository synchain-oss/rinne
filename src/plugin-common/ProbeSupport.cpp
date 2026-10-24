// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProbeSupport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/EngineParams.h"
#include "engine/SafetyMachine.h"
#include "state/StateMigration.h"

namespace rinne::probe
{

namespace
{

// ---- case / variant 表：从 ProbeLog 的规则表里按首现顺序解出来 ----
struct CaseTable
{
    std::vector<std::string> cases;
    std::vector<std::string> variants; // 不含 -

    CaseTable()
    {
        std::string_view rest = diag::probeCaseKeys();
        while (!rest.empty())
        {
            const std::size_t space = rest.find(' ');
            const std::string_view token = rest.substr(0, space);
            rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
            const std::size_t colon = token.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            addUnique(cases, std::string(token.substr(0, colon)));
            const std::string variant(token.substr(colon + 1));
            if (variant != "-")
            {
                addUnique(variants, variant);
            }
        }
    }

    static void addUnique(std::vector<std::string>& list, std::string value)
    {
        if (std::find(list.begin(), list.end(), value) == list.end())
        {
            list.push_back(std::move(value));
        }
    }
};

const CaseTable& caseTable()
{
    static const CaseTable table;
    return table;
}

std::string trimmed(const std::string& text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin]))
    {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1]))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool isSeparator(char c) noexcept
{
    return c == '/' || c == '\\';
}

// 不经文件系统库：判断只看文本，Windows 与 mac 上结果一样，测试不依赖平台
bool isAbsolutePath(std::string_view path) noexcept
{
    if (path.empty())
    {
        return false;
    }
    if (path[0] == '/' || (path[0] == '\\' && path.size() > 1 && path[1] == '\\'))
    {
        return true;
    }
    const bool letter = (path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z');
    return letter && path.size() >= 3 && path[1] == ':' && isSeparator(path[2]);
}

// ---- state 的小端读写 ----
void putU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void putF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    putU32(out, bits);
}

std::uint32_t getU32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float getF32(const std::uint8_t* p) noexcept
{
    const std::uint32_t bits = getU32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

constexpr std::uint8_t kGroupUnset = 0xFF;

char groupLetter(int group) noexcept
{
    return group >= 0 && group < 8 ? static_cast<char>('A' + group) : '-';
}

} // namespace

// ---- 钳位 ----
float clampFeedbackPercent(float percent) noexcept
{
    if (std::isnan(percent))
    {
        return 0.0f;
    }
    return std::min(std::max(percent, 0.0f), kFeedbackMaxPercent);
}

float clampDelayMs(float ms) noexcept
{
    if (std::isnan(ms))
    {
        return engine::kDefaultDelayMs;
    }
    return std::min(std::max(ms, engine::kMinDelayMs), engine::kMaxDelayMs);
}

float clampCeilingDb(float db) noexcept
{
    if (std::isnan(db))
    {
        return kCeilingDefaultDb;
    }
    return std::min(std::max(db, engine::kMinCeilingDb), engine::kMaxCeilingDb);
}

// ---- case / variant ----
std::size_t caseCount()
{
    return caseTable().cases.size();
}

std::string caseAt(std::size_t index)
{
    const auto& cases = caseTable().cases;
    return index < cases.size() ? cases[index] : std::string{};
}

std::size_t variantCount()
{
    return caseTable().variants.size();
}

std::string variantAt(std::size_t index)
{
    const auto& variants = caseTable().variants;
    return index < variants.size() ? variants[index] : std::string{};
}

std::vector<std::string> variantsFor(std::string_view caseId)
{
    std::vector<std::string> out;
    if (diag::isValidProbeCase(caseId, "-"))
    {
        out.emplace_back("-");
    }
    for (const std::string& variant : caseTable().variants)
    {
        if (diag::isValidProbeCase(caseId, variant))
        {
            out.push_back(variant);
        }
    }
    return out;
}

const char* guardName(diag::ProbeGuard guard) noexcept
{
    switch (guard)
    {
    case diag::ProbeGuard::kOff:
        return "off";
    case diag::ProbeGuard::kLow:
        return "low";
    case diag::ProbeGuard::kNormal:
        return "normal";
    case diag::ProbeGuard::kHigh:
        return "high";
    case diag::ProbeGuard::kNa:
        break;
    }
    return "na";
}

bool isValidSelection(const ProbeSelection& selection) noexcept
{
    return diag::isValidProbeCase(selection.caseId, selection.variant) && selection.buf <= diag::kProbeMaxBuf;
}

diag::ProbeCase toProbeCase(const ProbeSelection& selection, std::uint32_t sampleRate) noexcept
{
    diag::ProbeCase c;
    c.caseId = selection.caseId;
    c.variant = selection.variant;
    c.buf = selection.buf;
    c.guard = selection.guard;
    c.sampleRate = sampleRate;
    return c;
}

// ---- 隐藏参数 ----
AutoRequest parseAutoParams(int autoCase, int autoVariant, int autoRecordSeconds)
{
    AutoRequest request;
    if (autoRecordSeconds == 0)
    {
        return request;
    }
    if (autoRecordSeconds < 0 || autoRecordSeconds > static_cast<int>(kAutoRecordMaxSeconds))
    {
        request.status = AutoStatus::kBadRecordSeconds;
        return request;
    }
    if (autoCase < 1 || static_cast<std::size_t>(autoCase) > caseCount())
    {
        request.status = AutoStatus::kBadCase;
        return request;
    }
    if (autoVariant < 0 || static_cast<std::size_t>(autoVariant) > variantCount())
    {
        request.status = AutoStatus::kBadVariant;
        return request;
    }
    request.caseId = caseAt(static_cast<std::size_t>(autoCase) - 1);
    request.variant = autoVariant == 0 ? std::string("-") : variantAt(static_cast<std::size_t>(autoVariant) - 1);
    if (!diag::isValidProbeCase(request.caseId, request.variant))
    {
        request = AutoRequest{};
        request.status = AutoStatus::kBadCombination;
        return request;
    }
    request.status = AutoStatus::kOk;
    request.recordSeconds = static_cast<std::uint32_t>(autoRecordSeconds);
    return request;
}

// ---- 日志目录 ----
ProbeLogDir resolveLogDir(const EnvReader& readEnv, std::string_view documentsDir)
{
    ProbeLogDir dir;
    if (readEnv)
    {
        const std::string value = trimmed(readEnv(kLogDirEnv));
        if (isAbsolutePath(value))
        {
            dir.path = value;
            dir.fromEnvironment = true;
            return dir;
        }
    }
    if (!documentsDir.empty())
    {
        std::string base(documentsDir);
        while (!base.empty() && isSeparator(base.back()))
        {
            base.pop_back();
        }
        const char separator = base.find('\\') != std::string::npos ? '\\' : '/';
        dir.path = base + separator + kDefaultLogDirName;
    }
    return dir;
}

ProbeLogDir resolveLogDir(const EnvReader& readEnv)
{
    const std::string documents =
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getFullPathName().toStdString();
    return resolveLogDir(readEnv, documents);
}

std::string logDirLabel(const ProbeLogDir& dir)
{
    if (dir.path.empty())
    {
        return "none";
    }
    if (!dir.fromEnvironment)
    {
        return "default";
    }
    std::string path = dir.path;
    while (!path.empty() && isSeparator(path.back()))
    {
        path.pop_back();
    }
    const std::size_t cut = path.find_last_of("/\\");
    const std::string leaf = cut == std::string::npos ? path : path.substr(cut + 1);
    const std::string clean = diag::sanitizeText(leaf, 24);
    return clean.empty() ? "custom" : clean;
}

// ---- 冷却与按钮规则 ----
std::uint64_t RecoverCooldown::remainingMs(std::uint64_t nowMs) const noexcept
{
    if (!panicked_)
    {
        return 0;
    }
    const auto cooldownMs = static_cast<std::uint64_t>(engine::safety::kRecoverCooldownSeconds * 1000.0);
    const std::uint64_t elapsed = nowMs >= panicMs_ ? nowMs - panicMs_ : 0;
    return elapsed >= cooldownMs ? 0 : cooldownMs - elapsed;
}

bool pingAllowed(const ProbeStatus& status) noexcept
{
    return !status.pingRunning && (!status.transportPlaying || status.drySilent);
}

bool recordAllowed(const ProbeStatus& status, const ProbeSelection& selection) noexcept
{
    return !status.recording && isValidSelection(selection);
}

bool autoPingTrigger(bool wasPlaying, bool isPlaying) noexcept
{
    return wasPlaying && !isPlaying;
}

std::string statusText(const ProbeStatus& status)
{
    std::string out = "link: ";
    out += status.paired ? "paired" : "not paired";
    out += " (group ";
    out += groupLetter(status.group);
    out += ")\ntransport: ";
    out += status.transportPlaying ? "playing" : "stopped";
    out += ", dry: ";
    out += status.drySilent ? "silent" : "active";
    out += "\nrecord: ";
    out += status.recording ? "recording, " + std::to_string(status.recordRemainingSeconds) + " s left" : "idle";
    out += "\nping: ";
    if (status.pingRunning)
    {
        out += "running";
    }
    else
    {
        out += status.lastPingSamples < 0 ? "idle" : std::to_string(status.lastPingSamples) + " samples";
    }
    out += "\nsafety: ";
    out += status.tripped ? "tripped" : "ok";
    return out;
}

std::string countersText(const ProbeStatus& status)
{
    const diag::ProbeCounters& c = status.counters;
    std::string out = "blocks " + std::to_string(c.blocks) + "  dropped " + std::to_string(c.dropped) + "  max_n " +
                      std::to_string(c.maxN) + "  resync " + std::to_string(c.resync) + "  trips " +
                      std::to_string(c.trips);
    out += "\nfeedback " + diag::formatFixed(status.feedbackPercent, 1) + " %  D " +
           diag::formatFixed(status.delayMs, 1) + " ms  ceiling " + diag::formatFixed(status.ceilingDb, 1) + " dB";
    return out;
}

// ---- Ping ----
ProbePing::ProbePing(std::size_t maxDelaySamples)
    : capture_(static_cast<std::size_t>(util::kMlsLength) + maxDelaySamples, 0.0f)
{
}

bool ProbePing::start() noexcept
{
    if (active())
    {
        return false;
    }
    state_.store(State::kArmed, std::memory_order_release);
    return true;
}

void ProbePing::cancel() noexcept
{
    state_.store(State::kIdle, std::memory_order_release);
}

bool ProbePing::process(float* injection, const float* readback, std::size_t n) noexcept
{
    State s = state_.load(std::memory_order_acquire);
    if (s == State::kArmed)
    {
        position_ = 0;
        if (!state_.compare_exchange_strong(s, State::kRunning, std::memory_order_acq_rel))
        {
            return false;
        }
    }
    else if (s != State::kRunning)
    {
        return false;
    }
    const std::size_t capacity = capture_.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t p = position_ + i;
        if (injection != nullptr)
        {
            injection[i] = p < kPingBuffer.size() ? kPingBuffer[p] : 0.0f;
        }
        if (p < capacity)
        {
            capture_[p] = readback != nullptr ? readback[i] : 0.0f;
        }
    }
    position_ = std::min(position_ + n, capacity);
    if (position_ >= capacity)
    {
        State expected = State::kRunning;
        state_.compare_exchange_strong(expected, State::kDone, std::memory_order_release);
    }
    return true;
}

PingResult ProbePing::finish()
{
    PingResult result;
    if (state() != State::kDone)
    {
        return result;
    }
    const util::MlsPeak found = util::mlsLinearDelay(capture_.data(), capture_.size());
    result.peak = found.peak;
    result.runnerUp = found.runnerUp;
    result.ok = found.lag >= 0 && found.peak > 0.0 && found.peak >= kPingMinPeakRatio * found.runnerUp;
    result.delaySamples = result.ok ? static_cast<std::uint32_t>(found.lag) : 0u;
    state_.store(State::kIdle, std::memory_order_release);
    return result;
}

// ---- 探针 state ----
std::vector<std::uint8_t> encodeState(const ProbeStateData& data, state::Role role)
{
    std::vector<std::uint8_t> out;
    out.reserve(kStateBytes);
    putU32(out, state::kMagic);
    putU32(out, state::kProbeAbi);
    putU32(out, static_cast<std::uint32_t>(role));
    putU32(out, 1u);
    putU32(out, kStateChunkFourCc);
    putU32(out, static_cast<std::uint32_t>(kStatePayloadBytes));
    out.push_back(data.group >= 0 && data.group < 8 ? static_cast<std::uint8_t>(data.group) : kGroupUnset);
    out.insert(out.end(), 3, std::uint8_t{0});
    putF32(out, clampFeedbackPercent(data.feedbackPercent));
    putF32(out, clampDelayMs(data.delayMs));
    putF32(out, clampCeilingDb(data.ceilingDb));
    return out;
}

StateStatus decodeState(const std::uint8_t* data, std::size_t size, ProbeStateData& out) noexcept
{
    if (size == 0)
    {
        return StateStatus::kEmpty;
    }
    state::Header header;
    if (data == nullptr || !state::parseHeader(data, size, header) || header.magic != state::kMagic)
    {
        return StateStatus::kCorrupt;
    }
    if (header.abi != state::kProbeAbi)
    {
        return StateStatus::kNotProbe;
    }
    if (size != kStateBytes || header.chunkCount != 1u || getU32(data + 16) != kStateChunkFourCc ||
        getU32(data + 20) != kStatePayloadBytes)
    {
        return StateStatus::kCorrupt;
    }
    const std::uint8_t* payload = data + 24;
    const float feedback = getF32(payload + 4);
    const float delay = getF32(payload + 8);
    const float ceiling = getF32(payload + 12);
    const bool groupOk = payload[0] < 8 || payload[0] == kGroupUnset;
    if (!groupOk || !std::isfinite(feedback) || !std::isfinite(delay) || !std::isfinite(ceiling))
    {
        return StateStatus::kCorrupt;
    }
    out.group = payload[0] == kGroupUnset ? -1 : static_cast<int>(payload[0]);
    out.feedbackPercent = clampFeedbackPercent(feedback);
    out.delayMs = clampDelayMs(delay);
    out.ceilingDb = clampCeilingDb(ceiling);
    return StateStatus::kOk;
}

// ---- 复制诊断 ----
std::string buildDiagText(const diag::DiagInputs& base, const ProbeStatus& status, const ProbeLogDir& logDir,
                          const ProbeSelection& selection)
{
    std::string out = diag::buildDiagReport(base);
    out += "probe: log_dir=" + logDirLabel(logDir) + " recording=" + (status.recording ? "1" : "0") +
           " feedback_cap=" + diag::formatFixed(kFeedbackMaxPercent, 0) +
           " ceiling_db=" + diag::formatFixed(status.ceilingDb, 1) + "\n";
    const std::string buf = selection.buf == 0 ? std::string("-") : std::to_string(selection.buf);
    out += "probe_case: case=" + diag::sanitizeText(selection.caseId) +
           " variant=" + diag::sanitizeText(selection.variant) + " buf=" + buf +
           " guard=" + guardName(selection.guard) + "\n";
    out += "probe_ping: samples=" +
           (status.lastPingSamples < 0 ? std::string("none") : std::to_string(status.lastPingSamples)) + "\n";
    const diag::ProbeCounters& c = status.counters;
    out += "probe_log: blocks=" + std::to_string(c.blocks) + " dropped=" + std::to_string(c.dropped) +
           " max_n=" + std::to_string(c.maxN) + " resync=" + std::to_string(c.resync) +
           " trips=" + std::to_string(c.trips) + "\n";
    return out;
}

} // namespace rinne::probe
