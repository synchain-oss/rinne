// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/state.json
#include "state/StateMigration.h"

#include <utility>

namespace rinne::state
{
namespace
{

StateLoadResult withStatus(StateLoadStatus status, std::uint32_t sourceAbi) noexcept
{
    StateLoadResult result;
    result.status = status;
    result.sourceAbi = sourceAbi;
    return result;
}

StateLoadResult loadStateImpl(const std::uint8_t* data, std::size_t size, Role role, StateDocument& out,
                              std::uint32_t currentAbi, const MigrateFn* chain, std::size_t chainLength)
{
    if (size == 0u)
        return withStatus(StateLoadStatus::Empty, 0u);

    Header header;
    if (!parseHeader(data, size, header) || header.magic != kMagic)
        return withStatus(StateLoadStatus::Corrupt, 0u);

    if (header.abi > currentAbi)
    {
        StateLoadResult result = withStatus(StateLoadStatus::RejectedNewer, header.abi);
        result.preservedOriginal.assign(data, data + size);
        return result;
    }
    if (header.abi == kProbeAbi)
        return withStatus(StateLoadStatus::IgnoredProbe, header.abi);

    Container container;
    if (!decodeContainer(data, size, role, container))
        return withStatus(StateLoadStatus::Corrupt, header.abi);
    if (header.abi < currentAbi && !runMigrations(container, header.abi, currentAbi, chain, chainLength))
        return withStatus(StateLoadStatus::Corrupt, header.abi);

    StateDocument document;
    if (!decodeDocument(container, role, document))
        return withStatus(StateLoadStatus::Corrupt, header.abi);

    out = std::move(document);
    return withStatus(header.abi < currentAbi ? StateLoadStatus::Migrated : StateLoadStatus::Ok, header.abi);
}

} // namespace

bool runMigrations(Container& container, std::uint32_t fromAbi, std::uint32_t toAbi, const MigrateFn* chain,
                   std::size_t chainLength) noexcept
{
    // 最后一级用的是 chain[toAbi - 2]：链至少要有 toAbi - 1 项
    if (fromAbi == kProbeAbi || fromAbi > toAbi || toAbi - 1u > chainLength)
        return false;
    try
    {
        for (std::uint32_t abi = fromAbi; abi < toAbi; ++abi)
        {
            const MigrateFn step = chain[abi - 1u];
            if (step == nullptr || !step(container))
                return false;
        }
        return true;
    }
    catch (...)
    {
        return false; // 迁移函数里分配失败
    }
}

StateLoadResult loadState(const std::uint8_t* data, std::size_t size, Role role, StateDocument& out) noexcept
{
    return loadStateWith(data, size, role, out, kAbi, kMigrators.data(), kMigrators.size());
}

StateLoadResult loadStateWith(const std::uint8_t* data, std::size_t size, Role role, StateDocument& out,
                              std::uint32_t currentAbi, const MigrateFn* chain, std::size_t chainLength) noexcept
{
    try
    {
        return loadStateImpl(data, size, role, out, currentAbi, chain, chainLength);
    }
    catch (...)
    {
        // 只可能是内存不足（例如复制高 abi 的原字节时）；out 在最后一步才赋值，还是调用前的样子
        return withStatus(StateLoadStatus::Corrupt, 0u);
    }
}

} // namespace rinne::state
