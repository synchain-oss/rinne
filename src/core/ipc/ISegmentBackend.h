// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 共享内存段后端的抽象接口与两个平台无关的部分：段头初始化协议、映射句柄的租约与释放。
// 平台差异只有四处，留给子类：建或开（createOrOpen）、只开（openExisting / openExistingReadOnly）、解映射、锁页。
// 名字一律是 Naming 拼好的完整平台名（Windows 的 Local\…、POSIX 的 /…），后端不再加前缀，只在碰操作系统之前再核一遍。
//
// 线程：所有段操作只在持生命周期锁的非实时线程上做；音频线程只经 SegmentHandle::lease() 拿到映射后的裸指针，
// 做原子读写，不碰任何系统调用。
// 寿命：后端与句柄谁先析构都可以（后端析构时进程级的映射表把它忘掉，指着它的映射改用不依赖后端实例的收尾函数）；
// 一个视图只由映射它的那个后端实例解映射。句柄只在插件加载、改组时新建，不按拍新建：每个句柄的控制块
// （约百字节）到进程退出才释放。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>

#include "ipc/IpcDiag.h"
#include "ipc/Naming.h"
#include "ipc/ProcessInfo.h"
#include "ipc/SegmentLayout.h"
#include "util/MonoClock.h"

namespace rinne::ipc
{

// 打开与初始化的结果。除 kOk 外都不许用这个段：kAbiMismatch 显示「版本不兼容」；其余显示「IPC 不可用」、只出干声。
enum class InitResult : std::uint8_t
{
    kOk,
    kAbiMismatch, // magic 是我们的、abi 不同：读得出对方的版本，但绝不写
    kBusy, // 段头还没发布（别人正在初始化，或初始化者死了而接管不了）：立刻解映射，按 RetryBackoff 退避后重试
    kFailed, // 暂时性或未归类的失败（段还不存在、锁等待超时……），解映射后下一拍重试
    kFailedDenied, // 操作系统拒绝（多半是宿主沙盒），本进程内不会自己好
    kFailedBadName, // 段名不合法或超长：配置错误
};

constexpr bool initFailed(InitResult r) noexcept
{
    return r != InitResult::kOk && r != InitResult::kAbiMismatch;
}

// 诊断分类到返回值：拒绝与坏名字各有一个返回值，其余都是可重试的 kFailed
constexpr InitResult initResultFor(IpcDiagCode code) noexcept
{
    switch (code)
    {
    case IpcDiagCode::kShmDenied:
        return InitResult::kFailedDenied;
    case IpcDiagCode::kNameTooLong:
        return InitResult::kFailedBadName;
    case IpcDiagCode::kAbiMismatch:
        return InitResult::kAbiMismatch;
    default:
        return InitResult::kFailed;
    }
}

// 两种段头开头都是 magic、abi、generation 三个 u32（再加一个 u32 凑满 16 字节）；读之前先确认段至少这么大，
// 初始化者也绝不按字节去写这 16 字节
inline constexpr std::size_t kSegmentPrologueBytes = 16;

// magic 的三种状态：0 = 没初始化；kInitMarker = 有人认领了初始化、正在写；其余 = 初始化完成（是不是我们的再比对）
constexpr bool magicPublished(u32 magic) noexcept
{
    return magic != 0 && magic != kInitMarker;
}

// kBusy 之后的重试间隔（契约的一部分）：调用方先解映射，等 nextDelayMs() 毫秒再试；拿到 kBusy 以外的结果就 reset()。
// 第 k 次（从 0 起）在 [25, min(250, 25·2^(k+1))] 毫秒里均匀取值：指数退避，下限 25 ms，封顶 250 ms。
// 抖动让各持有者放手的空档错开相位：初始化者死了而 pid 接管不了时，段要等所有持有者同时放手才消失。
// 不读时钟、不分配、不加锁；种子由调用方给（例如本进程 pid 与实例地址混合），同一个种子给出同一串间隔。
class RetryBackoff
{
public:
    static constexpr u32 kMinDelayMs = 25;
    static constexpr u32 kMaxDelayMs = 250;

    explicit RetryBackoff(u64 seed) noexcept : state_(seed) {}

    u32 nextDelayMs() noexcept
    {
        const u32 ceiling = std::min<u32>(kMaxDelayMs, kMinDelayMs << std::min<u32>(attempt_ + 1, 4));
        if (attempt_ < 8)
        {
            ++attempt_;
        }
        return kMinDelayMs + static_cast<u32>(next() % (ceiling - kMinDelayMs + 1));
    }

