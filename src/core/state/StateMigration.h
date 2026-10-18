// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/state.json
//
// state 的加载入口：先判头（magic 与 abi），再按 abi 分流，最后解成整份文档。判读的先后是有意的：
//   1. 零字节：没有可恢复的东西（例如还没接上 state 的构建存下的工程）。保持默认，不提示。
//   2. 头读不出来、magic 不符：不是这个格式。保持默认，提示 state 未能恢复。
//   3. abi 高于本构建：整份拒载，把原字节交回调用方，保存时一个字节都不改地写回去。这一步先于 role 与块结构的
//      检查：更高的 abi 可能换了布局，按本构建的规则挑它的错，代价是把用户的新版数据丢掉。提示「工程由更新的版本
//      保存」由调用方负责。
//   4. abi 0：探针构建写的 state，只在探针身份下有效。只认头，内容一概不用，保持默认，不提示。
//   5. abi 1 起低于本构建的：按 kMigrators 一级一级升到本构建的 abi，任何一级失败即整份失败。
//   6. 解成文档；容器或载荷不合规时整份失败（保持默认，提示 state 未能恢复）。
// 「保持默认」由调用方实现：失败与忽略时 loadState 不碰 out，调用方传进来的是什么（通常是 makeDefaultDocument
// 的结果，或者实例当前的 state），出来还是什么，不会留下填了一半的状态。
// 坏字节不交回调用方：它们不该被原样存回工程文件。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "state/StateDocument.h"

namespace rinne::state
{

// 数值只用于诊断与测试；新状态只能追加。
enum class StateLoadStatus : std::uint8_t
{
    Ok = 0, // 本构建的 abi，已解出文档
    Migrated = 1, // 较低的 abi，逐级迁移后解出文档
    Empty = 2, // 零字节，什么都没改
    IgnoredProbe = 3, // 探针构建写的 abi 0，什么都没改
    RejectedNewer = 4, // abi 高于本构建，什么都没改；preservedOriginal 是整份原字节
    Corrupt = 5, // 不是这个格式或已损坏，什么都没改
};

struct StateLoadResult
{
    StateLoadStatus status = StateLoadStatus::Corrupt;
    std::uint32_t sourceAbi = 0; // 头里写的 abi（头读不出来时为 0）
    std::vector<std::uint8_t> preservedOriginal; // 只有 RejectedNewer 才有内容
};

inline constexpr std::uint32_t kProbeAbi = 0u;

// 就地把容器从 abi n 升到 n + 1。迁移在容器层上做：低版本的载荷未必过得了本构建的载荷校验，先迁移、再按本构建
// 的规则解码，能迁移的旧工程才不会被当成坏字节。迁移函数要改哪个块的载荷就自己读写哪个。
using MigrateFn = bool (*)(Container& container);

// kMigrators[i] 把 abi i + 1 升到 i + 2。abi 1 是第一版，还没有迁移函数。下一次改格式时把 kAbi 加一、在这里
// 追加一个函数，并新增一份该 abi 的基线 bin（旧的保留，作迁移测试的输入）。
inline constexpr std::array<MigrateFn, kAbi - 1u> kMigrators{};
static_assert(kMigrators.size() + 1u == kAbi, "abi 1 起每升一级都要有一个迁移函数");

// 逐级迁移：从 fromAbi 一级一级升到 toAbi，从 abi n 升到 n + 1 用 chain[n - 1]。fromAbi 必须 ≥ 1 且 ≤ toAbi；
// 链不够长、某一级为空或返回失败都整体失败，此时容器的内容不确定，调用方应当丢弃它。
// loadState 用 kMigrators 调它；单测用假链验证顺序与失败处理。
bool runMigrations(Container& container, std::uint32_t fromAbi, std::uint32_t toAbi, const MigrateFn* chain,
                   std::size_t chainLength) noexcept;

// 完整加载。只有 Ok 与 Migrated 会写 out。
StateLoadResult loadState(const std::uint8_t* data, std::size_t size, Role role, StateDocument& out) noexcept;

// 同 loadState，只是「本构建的 abi」与迁移链由参数给出：loadState 就是 loadStateWith(…, kAbi, kMigrators)。
// 只给单测用：abi 1 时迁移链为空，迁移分支要假装本构建是更高的 abi、配上假链才走得到。
StateLoadResult loadStateWith(const std::uint8_t* data, std::size_t size, Role role, StateDocument& out,
                              std::uint32_t currentAbi, const MigrateFn* chain, std::size_t chainLength) noexcept;

} // namespace rinne::state
