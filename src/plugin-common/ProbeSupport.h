// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针面板的支持件：面板和处理器共用的纯逻辑，头文件不碰 JUCE（只有 ProbeSupport.cpp 里取用户文档目录一处用 JUCE），
// 测试因此不必起界面。代码在正式构建里也编译，只在探针构建里由处理器实例化。
//   - 隐藏参数 auto_case / auto_variant / auto_record_s 的数值怎么变成 case、variant 与记录秒数（表外的组合拒绝）；
//   - 日志目录：环境变量 RINNE_PROBE_LOG_DIR，没有就用用户文档目录下的 Rinne Probe Logs；路径不进 state。
//     本文件不读环境变量（发布代码不许读），读取函数由探针构建的接线交进来；
//   - Ping：127 点 MLS、-20 dBFS 的注入与回读累积，用 Mls.h 的线性互相关求延迟。接到引擎的原始读头由处理器做；
//   - 面板上的规则：feedback 上限 80 %、Ping 何时可点、恢复的 1.5 s 冷却、选择器的取值；
//   - 探针 state：abi 0 的 RNST 头加一个自定块（group、feedback、D、ceiling）。abi 0 时正式版的加载方只读头、不解块，
//     所以这份字节在正式版里被整份忽略；
//   - 复制诊断：在 DiagReport 的文本后面追加探针几行，日志目录只给末级目录名或 default，不出现完整路径。
// 线程：除 ProbePing::process 外都只在非实时线程调用。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "diag/DiagReport.h"
#include "diag/ProbeLog.h"
#include "engine/EngineParams.h"
#include "state/StateCodec.h"
#include "util/Mls.h"

