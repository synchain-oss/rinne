// SPDX-License-Identifier: GPL-3.0-or-later
//
// 金丝雀：堆上越界写一个元素。AddressSanitizer 必须报 heap-buffer-overflow。

int main()
{
    volatile int* block = new int[4];
    block[4] = 1;
    delete[] const_cast<int*>(block);
    return 0;
}
