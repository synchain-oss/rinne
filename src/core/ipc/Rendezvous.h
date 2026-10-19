// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合：一个插件实例在会合段里认领、保住、放出属于自己角色的端点槽，并把音频线程要用的绑定发布出去。
// 全部在消息线程上跑（壳的 25 Hz 定时器调用 tick）；时间一律由调用方传进来，只有析构里的最后一次回收读真实时钟。
//
// ctl 是一个 64 位整字：高 32 位 gen、低 32 位 state（低 2 位阶段 + provisional 位）。每一次转移都是整字 CAS 并
// 让 gen + 1，所以同一个整字要 2^32 次转移之后才会再次出现。持有者记住自己最后写下的整字，每一拍比对一次，
// 就能分辨「还是我」与「被接管过」，不依赖任何时间假设，也没有 ABA。认领是两步 CAS：free -> claiming（写身份
// 字段）-> active；中间那一小段就是别人判定「幽灵认领」的窗口。
//
// 接管只认证据：观察者必须连续看到同一个 ctl 至少 N 毫秒（幽灵 1 s、死进程 5 s、同进程孤儿 30 s、休眠 2 s），
// 另加各档自己的字段：幽灵要 claim_ms 也老，休眠要 dormant_ms 也老，死进程与孤儿要心跳与 block_seq 也静止了同样久；
// 跨进程还要 pid 真的死了（探活拿不准一律按活，宁可晚接管）。
// 活着的外进程永远不自动接管，无论它看起来多陈旧。休眠槽只有工程恢复（kRestore）或用户确认（kManualConfirmed）
// 才拿，自动的冲突重试（kConflictRetry）永远不拿。
//
// 每一拍的顺序固定：先回收映射，再补上 rv，再扫描，再按「有没有写权」走 owner / claim，最后按需打开数据段并
// 发布绑定。任何一步失败都只影响这一拍，不会留下半开的映射：kBusy 一律立刻解映射，按 RetryBackoff 退避重试。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ipc/ISegmentBackend.h"
#include "ipc/LabelSeqlock.h"
#include "ipc/LoopBinding.h"
#include "ipc/Naming.h"
#include "ipc/SegmentLayout.h"
#include "ipc/SlotObserver.h"

namespace rinne::ipc
{

// 认领的意图。kSmart 的认领带 provisional 位（智能默认的认领，可被工程 state 的恢复抢占）；
// kRestore 与 kManualConfirmed 可以拿休眠槽，给出之后 kIntentHoldMs 之内的冲突重试仍按它们判；kConflictRetry 是
// 冲突期间的自动重试，永远不拿休眠槽
enum class ClaimIntent : std::uint8_t
{
    kSmart,
    kRestore,
    kManual,
    kManualConfirmed,
    kConflictRetry,
};

// 时间常数，全部是毫秒
struct RendezvousTiming
{
    static constexpr u64 kHeartbeatMs = 250;
    static constexpr u64 kGhostMs = 1000;
    static constexpr u64 kDeadMs = 5000;
    static constexpr u64 kOrphanMs = 30000;
    static constexpr u64 kDormantMs = 2000;
    static constexpr u64 kConflictRetryMs = 1000;
    // 工程恢复与用户确认的原意图有效期：拿休眠槽的证据攒够 2 s，再给一个扫描间隔
    static constexpr u64 kIntentHoldMs = kDormantMs + kHeartbeatMs;
    static constexpr u64 kLostHeartbeatMs = 2000;
    static constexpr u64 kLostStillMs = 2000;
};

// 下一次转移要写下的整字：gen + 1，换成新的 state
constexpr u64 nextCtl(u64 expected, u32 newState) noexcept
{
    return makeCtl(ctlGen(expected) + 1u, newState);
}

// 整字 CAS 的结果：成功时 word 是写下去的整字，失败时 word 是 CAS 看到的当前值
struct CtlCas
{
    bool ok = false;
    u64 word = 0;
};

CtlCas casCtl(std::atomic<u64>& ctl, u64 expected, u32 newState) noexcept;

enum class TakeoverTier : std::uint8_t
{
    kNone,
    kFree,
    kGhost,
    kDeadProcess,
    kOrphan,
    kDormant,
    kPreemptProvisional,
};

// 探活注入：只在这一个判定里调用（系统调用，放在最后）
using PidAliveFn = bool (*)(u32 pid) noexcept;

// 纯函数：只依据一份扫描样本判定能按哪一档接管。样本不稳定一律 kNone
TakeoverTier takeoverTier(const SlotSample& s, u64 nowMs, u32 selfPid, ClaimIntent intent,
                          PidAliveFn pidAlive) noexcept;

enum class RendezvousState : std::uint8_t
{
    kValidator, // IPC 关闭（validator 宿主）
    kUnassigned, // 组号 0
    kWaitingPrepare, // 有组号但还没 prepare（或已 release）：认领、重试与接管都只给已 prepare 的实例
    kOpening, // 正在开会合段或数据段；kBusy 退避或 kFailed 下一拍重试
    kIpcUnavailable, // 后端拒绝或段名不合法：本进程内不会自己好，不再重试
    kAbiMismatch, // 会合段是别的 abi 初始化的：绝不写它，每 kConflictRetryMs 重看一次
    kConflict, // 槽被别人占着，或者我们的被接管了；每 kConflictRetryMs 用 kConflictRetry 重试
    kActive, // 持有槽（active）且绑定已发布
    kDormant, // 持有槽但休眠（releaseResources）
};

struct StreamFormat
{
    u32 sampleRate = 0;
    u32 maxBlock = 0;
    u32 channels = 0;
};

struct RendezvousConfig
{
    Role role = Role::kInput;
    ISegmentBackend* backend = nullptr; // 不拥有；实例调用期间必须活着
    NameScope scope = NameScope::formal();
    bool ipcEnabled = true; // validator 宿主（pluginval、auval 等）下整体关闭，什么都不打开
    u32 version = 0; // 本构建的 packVersion(...)
    u32 format = 0; // kFormatVst3 / kFormatAu / 0
    u32 hostPpid = 0;
    u64 seed = 0; // RetryBackoff 的种子
};

class Rendezvous
{
public:
    explicit Rendezvous(const RendezvousConfig& config);
    ~Rendezvous();
    Rendezvous(const Rendezvous&) = delete;
    Rendezvous& operator=(const Rendezvous&) = delete;

