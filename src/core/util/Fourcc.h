// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 四字符码：共享内存段头与 state 容器开头的 magic。按内存字节序排：小端主机上段内前 4 字节读出来就是这四个字母，
// 用十六进制查看器看段内容时一眼认得出。两端同机同字节序，比对同一个常量即可，不用实现定义的多字符字面量 'ABCD'。
#pragma once

#include <cstdint>

namespace rinne::util
{

constexpr std::uint32_t fourcc(char c0, char c1, char c2, char c3) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(c0)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c1)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c2)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c3)) << 24);
}

// 字符串字面量形式：恰好四个字符（数组长度 5 含结尾的 0），多一个少一个都编不过
constexpr std::uint32_t fourcc(const char (&code)[5]) noexcept
{
    return fourcc(code[0], code[1], code[2], code[3]);
}

} // namespace rinne::util
