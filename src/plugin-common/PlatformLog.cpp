// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "PlatformLog.h"

#include <juce_events/juce_events.h> // MessageManager：write 的跨线程投递

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "ipc/IpcDiag.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/ProcessInfo.h"
#include "util/RinneVersion.h"

#if JUCE_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rinne::platformlog
{

namespace
{

// 全局（每个插件二进制一份）状态，有意不析构：宿主退出时的析构顺序不由我们定，段后端在处理器析构里还可能经
// IpcDiag 报一条，那时这把锁若已随静态对象析构，就是析构后使用。泄漏的只是一个文件名和一张小表。
struct State
{
    std::mutex mutex;
    std::unique_ptr<RotatingLogFile> log; // 非空 = 已装
    ipc::IpcDiagLimiter limiter;
};

State& state()
{
    static State* s = new State();
    return *s;
}

// write() 的快路径：没装时连锁都不取。真值只由 installAt / uninstall 在持锁时改
std::atomic<bool>& installedFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}

void writeOnMessageThread(const juce::String& line)
{
    juce::String text = line;
    if (text.length() > kMaxLineChars)
        text = text.substring(0, kMaxLineChars) + " ...(truncated)";
    const juce::String stamped = juce::Time::getCurrentTime().toISO8601(true) + " " + text;

    const std::lock_guard<std::mutex> lock(state().mutex);
    if (state().log != nullptr)
        state().log->append(stamped);
}

// IpcDiag 的 sink：段后端在失败点同步调用，调用方线程是段后端的非实时线程，不一定是消息线程。
// 先去重限流，再交给 write()。noexcept：吞掉一切异常，报告点在后端的失败清理路径里，不允许异常穿出
void ipcDiagSink(const ipc::IpcDiagEvent& event) noexcept
{
    try
    {
        ipc::IpcDiagLimiter::Verdict verdict;
        {
            const std::lock_guard<std::mutex> lock(state().mutex);
            if (state().log == nullptr)
                return;
            verdict = state().limiter.onEvent(event, juce::Time::getMillisecondCounter());
        }
        if (!verdict.emit)
            return;
        const std::string line = ipc::formatIpcDiagLine(event, verdict);
        write(juce::String::fromUTF8(line.data(), static_cast<int>(line.size())));
    }
    catch (...)
    {
    }
}

bool isKnownRole(const juce::String& role)
{
    return role == "input" || role == "output";
}

} // namespace

juce::File defaultLogFile(const juce::String& role)
{
    if (!isKnownRole(role))
        return {};
    const juce::String fileName = (build::kProbeBuild ? "probe-" : "") + role + ".log";
#if JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation(juce::File::windowsLocalAppData);
    if (base == juce::File())
        return {};
    return base.getChildFile("Synchain").getChildFile("Rinne").getChildFile("logs").getChildFile(fileName);
#elif JUCE_MAC
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    if (home == juce::File()) // 拿不到 home 时不往下拼：空 File 拼出来会落在文件系统根下
        return {};
    return home.getChildFile("Library")
        .getChildFile("Logs")
        .getChildFile("Synchain")
        .getChildFile("Rinne")
        .getChildFile(fileName);
#else
    return {};
#endif
}

RotatingLogFile::RotatingLogFile(juce::File file, juce::int64 maxBytes) : file_(std::move(file)), maxBytes_(maxBytes) {}

juce::File RotatingLogFile::rotatedFile() const
{
    return file_.getSiblingFile(file_.getFileNameWithoutExtension() + ".1" + file_.getFileExtension());
}

