// SPDX-License-Identifier: GPL-3.0-or-later
//
// 诊断日志文件（PlatformLog）：按大小轮转的追加式文件，以及装上之后的写行、截断与 IPC 诊断落行。全部落在本用例独占
// 的临时目录里，不碰默认位置（install(role) 的开场行要写真实的用户目录，这里只测它的下半段 installAt）。
// 与执行顺序无关：开始时先卸下（万一别的用例装过），结束时卸下、把 IpcDiag 的槽还给处理器的转发出口，逐个删文件。
#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include "InstanceHost.h"
#include "PlatformLog.h"
#include "ipc/IpcDiag.h"
#include "util/RinneVersion.h"

namespace
{

namespace ipc = rinne::ipc;
namespace platformlog = rinne::platformlog;
using rinne::InstanceHost;

// 本用例独占的临时目录；析构时只删自己建的文件和目录，不递归
class TempDir
{
public:
    TempDir()
        : dir_(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("rinne-host-tests-log-" + juce::String::toHexString(juce::Random().nextInt64())))
    {
        REQUIRE(dir_.createDirectory().wasOk());
    }
    ~TempDir()
    {
        for (const auto& sub : dir_.findChildFiles(juce::File::findDirectories, false))
        {
            for (const auto& f : sub.findChildFiles(juce::File::findFiles, false))
                f.deleteFile();
            for (const auto& nested : sub.findChildFiles(juce::File::findDirectories, false))
            {
                for (const auto& f : nested.findChildFiles(juce::File::findFiles, false))
                    f.deleteFile();
                nested.deleteFile();
            }
            sub.deleteFile();
        }
        for (const auto& f : dir_.findChildFiles(juce::File::findFiles, false))
            f.deleteFile();
        dir_.deleteFile();
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    juce::File file(const char* name) const { return dir_.getChildFile(name); }
    const juce::File& dir() const noexcept { return dir_; }

private:
    juce::File dir_;
};

// 装日志的用例：先卸下，结束时卸下并把 IpcDiag 的槽还给转发出口（转发目标还是用例开始时的那个）
class LogInstall
{
public:
    LogInstall() : forward_(InstanceHost::ipcDiagForwardTarget()) { platformlog::uninstall(); }
    ~LogInstall()
    {
        platformlog::uninstall();
        ipc::setIpcDiagSink(forward_);
        InstanceHost::installIpcDiagSink();
    }
    LogInstall(const LogInstall&) = delete;
    LogInstall& operator=(const LogInstall&) = delete;

private:
    ipc::IpcDiagSink forward_;
};

juce::StringArray linesOf(const juce::File& file)
{
    juce::StringArray lines;
    lines.addLines(file.loadFileAsString());
    lines.removeEmptyStrings(false);
    return lines;
}

} // namespace

TEST_CASE("platform log: each append is one UTF-8 line with a trailing newline", "[host][platform_log]")
{
    const TempDir temp;
    platformlog::RotatingLogFile log(temp.file("input.log"));
    REQUIRE(log.append("first"));
    REQUIRE(log.append(juce::String::fromUTF8("\xe7\xac\xac\xe4\xba\x8c line")));
    juce::MemoryBlock bytes;
    REQUIRE(temp.file("input.log").loadFileAsData(bytes));
    CHECK(bytes.toString() == juce::String::fromUTF8("first\n\xe7\xac\xac\xe4\xba\x8c line\n"));
    CHECK(log.rotatedFile() == temp.file("input.1.log"));
}

TEST_CASE("platform log: a missing parent directory is created on the first append", "[host][platform_log]")
{
    const TempDir temp;
    const juce::File file = temp.dir().getChildFile("logs").getChildFile("nested").getChildFile("output.log");
    platformlog::RotatingLogFile log(file);
    REQUIRE(log.append("hello"));
    CHECK(file.existsAsFile());
    CHECK(linesOf(file) == juce::StringArray{"hello"});
}

TEST_CASE("platform log: the file rotates before it would pass the size limit and keeps at most two",
          "[host][platform_log]")
{
    const TempDir temp;
    const juce::File file = temp.file("output.log");
    platformlog::RotatingLogFile log(file, 16);
    const juce::File rotated = log.rotatedFile();

    REQUIRE(log.append("0123456789")); // 11 字节
    CHECK_FALSE(rotated.exists());
    REQUIRE(log.append("abcdefghij")); // 22 > 16：先轮转
    CHECK(linesOf(rotated) == juce::StringArray{"0123456789"});
    CHECK(linesOf(file) == juce::StringArray{"abcdefghij"});
    REQUIRE(log.append("ABCDEFGHIJ")); // 再轮转：更旧的那一份被覆盖
    CHECK(linesOf(rotated) == juce::StringArray{"abcdefghij"});
    CHECK(linesOf(file) == juce::StringArray{"ABCDEFGHIJ"});
    CHECK(temp.dir().findChildFiles(juce::File::findFiles, false).size() == 2);

    SECTION("a single line longer than the limit still goes into an empty file")
    {
        const juce::File big = temp.file("input.log");
        platformlog::RotatingLogFile wide(big, 16);
        REQUIRE(wide.append("a line that is longer than sixteen bytes"));
        CHECK_FALSE(wide.rotatedFile().exists());
        CHECK(big.getSize() > 16);
    }
}

TEST_CASE("platform log: a line that cannot be written reports false", "[host][platform_log]")
{
    const TempDir temp;
    platformlog::RotatingLogFile none{juce::File()};
    CHECK_FALSE(none.append("dropped"));

    // 父目录的位置上是个文件：目录建不出来
    const juce::File blocker = temp.file("blocker");
    REQUIRE(blocker.replaceWithText("not a directory"));
    platformlog::RotatingLogFile blocked(blocker.getChildFile("input.log"));
    CHECK_FALSE(blocked.append("dropped"));
}

TEST_CASE("platform log: only the two roles map to a default log file", "[host][platform_log]")
{
    for (const char* role : {"input", "output"})
    {
        const juce::File file = platformlog::defaultLogFile(role);
#if JUCE_WINDOWS || JUCE_MAC
        REQUIRE(file != juce::File());
        CHECK(file.getFileName() == (rinne::build::kProbeBuild ? "probe-" : "") + juce::String(role) + ".log");
        CHECK(file.getParentDirectory().getFullPathName().contains("Rinne"));
#else
        CHECK(file == juce::File());
#endif
    }
    for (const char* role : {"", "probe", "Input", "../input", "input/../../x"})
        CHECK(platformlog::defaultLogFile(role) == juce::File());
}

TEST_CASE("platform log: once installed, lines and IPC diagnostics land in the file", "[host][platform_log]")
{
    const TempDir temp;
    const LogInstall guard;
    const juce::File file = temp.file("input.log");

    platformlog::write("before install"); // 没装：什么都不做
    REQUIRE(platformlog::installAt(file));
    CHECK(platformlog::isInstalled());
    CHECK(platformlog::installedFile() == file);
    CHECK_FALSE(platformlog::installAt(temp.file("output.log"))); // 先装者为准
    CHECK_FALSE(platformlog::installAt(juce::File()));

    platformlog::write("pairing: smart default chose group A (empty group)");
    ipc::IpcDiagEvent event;
    event.op = ipc::IpcDiagOp::kOpen;
    event.code = ipc::IpcDiagCode::kBusy;
    event.error = 33;
    event.detail = "lock wait timed out";
    ipc::reportIpcDiag(event);
    ipc::reportIpcDiag(event); // 同一键在窗口里只落一行

    const juce::StringArray lines = linesOf(file);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].endsWith(" pairing: smart default chose group A (empty group)"));
    CHECK(lines[1].contains(" ipc "));
    CHECK(lines[1].contains(juce::String(ipc::ipcDiagCodeName(ipc::IpcDiagCode::kBusy)) + " errno 33"));
    CHECK(lines[1].endsWith("-- lock wait timed out"));
    CHECK_FALSE(file.loadFileAsString().contains("before install"));
    CHECK_FALSE(temp.file("output.log").exists());

    platformlog::uninstall();
    CHECK_FALSE(platformlog::isInstalled());
    platformlog::write("after uninstall");
    CHECK(linesOf(file).size() == 2);
}

TEST_CASE("platform log: an overlong line is cut and marked", "[host][platform_log]")
{
    const TempDir temp;
    const LogInstall guard;
    const juce::File file = temp.file("output.log");
    REQUIRE(platformlog::installAt(file));

    platformlog::write(juce::String::repeatedString("x", platformlog::kMaxLineChars + 500));
    platformlog::write(juce::String::repeatedString("y", platformlog::kMaxLineChars));
    const juce::StringArray lines = linesOf(file);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].endsWith(" " + juce::String::repeatedString("x", platformlog::kMaxLineChars) + " ...(truncated)"));
    CHECK_FALSE(lines[0].contains(juce::String::repeatedString("x", platformlog::kMaxLineChars + 1)));
    CHECK(lines[1].endsWith(" " + juce::String::repeatedString("y", platformlog::kMaxLineChars)));
    CHECK_FALSE(lines[1].contains("(truncated)"));
}
