// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两端 state 胶水共用的判定，规则见 StateGlueCommon.h。
#include "state/StateGlueCommon.h"

#include <string>
#include <utility>

namespace rinne::state::glue
{

LoadStep loadForApply(const std::uint8_t* data, std::size_t size, Role role, bool settled, RawWriteBack& writeBack,
                      StateDocument& loaded) noexcept
{
    StateLoadResult result = loadState(data, size, role, loaded);
    LoadStep step;
    step.status = result.status;
    step.mode = applyModeFor(result.status, settled);
    writeBack.onLoad(std::move(result));
    return step;
}

void mergeLoaded(StateDocument& current, StateDocument&& loaded, ApplyMode mode) noexcept
{
    switch (mode)
    {
    case ApplyMode::Full:
        current = std::move(loaded);
        return;
    case ApplyMode::ParamsOnly:
        current.params = std::move(loaded.params);
        return;
    case ApplyMode::None:
        return;
    }
}

namespace
{

bool bytesForSaveImpl(StateDocument& document, const RawWriteBack& writeBack, const KnownParam* known,
                      std::size_t knownCount, std::vector<std::uint8_t>& out, SaveResult& result)
{
    if (writeBack.holding())
    {
        out = writeBack.bytes();
        result.heldOriginal = true;
        return true;
    }
    if (known == nullptr && knownCount != 0u)
        return false;

    std::vector<std::string> knownIds;
    knownIds.reserve(knownCount);
    for (std::size_t i = 0; i < knownCount; ++i)
    {
        if (known[i].id == nullptr)
            return false;
        knownIds.emplace_back(known[i].id);
        document.params.set(knownIds.back(), known[i].value);
    }
    std::size_t dropped = 0;
    const bool trimmed = trimToDecodable(document.params, knownIds, dropped);
    result.dropped = dropped;
    return trimmed && encodeDocument(document, out);
}

} // namespace

SaveResult bytesForSave(StateDocument& document, const RawWriteBack& writeBack, const KnownParam* known,
                        std::size_t knownCount, std::vector<std::uint8_t>& out) noexcept
{
    SaveResult result;
    out.clear();
    bool ok = false;
    try
    {
        ok = bytesForSaveImpl(document, writeBack, known, knownCount, out, result);
    }
    catch (...)
    {
        ok = false; // 只可能是内存不足
    }
    if (!ok)
    {
        out.clear();
        result.heldOriginal = false;
    }
    result.ok = ok;
    return result;
}

} // namespace rinne::state::glue
