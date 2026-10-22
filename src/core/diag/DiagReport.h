// SPDX-License-Identifier: GPL-3.0-or-later
//
// 「复制诊断」的文本生成：rinne-diag v1。key=value 的若干行，加最近 64 条事件；末尾的 ui 段（WebView 版本、缩放、
// 语言）由界面补，这里不写。由 core 单一生成，两个插件共用，Output 只输出字段子集（没有 loop / sync / order /
// safety）。
//
// 格式以本文件与 tests/core/diag 的快照为准。docs/UI_BRIDGE.md 的诊断白名单（diag.* 键）与此尚未对齐：对齐要改
// 桥契约的检查脚本，另行处理，在那之前复制诊断接不到桥上。
//
// 隐私靠结构保证，再靠清洗兜底：
//   - 输入里没有任何能装路径、用户名、主机名或原始轨道名的字段。轨道名只以 32 位散列的形式交进来，输出只有
//     same / different / unknown 加自己那一端的 8 位十六进制散列；
//   - 唯一的自由文本是产品、宿主、系统这几个短串。调用方只许交宿主与系统的名称和版本，不许交任何用户或机器
//     自己起的名字：不带分隔符的裸名字清洗不出来，这一层只是兜底。写出前过 sanitizeText：含路径分隔符、冒号、
//     @、~、% 的片段，整串不要（路径里的目录名可以带空格，前面的词也可能是目录名的一部分），
//     只留 ASCII 字母数字与 . _ + - ( ) 和空格，长度封顶；
//   - 事件是编码加两个数，名字来自固定表，不带自由文本。
// 数字一律用整数运算格式化成定点，不经 printf 的浮点，所以与区域设置无关，逐字节可复现。只在非实时线程使用。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "diag/DiagCounters.h"