namespace rinne::probe
{

// ---- 探针的保守默认 ----
inline constexpr float kFeedbackMaxPercent = 80.0f; // 比引擎的 150 保守
inline constexpr float kCeilingDefaultDb = -18.0f;
inline constexpr std::uint32_t kRecordSeconds = 60; // 面板上「记录 60 s」按钮
inline constexpr std::uint32_t kAutoRecordMaxSeconds = 3600;

float clampFeedbackPercent(float percent) noexcept; // 钳到 [0, 80]，NaN 当 0
float clampDelayMs(float ms) noexcept; // 钳到引擎允许的 D 范围，NaN 当默认值
float clampCeilingDb(float db) noexcept; // 钳到 [-24, 0]，NaN 当 -18

// ---- case / variant 表 ----
// 表的真源是 ProbeLog 的规则表（probeCaseKeys），这里只按它的首现顺序编号，不另存一份。
std::size_t caseCount();
std::string caseAt(std::size_t index); // 从 0 起；越界返回空串
std::size_t variantCount(); // 全部 variant 的个数，不含 -
std::string variantAt(std::size_t index);
std::vector<std::string> variantsFor(std::string_view caseId); // 该 case 允许的 variant，不区分的写 -

// 面板上的一次选择；开始记录时原样交给 C 行
struct ProbeSelection
{
    std::string caseId = "CB-01";
    std::string variant = "steady";
    std::uint32_t buf = 0; // 缓冲区大小（样本），0 写成 -
    diag::ProbeGuard guard = diag::ProbeGuard::kNa;
};

inline constexpr std::array<std::uint32_t, 9> kBufferChoices{0, 32, 64, 128, 256, 512, 1024, 2048, 4096};
inline constexpr std::array<diag::ProbeGuard, 5> kGuardChoices{diag::ProbeGuard::kOff, diag::ProbeGuard::kLow,
                                                               diag::ProbeGuard::kNormal, diag::ProbeGuard::kHigh,
                                                               diag::ProbeGuard::kNa};
const char* guardName(diag::ProbeGuard guard) noexcept;

bool isValidSelection(const ProbeSelection& selection) noexcept;
// C 行要的字段。返回值里的 string_view 指向 selection，selection 要活得比它久
diag::ProbeCase toProbeCase(const ProbeSelection& selection, std::uint32_t sampleRate) noexcept;

// ---- 隐藏参数 ----
// auto_case：0 = 不选，1..caseCount() 按 caseAt 的顺序；auto_variant：0 = -，k >= 1 是 variantAt(k - 1)；
// auto_record_s：0 = 不自动记录，1..kAutoRecordMaxSeconds。参数本身由处理器加进 APVTS，这里只解析与校验。
enum class AutoStatus : std::uint8_t
{
    kIdle, // auto_record_s 为 0：什么都不做
    kOk,
    kBadCase,
    kBadVariant,
    kBadCombination, // (case, variant) 不在规则表里
    kBadRecordSeconds,
};

struct AutoRequest
{
    AutoStatus status = AutoStatus::kIdle;
    std::string caseId;
    std::string variant;
    std::uint32_t recordSeconds = 0;
};

AutoRequest parseAutoParams(int autoCase, int autoVariant, int autoRecordSeconds);

// ---- 日志目录 ----
inline constexpr const char* kLogDirEnv = "RINNE_PROBE_LOG_DIR";
inline constexpr const char* kDefaultLogDirName = "Rinne Probe Logs";

struct ProbeLogDir
{
    std::string path; // UTF-8；空 = 没有可用的目录
    bool fromEnvironment = false;
};

using EnvReader = std::function<std::string(const char* name)>;

// 环境变量优先，但必须是绝对路径（相对路径要看宿主的工作目录，探针不猜）；否则 documentsDir/Rinne Probe Logs。
ProbeLogDir resolveLogDir(const EnvReader& readEnv, std::string_view documentsDir);
// 真文档目录（JUCE）。环境变量怎么读由调用方的 readEnv 决定，传空函数就是不读
ProbeLogDir resolveLogDir(const EnvReader& readEnv);
// 诊断里用的名字：默认目录写 default，否则只给末级目录名（过诊断的清洗）
std::string logDirLabel(const ProbeLogDir& dir);

// ---- 恢复冷却 ----
// 面板上 Panic 之后恢复按钮的冷却，时长与引擎的恢复冷却同一个常数。时间由调用方给（毫秒，单调）。
class RecoverCooldown
{
public:
    void onPanic(std::uint64_t nowMs) noexcept
    {
        panicked_ = true;
        panicMs_ = nowMs;
    }
    std::uint64_t remainingMs(std::uint64_t nowMs) const noexcept;
    bool ready(std::uint64_t nowMs) const noexcept { return remainingMs(nowMs) == 0; }

private:
    bool panicked_ = false;
    std::uint64_t panicMs_ = 0;
};

// ---- 面板读到的状态 ----
struct ProbeStatus
{
    bool transportPlaying = false;
    bool drySilent = false;
    bool paired = false;
    // 日志文件是否真的开了（ProbeLog::isOpen）。没开时数据全丢，所以默认假：处理器忘了填，面板会明说记录不可用
    bool logOpen = false;
    bool recording = false;
    bool pingRunning = false;
    bool tripped = false;
    int group = -1; // -1 = 未设，0..7 = A..H
    int lastPingSamples = -1; // -1 = 还没有结果
    std::uint32_t recordRemainingSeconds = 0;
    std::uint32_t sampleRate = 0;
    float feedbackPercent = 0.0f;
    float delayMs = engine::kDefaultDelayMs;
    float ceilingDb = kCeilingDefaultDb;
    diag::ProbeCounters counters;
};

// Ping 只在停走带或干声静音时可点，且同一时刻只跑一次
bool pingAllowed(const ProbeStatus& status) noexcept;
// 记录按钮：日志开着、没在记录，且选择在规则表里
bool recordAllowed(const ProbeStatus& status, const ProbeSelection& selection) noexcept;
// 走带从播放变停止时自动 Ping 一次
bool autoPingTrigger(bool wasPlaying, bool isPlaying) noexcept;

// recordRejected：上一次点记录时处理器没有开窗口（日志排队满了等，稍后重试即可），由面板记着、成功后清掉
std::string statusText(const ProbeStatus& status, bool recordRejected = false);
std::string countersText(const ProbeStatus& status);

// ---- Ping ----
inline constexpr float kPingAmplitude = 0.1f; // -20 dBFS
inline constexpr double kPingMinPeakRatio = 2.0; // 峰与次高值的比低于它就不信

constexpr std::array<float, util::kMlsLength> makePingBuffer() noexcept
{
    std::array<float, util::kMlsLength> out{};
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = util::kMls[i] > 0 ? kPingAmplitude : -kPingAmplitude;
    }
    return out;
}
inline constexpr std::array<float, util::kMlsLength> kPingBuffer = makePingBuffer();

