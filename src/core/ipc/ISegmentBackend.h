// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 共享内存段后端的抽象接口与两个平台无关的部分：段头初始化协议、映射句柄的租约与释放。
// 平台差异只有四处，留给子类：建或开（createOrOpen）、只开（openExisting / openExistingReadOnly）、解映射、锁页。
// 名字一律是 Naming 拼好的完整平台名（Windows 的 Local\…、POSIX 的 /…），后端不再加前缀。
//
// 线程：所有段操作只在持生命周期锁的非实时线程上做；音频线程只经 SegmentHandle::lease() 拿到映射后的裸指针，
// 做原子读写，不碰任何系统调用。
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "ipc/IpcDiag.h"
#include "ipc/SegmentLayout.h"
#include "util/MonoClock.h"

namespace rinne::ipc
{

// 打开与初始化的结果。除 kOk 外都不许用这个段：kAbiMismatch 显示「版本不兼容」；其余显示「IPC 不可用」、只出干声。
enum class InitResult : std::uint8_t
{
    kOk,
    kAbiMismatch, // magic 是我们的、abi 不同：读得出对方的版本，但绝不写
    kFailed, // 暂时性或未归类的失败（含段还不存在、创建者还没初始化完），下一拍重试
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

// 两种段头开头都是 magic、abi、generation 三个 u32；读这 16 字节之前先确认段至少这么大
inline constexpr std::size_t kSegmentPrologueBytes = 16;

namespace detail
{

constexpr std::size_t kPageSize = 4096;

// 创建者逐页写触碰：此刻段里没有有效数据（内核已清零），写安全；页面提前提交，音频线程第一次访问时不缺页
inline void writeTouchPages(void* base, std::size_t size) noexcept
{
    auto* p = static_cast<volatile unsigned char*>(base);
    for (std::size_t off = 0; off < size; off += kPageSize)
    {
        p[off] = 0;
    }
}

// 附着方逐页只读触碰，严禁写：对端可能正在写环，写一个字节就会清掉样本，读改写又会与并发的原子写构成丢写
inline void readTouchPages(const void* base, std::size_t size) noexcept
{
    const auto* p = static_cast<const volatile unsigned char*>(base);
    for (std::size_t off = 0; off < size; off += kPageSize)
    {
        (void)p[off];
    }
}

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
    std::size_t clearFrom = 0; // 覆盖式重初始化从这里清零到段尾
};

class ISegmentBackend
{
public:
    virtual ~ISegmentBackend() = default;

    // 建或开命名段。created = true 时内核已把整段清零；created = false 时要经 initHeader 判定能不能用。
    virtual InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) = 0;

    // 只开已存在的段，不建；段不存在时安静地返回 kFailed（25 Hz 重试路径，不刷诊断）
    virtual InitResult openExisting(const std::string& name, SegmentView& view) = 0;

    // 只读打开（只读探测用的最小权限）；没有权限区分的后端退回 openExisting
    virtual InitResult openExistingReadOnly(const std::string& name, SegmentView& view)
    {
        return openExisting(name, view);
    }

    // 解映射并关平台句柄；之后 view 置空
    virtual void unmap(SegmentView& view) = 0;

    // 创建者写触碰之后锁页，尽力而为：失败只报诊断、不失败。附着方不锁。
    virtual void tryLock(const SegmentView& view) { (void)view; }

    // 只读附着方的段头校验：只读触碰，单次 acquire 读 magic 与 abi，绝不写。参数是 const 引用，类型上就写不了，
    // 落在只读映射上也不会崩。magic 还是 0（创建者没初始化完）→ kFailed，下一拍重试。
    InitResult checkHeaderReadOnly(const SegmentView& view, const std::atomic<u32>& magic, const std::atomic<u32>& abi,
                                   const SegmentHeaderSpec& spec) const
    {
        if (view.base == nullptr || view.size < kSegmentPrologueBytes)
        {
            return InitResult::kFailed;
        }
        detail::readTouchPages(view.base, view.size);
        const u32 m = magic.load(std::memory_order_acquire);
        if (m == 0)
        {
            return InitResult::kFailed;
        }
        return verdict(view, m, abi.load(std::memory_order_acquire), spec);
    }