    void reset() noexcept { attempt_ = 0; }

private:
    // splitmix64
    u64 next() noexcept
    {
        u64 z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    u64 state_;
    u32 attempt_ = 0;
};

namespace detail
{

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kNoOffset = static_cast<std::size_t>(-1);

// 初始化者在发布之前绝不按字节写的地方：段头的前 16 字节（magic 此刻是初始化标记）与 init_pid（记着初始化者，
// 清掉了别人就无从判定它死没死）
struct ProtectedBytes
{
    std::size_t pidAt = kNoOffset; // init_pid 在段内的偏移；不在视图里时为 kNoOffset

    bool covers(std::size_t at) const noexcept
    {
        return at < kSegmentPrologueBytes || (pidAt != kNoOffset && at >= pidAt && at - pidAt < sizeof(u32));
    }
};

// 建段的初始化者逐页写触碰：此刻只有它在写，段里没有有效数据，页面提前提交，音频线程第一次访问时不缺页。
// 每页写最后一个字节，绕开受保护的字节
inline void writeTouchPages(void* base, std::size_t size, ProtectedBytes keep) noexcept
{
    auto* p = static_cast<volatile unsigned char*>(base);
    for (std::size_t off = 0; off < size; off += kPageSize)
    {
        const std::size_t at = std::min(off + kPageSize, size) - 1;
        if (!keep.covers(at))
        {
            p[at] = 0;
        }
    }
}

// 接管或附着认领的初始化者从 from 清零到段尾，跳过 init_pid（from 不小于 16，前 16 字节本来就不在范围里）
inline void clearBody(void* base, std::size_t from, std::size_t size, ProtectedBytes keep) noexcept
{
    auto* p = static_cast<unsigned char*>(base);
    from = std::max(from, kSegmentPrologueBytes);
    if (keep.pidAt != kNoOffset && keep.pidAt >= from && keep.pidAt < size)
    {
        std::memset(p + from, 0, keep.pidAt - from);
        from = std::min(size, keep.pidAt + sizeof(u32));
    }
    if (from < size)
    {
        std::memset(p + from, 0, size - from);
    }
}

// 附着方逐页只读触碰，严禁写：对端可能正在写环，写一个字节就会清掉样本，读改写又会与并发的原子写构成丢写。
// 只在看到 magic 已发布之后调用（初始化者的清零与写触碰都先于发布），读用单字节的 relaxed 原子读：
// 对端的音频线程此刻可能正在原子地写同一页，两边都是原子访问才不算数据竞争
inline void readTouchPages(const void* base, std::size_t size) noexcept
{
    const auto* p = static_cast<const unsigned char*>(base);
    for (std::size_t off = 0; off < size; off += kPageSize)
    {
        (void)reinterpret_cast<const std::atomic<unsigned char>*>(p + off)->load(std::memory_order_relaxed);
    }
}
static_assert(std::atomic<unsigned char>::is_always_lock_free && sizeof(std::atomic<unsigned char>) == 1);

} // namespace detail

// 一次映射的视图（只能移动）。所有权与解映射由 SegmentHandle 承载；裸视图没提升成句柄就丢掉时，映射留到进程退出。
struct SegmentView
{
    void* base = nullptr;
    std::size_t size = 0; // 真实段大小：同名段已存在时请求大小被忽略，后端一律回读操作系统给的值
    void* mapping = nullptr; // 平台句柄（Win32 的 HANDLE、POSIX 后端的私有记录），不透明
    bool created = false; // 本次调用是否新建了这个段
    std::string name; // 平台段名，只供诊断

    SegmentView() = default;
    SegmentView(SegmentView&& other) noexcept { *this = std::move(other); }
    SegmentView& operator=(SegmentView&& other) noexcept
    {
        if (this != &other)
        {
            base = std::exchange(other.base, nullptr);
            size = std::exchange(other.size, 0);
            mapping = std::exchange(other.mapping, nullptr);
            created = std::exchange(other.created, false);
            name = std::move(other.name);
            other.name.clear();
        }
        return *this;
    }
    SegmentView(const SegmentView&) = delete;
    SegmentView& operator=(const SegmentView&) = delete;
    ~SegmentView() = default;

