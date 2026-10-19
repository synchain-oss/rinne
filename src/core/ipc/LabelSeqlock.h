// SPDX-License-Identifier: GPL-3.0-or-later
//
// 端点槽里轨道名与颜色的标签（RoleSlot 偏移 128..255）：一个 128 字节的 seqlock。只有槽的属主写，而且只在 ctl 仍是
// 它最后写下的那个整字时写（那道检查由调用方做）；读方可以是任何一方，永远不加锁。
//
//   写：label_seq 从偶数 s CAS 到 s + 1（奇数 = 正在写）→ release fence → relaxed 写四个字段与 28 个字 →
//       label_seq 以 release 从 s + 1 CAS 到 s + 2（偶数 = 稳定，且比上一版大 2）。
//   读：acquire 读 label_seq，奇数就放弃；relaxed 读全部字段；acquire fence；relaxed 再读 label_seq，
//       与第一次不等就放弃（这一次的结果不可信）。
//
// 两头各有两道次序：写方奇数之后的 release fence 让「先放奇数、后写字段」不被重排，收尾的 release store 让「先写
// 字段、后放偶数」不被重排；读方第一次的 acquire load 让「先读序号、后读字段」不被重排，字段之后的 acquire fence 让
// 「先读字段、后再读序号」不被重排。后两道缺一不可：读到的字段只要有一个来自下一次写，那次写的 release fence 就与
// 读方的 acquire fence 同步，第二次读到的序号必然已经是奇数或更大，这一份被丢弃；而 acquire load 只挡它后面的访问，
// 挡不住字段的读被挪到第二次读序号之后。奇数那一步用 CAS 而不是 store：接管瞬间旧属主与新属主可能同时写，第二个
// 写方 CAS 失败就退回，不会把序号再 + 1 而把正在写的那一版说成稳定。收尾也用 CAS：写方慢到被新属主当成死掉、
// 序号已被 rewriteStuckLabel 接走时，它的收尾失败，不会把序号退回偶数、把新属主正在写的一版说成稳定。它晚到的字段
// 写仍可能落在新属主的字段之后，那一份由属主每个心跳重读自己的标签来修。
// RINNE_STRESS_MUTATE_NO_READ_FENCE 同时去掉这里读方的 acquire fence（同一道屏障），压测据此证明它在 arm64 上
// 不可省；x86 是 TSO，那里它只是编译器屏障，变异构建在 x86 上仍然是绿的。
//
// 读失败时 out 不动：调用方可以把上一次成功读到的标签继续用于显示。本文件的读取只做无分配的无锁拷贝；
// labelDisplayName 会分配（构造 std::string），只许非实时线程调用。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ipc/SegmentLayout.h"

