// SPDX-License-Identifier: GPL-3.0-or-later
//
// 压测在变异构建里的取证规则。变异构建（RINNE_STRESS_MUTATE_*，由 rinne_core 的 PUBLIC 定义带进来）必须变红，但「有一条
// 断言失败」不等于「抓住了这个变异」：别的用例的失败、非空转下限没达标、或从构造上就必然失败的检查，都会让整个
// 二进制变红而什么都没证明。所以每个用例在变异构建里先对自己那套机制的判据做正向核验：跑到命中数够了为止（有封顶），
// 达不到、或者某一轮的非空转计数没达标，就打印原因并立即结束进程。这样不会打印 Catch2 的汇总行，CI 的反转脚本判
// 「没有按预期失败」，这一步变红。不用 std::abort：Windows 本机跑变异构建时它会弹出错误报告窗口、挂住进程。
// 常规构建里这些函数什么都不做，每个用例只跑一遍。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace rinne::stress
{

#if defined(RINNE_STRESS_MUTATE_NO_READ_FENCE)
inline constexpr const char* kMutation = "no_read_fence";
#elif defined(RINNE_STRESS_MUTATE_NO_INTENT)
inline constexpr const char* kMutation = "no_intent";
#else
inline constexpr const char* kMutation = nullptr;
#endif

inline constexpr bool kMutated = kMutation != nullptr;
#if defined(RINNE_STRESS_MUTATE_NO_READ_FENCE)
inline constexpr bool kNoReadFence = true;
#else
inline constexpr bool kNoReadFence = false;
#endif
#if defined(RINNE_STRESS_MUTATE_NO_INTENT)
inline constexpr bool kNoIntent = true;
#else
inline constexpr bool kNoIntent = false;
#endif

// 变异构建里，一个用例要攒够这么多次本机制的判据命中才算抓住；最多跑这么久
inline constexpr unsigned long long kMutationHits = 20;
inline constexpr unsigned long long kMutationCapMs = 25000;

// 变异的证据不成立：说明原因，跳过 Catch2 的收尾直接结束进程（不出汇总行）
[[noreturn]] inline void mutationEvidenceMissing(const char* what)
{
    std::cout.flush();
    std::printf("[stress] 变异 %s 的证据不成立：%s，不能算抓住了这个变异\n", kMutation != nullptr ? kMutation : "-",
                what);
    std::fflush(stdout);
    std::_Exit(70);
}

// 变异构建里，每一轮的非空转计数都必须达标：没达标的失败不算抓住缺陷
inline void requireNonVacuous(bool ok, const char* what)
{
    if constexpr (kMutated)
    {
        if (!ok)
        {
            mutationEvidenceMissing(what);
        }
    }
    else
    {
        (void)ok;
        (void)what;
    }
}

// 这个用例的机制被变异了，命中还不够、也没到封顶：再跑一遍。常规构建与机制没被变异时只跑一遍
inline bool anotherPass(bool mechanismMutated, unsigned long long hits, unsigned long long elapsedMs) noexcept
{
    return mechanismMutated && hits < kMutationHits && elapsedMs < kMutationCapMs;
}

// 跑完之后：机制被变异了，命中却不够
inline void requireHits(bool mechanismMutated, unsigned long long hits, const char* what)
{
    if (!mechanismMutated)
    {
        return;
    }
    std::printf("[stress] 变异 %s：%s 命中 %llu 次（需要 %llu）\n", kMutation != nullptr ? kMutation : "-", what, hits,
                kMutationHits);
    std::fflush(stdout);
    if (hits < kMutationHits)
    {
        mutationEvidenceMissing(what);
    }
}

} // namespace rinne::stress