    // 只清字段，不解映射（视图不持有后端）；解映射由 SegmentHandle 经后端在非实时线程做
    void reset() noexcept
    {
        base = nullptr;
        size = 0;
        mapping = nullptr;
        created = false;
        name.clear();
    }
};

// 段头规格：magic 与 abi 由调用方给（会合段与数据段各有一套），后端只按协议比对
struct SegmentHeaderSpec
{
    u32 magic = 0;
    u32 abi = 0;
    std::size_t layoutBytes = 0; // 本 abi 的段大小；真实大小小于它就不用（abi 相同时）
    // 接管或附着认领时从这里清零到段尾；小于 16 时按 16 算，前 16 字节与 init_pid 从不按字节写
    std::size_t clearFrom = 0;
};

// 初始化协议用到的段头字段，都指向视图里的段头。initPid 为空时没有接管，初始化者死了只能等所有持有者放手
struct SegmentHeaderFields
{
    std::atomic<u32>* magic = nullptr;
    std::atomic<u32>* abi = nullptr;
    std::atomic<u32>* generation = nullptr;
    std::atomic<u32>* initPid = nullptr;
};

inline SegmentHeaderFields headerFields(RvHeader& h) noexcept
{
    return {&h.magic, &h.abi, &h.generation, &h.init_pid};
}

inline SegmentHeaderFields headerFields(RingHeader& h) noexcept
{
    return {&h.magic, &h.abi, &h.generation, &h.init_pid};
}

// 不经后端实例解映射一个视图：后端先于它映射出去、还没解映射的视图析构时，映射表到时用它收尾。
// 只许用视图里的字段，不许碰后端的状态。
using OrphanUnmap = void (*)(SegmentView& view) noexcept;

class ISegmentBackend
{
public:
    // 析构时让映射表忘掉自己：指着自己、还没解映射的映射（句柄还活着的、交给回收表的）改用 orphanUnmapper() 收尾。
    // 定义在文件末尾。具体后端在自己的析构函数开头先调 forgetRetired()（派生部分析构之后就不能再经它解映射了），
    // 这里再兜一次。后端析构与句柄的释放、回收都在持生命周期锁的非实时线程上。
    virtual ~ISegmentBackend();

    // 建或开命名段。created = true 时内核已把整段清零；created = false 时要经 initHeader 判定能不能用。
    virtual InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) = 0;

    // 只开已存在的段，不建；段不存在时安静地返回 kFailed（25 Hz 重试路径，不刷诊断）
    virtual InitResult openExisting(const std::string& name, SegmentView& view) = 0;

    // 只读打开（只读探测用的最小权限）；没有权限区分的后端退回 openExisting
    virtual InitResult openExistingReadOnly(const std::string& name, SegmentView& view)
    {
        return openExisting(name, view);
    }

    // 解映射并关平台句柄；之后 view 置空。noexcept：析构与回收路径都调它，内部取锁失败（std::system_error）
    // 按不可恢复处理
    virtual void unmap(SegmentView& view) noexcept = 0;

    // 后端先没了时收尾用的解映射（见 OrphanUnmap）；没有要收尾的东西时返回 nullptr（进程内后端）
    virtual OrphanUnmap orphanUnmapper() const noexcept { return nullptr; }

    // 初始化者写触碰之后锁页，尽力而为：失败只报诊断、不失败。附着方不锁。
    virtual void tryLock(const SegmentView& view) { (void)view; }

    // initHeader 里的两个交错点（见 onInitStageForTesting）：看到 magic 为 0、马上认领之前；看到死掉的初始化者、
    // 马上接管之前
    enum class InitStage : std::uint8_t
    {
        kBeforeClaim,
        kBeforeTakeover,
    };

    // 只读附着方的段头校验：只读触碰，单次 acquire 读 magic 与 abi，绝不写。参数是 const 引用，类型上就写不了，
    // 落在只读映射上也不会崩。还没初始化完（magic 为 0 或初始化标记）→ kBusy，不报诊断。
    InitResult checkHeaderReadOnly(const SegmentView& view, const std::atomic<u32>& magic, const std::atomic<u32>& abi,
                                   const SegmentHeaderSpec& spec) const
    {
        if (view.base == nullptr || view.size < kSegmentPrologueBytes)
        {
            return InitResult::kFailed;
        }
        const u32 m = magic.load(std::memory_order_acquire);
        if (!magicPublished(m))
        {
            return InitResult::kBusy;
        }
        detail::readTouchPages(view.base, view.size);
        return verdict(view, m, abi.load(std::memory_order_acquire), spec);
    }

