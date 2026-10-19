// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/state.json
//
// state 的加载入口：先判头（magic 与 abi），再按 abi 分流，最后解成整份文档。判读的先后是有意的：
//   1. 零字节：没有可恢复的东西（例如还没接上 state 的构建存下的工程）。保持当前，不提示。
//   2. 头读不出来、magic 不符：不是这个格式。保持当前，提示 state 未能恢复。
//   3. abi 高于本构建：整份拒载，把原字节交回调用方。这一步先于块结构的检查：更高的 abi 可能换了载荷，按本构建的
//      规则挑它的错，代价是把用户的新版数据丢掉。头的前 16 字节（magic、abi、role、块数）位置与含义永不改变，
//      所以 role 照样比得了：role 按整字比较，不符说明这是对端的字节，不该原样固化进本端的工程（判坏字节）。
//      提示「工程由更新的版本保存」由调用方负责。任何一次 setState 遇到它都拒载并保留原字节，保存时原样交回，
//      直到一份成功读入的 state 取代它（见 RawWriteBack）。
//   4. abi 0：探针构建写的 state，只在探针身份下有效。只认头，内容一概不用，保持当前，不提示。
//   5. abi 1 起低于本构建的：按 kMigrators 一级一级升到本构建的 abi，任何一级失败即整份失败。迁移只改载荷：容器
//      框架（头的长度、块头、补齐、块数的含义）跨 abi 不变，所以本构建的 decodeContainer 一定解得出旧字节；迁移
//      之后再查一遍容器上限，越界按坏字节处理。
//   6. 解成文档；容器或载荷不合规时整份失败（保持当前，提示 state 未能恢复）。
// 「保持当前」由调用方实现：失败与忽略时 loadState 不碰 out，调用方传进来的是什么（实例此刻的 state，还没读入过
// 成功的 state 时就是新实例的默认 state），出来还是什么，不会留下填了一半的状态。
// 应用方式与写回保护是两件事。应用方式按「定型」区分：定型之前每一份成功读入（Ok、Migrated）的 state 全量应用，
// 以最后一份为准；定型之后的 state（宿主预设、A/B、撤销）只改参数，并按 ARCHITECTURE.md 的 FADE_IN 淡变。定型指
// 第一次界面上的用户动作（经桥由界面发起的参数改动，如 beginParamGesture 起的拖动与 setLimiter
// 的单击；界面上的设置改动；手选组；确认接管）；宿主下发的参数与自动化 不算，任何认领也不算（provisional
// 的认领本来就要被后到的工程 state 抢占）。Empty、探针、Corrupt、RejectedNewer
// 不算成功读入。定型由调用方判断，本层不管。写回保护：与第几次 setState 无关，见 RawWriteBack。
// 坏字节不交回调用方：它们不该被原样存回工程文件。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
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

// 就地把容器从 abi n 升到 n + 1。迁移只改载荷：容器布局（头的长度、块头、补齐、块数的含义）跨 abi 不变，所以旧
// 字节总能先用本构建的 decodeContainer 解出来，再交给迁移函数。低版本的载荷未必过得了本构建的载荷校验，先迁移、
// 再按本构建的规则解码，能迁移的旧工程才不会被当成坏字节。迁移函数要改哪个块的载荷就自己读写哪个。
// loadState 在迁移成功之后重新检查块数与单块上限：迁移函数越界时整份判 Corrupt。
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

// 宿主序列里「保存时交回什么」。每次 loadState 之后用它的结果调一次 onLoad：
//   RejectedNewer：接管。保存时交回这份原字节，先前保留的那份被替换（用户最近给的工程为准）；
//   Ok / Migrated：一份成功读入的 state 取代保留的字节，之后保存的是实例当前的 state；
//   Empty / Corrupt / IgnoredProbe：什么都没读进来，保持原状：正在保留的继续保留，没在保留的仍不保留。
//   role 是对端的高 abi 字节判 Corrupt，所以不会接管。
// 线程：自身不做同步，调用方要和实例 state 用同一把锁（或者都在消息线程上）。
// 保留期间 setState 的参数改动不进 state：getState 交回的仍是原字节，这些改动保存时会丢。界面要常驻提示
// 「这份工程由更新版本保存，本版本不会改写它」（界面卡负责）。
class RawWriteBack
{
public:
    void onLoad(StateLoadResult&& result) noexcept
    {
        if (result.status == StateLoadStatus::RejectedNewer)
            held_ = std::move(result.preservedOriginal);
        else if (result.status == StateLoadStatus::Ok || result.status == StateLoadStatus::Migrated)
            held_.clear();
    }

    bool holding() const noexcept { return !held_.empty(); }

    // 保留着的原字节；没在保留时为空。
    const std::vector<std::uint8_t>& bytes() const noexcept { return held_; }

private:
    std::vector<std::uint8_t> held_;
};

} // namespace rinne::state
