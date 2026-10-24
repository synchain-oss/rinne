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

bool isSeparator(char c) noexcept
{
    return c == '/' || c == '\\';
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
ProbeLogDir resolveLogDir(std::string_view documentsDir)
{
    ProbeLogDir dir;
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

ProbeLogDir resolveLogDir()
{
    return resolveLogDir(
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getFullPathName().toStdString());
}

std::string logDirLabel(const ProbeLogDir& dir)
{
    return dir.path.empty() ? "none" : "default";
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
    return !status.pingRunning && !status.bypassed && (!status.transportPlaying || status.drySilent);
}

bool recordAllowed(const ProbeStatus& status, const ProbeSelection& selection) noexcept
{
    return status.logOpen && !status.recording && isValidSelection(selection);
}

bool autoPingTrigger(bool wasPlaying, bool isPlaying) noexcept
{
    return wasPlaying && !isPlaying;
}

std::string statusText(const ProbeStatus& status, bool recordRejected)
{
    std::string out = "link: ";
    out += status.paired ? "paired" : "not paired";
    out += " (group ";
    out += groupLetter(status.group);
    out += ")\ntransport: ";
    out += status.transportPlaying ? "playing" : "stopped";
    out += ", dry: ";
    out += status.drySilent ? "silent" : "active";
    if (status.bypassed)
    {
        out += ", bypassed";
    }
    out += "\nrecord: ";
    if (status.recording)
    {
        out += "recording, " + std::to_string(status.recordRemainingSeconds) + " s left";
    }
    else if (!status.logOpen)
    {
        out += "unavailable, log file not open";
    }
    else
    {
        out += recordRejected ? "idle, last start was refused (try again)" : "idle";
    }
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
    out += "\nstate: ";
    out += status.stateHeld ? "release-build state kept, panel changes are not saved" : "saved with the project";
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
namespace
{
// 向目标挪一格（0 或 ramp），每个样本最多一格：增益只走斜坡，从不阶跃
std::size_t stepToward(std::size_t pos, bool up, std::size_t ramp) noexcept
{
    if (up)
    {
        return pos < ramp ? pos + 1 : pos;
    }
    return pos > 0 ? pos - 1 : pos;
}

float gainAt(std::size_t pos, std::size_t ramp) noexcept
{
    return pos >= ramp ? 1.0f : static_cast<float>(pos) / static_cast<float>(ramp);
}
} // namespace

ProbePing::ProbePing(std::size_t maxDelaySamples, double sampleRate)
    : capture_(static_cast<std::size_t>(util::kMlsLength) + maxDelaySamples, 0.0f)
{
    if (std::isfinite(sampleRate) && sampleRate > 0.0)
    {
        ramp_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(kPingRampSeconds * sampleRate)));
    }
}

bool ProbePing::start(std::size_t settleSamples) noexcept
{
    if (active())
    {
        return false;
    }
    settleRequest_ = settleSamples;
    cancel_.store(false, std::memory_order_release);
    state_.store(State::kArmed, std::memory_order_release);
    return true;
}

void ProbePing::cancel() noexcept
{
    State s = state_.load(std::memory_order_acquire);
    if (s == State::kArmed || s == State::kRunning)
    {
        cancel_.store(true, std::memory_order_release);
    }
    else if (s == State::kDone)
    {
        state_.compare_exchange_strong(s, State::kIdle, std::memory_order_acq_rel);
    }
}

void ProbePing::abort() noexcept
{
    const State s = state_.load(std::memory_order_acquire);
    if (s != State::kArmed && s != State::kRunning)
    {
        return;
    }
    if (s == State::kRunning)
    {
        fadeInRequested_ = true; // 回送被压过：交给安全链的淡入，同跑完与取消（已经交过的不会再交）
    }
    // 阶段、系数与取消标记不用复位：Idle 时 process 不读它们，下一次 start 之后 beginRun 全部重设
    state_.store(State::kIdle, std::memory_order_release);
}

void ProbePing::reset() noexcept
{
    cancel_.store(false, std::memory_order_release);
    phase_ = Phase::kOff;
    dryPos_ = returnPos_ = ramp_;
    dryUp_ = returnUp_ = true;
    state_.store(State::kIdle, std::memory_order_release);
}

void ProbePing::beginRun() noexcept
{
    phase_ = Phase::kRampDown;
    dryPos_ = returnPos_ = ramp_;
    dryUp_ = returnUp_ = false;
    position_ = 0;
    cancelled_ = false;
    fadeInRequested_ = false;
    fadeInTaken_ = false;
    settleLeft_ = settleRequest_;
    holdLeft_ = 0;
}

void ProbePing::startRestore() noexcept
{
    phase_ = Phase::kRestore;
    dryUp_ = true;
    returnUp_ = false; // 回送不自己恢复，等淡入请求被取走
    fadeInRequested_ = true;
}

bool ProbePing::takeFadeInRequest() noexcept
{
    if (!fadeInRequested_ || fadeInTaken_)
    {
        return false;
    }
    fadeInTaken_ = true;
    holdLeft_ = ramp_;
    return true;
}

bool ProbePing::process(float* injection, const float* readback, float* dryScale, float* returnScale,
                        std::size_t n) noexcept
{
    if (n == 0)
    {
        return false; // 0 样本的回调什么都不推进
    }
    State s = state_.load(std::memory_order_acquire);
    if (s == State::kArmed)
    {
        if (cancel_.exchange(false, std::memory_order_acq_rel))
        {
            state_.store(State::kIdle, std::memory_order_release);
            return false;
        }
        beginRun();
        state_.store(State::kRunning, std::memory_order_release);
    }
    else if (s == State::kRunning)
    {
        if (cancel_.exchange(false, std::memory_order_acq_rel))
        {
            cancelled_ = true;
            if (phase_ == Phase::kRampDown || phase_ == Phase::kSettle || phase_ == Phase::kInject)
            {
                startRestore();
            }
        }
    }
    else
    {
        return false;
    }

    const std::size_t capacity = capture_.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        // 阶段的转换都在样本开头按上一个样本结束时的系数判，这样注入的每个样本写出的系数都已经是 0
        if (phase_ == Phase::kInject && position_ >= capacity)
        {
            startRestore();
        }
        if (phase_ == Phase::kSettle && settleLeft_ == 0)
        {
            phase_ = Phase::kInject;
        }
        if (phase_ == Phase::kRampDown && dryPos_ == 0 && returnPos_ == 0)
        {
            phase_ = settleLeft_ > 0 ? Phase::kSettle : Phase::kInject;
        }
        if (phase_ == Phase::kSettle)
        {
            --settleLeft_; // 进入时不为 0；减到 0 之后的那个样本开始注入：恰好静音 settleSamples 个样本
        }
        float inject = 0.0f;
        if (phase_ == Phase::kInject)
        {
            const std::size_t p = position_++;
            inject = p < kPingBuffer.size() ? kPingBuffer[p] : 0.0f;
            capture_[p] = readback != nullptr ? readback[i] : 0.0f;
        }
        if (phase_ == Phase::kRestore && dryPos_ >= ramp_)
        {
            phase_ = Phase::kHold;
        }
        if ((phase_ == Phase::kRestore || phase_ == Phase::kHold) && fadeInTaken_ && returnPos_ == 0)
        {
            // 回送已经是 0 且请求已取走：等安全链的下行淡出走完，再放回送
            if (holdLeft_ > 0)
            {
                --holdLeft_;
            }
            else
            {
                returnUp_ = true;
            }
        }
        dryPos_ = stepToward(dryPos_, dryUp_, ramp_);
        returnPos_ = stepToward(returnPos_, returnUp_, ramp_);
        if (phase_ == Phase::kHold && returnUp_ && returnPos_ >= ramp_ && dryPos_ >= ramp_)
        {
            phase_ = Phase::kOff;
            state_.store(cancelled_ ? State::kIdle : State::kDone, std::memory_order_release);
        }
        if (injection != nullptr)
        {
            injection[i] = inject;
        }
        if (dryScale != nullptr)
        {
            dryScale[i] = gainAt(dryPos_, ramp_);
        }
        if (returnScale != nullptr)
        {
            returnScale[i] = gainAt(returnPos_, ramp_);
        }
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

StateStatus decodeState(const std::uint8_t* data, std::size_t size, state::Role role, ProbeStateData& out) noexcept
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
    if (header.role != static_cast<std::uint32_t>(role))
    {
        return state::isKnownRole(static_cast<state::Role>(header.role)) ? StateStatus::kOtherRole
                                                                         : StateStatus::kCorrupt;
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

StateStatus ProbeStateKeeper::load(const std::uint8_t* data, std::size_t size, ProbeStateData& inOut)
{
    const StateStatus status = decodeState(data, size, role_, inOut);
    if (status == StateStatus::kOk)
    {
        held_.clear();
    }
    else if (status == StateStatus::kNotProbe)
    {
        held_.assign(data, data + size);
    }
    return status;
}

std::vector<std::uint8_t> ProbeStateKeeper::save(const ProbeStateData& current) const
{
    return held_.empty() ? encodeState(current, role_) : held_;
}

// ---- 复制诊断 ----
std::string buildDiagText(const diag::DiagInputs& base, const ProbeStatus& status, const ProbeLogDir& logDir,
                          const ProbeSelection& selection)
{
    std::string out = diag::buildDiagReport(base);
    out += "probe: log_dir=" + logDirLabel(logDir) + " log_open=" + (status.logOpen ? "1" : "0") +
           " recording=" + (status.recording ? "1" : "0") +
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
