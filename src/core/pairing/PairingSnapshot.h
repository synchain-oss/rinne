// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合段的只读快照：智能默认、一键修复与配对状态读的那一份。会合在组号 0 时不开段，首次智能默认又恰在组号 0 上做，
// 所以壳另持一份会合段的只读映射，由本类管：
//   - 只开已存在的段（openExistingReadOnly）、只读校验段头（checkHeaderReadOnly）：绝不建段、绝不写，
//     手里只有 const 指针；
//   - 自带一个 SlotObserver 扫 16 个槽；标签连读两次、两次一致才信（LabelSeqlock 文件头的约定），
//     读不稳的一拍留着上一份，槽换了主人又读不稳就当未知；
//   - 拼 GroupViews 与某一端的快照。每端都带 pid，这里不滤：「只看同 pid」由 PairingPolicy 判（别的进程的端点算占着，
//     既不参与匹配也不当空位），对端是别的进程时 PairStateModel 据 pid 报 crossProcess。
// 打开的节奏：段不存在（kFailed）或还没发布完（kBusy）时，按观察者的扫描间隔再试，不每拍都试一次注定失败的打开。
// 调用方这一拍要做智能决定时传 force：上一次是段不存在就不论间隔强制再试一次。同一线程上前一个实例这一拍刚建了段、
// 认领了组，后一个实例做决定之前必须看见，否则两个都会挑中同一个空组。调用方确知段在（本端的会合正持有它）时
// 也传 force。force 只对「段不存在」生效：上一次是还没发布完（例如别的进程初始化到一半崩了）时照样按扫描间隔，
// 否则段一直停在 kBusy、快照一直不能用、决定一直做不成，每拍都会强制打开一次。ABI 不同的段按会合的冲突重试间隔看，
// 同样不受 force 影响（那时本来就不做智能默认）；后端拒绝或段名不合法就不再试。
// 「段不存在」要连着两次打不开才信：POSIX 后端在生命周期锁正被别人拿着时同样返回 kFailed。
// 寿命：映射上之后一直留着，close 或析构时解映射，所以必须先于后端结束。映射着的段不会被删：Windows 上句柄让它活着；
// POSIX 后端的只读映射同样持着段锁文件的共享锁，离开时与任何持有者一样做「是不是最后一个」的判定，与本进程会合自己的
// 映射各算各的。
// 线程：只在非实时线程上用，调用方串行（壳的锁）。end 会分配（显示名），其余不分配。
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ipc/ISegmentBackend.h"
#include "ipc/LabelSeqlock.h"
#include "ipc/Naming.h"
#include "ipc/PairingPolicy.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "ipc/SlotObserver.h"
#include "pairing/PairStateModel.h"

namespace rinne::pairing
{

using rinne::ipc::u64;

// 一个端点槽的快照。stable 为假（没映射、组号越界、这一拍的样本前后不一致）时，样本里的字段都是默认值
struct EndFacts
{
    bool stable = false;
    u32 phase = ipc::kCtlPhaseFree;
    u32 pid = 0;
    u32 version = 0; // packVersion
    u32 sampleRate = 0;
    u32 maxBlock = 0;
    u32 channels = 0;
    u32 rtFlags = 0;
    u64 heartbeatMs = 0;
    // 以下两项不经观察者，调用时现读（映射着、组号合法就有，不看 stable）
    u32 safetyState = 0; // safety_state 整字，低 8 位是安全状态
    u32 maxNSeen = 0; // 本次 prepare 以来见过的最大块长（属主的音频线程写）
    // 标签：连续两次读到同一份才有
    u32 nameHash = 0; // 0 = 未知
    std::string name; // 显示前缀，合法 UTF-8 才有
    u32 colourArgb = 0; // 0 = 未知
};

// 配对状态要的对端快照：稳定、active 才算有对端（休眠不算）。lost 不在这里判，由调用方按会合的 peerLost 填
PeerFacts peerFactsOf(const EndFacts& end);

class PairingSnapshot
{
public:
    static constexpr u64 kRetryMs = ipc::SlotObserver::kScanIntervalMs; // 段不存在、还没发布完
    static constexpr u64 kAbiRetryMs = ipc::RendezvousTiming::kConflictRetryMs; // 别的 abi 的段

    // backend 不拥有，必须比本类活得久；段名按 scope 拼会合段的名字，不合法时 unavailable() 一开始就为真
    PairingSnapshot(ipc::ISegmentBackend* backend, const ipc::NameScope& scope);
    ~PairingSnapshot();

    PairingSnapshot(const PairingSnapshot&) = delete;
    PairingSnapshot& operator=(const PairingSnapshot&) = delete;

    // 每拍调一次。映射着就重扫 16 个槽与标签；没映射就按文件头的节奏再试打开。force = 这一拍要做智能决定，或确知段在；
    // 只在上一次是段不存在时生效
    void refresh(u64 nowMs, bool force);
    // 解映射；之后 refresh 会重新打开。析构时自动调
    void close() noexcept;

    bool mapped() const noexcept { return view_.base != nullptr; }
    // 能拿来做决定：映射着，或者已经连着两次确认段不存在（没有任何认领者，全空）
    bool usable() const noexcept;
    // 上一次打开看到的是别的 abi 的段（绝不写，也不拿来做决定）
    bool abiMismatch() const noexcept { return !mapped() && lastResult_ == ipc::InitResult::kAbiMismatch; }
    // 后端拒绝或段名不合法：不再试
    bool unavailable() const noexcept { return unavailable_; }

    // 16 个槽的快照，给 PairingPolicy 与 PairStateModel。没映射时全空；不稳定的样本按「正在认领」算（既不是空位也
    // 不是活端，这一拍谁都不拿它当空组）
    ipc::GroupViews groupViews(u64 nowMs) const noexcept;
    // 一个端点槽（组号 1..8）
    EndFacts end(u32 group, ipc::Role role) const;

private:
    struct LabelCache
    {
        u64 ctl = 0; // 读到这份标签时槽的 ctl
        bool known = false; // 连续两次读到同一份
        ipc::LabelPayload label;
    };

    const ipc::RvSegment* segment() const noexcept { return static_cast<const ipc::RvSegment*>(view_.base); }
    bool dueToOpen(u64 nowMs, bool force) const noexcept;
    void open(u64 nowMs);
    void readLabels() noexcept;
    ipc::EndView endView(u32 group, ipc::Role role, u64 nowMs) const noexcept;
    const LabelCache& label(u32 group, ipc::Role role) const noexcept;

    ipc::ISegmentBackend* backend_;
    std::string name_; // 会合段的平台段名；空 = 名字不合法
    ipc::SegmentView view_;
    ipc::InitResult lastResult_ = ipc::InitResult::kFailed; // 上一次打开的结果
    bool attempted_ = false;
    u64 lastAttemptMs_ = 0;
    u32 misses_ = 0; // 连续几次「段不存在」（kFailed），到 2 为止
    bool unavailable_ = false;
    ipc::SlotObserver observer_;
    std::array<std::array<LabelCache, 2>, ipc::kGroupCount> labels_{};
};

} // namespace rinne::pairing