namespace rinne::diag
{

inline constexpr std::size_t kEventCapacity = 64;
inline constexpr std::size_t kTextMaxLen = 48;

// 清洗一个自由文本：见文件头。结果只含 [A-Za-z0-9 ._+()-]，首尾无空格，不超过 maxLen
std::string sanitizeText(std::string_view raw, std::size_t maxLen = kTextMaxLen);

// 定点格式化：value 四舍五入到 decimals 位（0..6）。NaN 与无穷输出 "nan"。负零输出 "0.000"
std::string formatFixed(double value, int decimals);

// 8 位小写十六进制
std::string formatHex8(std::uint32_t value);

// 组号 1..8 -> "A".."H"，0 或越界 -> "-"
std::string groupLetter(std::uint32_t group);

enum class EventCode : std::uint8_t
{
    kConstruct, // 无参数
    kPrepare, // a = 采样率，b = 最大块长
    kRelease,
    kSetState, // a = state 的 abi
    kGroupSet, // a = 组号，b = 来源（DiagOrigin）
    kSmartDefault, // a = 组号，b = 原因序号
    kPairState, // a = 配对状态序号
    kPeerLost,
    kConflict, // a = 组号
    kSrMismatch, // a = 本端采样率，b = 对端采样率
    kResync, // a = 原因序号
    kTrip, // a = 原因序号
    kRecover,
    kPanic,
    kLoopOn,
    kLoopOff,
    kBypassOn,
    kBypassOff,
    kNonrealtime, // a = 1 进入离线，0 退出
    kCount, // 哨兵，不是事件码；新码一律加在它前面
};
// 事件码数量：由哨兵导出，名字表长度在 .cpp 里与它断言相等，所以新增码漏改名字表会编译失败
inline constexpr std::size_t kEventCodeCount = static_cast<std::size_t>(EventCode::kCount);

// 码的名字（固定表，ASCII）；越界返回 "unknown"
const char* eventName(EventCode code) noexcept;

struct DiagEvent
{
    std::uint64_t tMs = 0; // 自实例构造起的毫秒
    EventCode code = EventCode::kConstruct;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

// 最近 kEventCapacity 条事件的环，外加累计条数。消息线程使用，不加锁。音频线程的事件经队列转到这里
class DiagEventLog
{
public:
    void push(std::uint64_t tMs, EventCode code, std::uint32_t a = 0, std::uint32_t b = 0) noexcept;
    std::size_t size() const noexcept
    {
        return count_ < kEventCapacity ? static_cast<std::size_t>(count_) : kEventCapacity;
    }
    std::uint64_t total() const noexcept { return count_; }
    // 第 i 条（0 = 仍保留的最旧一条）；越界返回默认值
    DiagEvent at(std::size_t i) const noexcept;

private:
    std::array<DiagEvent, kEventCapacity> ring_{};
    std::uint64_t count_ = 0;
};

enum class DiagRole : std::uint8_t
{
    kInput,
    kOutput,
};
enum class DiagOrigin : std::uint8_t
{
    kNone,
    kSmart,
    kRestore,
    kManual,
};
enum class DiagTrackRelation : std::uint8_t
{
    kUnknown,
    kSame,
    kDifferent,
};
enum class DiagDelayMode : std::uint8_t
{
    kTime,
    kSync,
};
enum class DiagBpmSource : std::uint8_t
{
    kNone,
    kHost,
    kManual,
};
enum class DiagOrderVerdict : std::uint8_t
{
    kUnknown,
    kCorrect,
    kWrong,
};

// 对端的事实；present 为假时输出 "peer: none"
struct DiagPeer
{
    bool present = false;
    DiagRole role = DiagRole::kOutput;
    bool active = false; // 槽是 active（否则 dormant）
    bool samePid = false;
    std::uint32_t version = 0; // packVersion
    std::uint32_t sampleRate = 0;
    std::uint32_t maxBlock = 0;
    std::uint32_t channels = 0;
    DiagTrackRelation name = DiagTrackRelation::kUnknown;
    std::uint64_t heartbeatAgeMs = 0;
};

struct DiagPrepare
{
    std::uint32_t count = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t maxBlock = 0;
    bool nonrealtimeSeen = false;
    bool setStateBeforePrepare = false;
    std::uint32_t resetCalls = 0;
};

// 以下四段只有 Input 输出
struct DiagLoop
{
    double dUserMs = 0.0;
    std::uint32_t dEff = 0; // 样本
    std::uint32_t dMin = 0; // 样本
    DiagDelayMode mode = DiagDelayMode::kTime;
    double bpm = 0.0;
    DiagBpmSource bpmSource = DiagBpmSource::kNone;
    std::uint32_t tapsActive = 0;
    std::uint32_t tapDrop = 0;
};
struct DiagSync
{
    std::uint32_t resync = 0;
    std::uint32_t overflow = 0;
    std::uint32_t torn = 0;
    std::uint32_t overrun = 0;
    std::uint32_t wCasFail = 0;
    std::uint32_t unstable = 0;
    std::uint32_t offDriftBlocks = 0;
    std::uint32_t frozenMax = 0;
};
struct DiagOrder
{
    DiagOrderVerdict verdict = DiagOrderVerdict::kUnknown;
    double votes = 0.0;
    bool hostNsValid = false;
};
struct DiagSafety
{
    std::string state; // 状态机的名字（RUN、TRIPPED……），写出前清洗
    std::uint32_t trips = 0;
    double tailPeakMaxDb = 0.0;
    double limiterGrMaxDb = 0.0;
    std::uint32_t nonfiniteRing = 0;
    std::uint32_t clampRing = 0;
};

struct DiagInputs
{
    DiagRole role = DiagRole::kInput;
    // 自由文本，写出前清洗
    std::string product; // "Rinne Input"
    std::string version; // "0.9.0"
    std::string build; // 构建标签
    std::string format; // "VST3" / "AU"
    std::string host; // 宿主名与版本，不是主机名
    std::string os;
    std::string arch;
    std::string backend;
    std::string prefix; // 段名前缀
    std::uint32_t ipcAbi = 0;
    std::uint32_t group = 0;
    DiagOrigin origin = DiagOrigin::kNone;
    std::uint32_t pid = 0;
    std::string pairState; // 配对状态的机读标识，如 "connected"
    // 自己这一端轨道名的散列（0 = 宿主没给）；原始名字永远不进这里
    std::uint32_t trackHash = 0;
    DiagPeer peer;
    DiagPrepare prepare;
    DiagCountersSnapshot blocks;
    DiagLoop loop;
    DiagSync sync;
    DiagOrder order;
    DiagSafety safety;
    const DiagEventLog* events = nullptr; // 可空
};

// 生成文本。每行以 '\n' 结尾；不含 ui 段
std::string buildDiagReport(const DiagInputs& in);

} // namespace rinne::diag
