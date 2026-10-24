// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针面板的支持件：面板和处理器共用的纯逻辑，头文件不碰 JUCE（只有 ProbeSupport.cpp 里取用户文档目录一处用 JUCE），
// 测试因此不必起界面。源码只在探针构建里编进插件（sources-probe.cmake）；测试经包装 .cpp 在两种构建里都编、都覆盖。
//   - 隐藏参数 auto_case / auto_variant / auto_record_s 的数值怎么变成 case、variant 与记录秒数（表外的组合拒绝）；
//   - 日志目录：只有一个来源，用户文档目录下的 Rinne Probe Logs；路径不进 state，也不读环境变量（发布代码不许读）；
//   - Ping：127 点 MLS、-20 dBFS 的注入与回读累积，用 Mls.h 的线性互相关求延迟。接到引擎的原始读头由处理器做；
//   - 面板上的规则：feedback 上限 80 %、Ping 何时可点、恢复的 1.5 s 冷却、选择器的取值；
//   - 探针 state：abi 0 的 RNST 头加一个自定块（group、feedback、D、ceiling）。abi 0 时正式版的加载方只读头、不解块，
//     所以这份字节在正式版里被整份忽略；
//   - 复制诊断：在 DiagReport 的文本后面追加探针几行，日志目录只写 default 或 none，不出现路径的任何片段。
// 线程：除 ProbePing::process 外都只在非实时线程调用。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "diag/DiagReport.h"
#include "diag/ProbeLog.h"
#include "engine/EngineParams.h"
#include "engine/SafetyMachine.h"
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
inline constexpr const char* kDefaultLogDirName = "Rinne Probe Logs";

struct ProbeLogDir
{
    std::string path; // UTF-8；空 = 取不到文档目录，不开日志
};

// 日志目录只有一个来源：documentsDir/Rinne Probe Logs（documentsDir 为空时 path 为空）。没有环境变量这条路；
// 测试要换目录，直接给日志一个目录参数，不经这里。
ProbeLogDir resolveLogDir(std::string_view documentsDir);
// 真文档目录（JUCE）
ProbeLogDir resolveLogDir();
// 诊断里用的名字：有目录写 default，没有写 none。路径的任何片段（用户名、文件夹名）都不进诊断。
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
    bool loopOn = true; // 环路开关，不进探针 state
    // ProbeStateKeeper 正留着正式版写的字节：保存时交回那份字节，面板上的改动不进 state
    bool stateHeld = false;
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
inline constexpr double kPingRampSeconds = engine::safety::kFadeDownSeconds; // 进出 Ping 时干声与回送的斜坡

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

// 一次 Ping 的状态机。音频线程上每个样本给出干声与回送两个缩放系数（0..1，乘在调用方自己的增益上），系数只走线性斜坡，
// 逐样本的变化量不超过 1 / rampSamples()，所以 Ping 进出时增益不会阶跃：
//   1. 进入：干声与回送从 1 用 10 ms（kPingRampSeconds，同安全链的下行淡出）线性降到 0；两个都到 0 之后先静音
//      settleSamples 个样本（start 的参数），再开始注入。静音期间调用方的输出是 0，读头将要读到的环里的旧内容都换成
//      静音：长度要不短于当前 D 加两块，否则注入窗口的前 D 个样本读回的是 Ping 之前的信号，互相关的次峰会盖过真峰；
//      注入与回读窗口从注入的第一个样本起算，窗口长 127 + maxDelaySamples，求出来的延迟在 [0, maxDelaySamples]；
//   2. 回读攒满（或取消）：干声用 10 ms 线性回到 1；回送不自己恢复，状态机发出一次「需要 500 ms 淡入」的请求
//      （takeFadeInRequest），调用方在同一个回调里把它转交给安全链的淡入。请求被取走后再等一个斜坡长度（安全链的
//      下行淡出走完、增益到 0），回送系数才用 10 ms 线性回到 1；此后回送的音量完全由安全链的 500 ms 淡入决定；
//   3. 干声、回送都回到 1 之后状态才变成 Done（取消则回到 Idle）。调用方必须取走淡入请求，否则回送系数一直是 0。
// 取消发生在降、静音、注入、恢复任何一个阶段中途都一样走斜坡：系数从当前值出发，不跳。
// start / cancel / reset / finish 在非实时线程；process 与 takeFadeInRequest 在音频线程，定长循环，不分配、不加锁。
class ProbePing
{
public:
    enum class State : std::uint8_t
    {
        kIdle,
        kArmed, // start 已调用，音频线程还没接手
        kRunning, // 从第一块起到两个系数都回到 1
        kDone, // 回读攒满且系数已恢复：可以 finish
    };

    ProbePing(std::size_t maxDelaySamples, double sampleRate);

    // 空闲或上一次已结束时开始，两个系数降到 0 之后先静音 settleSamples 个样本再注入；正在跑返回 false
    bool start(std::size_t settleSamples = 0) noexcept;
    // 请求取消。音频线程在下一个回调接手，系数走斜坡恢复；结果丢弃，状态回到 Idle。空闲时什么都不做，已 Done 就丢结果
    void cancel() noexcept;
    // 无条件回到空闲。只在音频没有在跑的时候用（prepare / release），音频线程不会再来接手 cancel
    void reset() noexcept;
    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool active() const noexcept
    {
        const State s = state();
        return s == State::kArmed || s == State::kRunning;
    }
    std::size_t rampSamples() const noexcept { return ramp_; }

