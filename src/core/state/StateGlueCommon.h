// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两端 state 胶水共用的判定。Input 的 InputStateGlue 与 Output 的 OutputStateGlue 分属两个插件壳、互不 include，
// 下面这几条规则两端必须逐字一致，所以只写这一份，放在 core（不含 JUCE），由 tests/core/state 直接单测：
//   1. 加载结果怎么分流（applyModeFor、loadForApply、mergeLoaded）：
//        Ok / Migrated                  → 应用，并清掉写回保留（定型之前全量应用，定型之后只取参数）；
//        RejectedNewer                  → 不应用；写回保留接管原字节，保存时原样交回；
//        Empty / Corrupt / IgnoredProbe → 什么都不动：实例保持当前的 state（不是回到默认），写回保留也不动。
//   2. 定型（Settlement）：定型之前成功读入的 state 全量应用，定型之后只取参数。定型只由界面上的用户动作触发，
//      一次性，不可撤销；宿主下发的参数与自动化、任何认领都不触发。哪些桥调用算用户动作，见两端胶水的头文件。
//   3. 保存时交出什么（bytesForSave）：正在保留原字节时原样交出；否则先把已知参数的当前值 set 进文档，
//      再 trimToDecodable（只从末尾丢未知参数 id），最后编码；任何一步失败都交出空块，不交半截字节。
// 契约见 docs/contracts/STATE_SCHEMA.md 的「加载」与「PRMS」两节。
// 线程：这里的函数与类自身不做同步，胶水在自己的锁里调用它们；都会分配内存，不能进音频回调。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace rinne::state::glue
{

// 一次 setState 怎样应用读入的 state。数值只用于诊断与测试。
enum class ApplyMode : std::uint8_t
{
    None = 0, // 不应用：Empty、IgnoredProbe、Corrupt、RejectedNewer
    Full = 1, // 定型之前：参数、全部设置与未知项，整份换成读入的
    ParamsOnly = 2, // 定型之后：只换参数列表（含未知 id），设置（含组号与未知键）与未知块保持当前
};

// 成功读入：只有这两种会应用，也只有它们取代写回保留。
constexpr bool isSuccessfulLoad(StateLoadStatus status) noexcept
{
    return status == StateLoadStatus::Ok || status == StateLoadStatus::Migrated;
}

constexpr ApplyMode applyModeFor(StateLoadStatus status, bool settled) noexcept
{
    if (!isSuccessfulLoad(status))
        return ApplyMode::None;
    return settled ? ApplyMode::ParamsOnly : ApplyMode::Full;
}

struct LoadStep
{
    StateLoadStatus status = StateLoadStatus::Corrupt;
    ApplyMode mode = ApplyMode::None;
};

// setState 的前半：loadState，再按结果更新写回保留（RejectedNewer 接管、成功读入清掉、其余不动），最后定应用方式。
// 只有 mode 不是 None 时 loaded 才是读入的文档；否则 loaded 保持调用前的样子。
LoadStep loadForApply(const std::uint8_t* data, std::size_t size, Role role, bool settled, RawWriteBack& writeBack,
                      StateDocument& loaded) noexcept;

// setState 的后半：按应用方式把读入的文档并进实例的文档。
//   Full        整份换成读入的，以最后一份为准；
//   ParamsOnly  只换参数列表，设置与未知块保持当前。Output 没有参数，它的 ParamsOnly 什么都不改；
//   None        什么都不动。
void mergeLoaded(StateDocument& current, StateDocument&& loaded, ApplyMode mode) noexcept;

// 定型：第一次用户动作让实例定型，之后不可撤销。
class Settlement
{
public:
    bool settled() const noexcept { return settled_; }

    // 返回 true 表示这一次调用就是定型的那一刻，一个实例只有一次；已经定型时什么都不改，返回 false。
    bool markUserAction() noexcept
    {
        if (settled_)
            return false;
        settled_ = true;
        return true;
    }

private:
    bool settled_ = false;
};

// 参数层交给保存的一个已知参数：id 与当前的明文值。
struct KnownParam
{
    const char* id = nullptr;
    float value = 0.0f;
};

struct SaveResult
{
    bool ok = false; // out 是能交给宿主的整份 state
    bool heldOriginal = false; // out 是写回保留的原字节
    std::size_t dropped = 0; // 这次为了压进上限丢掉的未知参数 id 条数
};

// 保存时交出什么，顺序固定：
//   1. 正在保留原字节：原样复制到 out，文档与已知参数一概不看（保留期间的改动不进 state）；
//   2. 否则把 known 里的每个参数 set 进 document.params（已有的就地改值，没有的追加在末尾）；
//   3. 再 trimToDecodable：只从末尾丢未知 id，丢了几条记进 dropped；
//   4. 编码进 out。
// 第 2、3 步改的是实例自己的文档：丢掉的未知 id 本来就写不回去，下次保存不会再丢一次，也不会再计一次。
// 先 set 再 trim：反过来的话，trim 时列表里还没有补上的参数，补完又可能超出上限，整份编不出来。
// 任何一步失败（含内存不足）：ok 为 false，out 为空。宿主存下空块，下次加载判 Empty，保持当前。
// known 为空指针时 knownCount 必须为 0（Output 就这样调）；known 里的 id 不能为空指针。
SaveResult bytesForSave(StateDocument& document, const RawWriteBack& writeBack, const KnownParam* known,
                        std::size_t knownCount, std::vector<std::uint8_t>& out) noexcept;

} // namespace rinne::state::glue
