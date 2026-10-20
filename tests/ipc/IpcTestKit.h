// SPDX-License-Identifier: GPL-3.0-or-later
//
// 跨进程用例共用的东西：一个带测试作用域与平台后端的架子（POSIX 上连同临时锁目录）、peer 的启动与结果行解析、
// 以及两边都要用的位置编码/解码。时间与等待仍由用例自己给；这里不含任何 sleep 循环。
//
// 用例先问 peerUsable()，不可用时 SKIP 并说明原因：Linux 的 PlatformSegmentBackend 是进程内后端，跨进程不可能；
// 构建时没把 rinne_ipc_peer 接进来（RINNE_IPC_NO_PEER）也一样。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"
#include "ipc/ProcessInfo.h"
#include "support/Spawn.h"

#if defined(_WIN32)
#include "ipc/SegmentBackendWin32.h"
#elif defined(__APPLE__)
#include "core/ipc/backends/TempDir.h"
#include "ipc/SegmentBackendPosix.h"
#endif

namespace rinne::test
{

using namespace rinne::ipc;

#if defined(_WIN32) || defined(__APPLE__)
inline constexpr bool kCrossProcessSupported = true;
#else
// Linux 的 PlatformSegmentBackend 是 SegmentBackendInProcess：插件在同一进程里互相看得见，跨进程不可见
inline constexpr bool kCrossProcessSupported = false;
#endif

inline const char* peerExecutable() noexcept
{
#ifdef RINNE_IPC_PEER_PATH
    return RINNE_IPC_PEER_PATH;
#else
    return nullptr;
#endif
}

inline bool peerUsable() noexcept
{
    return kCrossProcessSupported && peerExecutable() != nullptr;
}

inline std::string peerSkipReason()
{
    if (!kCrossProcessSupported)
    {
        return "this platform uses the in-process segment backend; cross-process IPC cannot be tested here";
    }
    if (peerExecutable() == nullptr)
    {
        return "rinne_ipc_peer was not wired into this build (tools off or target missing)";
    }
    return {};
}

constexpr u32 kBlockFrames = 480;

// 位置编码：绝对位置的低 24 位放左声道、高 24 位放右声道；两边都按位拼回。
// tools/rinne_ipc_peer.cpp 里有一份同样的编码、解码与 readBlock（peer 不依赖测试头），改动须同改。
inline void encodeRamp(u64 position, float& left, float& right) noexcept
{
    left = static_cast<float>(position & 0xFFFFFFull);
    right = static_cast<float>((position >> 24) & 0xFFFFFFull);
}

inline bool decodeRamp(float left, float right, u64& position) noexcept
{
    if (!(left >= 0.0f) || !(left < 16777216.0f) || !(right >= 0.0f) || !(right < 16777216.0f))
    {
        return false;
    }
    const u64 l = static_cast<u64>(left);
    const u64 r = static_cast<u64>(right);
    if (static_cast<float>(l) != left || static_cast<float>(r) != right)
    {
        return false;
    }
    position = l | (r << 24);
    return true;
}

// 测试作用域 + 平台后端（POSIX 再带一个临时锁目录）与它名下的输出文件。启动 peer 用 spawn()，文件在析构时逐个删掉；
// POSIX 的锁目录由 TempDir 自己收。删除只针对本类新建的文件（非递归）。
class PeerRig
{
public:
    PeerRig() : scope_(NameScope::randomForTesting()), naming_(scope_)
    {
#if defined(_WIN32)
        backend_ = std::make_unique<SegmentBackendWin32>();
#elif defined(__APPLE__)
        tempDir_ = std::make_unique<rinne::test::TempDir>();
        lockDir_ = tempDir_->path();
        backend_ = std::make_unique<SegmentBackendPosix>(SegmentBackendPosix::LockDirForTesting{lockDir_});
#endif
    }

    ~PeerRig()
    {
        for (const std::string& file : files_)
        {
            std::error_code error;
            std::filesystem::remove(file, error);
        }
    }
    PeerRig(const PeerRig&) = delete;
    PeerRig& operator=(const PeerRig&) = delete;

    const NameScope& scope() const noexcept { return scope_; }
    const Naming& naming() const noexcept { return naming_; }
    ISegmentBackend& backend() noexcept { return *backend_; }

