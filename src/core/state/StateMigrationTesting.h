// SPDX-License-Identifier: GPL-3.0-or-later
//
// 只给单测用的加载入口。处理器一律调 StateMigration.h 的 loadState；这个头不由公开头 include，src 下其他代码即使
// include 了 StateMigration.h 也拿不到 loadStateWith。
#pragma once

#include <cstddef>
#include <cstdint>

#include "state/StateMigration.h"

namespace rinne::state::testing
{

// 同 loadState，只是「本构建的 abi」与迁移链由参数给出：loadState 就是 loadStateWith(…, kAbi, kMigrators)。
// abi 1 时迁移链为空，迁移分支要假装本构建是更高的 abi、配上假链才走得到。
StateLoadResult loadStateWith(const std::uint8_t* data, std::size_t size, Role role, StateDocument& out,
                              std::uint32_t currentAbi, const MigrateFn* chain, std::size_t chainLength) noexcept;

} // namespace rinne::state::testing
