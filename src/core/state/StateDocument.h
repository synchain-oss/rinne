// SPDX-License-Identifier: GPL-3.0-or-later
//
// 一个插件实例的整份 state。处理器存取 state（经参数层的胶水）只和这一层、以及 StateMigration.h 的 loadState
// 打交道；它把 StateCodec.h 的容器层与载荷层拼起来，自己只管「哪一端认识哪个块」。
//
// 每一端认识的块：
//   Input  = PRMS + CFGS（配置的唯一真源在 Input：参数值与全部设置都由它存）
//   Output = CFGS（只有它自己决定分组与外观要用的几项）
// 其余的块（未知 fourcc，以及只有对端才认识的块，例如 Output 收到的 PRMS）连原始字节一起进 unknownChunks，
// 保存时按原序原样写回。
//
// 编码顺序固定：Input 先 PRMS 后 CFGS，Output 只有 CFGS，然后是 unknownChunks。已知块总是写出来，哪怕解码时
// 缺了它：缺的部分已经按默认值补上，写回去的就是这份默认值。
#pragma once

#include <cstdint>
#include <vector>

#include "state/StateCodec.h"

namespace rinne::state
{

struct StateDocument
{
    Role role = Role::Input;
    Params params; // 只有 Input 用；Output 恒为空
    Configs configs;
    std::vector<Chunk> unknownChunks; // 这一端不认识的块，按出现序原样回写
};

// 新实例的起点：参数列表为空（由参数层按参数表补全），设置全部取默认值。
StateDocument makeDefaultDocument(Role role);

// 解码。已知块缺失不算失败（按默认值）。
// 整份失败（不碰 out）：role 与容器不符；同一个已知块出现两次；已知块的载荷解不开；不认识的块多到写回时容不下
// 这一端的已知块。
bool decodeDocument(const Container& container, Role role, StateDocument& out) noexcept;

// 编码。失败：role 不认识；unknownChunks 里混进这一端的已知块（会写出两份）；任何一层超出格式上限。
// 由 decodeDocument 解出来、没再加东西的文档一定编得出来。
bool encodeDocument(const StateDocument& document, std::vector<std::uint8_t>& out) noexcept;

} // namespace rinne::state