    // 段头初始化协议（会合段与数据段通用）。magic 最后发布，它就是「初始化完成」的标志：
    //   created          写触碰全段 → 锁页 → 写 abi、generation = 1、initData() 写几何字段 → release 写 magic；
    //   附着、magic 非 0 只读触碰 → 比对 magic、abi 与真实大小（magic 不是我们的 → kFailed；abi 不同 →
    //                    kAbiMismatch；段比本 abi 的布局小 → kFailed）；
    //   附着、magic 为 0 allowOverwrite = false 时直接 kFailed（下一拍重试）；否则每 50 ms 看一次、最多 10 次，
    //                    仍是 0 就说明创建者死在初始化中途：覆盖式重初始化（从 clearFrom 清零、generation + 1、
    //                    initData()、CAS magic 0 → magic）。两个竞争者写的内容相同，竞态是良性的；CAS 失败说明
    //                    别人先完成了，按附着比对。
    // 覆盖分支只给建段的一方（Input 与 Output 都是）；只读探测的一方一律 allowOverwrite = false。
    InitResult initHeader(SegmentView& view, std::atomic<u32>* magic, std::atomic<u32>* abi,
                          std::atomic<u32>* generation, const SegmentHeaderSpec& spec,
                          const std::function<void()>& initData = {}, bool allowOverwrite = true)
    {
        if (view.base == nullptr || magic == nullptr || abi == nullptr)
        {
            return InitResult::kFailed;
        }
        if (view.size < kSegmentPrologueBytes || (view.created && view.size < spec.layoutBytes))
        {
            report(view, IpcDiagCode::kBadSize, "segment is smaller than its header or layout");
            return InitResult::kFailed;
        }

        if (view.created)
        {
            detail::writeTouchPages(view.base, view.size);
            tryLock(view);
            abi->store(spec.abi, std::memory_order_relaxed);
            if (generation != nullptr)
            {
                generation->store(1, std::memory_order_relaxed);
            }
            if (initData)
            {
                initData();
            }
            magic->store(spec.magic, std::memory_order_release);
            return InitResult::kOk;
        }

        detail::readTouchPages(view.base, view.size);
        u32 m = magic->load(std::memory_order_acquire);
        if (m != 0)
        {
            return verdict(view, m, abi->load(std::memory_order_acquire), spec);
        }
        if (!allowOverwrite)
        {
            return InitResult::kFailed;
        }
        for (int i = 0; i < 10 && m == 0; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            m = magic->load(std::memory_order_acquire);
        }
        if (m != 0)
        {
            return verdict(view, m, abi->load(std::memory_order_acquire), spec);
        }

        if (view.size < spec.layoutBytes)
        {
            report(view, IpcDiagCode::kBadSize, "uninitialised segment is smaller than the layout; not overwriting");
            return InitResult::kFailed;
        }
        if (spec.clearFrom < view.size)
        {
            std::memset(static_cast<char*>(view.base) + spec.clearFrom, 0, view.size - spec.clearFrom);
        }
        abi->store(spec.abi, std::memory_order_relaxed);
        if (generation != nullptr)
        {
            generation->fetch_add(1, std::memory_order_relaxed);
        }
        if (initData)
        {
            initData();
        }
        u32 expected = 0;
        if (!magic->compare_exchange_strong(expected, spec.magic, std::memory_order_release, std::memory_order_acquire))
        {
            return verdict(view, expected, abi->load(std::memory_order_acquire), spec);
        }
        return InitResult::kOk;
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

    // magic 已非 0 时的判定
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

// 映射控制块：持有视图；非实时线程经 shared_ptr 保活，音频线程只增减 leaseCount（原子，无系统调用）
struct SegmentMapping
{
    SegmentView view;
    std::atomic<u32> leaseCount{0};
    std::atomic<bool> releaseRequested{false};

    explicit SegmentMapping(SegmentView&& v) : view(std::move(v)) {}
};

} // namespace detail

// 映射句柄：引用计数 + 按块租约 + 宽限期释放。
//   - shared_ptr 只在非实时线程上碰；音频线程经原子裸指针 implPtr_ 拿到控制块，增减 leaseCount。
//   - release(now)：先置 releaseRequested，再摘掉 implPtr_（不再发放新租约），然后在 leaseCount 归零、
//     且又过了 kReleaseGraceMs 之后才经后端解映射。
// 两道保护各管一段：lease() 里「读到裸指针」与「leaseCount + 1」之间那一小段只有宽限期兜着（计数还没加上，
// 握手与计数复核都看不见它）；租约发放之后才归计数复核与握手管。前一段依赖「500 ms 宽限 > 音频线程在这两步之间
// 可能被挂起的时长」这个前提，这是有意接受的设计假设；单测用显式的静默点代替真实时间，测的是后一段。
// 析构早于宽限期届满时（插件卸载的常态），映射留到进程退出，音频线程可能仍持有的裸指针不会悬空。
class SegmentHandle
{
public:
    // 两个 4 Hz 心跳的间隔，远大于宿主的最大块时长
    static constexpr u64 kReleaseGraceMs = 500;

