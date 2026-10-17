// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "ipc/SegmentBackendInProcess.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>

#include "ipc/Naming.h"

namespace rinne::ipc
{

namespace
{

// 64 字节对齐的清零缓冲（vector<unsigned char> 只保证 1 字节对齐，放不下 alignas(64) 的段结构），
// 对应共享内存建段时内核清零的语义
struct AlignedBuffer
{
    unsigned char* data = nullptr;
    std::size_t size = 0;

    explicit AlignedBuffer(std::size_t n)
        : data(static_cast<unsigned char*>(::operator new(n, std::align_val_t(64)))), size(n)
    {
        std::memset(data, 0, n);
    }
    ~AlignedBuffer() { ::operator delete(data, std::align_val_t(64)); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
};

std::mutex& segmentsMutex()
{
    static std::mutex m;
    return m;
}

std::map<std::string, std::shared_ptr<AlignedBuffer>>& segments()
{
    static std::map<std::string, std::shared_ptr<AlignedBuffer>> m;
    return m;
}

InitResult rejectName(const std::string& name, NameStatus status) noexcept
{
    reportBadSegmentName(name, status);
    return InitResult::kFailedBadName;
}

} // namespace

InitResult SegmentBackendInProcess::createOrOpen(const std::string& name, std::size_t size, SegmentView& view)
{
    view.reset();
    if (size == 0)
    {
        return InitResult::kFailed;
    }
    if (const NameStatus status = validateSegmentName(kHostNamePlatform, name); status != NameStatus::kOk)
    {
        return rejectName(name, status);
    }

    const std::lock_guard<std::mutex> lock(segmentsMutex());
    auto& map = segments();
    if (const auto it = map.find(name); it != map.end())
    {
        view.base = it->second->data;
        view.size = it->second->size;
        view.created = false;
        view.name = name;
        return InitResult::kOk;
    }
    auto buffer = std::make_shared<AlignedBuffer>(size);
    map[name] = buffer;
    view.base = buffer->data;
    view.size = size;
    view.created = true;
    view.name = name;
    return InitResult::kOk;
}

InitResult SegmentBackendInProcess::openExisting(const std::string& name, SegmentView& view)
{
    view.reset();
    if (const NameStatus status = validateSegmentName(kHostNamePlatform, name); status != NameStatus::kOk)
    {
        return rejectName(name, status);
    }
    const std::lock_guard<std::mutex> lock(segmentsMutex());
    auto& map = segments();
    const auto it = map.find(name);
    if (it == map.end())
    {
        return InitResult::kFailed;
    }
    view.base = it->second->data;
    view.size = it->second->size;
    view.created = false;
    view.name = name;
    return InitResult::kOk;
}

void SegmentBackendInProcess::unmap(SegmentView& view)
{
    view.reset();
}

void SegmentBackendInProcess::resetAllForTesting()
{
    const std::lock_guard<std::mutex> lock(segmentsMutex());
    segments().clear();
}

} // namespace rinne::ipc