    // 段头初始化协议（会合段与数据段通用）。从不阻塞、从不睡眠；谁来初始化只由 CAS 决定：
    //   magic 为 0       建段者，或者 allowOverwrite 的附着方：CAS 把 magic 从 0 换成初始化标记 kInitMarker，赢的一方
    //                    是唯一的初始化者，立刻写下自己的 pid（init_pid），然后初始化：建段者（内核刚清零的新段）逐页
    //                    写触碰并锁页，附着方从 clearFrom 清零到段尾；再写 abi、generation + 1、initData()，最后以
    //                    release 发布 magic。认领先于任何写，magic 为 0 的段从没被谁用过，所以附着方抢在慢的建段者
    //                    前面认领也无害；输了 CAS 的一方从不清零。
    //   初始化标记        别人认领了。init_pid 非 0、不是本进程、并且已经死了：allowOverwrite 的一方用 CAS 把 init_pid
    //                    从死者换成自己来接管，赢了就按附着方的办法重初始化（generation 再 + 1）、发布；输了、
    //                    初始化者还活着、pid 还没写下、探活拿不准（一律按活）：kBusy（allowOverwrite 时诊断 kBusy）。
    //   已发布           只读触碰（只在看到发布之后）→ 比对 magic、abi 与真实大小（不是我们的 magic → kFailed；
    //                    abi 不同 → kAbiMismatch；段比本 abi 的布局小 → kFailed）。
    // kBusy：调用方立刻解映射，按 RetryBackoff 退避后重试，不得在原映射上重试。初始化者死了而接管不了时（pid 被复用、
    // 拿不准），靠所有持有者同时放手让段消失（Windows 随最后一个句柄回收，POSIX 由下一个建段者清掉残段）：每次尝试只
    // 映射几微秒，再加上抖动，各持有者放手的空档总会重叠。
    // initData 不得抛出：抛出时 magic 停在初始化标记、init_pid 是活着的本进程，只能等所有持有者放手。
    // 只读探测的一方一律 allowOverwrite = false：只初始化自己新建的段，不认领别人建的、不接管，看到初始化标记
    // 也只返回 kBusy、不报诊断。
    InitResult initHeader(SegmentView& view, const SegmentHeaderFields& h, const SegmentHeaderSpec& spec,
                          const std::function<void()>& initData = {}, bool allowOverwrite = true)
    {
        if (view.base == nullptr || h.magic == nullptr || h.abi == nullptr)
        {
            return InitResult::kFailed;
        }
        if (view.size < kSegmentPrologueBytes || (view.created && view.size < spec.layoutBytes))
        {
            report(view, IpcDiagCode::kBadSize, "segment is smaller than its header or layout");
            return InitResult::kFailed;
        }
        detail::ProtectedBytes keep;
        if (h.initPid != nullptr)
        {
            const auto at = reinterpret_cast<std::uintptr_t>(h.initPid) - reinterpret_cast<std::uintptr_t>(view.base);
            keep.pidAt = at < view.size ? static_cast<std::size_t>(at) : detail::kNoOffset;
        }
        const u32 self = currentProcessId();
        const auto initialise = [&](bool fresh) {
            if (fresh)
            {
                detail::writeTouchPages(view.base, view.size, keep);
                tryLock(view);
            }
            else
            {
                detail::clearBody(view.base, spec.clearFrom, view.size, keep);
            }
            h.abi->store(spec.abi, std::memory_order_relaxed);
            if (h.generation != nullptr)
            {
                h.generation->fetch_add(1, std::memory_order_relaxed);
            }
            if (initData)
            {
                initData();
            }
            h.magic->store(spec.magic, std::memory_order_release);
            return InitResult::kOk;
        };
        const auto tooSmallToInitialise = [&] {
            if (view.size >= spec.layoutBytes)
            {
                return false;
            }
            report(view, IpcDiagCode::kBadSize, "unpublished segment is smaller than the layout; not initialising");
            return true;
        };

        u32 m = h.magic->load(std::memory_order_acquire);
        if (m == 0 && (view.created || allowOverwrite))
        {
            if (tooSmallToInitialise())
            {
                return InitResult::kFailed;
            }
            onInitStageForTesting(InitStage::kBeforeClaim, view);
            if (h.magic->compare_exchange_strong(m, kInitMarker, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (h.initPid != nullptr)
                {
                    h.initPid->store(self, std::memory_order_release);
                }
                return initialise(view.created);
            }
            // 输了：m 是别人写下的值（初始化标记或已发布），往下按它判定
        }
        if (m == kInitMarker && allowOverwrite && h.initPid != nullptr)
        {
            const u32 owner = h.initPid->load(std::memory_order_acquire);
            if (owner != 0 && owner != self && !isProcessAlive(owner))
            {
                if (tooSmallToInitialise())
                {
                    return InitResult::kFailed;
                }
                onInitStageForTesting(InitStage::kBeforeTakeover, view);
                u32 expected = owner;
                if (h.initPid->compare_exchange_strong(expected, self, std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
                {
                    return initialise(false);
                }
            }
            m = h.magic->load(std::memory_order_acquire); // 接管输了时，胜者可能已经发布
        }
        if (!magicPublished(m))
        {
            if (m == kInitMarker && allowOverwrite) // 只读探测的一方不报诊断（25 Hz 重试路径）
            {
                report(view, IpcDiagCode::kBusy, "segment is being initialised by another holder; unmap and back off");
            }
            return InitResult::kBusy;
        }
        detail::readTouchPages(view.base, view.size);
        return verdict(view, m, h.abi->load(std::memory_order_acquire), spec);
    }

protected:
    // 只给单测的后端覆盖，在两个交错点上停住调用线程，确定性地构造竞争（发布代码不覆盖，contract-static-ipc 扫描
    // src/** 判红）
    virtual void onInitStageForTesting(InitStage stage, const SegmentView& view) noexcept
    {
        (void)stage;
        (void)view;
    }

    // 让映射表忘掉本后端：具体后端的析构函数开头调用。幂等。定义在文件末尾
    void forgetRetired() noexcept;

    // 后端在碰操作系统之前核对名字：不合格时报诊断（超长归 kNameTooLong）并返回 kFailedBadName，合格返回 kOk
    static InitResult checkName(NamePlatform platform, const std::string& name) noexcept
    {
        const NameStatus status = validateSegmentName(platform, name);
        if (status == NameStatus::kOk)
        {
            return InitResult::kOk;
        }
        IpcDiagEvent e;
        e.op = IpcDiagOp::kName;
        e.code = status == NameStatus::kTooLong ? IpcDiagCode::kNameTooLong : IpcDiagCode::kOther;
        e.segment = name.c_str();
        e.detail = status == NameStatus::kTooLong ? "segment name longer than 31 characters"
                                                  : "segment name has a bad platform head or characters";
        reportIpcDiag(e);
        return InitResult::kFailedBadName;
    }

private:
    static void report(const SegmentView& view, IpcDiagCode code, const char* detail) noexcept
    {
        IpcDiagEvent e;
        e.op = IpcDiagOp::kHeader;
        e.code = code;
        e.segment = view.name.c_str();
        e.detail = detail;
        reportIpcDiag(e);
    }

    // magic 已发布时的判定
    static InitResult verdict(const SegmentView& view, u32 m, u32 a, const SegmentHeaderSpec& spec) noexcept
    {
        if (m != spec.magic)
        {
            report(view, IpcDiagCode::kForeign, "segment header magic is not ours");
            return InitResult::kFailed;
        }
        if (a != spec.abi)
        {
            report(view, IpcDiagCode::kAbiMismatch, "segment was created by an incompatible abi");
            return InitResult::kAbiMismatch;
        }
        if (view.size < spec.layoutBytes)
        {
            report(view, IpcDiagCode::kBadSize, "segment is smaller than the layout of its abi");
            return InitResult::kFailed;
        }
        return InitResult::kOk;
    }
};

namespace detail
{

// 映射控制块：持有视图与租约计数。音频线程只经裸指针增减 leaseCount、读 view.base；其余字段只在非实时线程上、
// 持映射表的锁时碰。
// 控制块一经分配就到进程退出才释放：解映射之后它仍留在映射表里（约百字节）。这样 lease() 里「读到裸指针」与
// 「计数 + 1」之间不管被挂起多久，加计数时碰到的都是活着的内存；随后看到 releaseRequested 就退还计数，
// 从不去读已经解映射的 base。两道保护都不再依赖时间。
struct SegmentMapping
{
    enum class Bucket : std::uint8_t
    {
        kLive, // 句柄还独占着它
        kPending, // 句柄放下了，宽限期还没走完
        kDone, // 已解映射，留到进程退出
    };

    SegmentView view;
    std::atomic<u32> leaseCount{0};
    std::atomic<bool> releaseRequested{false};
    ISegmentBackend* backend = nullptr; // 解映射用；解映射之后、或后端先没了，为空
    OrphanUnmap orphanUnmap = nullptr; // 后端先没了时的收尾，建句柄时向后端要好
    u64 graceStartMs = 0;
    bool graceStarted = false;
    bool unmapped = false;
    Bucket bucket = Bucket::kLive;
    SegmentMapping* prev = nullptr; // 在映射表里的双向链
    SegmentMapping* next = nullptr;

    SegmentMapping(SegmentView&& v, ISegmentBackend* b)
        : view(std::move(v)), backend(b), orphanUnmap(b != nullptr ? b->orphanUnmapper() : nullptr)
    {
    }
};
static_assert(std::atomic<bool>::is_always_lock_free, "音频线程在 lease() 里读释放请求");
static_assert(std::atomic<SegmentMapping*>::is_always_lock_free, "音频线程在 lease() 里读控制块指针");

// 宽限期的判定：租约归零之后再过 500 ms。时钟读出 0 时既不开始也不推进；时间倒退时从现在重新计，
// 绝不因为无符号相减回绕而提前结束。
constexpr u64 kReleaseGraceMs = 500;

inline bool graceElapsed(SegmentMapping& m, u64 nowMs) noexcept
{
    if (m.leaseCount.load(std::memory_order_seq_cst) != 0) // 握手的非实时一侧，见 SegmentHandle::lease()
    {
        return false;
    }
    if (nowMs == 0)
    {
        return false;
    }
    if (!m.graceStarted || nowMs < m.graceStartMs)
    {
        m.graceStarted = true;
        m.graceStartMs = nowMs;
        return false;
    }
    return nowMs - m.graceStartMs >= kReleaseGraceMs;
}

// 宽限期满就解映射、关平台句柄：后端还在就经后端，后端先没了就用建句柄时要好的收尾函数。控制块不释放
inline bool unmapIfReady(SegmentMapping& m, u64 nowMs) noexcept
{
    if (m.unmapped)
    {
        return true;
    }
    if (!graceElapsed(m, nowMs))
    {
        return false;
    }
    if (m.backend != nullptr)
    {
        m.backend->unmap(m.view);
    }
    else if (m.orphanUnmap != nullptr && m.view.base != nullptr)
    {
        m.orphanUnmap(m.view);
    }
    m.view.reset();
    std::string().swap(m.view.name);
    m.backend = nullptr;
    m.unmapped = true;
    return true;
}

// 进程级的映射表（每个链接了 rinne_core 的二进制一张，永不析构），登记每一个句柄建出的控制块：
//   live     句柄还独占着；
//   pending  句柄析构或被覆盖时宽限期还没走完，由 reap() 在之后的某一拍走完宽限期再解映射；
//   done     已解映射，留到进程退出。
// 后端析构时 forget() 把 live 与 pending 里指着它的都改成用收尾函数解映射、关平台句柄，不靠后端实例。
// 侵入式双向链，插入与摘除不分配；一把锁，只在非实时线程上取（取锁失败按不可恢复处理）。
// 泄漏检测把这些控制块当可达内存。
class MappingRegistry
{
public:
    using Bucket = SegmentMapping::Bucket;

    static MappingRegistry& instance() noexcept
    {
        static MappingRegistry* table = new MappingRegistry();
        return *table;
    }

    void adopt(SegmentMapping* m) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        push(Bucket::kLive, m);
    }

    // 句柄的释放：宽限期满就解映射，控制块挪进 done
    bool releaseLive(SegmentMapping& m, u64 nowMs) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!unmapIfReady(m, nowMs))
        {
            return false;
        }
        moveTo(Bucket::kDone, m);
        return true;
    }

    // 句柄放下了还没释放完的控制块：交给回收表
    void retire(SegmentMapping& m) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        moveTo(m.unmapped ? Bucket::kDone : Bucket::kPending, m);
    }

