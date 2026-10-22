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
//   - 没有字段收原始轨道名：轨道名只以 32 位散列的形式交进来，输出只有 same / different / unknown 加自己那一端
//     的 8 位十六进制散列。这个散列是指纹，不是匿名化：调用方交的是不带盐的 32 位散列，四个可打印 ASCII 字符
//     以内的短名能被字典秒级还原，常见名可撞库还原，独有名字可用来核对猜测。散列只用来比较两端「相同 / 不同」，
//     但报告一旦贴进公开 issue 就收不回来，看的人可以拿它核对猜测；
//   - 自由文本有 11 个 std::string 字段（产品、版本、构建标签、格式、宿主、系统、架构、后端、段名前缀、配对状态、
//     安全状态），写出前一律过 sanitizeText：含路径分隔符、冒号、@、~、% 的片段，整串不要（路径里的目录名可以带
//     空格，前面的词也可能是目录名的一部分），只留 ASCII 字母数字与 . _ + - ( ) 和空格，长度封顶；
//   - 清洗只是兜底：调用方只许交宿主与系统的名称和版本，不许交任何用户或机器自己起的名字，不带分隔符的裸名字
//     清洗不出来。配对状态与安全状态仍是自由文本，接线的一侧要交机读标识，不是用户起的名字；
//   - 事件是编码加两个数，名字来自固定表，不带自由文本。
// 末尾的 ui 段由界面补，不经过本文件的清洗：界面补的那几项必须遵守同一套规则（固定名字表，或过 sanitizeText）。
// 数字一律用整数运算格式化成定点，不经 printf 的浮点，所以与区域设置无关，逐字节可复现。只在非实时线程使用。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "diag/DiagCounters.h"

namespace rinne::diag
{

inline constexpr std::size_t kEventCapacity = 64;
inline constexpr std::size_t kTextMaxLen = 48;

// 清洗一个自由文本：见文件头；有任何一段带路径或账号特征，整串返回空。结果只含 [A-Za-z0-9 ._+()-]，首尾无空格，不超过
// maxLen
std::string sanitizeText(std::string_view raw, std::size_t maxLen = kTextMaxLen);

// 定点格式化：value 四舍五入到 decimals 位（0..6）。NaN、无穷和绝对值超过 1e15 的数都输出 "nan"。负零输出 "0.000"
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

// 事件行的参数约定：组号在头行写字母（groupLetter），在事件行写十进制数；group_set 的来源参数写 DiagOrigin 的
// 数值，名字表 originName 与之一一对应。smart_default 与 resync、trip 的原因序号和 pair_state 的配对状态序号由
// 调用方给，仓里还没有这些序号的真源枚举，所以这里不钉名字表；接线的一侧定下真源后再补，届时这些数值属于
// rinne-diag v1，只许在末尾追加。

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
// 事件按入环顺序输出，不按时间排序：tMs 不保证单调，格式说明也不作此保证
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
// 来源序号：调用方可能把它当作 group_set 事件的 b 传进事件环，事件行会原样写出这个十进制数，所以数值属于
// rinne-diag v1 的格式，只许在末尾追加，不许改已有项的数值，也不在中间插项后重排。名字表（originName）与这里
// 的数值一一对应。
enum class DiagOrigin : std::uint8_t
{
    kNone = 0,
    kSmart = 1,
    kRestore = 2,
    kManual = 3,
};
static_assert(static_cast<std::uint8_t>(DiagOrigin::kNone) == 0u &&
                  static_cast<std::uint8_t>(DiagOrigin::kSmart) == 1u &&
                  static_cast<std::uint8_t>(DiagOrigin::kRestore) == 2u &&
                  static_cast<std::uint8_t>(DiagOrigin::kManual) == 3u,
              "来源序号属于 rinne-diag v1，只许追加");
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
    std::uint32_t maxNSeen =
        0; // 本次 prepare 以来的最大块长，调用方从共享段的 max_n_seen 填；不同于 blocks.nMax（终身）
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

// 轨道名的隐私靠这个类型保证：原始名字是 std::string，DiagInputs 里装不下它，只装得下散列
static_assert(std::is_same_v<decltype(DiagInputs::trackHash), std::uint32_t>, "轨道名只能以 32 位散列交进来");

// 生成文本。每行以 '\n' 结尾；不含 ui 段
std::string buildDiagReport(const DiagInputs& in);

} // namespace rinne::diag
