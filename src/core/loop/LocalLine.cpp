// SPDX-License-Identifier: GPL-3.0-or-later
//
// 本地延迟线：搬进与读出都在 LocalLine.h 里内联（热路径），这一份只钉住它的契约。
//
// 契约有三条，每一条都关系到「永不读旧数据或别名数据」这条安全不变量：
//   1. 容量是 2 的幂，取下标用按位与，任何绝对位置都能直接寻址。
//   2. 清空只改两个整数（pulled 与 lineValidFrom），不动缓冲区里的样本。清空之后那些位置落在有效区间外，
//      读出来是 0。清空不需要把几兆字节清零，因此可以在音频线程上做。
//   3. 尾巴快照是预拷贝：重同步时把主区最后那一段先拷出来，此后读旧位置一律从快照读，
//      不再依赖主区（主区可能已被环覆盖）。
//
// 这一份做两件事：把「容量必须是 2 的幂」与「容量容得下 D 的上限加一个最大块长」在编译期钉住；
// 把清空的语义写成注释钉住（清空不等于 memset，也不等于只清 pulled）。
#include <cstdint>

#include "loop/LocalLine.h"

namespace rinne::loop
{

namespace
{
// 向上取整（正数）。std::ceil 在 C++17 里不是 constexpr，这里要的是编译期能算的同一件事。
constexpr std::int64_t ceilToInt(double v) noexcept
{
    const auto i = static_cast<std::int64_t>(v);
    return (static_cast<double>(i) < v) ? i + 1 : i;
}

// 延迟线容量的算式，与 LoopTiming.h 的 lineFramesFor 同一件事：4 s（D 的上限）+ 一个最大块长 + 余量，
// 向上取到 2 的幂。这一份是它的编译期镜像，用来在编译期断言「容量是 2 的幂」与「容量够用」。
// lineFramesFor 本体不是 constexpr（用了 std::ceil），所以两者的一致性由
// tests/core/loop/test_local_line.cpp 在运行时对拍：那边的断言是「容量是 2 的幂」与「容量 >= 4 s 加一块」，
// 与这里的判据是同一件事的两种说法。
constexpr std::int64_t capacityFor(double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const std::int64_t raw = ceilToInt(4.0 * sr) + 32768 + 64;
    std::int64_t n = 1;
    while (n < raw)
    {
        n <<= 1;
    }
    return n;
}
} // namespace

// 容量必须是 2 的幂：取下标用按位与。
static_assert((capacityFor(44100.0) & (capacityFor(44100.0) - 1)) == 0, "延迟线容量必须是 2 的幂");
static_assert((capacityFor(48000.0) & (capacityFor(48000.0) - 1)) == 0, "延迟线容量必须是 2 的幂");
static_assert((capacityFor(96000.0) & (capacityFor(96000.0) - 1)) == 0, "延迟线容量必须是 2 的幂");
static_assert((capacityFor(192000.0) & (capacityFor(192000.0) - 1)) == 0, "延迟线容量必须是 2 的幂");

// 容量必须容得下 D 的上限（4 s）加一个最大块长（32768）：44.1/48 kHz 下是 2^18，192 kHz 下是 2^20。
static_assert(capacityFor(48000.0) >= 4 * 48000 + 32768, "延迟线必须容得下 D 的上限加一个最大块长");
static_assert(capacityFor(192000.0) >= 4 * 192000 + 32768, "延迟线必须容得下 D 的上限加一个最大块长");
static_assert(capacityFor(44100.0) >= 4 * 44100 + 32768, "延迟线必须容得下 D 的上限加一个最大块长");
static_assert(capacityFor(96000.0) >= 4 * 96000 + 32768, "延迟线必须容得下 D 的上限加一个最大块长");

// 尾巴区要放得下重同步的淡出尾巴（5 ms）：192 kHz 下是 960 帧。
static_assert(LocalLine::kTailFrames >= 192000 * 5 / 1000, "尾巴区至少放得下 192 kHz 下的 5 ms 尾巴");

// 尾巴区放不下 20 ms 的 onset 是允许的：onset 是新读头的包络（读头自己的淡入），不进尾巴区。
// 尾巴区只服务「旧读头的淡出尾巴」。

} // namespace rinne::loop