    std::size_t reap(u64 nowMs) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::size_t unmapped = 0;
        for (SegmentMapping* m = head(Bucket::kPending); m != nullptr;)
        {
            SegmentMapping* const next = m->next;
            if (unmapIfReady(*m, nowMs))
            {
                moveTo(Bucket::kDone, *m);
                ++unmapped;
            }
            m = next;
        }
        return unmapped;
    }

    // 后端析构：句柄还活着的、交给回收表的映射都不再经它解映射，到时用收尾函数（orphanUnmap）解映射
    void forget(const ISegmentBackend* backend) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Bucket b : {Bucket::kLive, Bucket::kPending})
        {
            for (SegmentMapping* m = head(b); m != nullptr; m = m->next)
            {
                if (m->backend == backend)
                {
                    m->backend = nullptr;
                }
            }
        }
    }

    std::size_t count(Bucket b) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const SegmentMapping* m = head(b); m != nullptr; m = m->next)
        {
            ++n;
        }
        return n;
    }

private:
    MappingRegistry() = default;

    SegmentMapping*& head(Bucket b) noexcept { return heads_[static_cast<std::size_t>(b)]; }

    void push(Bucket b, SegmentMapping* m) noexcept
    {
        m->bucket = b;
        m->prev = nullptr;
        m->next = head(b);
        if (m->next != nullptr)
        {
            m->next->prev = m;
        }
        head(b) = m;
    }

    void moveTo(Bucket b, SegmentMapping& m) noexcept
    {
        if (m.prev != nullptr)
        {
            m.prev->next = m.next;
        }
        else
        {
            head(m.bucket) = m.next;
        }
        if (m.next != nullptr)
        {
            m.next->prev = m.prev;
        }
        push(b, &m);
    }

    std::mutex mutex_;
    SegmentMapping* heads_[3] = {nullptr, nullptr, nullptr};
};

} // namespace detail

