// SPDX-License-Identifier: GPL-3.0-or-later
//
// 金丝雀：两个线程无同步地写同一个 int。ThreadSanitizer 必须报 data race；不报，说明 tsan 配置没有真的在检查。

#include <thread>

int main()
{
    int shared = 0;
    auto work = [&shared] {
        for (int i = 0; i < 1000; ++i)
            *const_cast<volatile int*>(&shared) = i;
    };
    std::thread a(work);
    std::thread b(work);
    a.join();
    b.join();
    return shared == 12345 ? 1 : 0;
}