    // 组号 0..8。换组先按 release 的规则放开旧槽，下一拍用 intent 认领新组；同组就是更新下一次认领的意图
    void setGroup(u32 group, ClaimIntent intent, u64 nowMs);
    // 打开认领的门。已持有一个休眠槽时 dormant -> active（claim_ms 不变）、重写格式字段、Output 重新发布写权、
    // 重新发布绑定；已持有 active 槽时只重写格式字段（Output 重新发布写权），ctl 不动
    void prepare(const StreamFormat& format, u64 nowMs);
    // 关上认领的门。持有 active 槽时 active -> dormant，写 dormant_ms，重新发布绑定；CAS 失败就是被接管了
    void releaseResources(u64 nowMs);
    // 属主把自己 active+provisional 的槽确认成正式 active（清掉 provisional 位）
    void confirmClaim(u64 nowMs);
    void setNonrealtime(bool on) noexcept;
    // 记住标签并标脏；tick 拿到写权时写进槽里。返回「现在是否已经写下」
    bool setLabel(std::string_view utf8Name, u32 colourArgb);
    void tick(u64 nowMs);

    RendezvousState state() const noexcept { return state_; }
    u32 group() const noexcept { return group_; }
    Role role() const noexcept { return config_.role; }
    u64 myCtl() const noexcept { return myCtl_; } // 0 = 什么都没持有
    u64 writerToken() const noexcept { return token_; } // Output 且数据段已打开时才有
    const SlotObserver& observer() const noexcept { return observer_; }
    bool peerLost(u64 nowMs) const noexcept;
    u32 abiMismatchVersion() const noexcept { return abiVersion_; }
    InitResult lastResult() const noexcept { return lastResult_; }
    u32 consecutiveFailures() const noexcept { return failures_; }
    BindingPublisher& bindings() noexcept { return bindings_; }
    const BindingPublisher& bindings() const noexcept { return bindings_; }
    static std::size_t liveInstances() noexcept;

private:
    bool openRendezvous(u64 nowMs);
    bool openRing(u64 nowMs);
    void claimTick(RvSegment& rv, u64 nowMs);
    void ownerTick(RvSegment& rv, u64 nowMs);
    void publishBinding();
    void refreshState() noexcept;
    void writeIdentity(RoleSlot& slot) noexcept;
    void upkeepLabel(RoleSlot& slot, u64 nowMs, bool heartbeatDue) noexcept;
    void upkeepWriter() noexcept;
    bool intentHeld(u64 nowMs) noexcept;
    void lostOwnership(u64 nowMs);
    void letGo(u64 nowMs);
    void writeFormat(RoleSlot& slot) noexcept;
    u64 makeWriterToken(u64 nowMs) noexcept;

    RendezvousConfig config_;
    Naming naming_;
    u32 group_ = 0;
    ClaimIntent intent_ = ClaimIntent::kManual;
    bool prepared_ = false;
    StreamFormat format_;
    bool nonrealtime_ = false;
    u64 myCtl_ = 0;
    u64 claimMs_ = 0;
    u64 dormantMs_ = 0;
    u32 cfgFlags_ = 0;
    u64 lastHeartbeatMs_ = 0;
    u64 token_ = 0;
    HandleKeeper keeper_;
    SegmentHandle* rv_ = nullptr;
    SegmentHandle* ring_ = nullptr;
    BindingPublisher bindings_;
    SlotObserver observer_;
    RetryBackoff backoff_;
    u64 retryAtMs_ = 0;
    u64 nextConflictRetryMs_ = 0;
    u64 intentFromMs_ = 0; // 当前意图的有效期起点：setGroup 或观察者重置的那一拍，取较晚的
    RendezvousState state_ = RendezvousState::kUnassigned;
    LabelPayload label_;
    bool labelDirty_ = false;
    u64 labelOddSinceMs_ = 0;
    u32 labelOddSeq_ = 0;
    u32 abiVersion_ = 0;
    InitResult lastResult_ = InitResult::kOk;
    u32 failures_ = 0;
    bool permanent_ = false;
};

} // namespace rinne::ipc