inline ISegmentBackend::~ISegmentBackend()
{
    forgetRetired();
}

inline void ISegmentBackend::forgetRetired() noexcept
{
    detail::MappingRegistry::instance().forget(this);
}

// 映射句柄：独占控制块 + 按块租约 + 宽限期释放。
//   - 控制块只在非实时线程上碰；音频线程经原子裸指针 implPtr_ 拿到它，增减 leaseCount。
//   - release(now)：先置 releaseRequested，再摘掉 implPtr_（不再发放新租约），然后在 leaseCount 归零、且又过了
//     kReleaseGraceMs 之后经后端解映射。控制块不释放，留在映射表里到进程退出。
//   - 析构、或者移动赋值覆盖一个还没释放完的句柄：映射交给回收表，由 reapRetired() 在之后的某一拍走完宽限期再
//     解映射、关平台句柄（插件卸载时这是常态）。持生命周期锁的非实时线程要定时调用 reapRetired()（会合层的 25 Hz
//     定时器）；任何句柄的 release() 也顺手回收一次。
//   - 后端先于句柄析构也可以（例如句柄与后端是同一个对象的成员、句柄声明在前）：到时用后端事先给的收尾函数
//     解映射、关平台句柄。
//   - 只在插件加载、改组时新建，不按拍新建：控制块永不释放，按拍新建会让内存线性增长（retiredDone() 供诊断）。
// 时间由调用方传入，必须是 util::monoNowMs() 那一个跨进程单调时钟。
class SegmentHandle
{
public:
    // 两个 4 Hz 心跳的间隔，远大于宿主的最大块时长
    static constexpr u64 kReleaseGraceMs = detail::kReleaseGraceMs;