    // 每个回调调一次（n 为本块样本数）。injection 取得本块要注入的样本（序列放完之后是 0），readback 是读头的原始回读，
    // dryScale / returnScale 取得逐样本的缩放系数（可以为空）。没在跑返回 false，四个缓冲都不动，调用方当系数为 1。
    // n 为 0 时立即返回 false，什么都不推进：不接手 start、不取走取消请求
    bool process(float* injection, const float* readback, float* dryScale, float* returnScale, std::size_t n) noexcept;
    // 音频线程，每个回调在 process 之后调一次：回读攒满或取消之后的第一次返回 true，此后到下一次 Ping 之前都是 false。
    // 返回 true 的那个回调里，调用方对安全链发一次 500 ms 淡入的请求
    bool takeFadeInRequest() noexcept;

    // 状态为 Done 之后在非实时线程调用，结果取走后回到 Idle
    PingResult finish();

private:
    static_assert(std::atomic<State>::is_always_lock_free, "音频线程每块都读 state_");
    static_assert(std::atomic<bool>::is_always_lock_free, "音频线程每块都读 cancel_");

    enum class Phase : std::uint8_t
    {
        kOff,
        kRampDown, // 干声与回送降到 0，不注入
        kSettle, // 两个系数都是 0，等环里的旧内容换成静音，不注入
        kInject, // 注入并累积回读
        kRestore, // 干声回升，回送保持 0
        kHold, // 干声已恢复，等淡入请求被取走并过一个斜坡长度，再放回送
    };

    void beginRun() noexcept;
    void startRestore() noexcept;

    std::vector<float> capture_;
    std::size_t ramp_ = 1;
    // start 在发布 kArmed 之前写，音频线程在接手（读到 kArmed）时读；不在跑的时候音频线程不读它
    std::size_t settleRequest_ = 0;
    std::size_t position_ = 0;
    // 以下只在音频线程动。系数是整数位置 / ramp_（0 = 静音，ramp_ = 原样），每个样本最多挪一格：
    // 不累加浮点，所以斜坡的两头恰好落在 0 和 1 上
    Phase phase_ = Phase::kOff;
    std::size_t dryPos_ = 1;
    std::size_t returnPos_ = 1;
    bool dryUp_ = true; // 目标是 ramp_（否则是 0）
    bool returnUp_ = true;
    bool cancelled_ = false;
    bool fadeInRequested_ = false;
    bool fadeInTaken_ = false;
    std::size_t settleLeft_ = 0;
    std::size_t holdLeft_ = 0;
    std::atomic<State> state_{State::kIdle};
    std::atomic<bool> cancel_{false};
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
    kNotProbe, // 头合格、角色是本端，但不是 abi 0（正式版写的）：探针不解析，由 ProbeStateKeeper 原样留住
    kOtherRole, // 头合格，但角色是对端（另一个插件）的：拒收
    kCorrupt,
};

// abi 0 的 RNST 头（kProbeAbi；加载方只读头）加一个 PRBE 块
std::vector<std::uint8_t> encodeState(const ProbeStateData& data, state::Role role);
// role 是本端的角色，头里的角色不符就拒收（对端存下的字节不该进本端）。失败时 out
// 不变；合格的字节里超范围的数值钳到面板的范围
StateStatus decodeState(const std::uint8_t* data, std::size_t size, state::Role role, ProbeStateData& out) noexcept;

// 工程里的 state 与探针的往返。读到正式版写的字节（kNotProbe）时，探针一个字节也不解析，把整份原字节留住；保存时原样
// 交回这份字节，不用探针自己的 abi 0 state 去覆盖工程里的正式配置（与正式版「高 abi 拒载并原样回写」同一条规则）。
// 留住期间面板上的改动不进 state，保存时会丢（面板的状态文本有一行写明，见 ProbeStatus::stateHeld）。
// 之后读到探针自己的合格字节（kOk）就放掉留住的字节。
// kEmpty、kCorrupt、kOtherRole：什么都没读进来，保持原状。线程：处理器的 state 回调所在的线程，自身不做同步。
class ProbeStateKeeper
{
public:
    explicit ProbeStateKeeper(state::Role role) noexcept : role_(role) {}

    StateStatus load(const std::uint8_t* data, std::size_t size, ProbeStateData& inOut);
    bool holding() const noexcept { return !held_.empty(); }
    // 留着正式字节时是那份原字节，否则是 current 的探针 state
    std::vector<std::uint8_t> save(const ProbeStateData& current) const;

private:
    state::Role role_;
    std::vector<std::uint8_t> held_;
};

// ---- 复制诊断 ----
// DiagReport 的文本，后面追加探针几行。日志目录只给 logDirLabel，不含完整路径、用户名、主机名。
std::string buildDiagText(const diag::DiagInputs& base, const ProbeStatus& status, const ProbeLogDir& logDir,
                          const ProbeSelection& selection);

} // namespace rinne::probe