    SegmentHandle() = default;

    // 接管视图（createOrOpen / openExisting 与 initHeader 之后，非实时线程）
    SegmentHandle(SegmentView&& view, ISegmentBackend* backend)
        : impl_(std::make_shared<detail::SegmentMapping>(std::move(view))), backend_(backend)
    {
        implPtr_.store(impl_.get(), std::memory_order_release);
    }

    ~SegmentHandle() { release(util::monoNowMs()); }

    SegmentHandle(const SegmentHandle&) = delete;
    SegmentHandle& operator=(const SegmentHandle&) = delete;

    // 移动只转移所有权，不触发释放。调用方要先 release() 或把旧句柄移进待回收列表再覆盖，否则旧映射就泄漏了。
    SegmentHandle(SegmentHandle&& o) noexcept
        : impl_(std::move(o.impl_)), backend_(o.backend_), graceStartMs_(o.graceStartMs_),
          graceStarted_(o.graceStarted_)
    {
        implPtr_.store(impl_.get(), std::memory_order_release);
        o.implPtr_.store(nullptr, std::memory_order_release);
        o.backend_ = nullptr;
        o.graceStartMs_ = 0;
        o.graceStarted_ = false;
    }
    SegmentHandle& operator=(SegmentHandle&& o) noexcept
    {
        if (this != &o)
        {
            impl_ = std::move(o.impl_);
            backend_ = o.backend_;
            graceStartMs_ = o.graceStartMs_;
            graceStarted_ = o.graceStarted_;
            implPtr_.store(impl_.get(), std::memory_order_release);
            o.implPtr_.store(nullptr, std::memory_order_release);
            o.backend_ = nullptr;
            o.graceStartMs_ = 0;
            o.graceStarted_ = false;
        }
        return *this;
    }

    // 下面三个只读 implPtr_，不碰 shared_ptr；不持租约时只许非实时线程用（它是唯一的解映射者，不和自己并发）。
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
    // 四个访问（这里的 fetch_add 与 load、release() 的 store、unmapIfIdle() 的 load）一律 seq_cst：
    // 只用 acquire / release 时，C++ 允许两边的 store→load 重排、同时读到旧值，音频拿到租约的同时
    // 非实时线程判「没有在途租约」开始计宽限期。四个都在同一个全序里时，至少有一边看得见另一边。
    // 代价：x86 上只有 release() 的那条 store 从 mov 变成 xchg（不在音频热路径）；arm64 上两条 load 编成 LDAR，
    // 不会被编成可以越过前面 STLR / LDADDAL 提前读的 LDAPR。
    Lease lease() const
    {
        detail::SegmentMapping* p = implPtr_.load(std::memory_order_acquire);
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

    // 非实时线程请求释放。返回 true = 已解映射；false = 租约在途或宽限期未满，下一拍再调。幂等。
    bool release(u64 nowMs)
    {
        if (!impl_)
        {
            return true;
        }
        impl_->releaseRequested.store(true, std::memory_order_seq_cst); // 握手的这一侧，见 lease()
        implPtr_.store(nullptr, std::memory_order_release);
        return unmapIfIdle(nowMs);
    }

private:
    bool unmapIfIdle(u64 nowMs)
    {
        if (!impl_)
        {
            return true;
        }
        if (impl_->leaseCount.load(std::memory_order_seq_cst) != 0) // 握手的这一侧，见 lease()
        {
            return false;
        }
        if (!graceStarted_)
        {
            graceStarted_ = true;
            graceStartMs_ = nowMs;
            return false;
        }
        if (nowMs - graceStartMs_ < kReleaseGraceMs)
        {
            return false;
        }
        if (backend_ != nullptr)
        {
            backend_->unmap(impl_->view);
        }
        impl_.reset();
        backend_ = nullptr;
        graceStarted_ = false;
        graceStartMs_ = 0;
        return true;
    }

    std::shared_ptr<detail::SegmentMapping> impl_; // 只在非实时线程上碰
    std::atomic<detail::SegmentMapping*> implPtr_{nullptr}; // 音频线程经它拿裸指针
    ISegmentBackend* backend_ = nullptr;
    u64 graceStartMs_ = 0;
    bool graceStarted_ = false;
};

} // namespace rinne::ipc
