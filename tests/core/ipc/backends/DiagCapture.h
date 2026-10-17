// SPDX-License-Identifier: GPL-3.0-or-later
//
// 测试用的 IpcDiag sink：作用域内把后端报出的诊断收进一张表，析构时换回原来的 sink。用例在同一个线程上顺序跑，
// 后端也在调用线程上同步报告，所以一张受锁保护的表就够了。
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "ipc/IpcDiag.h"

namespace rinne::test
{

struct CapturedDiag
{
    ipc::IpcDiagOp op;
    ipc::IpcDiagCode code;
    int error;
    std::string segment;
    std::string path;
    std::string detail;
};

class DiagCapture
{
public:
    DiagCapture()
    {
        clear();
        previous_ = ipc::setIpcDiagSink(&DiagCapture::sink);
    }
    ~DiagCapture() { ipc::setIpcDiagSink(previous_); }
    DiagCapture(const DiagCapture&) = delete;
    DiagCapture& operator=(const DiagCapture&) = delete;

    std::vector<CapturedDiag> events() const
    {
        const std::lock_guard<std::mutex> lock(mutex());
        return store();
    }
    void clear()
    {
        const std::lock_guard<std::mutex> lock(mutex());
        store().clear();
    }
    bool has(ipc::IpcDiagCode code) const
    {
        for (const auto& e : events())
        {
            if (e.code == code)
            {
                return true;
            }
        }
        return false;
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::vector<CapturedDiag>& store()
    {
        static std::vector<CapturedDiag> v;
        return v;
    }
    static void sink(const ipc::IpcDiagEvent& e) noexcept
    {
        try
        {
            const std::lock_guard<std::mutex> lock(mutex());
            store().push_back({e.op, e.code, e.error, e.segment, e.path, e.detail});
        }
        catch (...)
        {
        }
    }

    ipc::IpcDiagSink previous_ = nullptr;
};

} // namespace rinne::test
