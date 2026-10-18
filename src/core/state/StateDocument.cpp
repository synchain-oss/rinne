// SPDX-License-Identifier: GPL-3.0-or-later
//
// 整份 state 的实现：只做「哪一端认识哪个块」的取舍，字节层面的事全部交给 StateCodec。
#include "state/StateDocument.h"

#include <utility>

namespace rinne::state
{
namespace
{

bool isKnownChunk(Role role, std::uint32_t fourCc) noexcept
{
    return fourCc == kFourCcCfgs || (role == Role::Input && fourCc == kFourCcPrms);
}

std::size_t knownChunkCount(Role role) noexcept
{
    return role == Role::Input ? 2u : 1u;
}

bool decodeDocumentImpl(const Container& container, Role role, StateDocument& out)
{
    if (!isKnownRole(role) || container.role != role)
        return false;

    StateDocument result;
    result.role = role;
    bool hasParams = false;
    bool hasConfigs = false;
    for (const auto& chunk : container.chunks)
    {
        if (!isKnownChunk(role, chunk.fourCc))
        {
            result.unknownChunks.push_back(chunk);
            continue;
        }
        // 同一个已知块出现两次，以哪一份为准都说不通
        bool& seen = chunk.fourCc == kFourCcPrms ? hasParams : hasConfigs;
        if (seen)
            return false;
        seen = true;
        const bool decoded = chunk.fourCc == kFourCcPrms
                                 ? decodeParams(chunk.payload.data(), chunk.payload.size(), result.params)
                                 : decodeConfigs(chunk.payload.data(), chunk.payload.size(), role, result.configs);
        if (!decoded)
            return false;
    }
    // 解得开的就要写得回去：写回时已知块一个不少
    if (result.unknownChunks.size() + knownChunkCount(role) > kMaxChunkCount)
        return false;

    out = std::move(result);
    return true;
}

// role 不认识时 encodeConfigs 先失败，这里不必再判一次。
bool encodeDocumentImpl(const StateDocument& document, std::vector<std::uint8_t>& out)
{
    Container container;
    container.role = document.role;
    if (document.role == Role::Input)
    {
        Chunk params{kFourCcPrms, {}};
        if (!encodeParams(document.params, params.payload))
            return false;
        container.chunks.push_back(std::move(params));
    }
    Chunk configs{kFourCcCfgs, {}};
    if (!encodeConfigs(document.configs, document.role, configs.payload))
        return false;
    container.chunks.push_back(std::move(configs));

    for (const auto& chunk : document.unknownChunks)
    {
        if (isKnownChunk(document.role, chunk.fourCc))
            return false;
        container.chunks.push_back(chunk);
    }
    return encodeContainer(container, out);
}

} // namespace

StateDocument makeDefaultDocument(Role role)
{
    StateDocument document;
    document.role = role;
    return document;
}

bool decodeDocument(const Container& container, Role role, StateDocument& out) noexcept
{
    try
    {
        return decodeDocumentImpl(container, role, out);
    }
    catch (...)
    {
        return false; // 只可能是内存不足；out 在最后一步才赋值，还是调用前的样子
    }
}

bool encodeDocument(const StateDocument& document, std::vector<std::uint8_t>& out) noexcept
{
    try
    {
        return encodeDocumentImpl(document, out);
    }
    catch (...)
    {
        return false;
    }
}

} // namespace rinne::state