    std::vector<std::string> commonArgs() const
    {
        std::vector<std::string> args{"--scope", std::string(scope_.tag())};
#if defined(__APPLE__)
        args.push_back("--lock-dir");
        args.push_back(lockDir_);
#endif
        return args;
    }

    std::unique_ptr<Spawned> spawn(const std::vector<std::string>& args, const std::string& outStem)
    {
        const char* exe = peerExecutable();
        if (exe == nullptr)
        {
            return nullptr;
        }
        std::vector<std::string> all = args;
        const std::vector<std::string> common = commonArgs();
        all.insert(all.end(), common.begin(), common.end());
        return Spawned::start(exe, all, outputPath(outStem));
    }

private:
    std::string outputPath(const std::string& stem)
    {
#if defined(__APPLE__)
        const std::filesystem::path directory = tempDir_->path();
#else
        const std::filesystem::path directory = std::filesystem::temp_directory_path();
#endif
        const std::string name =
            "rinne-ipc-" + std::to_string(currentProcessId()) + "-" + std::to_string(fileCounter_++) + "-" + stem;
        const std::string path = (directory / name).string();
        files_.push_back(path);
        return path;
    }

    NameScope scope_;
    Naming naming_;
    std::unique_ptr<ISegmentBackend> backend_;
    std::vector<std::string> files_;
    u32 fileCounter_ = 0;
#if defined(__APPLE__)
    std::unique_ptr<rinne::test::TempDir> tempDir_;
    std::string lockDir_;
#endif
};

struct PeerResult
{
    bool ok = false;
    std::string state;
    unsigned long long blocks = 0;
    unsigned long long frames = 0;
    unsigned long long valid = 0;
    unsigned long long bad = 0;
    unsigned long long resyncs = 0;
    unsigned long long abiVersion = 0;
};

inline bool parseDecimal(const std::string& text, unsigned long long& value) noexcept
{
    if (text.empty())
    {
        return false;
    }
    unsigned long long result = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        result = result * 10 + static_cast<unsigned long long>(c - '0');
    }
    value = result;
    return true;
}

inline std::string fieldOf(const std::string& line, const std::string& key)
{
    const std::string prefix = key + "=";
    const std::size_t at = line.find(prefix);
    if (at == std::string::npos)
    {
        return {};
    }
    const std::size_t begin = at + prefix.size();
    const std::size_t end = line.find(' ', begin);
    return line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// 解析最后一行 result…；缺字段或不是十进制数就 ok = false
inline PeerResult parsePeerResult(const std::string& output)
{
    PeerResult result;
    const std::size_t at = output.rfind("result ");
    if (at == std::string::npos)
    {
        return result;
    }
    const std::size_t end = output.find('\n', at);
    std::string line = output.substr(at, end == std::string::npos ? std::string::npos : end - at);
    while (!line.empty() && line.back() == '\r')
    {
        line.pop_back(); // Windows 文本模式的标准输出以 CR LF 结尾
    }
    result.state = fieldOf(line, "state");
    const bool numbers =
        parseDecimal(fieldOf(line, "blocks"), result.blocks) && parseDecimal(fieldOf(line, "frames"), result.frames) &&
        parseDecimal(fieldOf(line, "valid"), result.valid) && parseDecimal(fieldOf(line, "bad"), result.bad) &&
        parseDecimal(fieldOf(line, "resyncs"), result.resyncs) &&
        parseDecimal(fieldOf(line, "abiVersion"), result.abiVersion);
    result.ok = !result.state.empty() && numbers;
    return result;
}

inline std::string segmentName(const Naming& naming, SegmentId id)
{
    std::string name;
    REQUIRE(naming.name(id, name) == NameStatus::kOk);
    return name;
}

// 只读打开一份视图、抄前面 bytes 个字节、立刻解映射。abi 用例用它证明我们一个字节都没写
inline std::vector<unsigned char> readHeaderBytes(ISegmentBackend& backend, const std::string& name, std::size_t bytes)
{
    SegmentView view;
    REQUIRE(backend.openExistingReadOnly(name, view) == InitResult::kOk);
    const auto* base = static_cast<unsigned char*>(view.base);
    std::vector<unsigned char> out(base, base + bytes);
    backend.unmap(view);
    return out;
}

} // namespace rinne::test
