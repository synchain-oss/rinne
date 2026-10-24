// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针日志底座：把逐块记录与事件写成 scripts/probe/LOG-FORMAT.md 规定的 CSV（H / C / B / S / E / Z 六种行）。
// 格式以那份文档为唯一真源，列名、列序、枚举取值、上界逐字照写；analyze-probe.py 对任何不合格式的行整次作废。
//
// 线程分工：
//   音频线程  只做 onBlock / onAudioEvent / noteTrip：把定长记录推进无锁 SPSC 环，不分配、不加锁、不做 I/O、
//             不抛异常；环满就丢掉新来的这一条并计数（seq 在文件里留下缺口，S 与 Z 的 dropped 同步增加）。
//             调用方写不出合格 B 行的块（onBlock 的 writable 为假）在窗口内按同一套账计丢弃。
//   [P] 线程  本类自带的落盘线程（不靠宿主的消息线程）：排空两条环与排队的行，格式化，写文件，每秒写一条 S，
//             收到 close() 之后写 Z 并关文件。宿主在导出、载入工程、扫描插件时可能很久不跑消息线程，S
//             因此不能挂在它上面。
//   其他线程  beginCase / endCase / logEvent
//   在调用方的线程里取时间戳、格式化好一行，排进带锁的队列（非实时线程才可以用锁）。
// core 不读环境变量：日志目录由调用方经 ProbeLogOptions::directory 传入。
//
// 记录布局：BlockRecord 严格 32 字节（seq / callSeq / monoNs / hostNs），其余列由块末的 BlockSnapshot（另一条
// 32 字节）提供，环里每槽 64 字节。快照里 m、off、w 各占 41 位，n 与 nDecl 各占 21 位，装下契约允许的全部取值。
//
// 单调时间：MonoClock 只有毫秒（Windows 上还是约 16 ms 的刻度），逐块时间戳不够用，所以这里自带纳秒读数
// probeMonoNs()：Windows 用 QueryPerformanceCounter，POSIX 用 CLOCK_MONOTONIC_RAW。它是单调时钟，不是墙钟。
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "diag/ProbeEvents.h"