bool RotatingLogFile::append(const juce::String& line)
{
    if (file_ == juce::File())
        return false;

    const juce::String text = line + "\n";
    const auto bytes = text.getNumBytesAsUTF8();
    if (file_.existsAsFile())
    {
        const auto size = file_.getSize();
        // 当前文件非空且这一行写进去会超阈值 ⇒ 先轮转。改名失败（比如被别的进程占着）就接着往当前文件写：
        // 宁可超一点，也不丢这一行。moveFileTo 会先删掉已存在的目标，所以只留一份旧的
        if (size > 0 && size + static_cast<juce::int64>(bytes) > maxBytes_)
            file_.moveFileTo(rotatedFile());
    }

    const auto dir = file_.getParentDirectory();
    if (!dir.isDirectory() && dir.createDirectory().failed())
        return false;

    const char* data = text.toRawUTF8();
#if JUCE_WINDOWS
    // FILE_APPEND_DATA：每次 WriteFile 都由内核原子地落在文件尾，几个进程同时追加也不互相覆盖。
    // 共享删除权限让别的写者在本进程开着文件时也能轮转改名
    const HANDLE h = ::CreateFileW(file_.getFullPathName().toWideCharPointer(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(h, data, static_cast<DWORD>(bytes), &written, nullptr) != FALSE &&
                    static_cast<std::size_t>(written) == bytes;
    ::CloseHandle(h);
    return ok;
#else
    // O_APPEND + 一行一次 write：「移到文件尾 + 写」由内核原子完成。juce::FileOutputStream 是 open + lseek + 缓冲写，
    // 两个写者拿到同一个尾偏移就会互相覆盖整行
    auto remaining = bytes;
    const int fd = ::open(file_.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool ok = true;
    while (remaining > 0)
    {
        const auto n = ::write(fd, data, remaining);
        if (n < 0 && errno == EINTR)
            continue;
        // n == 0 也收手：普通文件上本不该出现，真出现时原地重试就是在消息线程上死循环
        if (n <= 0)
        {
            ok = false;
            break;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    return ok;
#endif
}

bool installAt(const juce::File& file)
{
    if (file == juce::File())
        return false;
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        if (state().log != nullptr)
            return false; // 先装者为准：同一个二进制里的第二个实例走到这里是正常的
        state().log = std::make_unique<RotatingLogFile>(file);
        state().limiter = ipc::IpcDiagLimiter{};
        installedFlag().store(true, std::memory_order_release);
    }
    // 锁外设 sink：sink 自己会取同一把锁。IpcDiag 只有一个槽，装上就是本日志
    ipc::setIpcDiagSink(&ipcDiagSink);
    return true;
}

bool install(const juce::String& role)
{
    if (!installAt(defaultLogFile(role)))
        return false;
    juce::String opened;
    opened << "Rinne " << role << " " << build::kVersion;
    if (const juce::String label = build::buildLabel(); label.isNotEmpty())
        opened << " (" << label << ")";
    opened << " log opened (pid " << static_cast<juce::int64>(ipc::currentProcessId()) << ", host "
           << juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName() << ", ipc backend "
           << ipc::platformSegmentBackendName() << ")";
    write(opened);
    return true;
}

void uninstall()
{
    if (ipc::ipcDiagSink() == &ipcDiagSink)
        ipc::setIpcDiagSink(nullptr);
    const std::lock_guard<std::mutex> lock(state().mutex);
    installedFlag().store(false, std::memory_order_release);
    state().log.reset();
    state().limiter = ipc::IpcDiagLimiter{};
}

bool isInstalled() noexcept
{
    return installedFlag().load(std::memory_order_acquire);
}

juce::File installedFile()
{
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().log != nullptr ? state().log->file() : juce::File();
}

void write(const juce::String& line)
{
    if (!isInstalled())
        return;
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        writeOnMessageThread(line);
        return;
    }
    // 不在消息线程：按值拷贝投递；没有 MessageManager 时 callAsync 返回 false，这一行丢弃。
    // 这一支不是「契约被违反时的兜底」：段后端会在 prepare / release 回调里被调到，那两个回调跑在哪个线程
    // 由宿主定，JUCE 不保证是消息线程，所以这里不加断言
    juce::MessageManager::callAsync([line] { writeOnMessageThread(line); });
}

} // namespace rinne::platformlog
