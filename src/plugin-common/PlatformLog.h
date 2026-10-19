// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 插件侧的诊断日志文件。只在用户打开「记录诊断日志」或探针构建时由调用方装上；没装时 write() 什么都不做。
//
// 落点：Windows %LOCALAPPDATA%\Synchain\Rinne\logs\<role>.log，mac ~/Library/Logs/Synchain/Rinne/<role>.log；
// role 只认 input、output（探针构建写 probe-<role>.log），别的值不拼进路径。两个插件是两个二进制，各写各的文件。
// 单文件 1 MiB 轮转：写之前若会超过 kMaxLogBytes，先把当前文件改名为 <名>.1.log（覆盖更旧的那一份）再新开，
// 每个角色最多留两份。写不进去就静默跳过。
//
// 多个写者：同一份 <role>.log 可能同时有几个写者（同一宿主里同角色的 AU 与 VST3，同时开着的几个宿主进程）。
// 每行一次追加写：POSIX 用 O_APPEND，Windows 用 FILE_APPEND_DATA，「移到文件尾再写」由内核原子完成，
// 行与行不交错、不互相覆盖（POSIX 上的短写重试除外：剩下的半行另起一次追加，理论上可能夹进别的写者的一行）。
// 轮转不跨写者协调：两个写者同时越过阈值时可能连着轮转两次，较旧的那份备份被覆盖。对诊断日志可以接受。
//
// 线程：文件只在消息线程写。write() 可在任何非实时线程调用：就在消息线程上时就地写，否则经 MessageManager::callAsync
// 投递（行内容按值拷贝），没有 MessageManager 时丢弃。音频线程一律不许调（文件 I/O、锁、堆分配）。
//
// IPC 诊断：装上时把 rinne_core 的 IpcDiag sink 接到本日志，事件先过 IpcDiagLimiter 去重限流再落行。
// 运行期的文案一律 ASCII。
#pragma once

#include <juce_core/juce_core.h>

namespace rinne::platformlog
{

inline constexpr juce::int64 kMaxLogBytes = 1024 * 1024;

// 单行上限（字符）：诊断行本来都很短，这条只防一条异常长的行把整份日志撑过阈值；超出部分截掉并标 ...(truncated)
inline constexpr int kMaxLineChars = 2048;

// 角色的默认日志文件；role 不是 input / output、或平台没有日志目录时返回空 File
juce::File defaultLogFile(const juce::String& role);

// 按大小轮转的追加式文本文件。不加锁、不管线程：调用方保证只在一个线程上用它（全局实例只在消息线程上写）。
class RotatingLogFile
{
public:
    explicit RotatingLogFile(juce::File file, juce::int64 maxBytes = kMaxLogBytes);

    // 追加一行（自动补 '\n'，UTF-8，不做行尾转换）。父目录不存在时先建。返回 false = 这一行没写进去；不抛异常。
    bool append(const juce::String& line);

    const juce::File& file() const noexcept { return file_; }
    // <名>.1<扩展名>，例如 input.log → input.1.log（保留扩展名，系统自带的日志查看器照样能打开）
    juce::File rotatedFile() const;

private:
    juce::File file_;
    juce::int64 maxBytes_;
};

// 生产入口：本角色的默认日志文件 + IPC 诊断 sink，再写一行开场（角色、版本、构建标签、pid、宿主可执行名）。
// 每个插件二进制只装一次，之后的调用是空操作。返回 true = 这次真的装上了。线程：任意非实时线程。
bool install(const juce::String& role);

// 指定文件装上（install 的下半段，单测把日志放进临时目录）。已装时不替换、返回 false。
bool installAt(const juce::File& file);

// 卸下：恢复 IpcDiag 的空操作 sink，丢掉文件。生产代码从不调用，日志与插件二进制同寿。
void uninstall();

bool isInstalled() noexcept;
juce::File installedFile();

// 写一行（自动加本地时间戳前缀）。没装时什么都不做。
void write(const juce::String& line);

} // namespace rinne::platformlog
