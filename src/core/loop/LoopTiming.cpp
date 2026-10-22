// SPDX-License-Identifier: GPL-3.0-or-later
//
// 流相对寻址与环路延迟的几何：函数本身在 LoopTiming.h 里内联（纯函数、没有状态，头文件内联是为了让
// 音频线程的热路径不跨翻译单元调用）。这一份只钉住它的编译期契约：改一个时长或上限时如果破坏了
// 「下限 ≤ 默认 ≤ 上限」这类关系，编译就红，不必等运行时才发现。
//
// 只断言编译期能算的量：几何函数用了 std::ceil / std::llround，不是 constexpr，量的关系由
// tests/core/loop/test_loop_timing.cpp 在运行时钉住（差值的有符号性、D 的抬升与截断、下限的回落）。
#include "loop/LoopTiming.h"

namespace rinne::loop
{

// D 的范围：1..4000 ms，默认 100 ms。默认必须落在范围内。
static_assert(kDelayMinMs > 0.0, "D 的下限必须为正");
static_assert(kDelayMinMs <= kDelayDefaultMs, "默认 D 必须不小于 D 的下限");
static_assert(kDelayDefaultMs <= kDelayMaxMs, "默认 D 必须不大于 D 的上限");

// 漂移量的定义：m = W − (R + off)。写头领先为正，读位跑到写头前面为负。
// 全部走有符号运算：W ≈ 2^40 之类的位置上也正确（这一条由单测钉住）。
static_assert(drift(1000, 400, 0) == 600, "漂移量是写头减去读位与偏移之和");
static_assert(drift(400, 1000, 0) == -600, "读位跑到写头前面时漂移量为负");
static_assert(drift(0, 0, -100) == 100, "偏移为负时写头照样领先");

// 帧位置的差值：回绕之后仍然给出正确的有符号差。
static_assert(frameDiff(10u, 4u) == 6, "帧位置的差是右操作数减左操作数");
static_assert(frameDiff(4u, 10u) == -6, "帧位置的差是有符号的");
static_assert(frameDiff(0u, 0u) == 0, "相同位置的差是 0");

} // namespace rinne::loop