    SegmentHandle() = default;

    // 接管视图（createOrOpen / openExisting 与 initHeader 之后，非实时线程）
    SegmentHandle(SegmentView&& view, ISegmentBackend* backend)
        : impl_(new detail::SegmentMapping(std::move(view), backend))
    {
        detail::MappingRegistry::instance().adopt(impl_);
        implPtr_.store(impl_, std::memory_order_release);
    }

    ~SegmentHandle() { abandon(); }

    SegmentHandle(const SegmentHandle&) = delete;
    SegmentHandle& operator=(const SegmentHandle&) = delete;

    // 移动转移所有权；移动赋值先放下自己原来的那一块（走不完宽限期的交给回收表）
    SegmentHandle(SegmentHandle&& o) noexcept { takeFrom(o); }
    SegmentHandle& operator=(SegmentHandle&& o) noexcept
    {
        if (this != &o)
        {
            abandon();
            takeFrom(o);
        }
        return *this;
    }

    // 下面三个只读 implPtr_；不持租约时只许非实时线程用（它是唯一的解映射者，不和自己并发）。
    // 音频线程一律经 Lease::base() 访问。
    bool valid() const { return implPtr_.load(std::memory_order_acquire) != nullptr; }
    void* base() const
    {
        auto* p = implPtr_.load(std::memory_order_acquire);
        return p != nullptr ? p->view.base : nullptr;
    }
    std::size_t size() const
    {
        auto* p = implPtr_.load(std::memory_order_acquire);
        return p != nullptr ? p->view.size : 0;
    }