namespace rinne::diag
{

// ---- 契约里的上界（LOG-FORMAT.md） ----
inline constexpr std::uint32_t kProbeMaxBlock = 1048576; // n、n_decl
inline constexpr std::int64_t kProbeMaxPos = 1099511627776; // |m|、w、|off|
inline constexpr std::uint64_t kProbeMaxD = 2147483648u;
inline constexpr std::uint32_t kProbeMaxBuf = 65536;
inline constexpr std::uint32_t kProbeMaxSampleRate = 768000;
inline constexpr std::uint64_t kProbeMaxPid = 4294967295u;
inline constexpr std::size_t kProbeNameBytes = 200; // 轨道名编码前的字节上限

// 单调纳秒，所有 t_ns 与 mono_ns 都读它
std::uint64_t probeMonoNs() noexcept;

// 计数器刻度换纳秒（Windows 的 QueryPerformanceCounter 用）。freq 为 0 返回 0，不除零；先除后乘，不会随运行时间溢出
std::uint64_t probeTicksToNs(std::uint64_t ticks, std::uint64_t freq) noexcept;

// ---- 实例头 ----
enum class ProbeRole : std::uint8_t
{
    kInput,
    kOutput
};
enum class ProbeFormat : std::uint8_t
{
    kVst3,
    kAu
};
enum class ProbeHost : std::uint8_t
{
    kCubase,
    kLuna,
    kOther
};
enum class ProbeOs : std::uint8_t
{
    kWin,
    kMac
};
enum class ProbeArch : std::uint8_t
{
    kX64,
    kArm64
};

struct ProbeHeader
{
    ProbeRole role = ProbeRole::kInput;
    ProbeFormat format = ProbeFormat::kVst3;
    ProbeHost host = ProbeHost::kOther;
    ProbeOs os = ProbeOs::kWin;
    ProbeArch arch = ProbeArch::kX64;
    std::uint32_t pid = 0;
    std::uint32_t ppid = 0;
    std::string tok4; // 4 位小写十六进制；空 = 打开文件时自动分配
    std::string build; // 提交散列前 8 位（小写十六进制）；不合格式写成 00000000
};

// 按当前平台与进程填好 os、arch、pid、ppid，调用方只给角色、格式、宿主和构建标识。取 ppid 在 Windows 上要遍历进程快照，
// 一个实例取一次。
ProbeHeader makeProbeHeader(ProbeRole role, ProbeFormat format, ProbeHost host, std::string build);

// ---- 测试格 ----
enum class ProbeGuard : std::uint8_t
{
    kOff,
    kLow,
    kNormal,
    kHigh,
    kNa
};

struct ProbeCase
{
    std::string_view caseId; // CB-01 … LU-09
    std::string_view variant; // steady …，不区分写 -
    std::uint32_t buf = 0; // 缓冲区大小（样本）；0 写成 -（不适用）
    ProbeGuard guard = ProbeGuard::kNa;
    std::uint32_t sampleRate = 0;
};

// (case, variant) 是否是契约规则表里的组合；表外的组合分析器判格式错
bool isValidProbeCase(std::string_view caseId, std::string_view variant) noexcept;

// 契约规则表里的全部组合，形如 " CB-01:steady CB-02:toggle … "（首尾与每对之间各一个空格）
std::string_view probeCaseKeys() noexcept;

// ---- 逐块记录 ----
struct BlockRecord
{
    std::uint64_t seq = 0; // 实例级块序号，只对实际写出（或试图写出）的 B 行递增
    std::uint64_t callSeq = 0; // 全局回调顺序号（共享内存头 fetch_add 的结果）
    std::uint64_t monoNs = 0; // 本块开始时的单调纳秒
    std::uint64_t hostNs = 0; // 宿主时间戳；无效时 0
};
static_assert(sizeof(BlockRecord) == 32, "块记录严格 32 字节");

struct BlockSnapshot
{
    // word[0] = m(41 位有符号) | n(21 位) << 41 | playing << 62 | nonrt << 63
    // word[1] = off(41 位有符号) | nDecl(21 位) << 41 | hostNsValid << 62 | bypassed << 63
    // word[2] = w(41 位) | rs(8 位) << 41
    // word[3] = d(32 位) | thread(32 位) << 32
    std::uint64_t word[4] = {};
};
static_assert(sizeof(BlockSnapshot) == 32, "块快照严格 32 字节");

// 环里的一槽：一条记录加它的快照
struct BlockSlot
{
    BlockRecord record;
    BlockSnapshot snapshot;
};
static_assert(sizeof(BlockSlot) == 64, "环槽 64 字节");
static_assert(std::is_trivially_copyable<BlockSlot>::value, "入环只做定长拷贝：不分配、不抛异常");

// 音频线程交进来的一块：自然类型的字段。超出契约上界的值在打包时钳到上界（m、off 钳到 ±(2^40 - 1)），
// 保证写出来的文件永远能被分析器读进去；rs 钳到 255。
struct BlockFields
{
    std::uint64_t callSeq = 0;
    std::uint64_t monoNs = 0;
    std::uint64_t hostNs = 0; // hostNsValid 为假时忽略，写 0
    std::uint32_t n = 0;
    std::uint32_t nDecl = 0;
    std::uint32_t thread = 0; // 线程标识的 32 位散列（写成 8 位十六进制）
    std::uint32_t rs = 0;
    std::uint32_t d = 0;
    std::int64_t m = 0;
    std::uint64_t w = 0;
    std::int64_t off = 0;
    bool hostNsValid = false;
    bool playing = false;
    bool nonrt = false;
    bool bypassed = false;
};

BlockSlot packBlock(std::uint64_t seq, const BlockFields& fields) noexcept;
BlockFields unpackBlock(const BlockSlot& slot) noexcept;

// 线程标识的 32 位散列（FNV-1a 混过一遍），不是原始线程号
std::uint32_t probeThreadHash(std::uint64_t threadToken) noexcept;

// ---- 行格式化（纯函数，追加到 out；行以 LF 结尾） ----
// 文本字段的百分号编码：保留 A-Za-z0-9._~-，其余字节写 %XX；编码前至多 maxBytes 字节，在 UTF-8 字符边界截断
std::string probeEncodeText(std::string_view text, std::size_t maxBytes = kProbeNameBytes);

// 第 1 行（魔数）和第 2 行（H）
void appendProbeMagicAndHeader(std::string& out, const ProbeHeader& header);
// 取值不合契约时返回 false 且 out 不变
bool appendProbeCaseRow(std::string& out, std::uint64_t tNs, const ProbeCase& c);
void appendProbeBlockRow(std::string& out, ProbeRole role, const BlockSlot& slot);

struct ProbeCounters
{
    std::uint64_t blocks = 0; // 累计记录块数（进了环、且没有被落盘一侧丢掉的 B 行）
    // 累计丢弃数：块（环满、文件到上限）、音频事件（事件环满）、排队的行（队列满）一起算，任何丢记录的路径都要计。
    // 所以 blocks + dropped 等于试图记录的块数，只在没有事件和排队行被丢时才成立；块的缺口以 seq 为准
    std::uint64_t dropped = 0;
    std::uint64_t maxN = 0; // 见过的最大块长
    std::uint64_t resync = 0; // 累计重同步
    std::uint64_t trips = 0; // 累计安全跳闸（Panic 除外）
};
void appendProbeSummaryRow(std::string& out, std::uint64_t tNs, const ProbeCounters& counters);
void appendProbeCloseRow(std::string& out, std::uint64_t tNs, std::uint64_t blocksTotal, std::uint64_t droppedTotal);

// 事件参数。数值放 a、b，文本（轨道名）放 text；怎么写成文本由事件表里的参数类型决定
struct ProbeEventArgs
{
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::string_view text;
};
// 事件码不在探针段表里时返回 false 且 out 不变
bool appendProbeEventRow(std::string& out, std::uint64_t tNs, ProbeEventCode code, const ProbeEventArgs& args);

// ---- 文件名与 tok4 ----
// probe-<role>-<pid>-<tok4>.csv，role 为 I 或 O
std::string probeLogFileName(ProbeRole role, std::uint32_t pid, std::string_view tok4);

// 由 (pid, 单调毫秒, 进程内实例计数) 混出 4 位小写十六进制。纯函数：同样的输入同样的输出。
std::string deriveProbeTok4(std::uint32_t pid, std::uint64_t monoMs, std::uint32_t counter);

// 本进程内分配一个没用过的 tok4：实例计数自增、混出候选，候选已被用过就顺延到下一个空位，所以同进程的两个实例一定不同。
// 65536 个全用完之后重新开始（一个进程里不会有这么多探针实例）。
std::string allocateProbeTok4(std::uint32_t pid, std::uint64_t monoMs);

// 为自动分配 tok4 的实例挑一个目录里还不存在的文件：最多向 source 要 attempts 个候选，第一个不存在的就是结果。
// 全部撞上已有文件（别的进程留下的）返回 false，tok4 与 path 不改；调用方因此不会去覆盖别人的日志。
using ProbeTok4Source = std::string (*)(void* context);
bool findFreeProbeLogPath(const std::string& directoryUtf8, ProbeRole role, std::uint32_t pid, int attempts,
                          ProbeTok4Source source, void* sourceContext, std::string& tok4, std::string& pathUtf8);

// ---- 定长单生产者单消费者环 ----
// 容量取 2 的幂；生产者只写 head_，消费者只写 tail_，下标 64 位单调递增再取掩码，所以满和空不会混淆，整个容量都能用。
// 槽的内容用 release / acquire 配对交接：消费者拿到的一定是写完整的一槽。满了 tryPush 返回 false，不覆盖、不阻塞。
template<typename T>
class SpscRing
{
public:
    explicit SpscRing(std::size_t minCapacity)
    {
        std::size_t cap = 2;
        while (cap < minCapacity)
        {
            cap <<= 1;
        }
        mask_ = cap - 1;
        slots_ = std::make_unique<T[]>(cap);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const noexcept { return mask_ + 1; }

    // 生产者线程
    bool tryPush(const T& value) noexcept
    {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity())
        {
            return false;
        }
        slots_[static_cast<std::size_t>(head) & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 生产者线程：现在是否已满。只有消费者会腾出空位，所以返回假之后紧接着的 tryPush 一定成功
    bool full() const noexcept
    {
        return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire) >= capacity();
    }

    // 消费者线程
    bool tryPop(T& out) noexcept
    {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head)
        {
            return false;
        }
        out = slots_[static_cast<std::size_t>(tail) & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // 任意线程，只作诊断
    std::size_t size() const noexcept
    {
        return static_cast<std::size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "音频线程每块都碰 head_");

    std::unique_ptr<T[]> slots_;
    std::size_t mask_ = 0;
    // 两个下标各占一条缓存行，生产者与消费者互不拖累。不用 alignas：MSVC 对被对齐撑大的类型报 C4324，告警即错误
    std::atomic<std::uint64_t> head_{0};
    [[maybe_unused]] char padHead_[64 - sizeof(std::atomic<std::uint64_t>)] = {};
    std::atomic<std::uint64_t> tail_{0};
    [[maybe_unused]] char padTail_[64 - sizeof(std::atomic<std::uint64_t>)] = {};
};

// ---- 探针日志 ----
using ProbeClock = std::uint64_t (*)(void* context) noexcept;

struct ProbeLogOptions
{
    std::string directory; // 日志目录（UTF-8），不存在就创建；由调用方传入
    ProbeHeader header;
    std::size_t blockRingSlots = std::size_t{1} << 18; // 每槽 64 字节：默认 16 MiB，构造时一次分配
    std::size_t audioEventSlots = 256;
    std::size_t pendingLimit = 4096; // 非音频线程排队的行数上限，满了丢弃并计数
    std::uint64_t summaryIntervalNs = 1000000000u; // S 行间隔
    // 单个文件的字节上限。文件按实例一个，不能中途切到新文件（分析器把每个文件当一个实例，新文件会重复 C 行的键），
    // 所以到上限之后不再写 B 行，按丢弃计数；S、E、Z 照写。
    std::uint64_t maxFileBytes = std::uint64_t{512} << 20;
    bool startThread = true; // 假 = 不起 [P] 线程，调用方自己 pump（测试用）
    ProbeClock clock = nullptr; // 空 = probeMonoNs
    void* clockContext = nullptr;
};

class ProbeLog
{
public:
    explicit ProbeLog(ProbeLogOptions options);
    ~ProbeLog();

    ProbeLog(const ProbeLog&) = delete;
    ProbeLog& operator=(const ProbeLog&) = delete;

    // 文件是否已创建且写出了魔数与 H 行。为假时其余调用都安全，数据一律丢弃并计数。
    bool isOpen() const noexcept { return open_; }
    const std::string& tok4() const noexcept { return tok4_; }
    const std::string& filePath() const noexcept { return path_; }

    // ---- 音频线程（同一时刻只能有一个调用者） ----
    // 每个回调调一次。窗口外只更新 S 要用的计数；窗口内把记录推进环。
    // fields.monoNs 是本块开始时读的单调纳秒，必须与 options.clock（缺省 probeMonoNs）同一个时钟：开始时刻早于当前
    // C 行 t_ns 的块（beginCase 落在这块的中途）算窗口外，不写、不占 seq、不计丢弃，保证每个 B 的 mono_ns 都不早于 C 的
    // t_ns。 这条规则的代价是：正好跨在两格交界上的那一块不记。
    // writable 为假：这一块写不出合格的 B 行（例如调用方没拿到 call_seq，写出去就破坏 call_seq 的严格递增），只当丢弃。
    // S 要用的计数（max_n、resync）照常更新；窗口内（同上，开始时刻不早于 C 行 t_ns）占一个 seq、dropped 加一，与环满
    // 走同一套账，文件里的缺口就是它的位置；窗口外什么都不占、不计
    void onBlock(const BlockFields& fields, bool writable = true) noexcept;
    // 音频线程自己发的事件（first_block、first_paired_block、process_mode）。monoNs 由调用方读时钟传入
    void onAudioEvent(ProbeEventCode code, std::uint64_t a, std::uint64_t b, std::uint64_t monoNs) noexcept;
    // 一次安全跳闸（Panic 除外）；任意线程
    void noteTrip() noexcept { trips_.fetch_add(1, std::memory_order_relaxed); }

    // ---- 其他线程 ----
    // 开始一个测试格：先取时间戳、写 C 行、补写状态快照，最后才打开块记录，保证 C.t_ns 不晚于该窗口第一块的 mono_ns。
    // 取值不合契约、已关闭、或排队的行放不下 C 行加全部快照时返回 false，什么都不改（调用方可以稍后重试）。
    bool beginCase(const ProbeCase& c);
    // 关掉块记录。窗口里剩下的块照常排空
    void endCase();
    bool isRecording() const noexcept { return recording_.load(std::memory_order_acquire); }
    // 记一个事件（带发生时的时间戳）；prepare、backend、paired、shm_denied、set_state、track_props 同时记下最新值，
    // 供下一个 C 行之后补写快照
    void logEvent(ProbeEventCode code, const ProbeEventArgs& args = {});

    ProbeCounters counters() const noexcept;

    // [P] 线程的一圈：排空、格式化、写文件、到点写 S。startThread 为真时由内部线程每 20 ms 调一次，调用方不要碰
    void pump();

    // 写 Z、关文件、停线程。幂等；析构会调。调用之后不要再调音频线程的接口
    void close();

private:
    struct StoredEvent
    {
        bool seen = false;
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::string text;
    };
    // 快照里的六种事件，按补写顺序
    static constexpr std::size_t kSnapshotKinds = 6;
    static std::size_t snapshotSlot(ProbeEventCode code) noexcept;

    struct AudioEventSlot
    {
        std::uint64_t tNs = 0;
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::uint64_t code = 0;
    };
    static_assert(sizeof(AudioEventSlot) == 32, "音频事件槽 32 字节");
    static_assert(std::is_trivially_copyable<AudioEventSlot>::value, "入环只做定长拷贝：不分配、不抛异常");

    std::uint64_t now() const noexcept;
    void enqueueLocked(std::string&& line);
    void pumpLocked(bool final);
    void writeOut();
    void dropBlock() noexcept;
    void threadMain();
    void openFile();

    ProbeLogOptions options_;
    ProbeRole role_;
    SpscRing<BlockSlot> blocks_;
    SpscRing<AudioEventSlot> audioEvents_;
    bool open_ = false;
    std::string tok4_;
    std::string path_;
    std::ofstream file_;

    // 音频线程独占
    std::uint64_t nextSeq_ = 0;
    // 计数：音频线程写，其他线程读
    std::atomic<std::uint64_t> blocksPushed_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> maxN_{0};
    std::atomic<std::uint64_t> resync_{0};
    std::atomic<std::uint64_t> trips_{0};
    std::atomic<std::uint64_t> caseStartNs_{0}; // 当前窗口的 C.t_ns；beginCase 在打开 recording_ 之前写
    std::atomic<bool> recording_{false};
    std::atomic<bool> closed_{false};
    // 以上都是音频线程每块要碰的原子量，必须 lock-free（decltype 跟着成员走，改了成员类型断言仍然管着它）
    static_assert(decltype(blocksPushed_)::is_always_lock_free, "音频线程每块都加 blocksPushed_");
    static_assert(decltype(dropped_)::is_always_lock_free, "音频线程丢块时加 dropped_");
    static_assert(decltype(maxN_)::is_always_lock_free, "音频线程每块都读写 maxN_");
    static_assert(decltype(resync_)::is_always_lock_free, "音频线程每块都加 resync_");
    static_assert(decltype(trips_)::is_always_lock_free, "安全跳闸时加 trips_");
    static_assert(decltype(caseStartNs_)::is_always_lock_free, "音频线程每块都读 caseStartNs_");
    static_assert(decltype(recording_)::is_always_lock_free, "音频线程每块都读 recording_");
    static_assert(decltype(closed_)::is_always_lock_free, "音频线程每块都读 closed_");

    // 非实时线程共享，mutex_ 保护
    std::mutex mutex_;
    std::vector<std::string> pending_;
    StoredEvent stored_[kSnapshotKinds];

    // [P] 一侧，pumpMutex_ 保护
    std::mutex pumpMutex_;
    std::vector<std::string> scratch_;
    std::string out_;
    std::uint64_t bytesWritten_ = 0;
    std::uint64_t nextSummaryNs_ = 0;
    bool broken_ = false;
    bool finished_ = false;

    std::mutex closeMutex_;
    std::condition_variable wake_;
    std::mutex wakeMutex_;
    bool stopRequested_ = false;
    std::thread thread_;
};

} // namespace rinne::diag
