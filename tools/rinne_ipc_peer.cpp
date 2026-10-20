// SPDX-License-Identifier: GPL-3.0-or-later
//
// rinne_ipc_peer：跨进程测试用的对端。起一个真实的插件实例骨架：一条 25 Hz 的消息线程跑 Rendezvous 的 tick，
// 一条 10 ms 的「音频」线程按绑定块写/读环。输出逐行刷出，测试按行解析：开始一行 peer…，状态变化各一行 state…，
// 结束一行 result…。
//
// 正常模式用 kIpcAbi 的 Rendezvous；--abi 给别的值时不能走 Rendezvous（它恒用 kIpcAbi），改为自己建会合段、
// 用对方的 abi 初始化、把版本写进自己角色的端点槽：这就是另一个 abi 的构建留下的现场，供本进程的 abi 测试观察。
//
// 退出码：0 正常结束，2 参数错（用法到 stderr），3 本平台只有进程内后端（Linux），跨进程不可能。
//
// 标准输出是测试的接口：每一行都以 '\n' 结尾并立刻 flush；音频线程一行都不打印。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "ipc/Naming.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/ProcessInfo.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "util/MonoClock.h"

namespace
{

using namespace rinne::ipc;

using Clock = std::chrono::steady_clock;

constexpr u32 kFramesPerBlock = 480;

struct Options
{
    Role role = Role::kInput;
    bool haveRole = false;
    u32 group = 0;
    std::optional<NameScope> scope;
    u64 seconds = 3;
    std::string pattern = "ramp";
    u32 abi = kIpcAbi;
    ClaimIntent intent = ClaimIntent::kManual;
    std::string lockDir;
    u32 version = 0;
    bool haveVersion = false;
};

void printUsage()
{
    std::fprintf(stderr, "usage: rinne_ipc_peer --role input|output --group 1..8 --scope <test tag> [--seconds S=3]\n"
                         "                      [--pattern ramp] [--abi N] [--intent manual|smart|restore|confirmed]\n"
                         "                      [--lock-dir <dir>] [--version <u32>]\n");
}

bool parseU64(const std::string& text, u64& out)
{
    if (text.empty())
    {
        return false;
    }
    u64 value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (value > (UINT64_MAX - digit) / 10u)
        {
            return false;
        }
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

bool parseU32(const std::string& text, u32& out)
{
    u64 value = 0;
    if (!parseU64(text, value) || value > 0xffffffffull)
    {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

bool parseOptions(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        std::string key = arg;
        std::string inlineValue;
        bool hasInline = false;
        const std::size_t eq = arg.find('=');
        if (eq != std::string::npos)
        {
            key = arg.substr(0, eq);
            inlineValue = arg.substr(eq + 1);
            hasInline = true;
        }
        const auto take = [&](std::string& out) {
            if (hasInline)
            {
                out = inlineValue;
                return true;
            }
            if (i + 1 >= argc)
            {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (key == "--role")
        {
            std::string value;
            if (!take(value))
            {
                return false;
            }
            if (value == "input")
            {
                options.role = Role::kInput;
            }
            else if (value == "output")
            {
                options.role = Role::kOutput;
            }
            else
            {
                return false;
            }
            options.haveRole = true;
        }
        else if (key == "--group")
        {
            std::string value;
            if (!take(value) || !parseU32(value, options.group) || options.group < 1 || options.group > kGroupCount)
            {
                return false;
            }
        }
        else if (key == "--scope")
        {
            std::string value;
            if (!take(value))
            {
                return false;
            }
            const auto scope = NameScope::forTesting(value);
            if (!scope.has_value())
            {
                return false;
            }
            options.scope = *scope;
        }
        else if (key == "--seconds")
        {
            std::string value;
            if (!take(value) || !parseU64(value, options.seconds) || options.seconds < 1 || options.seconds > 3600)
            {
                return false;
            }
        }
        else if (key == "--pattern")
        {
            std::string value;
            if (!take(value) || value != "ramp")
            {
                return false;
            }
            options.pattern = value;
        }
        else if (key == "--abi")
        {
            std::string value;
            if (!take(value) || !parseU32(value, options.abi) || options.abi == 0)
            {
                return false;
            }
        }
        else if (key == "--intent")
        {
            std::string value;
            if (!take(value))
            {
                return false;
            }
            if (value == "manual")
            {
                options.intent = ClaimIntent::kManual;
            }
            else if (value == "smart")
            {
                options.intent = ClaimIntent::kSmart;
            }
            else if (value == "restore")
            {
                options.intent = ClaimIntent::kRestore;
            }
            else if (value == "confirmed")
            {
                options.intent = ClaimIntent::kManualConfirmed;
            }
            else
            {
                return false;
            }
        }
        else if (key == "--lock-dir")
        {
            if (!take(options.lockDir))
            {
                return false;
            }
        }
        else if (key == "--version")
        {
            std::string value;
            if (!take(value) || !parseU32(value, options.version))
            {
                return false;
            }
            options.haveVersion = true;
        }
        else
        {
            return false;
        }
    }
    if (!options.haveRole || !options.scope.has_value() || options.group == 0)
    {
        return false;
    }
    if (!options.haveVersion)
    {
        options.version = options.abi == kIpcAbi ? packVersion(0, 9, 0) : packVersion(2, 0, 0);
    }
    return true;
}

const char* roleName(Role role) noexcept
{
    return role == Role::kInput ? "input" : "output";
}

const char* stateName(RendezvousState state) noexcept
{
    switch (state)
    {
    case RendezvousState::kValidator:
        return "validator";
    case RendezvousState::kUnassigned:
        return "unassigned";
    case RendezvousState::kWaitingPrepare:
        return "waitingPrepare";
    case RendezvousState::kOpening:
        return "opening";
    case RendezvousState::kIpcUnavailable:
        return "ipcUnavailable";
    case RendezvousState::kAbiMismatch:
        return "abiMismatch";
    case RendezvousState::kConflict:
        return "conflict";
    case RendezvousState::kActive:
        return "active";
    case RendezvousState::kDormant:
        return "dormant";
    }
    return "unknown";
}

u64 elapsedMs(Clock::time_point start) noexcept
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    return ms > 0 ? static_cast<u64>(ms) : 0;
}

// Linux 只有进程内后端：这几个入口在那边不会被调用，但工具在所有平台都参与构建，零警告是硬要求
[[maybe_unused]] void printStart(const Options& options)
{
    std::printf("peer pid=%u role=%s group=%u abi=%u version=%u\n", currentProcessId(), roleName(options.role),
                options.group, options.abi, options.version);
    std::fflush(stdout);
}

void printState(const char* name, u64 tMs)
{
    std::printf("state %s t=%llu\n", name, static_cast<unsigned long long>(tMs));
    std::fflush(stdout);
}

void printResult(const char* state, u64 blocks, u64 frames, u64 valid, u64 bad, u64 resyncs, u32 abiVersion)
{
    std::printf("result state=%s blocks=%llu frames=%llu valid=%llu bad=%llu resyncs=%llu abiVersion=%u\n", state,
                static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(valid), static_cast<unsigned long long>(bad),
                static_cast<unsigned long long>(resyncs), abiVersion);
    std::fflush(stdout);
}

// 写进环里的绝对位置编码：左声道低 24 位，右声道高 24 位；两个 float 都是精确的整数，读方按位拼回来。
// 编码、解码与 readBlock 在 tests/ipc/IpcTestKit.h 和 test_peer_continuity.cpp 里各有一份：peer 是独立的可执行文件，
// 不依赖测试头；改任何一边都必须同改另一边，否则两个方向的连续性用例会一起报 bad。
void encodeRamp(u64 position, float& left, float& right) noexcept
{
    left = static_cast<float>(position & 0xFFFFFFull);
    right = static_cast<float>((position >> 24) & 0xFFFFFFull);
}

bool decodeRamp(float left, float right, u64& position) noexcept
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

struct Counters
{
    u64 blocks = 0;
    u64 frames = 0;
    u64 valid = 0;
    u64 bad = 0;
    u64 resyncs = 0;
};

// 音频线程：不分配、不加锁、不打印、不读墙钟。照契约经 RingWriter 写，每块先查写权
void writeBlock(RingSegment& ring, u64 writerToken, RingWriter& writer, Counters& counters) noexcept
{
    if (!LoopRing::isWriter(ring, writerToken))
    {
        return;
    }
    const RingWriteResult result = writer.write(
        ring, kFramesPerBlock, [](u32, u64 position, float& left, float& right) { encodeRamp(position, left, right); });
    if (result == RingWriteResult::kOk)
    {
        ++counters.blocks;
        counters.frames += kFramesPerBlock;
    }
}

void readBlock(RingSegment& ring, i64& readPos, float* left, float* right, Counters& counters) noexcept
{
    const u64 writeHead = LoopRing::writeHead(ring);
    if (readPos < 0)
    {
        readPos = static_cast<i64>(writeHead);
    }
    if (frameDiff(writeHead, static_cast<u64>(readPos)) > static_cast<i64>(kRingFrames / 2))
    {
        readPos = static_cast<i64>(writeHead);
        ++counters.resyncs;
    }
    const i64 available = frameDiff(writeHead, static_cast<u64>(readPos));
    if (available > 0)
    {
        const u32 frames = static_cast<u32>(std::min<i64>(available, static_cast<i64>(kFramesPerBlock)));
        const RingRead read = LoopRing::read(ring, readPos, frames, left, right);
        for (i64 position = read.validFrom; position < read.validTo; ++position)
        {
            u64 decoded = 0;
            if (decodeRamp(left[position - readPos], right[position - readPos], decoded) &&
                decoded == static_cast<u64>(position))
            {
                ++counters.valid;
            }
            else
            {
                ++counters.bad;
            }
        }
        readPos += frames;
        counters.frames += frames;
    }
    ++counters.blocks;
}

[[maybe_unused]] int runNormal(const Options& options, ISegmentBackend& backend)
{
    const auto wallStart = Clock::now();
    RendezvousConfig config;
    config.role = options.role;
    config.backend = &backend;
    config.scope = *options.scope;
    config.ipcEnabled = true;
    config.version = options.version;
    config.format = kFormatVst3;
    config.hostPpid = parentProcessId();
    // 同一个进程只有一个 peer，pid 已经足够区分；再混进后端地址，写法与插件里保持一致
    config.seed = static_cast<u64>(currentProcessId()) ^ reinterpret_cast<std::uintptr_t>(&backend);

    Counters counters;
    RendezvousState finalState = RendezvousState::kUnassigned;
    u32 abiVersion = 0;
    {
        Rendezvous rendezvous(config);
        const u64 now = rinne::util::monoNowMs();
        rendezvous.setGroup(options.group, options.intent, now);
        rendezvous.prepare({48000, kFramesPerBlock, 2}, now);

        std::atomic<bool> stop{false};
        RendezvousState lastState = rendezvous.state();

        std::thread message([&] {
            for (;;)
            {
                rendezvous.tick(rinne::util::monoNowMs());
                const RendezvousState state = rendezvous.state();
                if (state != lastState)
                {
                    lastState = state;
                    printState(stateName(state), elapsedMs(wallStart));
                }
                if (stop.load(std::memory_order_acquire))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        });

        std::vector<float> left(kFramesPerBlock);
        std::vector<float> right(kFramesPerBlock);
        std::thread audio([&] {
            RingWriter writer;
            writer.reset(48000);
            i64 readPos = -1;
            u64 boundSeq = 0;
            for (;;)
            {
                const LoopBinding* binding = rendezvous.bindings().current();
                if (binding != nullptr)
                {
                    if (binding->seq != boundSeq)
                    {
                        // 换了绑定（换写方或换段）：写方的回收计数从头算
                        boundSeq = binding->seq;
                        writer.reset(48000);
                    }
                    BoundBlock block(binding);
                    if (block.ok() && block.stillMine())
                    {
                        if (options.role == Role::kOutput)
                        {
                            writeBlock(block.ring(), block.binding().writerToken, writer, counters);
                        }
                        else
                        {
                            readBlock(block.ring(), readPos, left.data(), right.data(), counters);
                        }
                        block.mine().block_seq.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (stop.load(std::memory_order_acquire))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        const auto deadline = wallStart + std::chrono::seconds(options.seconds);
        while (Clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop.store(true, std::memory_order_release);
        message.join();
        audio.join();
        finalState = rendezvous.state();
        abiVersion = rendezvous.abiMismatchVersion();
    }

    printResult(stateName(finalState), counters.blocks, counters.frames, counters.valid, counters.bad, counters.resyncs,
                abiVersion);
    return 0;
}

// 另一个 abi 的构建留下的现场：不用 Rendezvous，自己建/附着会合段并按那个 abi 初始化，再把版本写进端点槽
[[maybe_unused]] int runForeign(const Options& options, ISegmentBackend& backend)
{
    const auto wallStart = Clock::now();
    const Naming naming(*options.scope);
    std::string rvName;
    if (naming.name(SegmentId::rendezvous(), rvName) != NameStatus::kOk)
    {
        std::fprintf(stderr, "peer error: cannot name the rendezvous segment\n");
        return 2;
    }

    std::optional<SegmentView> held;
    const char* last = "unassigned";
    const char* printed = "";
    const auto deadline = wallStart + std::chrono::seconds(options.seconds);

    for (;;)
    {
        if (!held.has_value())
        {
            SegmentView view;
            if (backend.createOrOpen(rvName, kRvBytes, view) == InitResult::kOk)
            {
                auto* rv = static_cast<RvSegment*>(view.base);
                const InitResult init = backend.initHeader(
                    view, headerFields(rv->header), {kRvMagic, options.abi, kRvBytes, kRvHeaderBytes}, [rv] {
                        rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
                        rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
                        rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
                    });
                if (init == InitResult::kOk)
                {
                    const std::size_t at = slotVersionOffset(options.group, options.role == Role::kOutput);
                    reinterpret_cast<std::atomic<u32>*>(static_cast<unsigned char*>(view.base) + at)
                        ->store(options.version, std::memory_order_relaxed);
                    held.emplace(std::move(view));
                    last = "foreignPublished";
                    printState(last, elapsedMs(wallStart));
                    printed = last;
                }
                else
                {
                    backend.unmap(view);
                    if (init == InitResult::kAbiMismatch)
                    {
                        last = "abiMismatch";
                        if (std::strcmp(printed, "abiMismatch") != 0)
                        {
                            printState(last, elapsedMs(wallStart));
                            printed = "abiMismatch";
                        }
                    }
                }
            }
        }

        if (held.has_value() || Clock::now() >= deadline)
        {
            break;
        }
        const auto nextAttempt = std::min(deadline, Clock::now() + std::chrono::seconds(1));
        while (Clock::now() < nextAttempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    if (held.has_value())
    {
        // 一直留到最后：对端在我们退出之前都能看见这个别的 abi 的会合段
        while (Clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        backend.unmap(*held);
    }

    printResult(last, 0, 0, 0, 0, 0, 0);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseOptions(argc, argv, options))
    {
        printUsage();
        return 2;
    }

#if defined(_WIN32) || defined(__APPLE__)
#if defined(__APPLE__)
    std::unique_ptr<PlatformSegmentBackend> backend;
    if (!options.lockDir.empty())
    {
        backend = std::make_unique<PlatformSegmentBackend>(SegmentBackendPosix::LockDirForTesting{options.lockDir});
    }
    else
    {
        backend = std::make_unique<PlatformSegmentBackend>();
    }
#else
    auto backend = std::make_unique<PlatformSegmentBackend>(); // --lock-dir 在 Windows 上接受并忽略
#endif
    printStart(options);
    return options.abi == kIpcAbi ? runNormal(options, *backend) : runForeign(options, *backend);
#else
    std::fprintf(stderr, "peer unsupported backend=inprocess\n");
    return 3;
#endif
}