struct PingResult
{
    bool ok = false;
    std::uint32_t delaySamples = 0;
    double peak = 0.0;
    double runnerUp = 0.0;
};

// 一次 Ping 的状态机：Idle -> Armed（start）-> Running（process 的第一块）-> Done（回读攒满）。
// 注入从回读窗口的第一个样本开始，窗口长 127 + maxDelaySamples，求出来的延迟在 [0, maxDelaySamples]。
// start / cancel / finish 在非实时线程；process 在音频线程，定长循环、不分配、不加锁。
class ProbePing
{
public:
    enum class State : std::uint8_t
    {
        kIdle,
        kArmed,
        kRunning,
        kDone,
    };

    explicit ProbePing(std::size_t maxDelaySamples);

    bool start() noexcept; // 空闲或上一次已结束时开始；正在跑返回 false
    void cancel() noexcept;
    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool active() const noexcept
    {
        const State s = state();
        return s == State::kArmed || s == State::kRunning;
    }

    // 回送增益与干声增益在 Ping 期间一律强制为 0，注入的序列才不会被回送放大、也不会被干声污染相关峰
    float returnGain(float requested) const noexcept { return active() ? 0.0f : requested; }
    float dryGain(float requested) const noexcept { return active() ? 0.0f : requested; }

    // 每个回调调一次（n 为本块样本数）。injection 取得本块要注入的样本（序列放完之后是 0），readback 是读头的原始回读。
    // 没在跑返回 false，injection 不动
    bool process(float* injection, const float* readback, std::size_t n) noexcept;

    // 状态为 Done 之后在非实时线程调用，结果取走后回到 Idle
    PingResult finish();

private:
    static_assert(std::atomic<State>::is_always_lock_free, "音频线程每块都读 state_");

    std::vector<float> capture_;
    std::size_t position_ = 0;
    std::atomic<State> state_{State::kIdle};
};

// ---- 探针 state ----
inline constexpr std::uint32_t kStateChunkFourCc = state::makeFourCc('P', 'R', 'B', 'E');
inline constexpr std::size_t kStatePayloadBytes = 16;
inline constexpr std::size_t kStateBytes = state::kHeaderBytes + 8 + kStatePayloadBytes;

struct ProbeStateData
{
    int group = -1; // -1 = 未设，0..7 = A..H
    float feedbackPercent = 0.0f;
    float delayMs = engine::kDefaultDelayMs;
    float ceilingDb = kCeilingDefaultDb;
};

enum class StateStatus : std::uint8_t
{
    kOk,
    kEmpty, // 零字节
    kNotProbe, // 头合格但不是 abi 0（正式版写的）：探针不接，免得覆盖面板上的设置
    kCorrupt,
};

// abi 0 的 RNST 头（kProbeAbi；加载方只读头）加一个 PRBE 块
std::vector<std::uint8_t> encodeState(const ProbeStateData& data, state::Role role);
// 失败时 out 不变；合格的字节里超范围的数值钳到面板的范围
StateStatus decodeState(const std::uint8_t* data, std::size_t size, ProbeStateData& out) noexcept;

// ---- 复制诊断 ----
// DiagReport 的文本，后面追加探针几行。日志目录只给 logDirLabel，不含完整路径、用户名、主机名。
std::string buildDiagText(const diag::DiagInputs& base, const ProbeStatus& status, const ProbeLogDir& logDir,
                          const ProbeSelection& selection);

} // namespace rinne::probe