namespace rinne::ipc
{

// 标签的内容。nameLen 与 nameHash 描述完整名字，name_words 只装按码点截到 ≤ 112 字节的前缀（供显示）
struct LabelPayload
{
    u32 colourArgb = 0;
    u32 nameLen = 0;
    u32 nameHash = 0;
    u32 words[kLabelNameWords] = {};
};

inline bool operator==(const LabelPayload& a, const LabelPayload& b) noexcept
{
    if (a.colourArgb != b.colourArgb || a.nameLen != b.nameLen || a.nameHash != b.nameHash)
    {
        return false;
    }
    for (u32 i = 0; i < kLabelNameWords; ++i)
    {
        if (a.words[i] != b.words[i])
        {
            return false;
        }
    }
    return true;
}

inline bool operator!=(const LabelPayload& a, const LabelPayload& b) noexcept
{
    return !(a == b);
}

// FNV-1a 32：与对端工具的哈希口径一致，名字的同一性只靠它（前缀只供显示）
inline constexpr u32 kFnv1aOffset = 2166136261u;
inline constexpr u32 kFnv1aPrime = 16777619u;

inline u32 fnv1a32(std::string_view bytes) noexcept
{
    u32 h = kFnv1aOffset;
    for (const char c : bytes)
    {
        h ^= static_cast<u32>(static_cast<unsigned char>(c));
        h *= kFnv1aPrime;
    }
    return h;
}

// 不超过 maxBytes、又不切断码点的最长前缀：从 min(size, maxBytes) 往回退到不是续字节（10xxxxxx）的位置。
// UTF-8 的续字节最多 3 个，所以最多退 3 步
inline std::size_t utf8PrefixLength(std::string_view utf8, std::size_t maxBytes) noexcept
{
    std::size_t n = utf8.size() < maxBytes ? utf8.size() : maxBytes;
    while (n > 0 && n < utf8.size() && (static_cast<unsigned char>(utf8[n]) & 0xC0) == 0x80)
    {
        --n;
    }
    return n;
}

// 名字太长时 nameLen 饱和在 u32 上限；nameHash 始终按完整名字算，words 只装前缀
inline LabelPayload makeLabelPayload(std::string_view utf8Name, u32 colourArgb) noexcept
{
    LabelPayload p;
    p.colourArgb = colourArgb;
    const std::size_t len = utf8Name.size();
    p.nameLen = len > static_cast<std::size_t>(0xffffffffu) ? 0xffffffffu : static_cast<u32>(len);
    p.nameHash = fnv1a32(utf8Name);
    const std::size_t prefix = utf8PrefixLength(utf8Name, kLabelNameBytes);
    for (std::size_t i = 0; i < prefix; ++i)
    {
        const std::size_t word = i / 4;
        const std::size_t shift = (i % 4) * 8;
        p.words[word] |= static_cast<u32>(static_cast<unsigned char>(utf8Name[i])) << shift;
    }
    return p;
}

// 把 words 还原成显示用的前缀：遇到第一个 0 字节或满 112 字节为止。会分配，只许非实时线程调用
inline std::string labelDisplayName(const LabelPayload& p)
{
    std::string out;
    out.reserve(kLabelNameBytes);
    for (u32 i = 0; i < kLabelNameWords; ++i)
    {
        for (u32 j = 0; j < 4; ++j)
        {
            const auto c = static_cast<unsigned char>((p.words[i] >> (j * 8)) & 0xffu);
            if (c == 0)
            {
                return out;
            }
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// 写标签。调用方必须先核过 ctl 仍是自己最后写下的整字（只有属主写标签），这里不重复那道检查。
// 序号是奇数时（别人正在写，或者上一版写了一半就死掉）直接放弃：宁可这一版不写，也不打断别人的写。
// 写了一半就死掉的属主留下的奇数由接手这个槽的新属主用 rewriteStuckLabel 处理，不在这里猜
namespace detail
{
// 序号已经由调用方 CAS 成奇数 odd 之后：写字段，再以 release 从 odd CAS 到 odd + 1（偶数）。
// 返回 false = 写的途中序号被别人接走了（见文件头），这一版没有发布
inline bool finishLabelWrite(RoleSlot& slot, u32 odd, const LabelPayload& p) noexcept
{
    // 序号先变成奇数，字段才能开始动；否则读方可能读到新旧混在一起的一版
    std::atomic_thread_fence(std::memory_order_release);
    slot.colour_argb.store(p.colourArgb, std::memory_order_relaxed);
    slot.name_len.store(p.nameLen, std::memory_order_relaxed);
    slot.name_hash.store(p.nameHash, std::memory_order_relaxed);
    for (u32 i = 0; i < kLabelNameWords; ++i)
    {
        slot.name_words[i].store(p.words[i], std::memory_order_relaxed);
    }
    // 偶数收尾以 release 发布：看到这个序号的读方，它之前的字段写都已经可见
    u32 expected = odd;
    return slot.label_seq.compare_exchange_strong(expected, odd + 1, std::memory_order_release,
                                                  std::memory_order_relaxed);
}
} // namespace detail

inline bool writeLabel(RoleSlot& slot, const LabelPayload& p) noexcept
{
    u32 s = slot.label_seq.load(std::memory_order_relaxed);
    if ((s & 1u) != 0)
    {
        return false;
    }
    // 接管瞬间旧属主可能还在写：CAS 只让一个写方进临界区，另一个退回而不是把序号再 + 1
    if (!slot.label_seq.compare_exchange_strong(s, s + 1, std::memory_order_relaxed, std::memory_order_relaxed))
    {
        return false;
    }
    return detail::finishLabelWrite(slot, s + 1, p);
}

// 新属主接手一个停在奇数上的标签（上一任写到一半就死了）：把序号从 stuckOdd CAS 到 stuckOdd + 2（仍是奇数，表示
// 「现在是我在写」），写完整的一份，再存成偶数。整个过程序号都不会是偶数配着半份字段，读方照常只拿到完整的版本。
// 只许属主调用，而且只在同一个奇数已经停了足够久之后（会合层按心跳判断）：序号变过、或者不是奇数都返回 false
inline bool rewriteStuckLabel(RoleSlot& slot, u32 stuckOdd, const LabelPayload& p) noexcept
{
    if ((stuckOdd & 1u) == 0)
    {
        return false;
    }
    u32 expected = stuckOdd;
    if (!slot.label_seq.compare_exchange_strong(expected, stuckOdd + 2, std::memory_order_relaxed,
                                                std::memory_order_relaxed))
    {
        return false;
    }
    return detail::finishLabelWrite(slot, stuckOdd + 2, p);
}

// 读标签。返回 false 时 out 原样不动（调用方可以继续显示上一次成功读到的版本）
inline bool readLabel(const RoleSlot& slot, LabelPayload& out) noexcept
{
    const u32 s1 = slot.label_seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0)
    {
        return false;
    }
    LabelPayload local;
    local.colourArgb = slot.colour_argb.load(std::memory_order_relaxed);
    local.nameLen = slot.name_len.load(std::memory_order_relaxed);
    local.nameHash = slot.name_hash.load(std::memory_order_relaxed);
    for (u32 i = 0; i < kLabelNameWords; ++i)
    {
        local.words[i] = slot.name_words[i].load(std::memory_order_relaxed);
    }
#if !defined(RINNE_STRESS_MUTATE_NO_READ_FENCE)
    // 字段读完之前不许再读序号：少了它，写方在两次读之间改完一整版，读方会拿着一半新一半旧的字段当稳定版
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
    const u32 s2 = slot.label_seq.load(std::memory_order_relaxed);
    if (s1 != s2)
    {
        return false;
    }
    out = local;
    return true;
}

} // namespace rinne::ipc