    // 音频线程的按块租约：持有期间保证不解映射。块首 lease()，块末析构归还。
    class Lease
    {
    public:
        Lease() = default;
        ~Lease()
        {
            if (p_ != nullptr)
            {
                p_->leaseCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
        Lease& operator=(Lease&& o) noexcept
        {
            if (this != &o)
            {
                if (p_ != nullptr)
                {
                    p_->leaseCount.fetch_sub(1, std::memory_order_acq_rel);
                }
                p_ = o.p_;
                o.p_ = nullptr;
            }
            return *this;
        }
        explicit operator bool() const { return p_ != nullptr; }
        void* base() const { return p_ != nullptr ? p_->view.base : nullptr; }
        std::size_t size() const { return p_ != nullptr ? p_->view.size : 0; }

    private:
        friend class SegmentHandle;
        // 计数在构造之前由 lease() 加好；这里只拿裸指针
        explicit Lease(detail::SegmentMapping* p) : p_(p) {}
        detail::SegmentMapping* p_ = nullptr;
    };

    // 音频线程每块调用：读裸指针 → leaseCount + 1 → 查 releaseRequested，已置就退还并返回空租约。
    // 与 release() 构成 Dekker 式握手：音频「加计数，再读请求」，非实时线程「置请求，再读计数」。
    // 四个访问（finishLease 的 fetch_add 与 load、release() 的 store、graceElapsed() 的 load）一律 seq_cst：
    // 只用 acquire / release 时，C++ 允许两边的 store→load 重排、同时读到旧值，音频拿到租约的同时
    // 非实时线程判「没有在途租约」开始计宽限期。四个都在同一个全序里时，至少有一边看得见另一边。
    // 代价：x86 上只有 release() 的那条 store 从 mov 变成 xchg（不在音频热路径）；arm64 上两条 load 编成 LDAR，
    // 不会被编成可以越过前面 STLR / LDADDAL 提前读的 LDAPR。
    // 读到裸指针之后被挂起多久都不要紧：控制块永不释放，加计数总落在活着的内存上，之后看到释放请求就退还，
    // 不去读已经解映射的 base。
    Lease lease() const { return finishLease(implPtr_.load(std::memory_order_acquire)); }

    // 非实时线程请求释放。返回 true = 已解映射；false = 租约在途或宽限期未满，下一拍再调。幂等。
    // 顺手回收一次被丢下的映射。
    bool release(u64 nowMs)
    {
        auto& table = detail::MappingRegistry::instance();
        table.reap(nowMs);
        if (impl_ == nullptr)
        {
            return true;
        }
        impl_->releaseRequested.store(true, std::memory_order_seq_cst); // 握手的这一侧，见 lease()
        implPtr_.store(nullptr, std::memory_order_release);
        if (!table.releaseLive(*impl_, nowMs))
        {
            return false;
        }
        impl_ = nullptr; // 已解映射，控制块留到进程退出
        return true;
    }

    // 走完被丢下的映射的宽限期并解映射；返回这一次解映射的个数。持生命周期锁的非实时线程定时调用。
    static std::size_t reapRetired(u64 nowMs) { return detail::MappingRegistry::instance().reap(nowMs); }

    // 诊断与单测用：还在等宽限期的被丢下的映射数；已解映射、留到进程退出的控制块数（按拍新建句柄时它会一直涨）
    static std::size_t retiredPending()
    {
        return detail::MappingRegistry::instance().count(detail::SegmentMapping::Bucket::kPending);
    }
    static std::size_t retiredDone()
    {
        return detail::MappingRegistry::instance().count(detail::SegmentMapping::Bucket::kDone);
    }

private:
    friend struct LeaseSeamForTesting;

    // lease() 的第二步，握手的音频一侧：加计数，再查释放请求。单独拆出来只为单测在「读到裸指针」与这一步之间
    // 插入完整的释放与解映射
    static Lease finishLease(detail::SegmentMapping* p) noexcept
    {
        if (p == nullptr)
        {
            return Lease{};
        }
        p->leaseCount.fetch_add(1, std::memory_order_seq_cst);
        if (p->releaseRequested.load(std::memory_order_seq_cst))
        {
            p->leaseCount.fetch_sub(1, std::memory_order_acq_rel);
            return Lease{};
        }
        return Lease(p);
    }

    // 放下自己的那一块：这一拍能释放完就释放，否则交给回收表。noexcept：映射表的锁取不到（std::system_error）
    // 与 unmap 一样按不可恢复处理
    void abandon() noexcept
    {
        if (impl_ == nullptr)
        {
            return;
        }
        if (!release(util::monoNowMs()) && impl_ != nullptr)
        {
            detail::MappingRegistry::instance().retire(*impl_);
        }
        impl_ = nullptr;
    }

    void takeFrom(SegmentHandle& o) noexcept
    {
        impl_ = std::exchange(o.impl_, nullptr);
        implPtr_.store(o.implPtr_.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
    }

    detail::SegmentMapping* impl_ = nullptr; // 独占的控制块；只在非实时线程上碰
    std::atomic<detail::SegmentMapping*> implPtr_{nullptr}; // 音频线程经它拿裸指针；release 之后为空
};

} // namespace rinne::ipc
