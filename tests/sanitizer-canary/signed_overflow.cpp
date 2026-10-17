// SPDX-License-Identifier: GPL-3.0-or-later
//
// 金丝雀：有符号整数溢出。UndefinedBehaviorSanitizer 在 -fno-sanitize-recover=all 下必须报 runtime error 并以非 0
// 退出。

#include <climits>

int main()
{
    volatile int value = INT_MAX;
    value = value + 1;
    return 0;
}
