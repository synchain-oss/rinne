// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#pragma once

// 系统级界面全局默认：跨工程，不随工程状态走。承载：
//   - 语言：用户是否显式选过、选的是哪一种。两样都要记：只记「选过」不记「选的是哪个」，
//     重装或移除插件再加载会得到「不再问、语言却回到默认」，比不记更糟；
//   - 规则确认：用户看过并确认的规则页版本号，0 表示没确认过；
//   - 首启引导「不再显示」：Input 与 Output 各存一份，两端讲的是两个界面，共用一个位会让只装过一端的用户
//     永远看不到另一端的引导；
//   - 缩放默认：缩放确认「保持」后落的百分比，0 表示没存过；按角色分键，两端的档位表不同，共用一个键会互相污染；
//   - 诊断日志开关。
//
// 落盘位置：用户应用数据目录下的 Synchain/Rinne/ui-defaults.settings（macOS 在 Application Support 下）。
//
// 实现纪律：不驻留任何进程内状态，每次读写现开一份 juce::PropertiesFile，读完写完即析构。
// 同一宿主里的两个实例因此永远看到磁盘上的同一份真值，不需要跨实例广播；调用点稀疏
// （开窗一次，用户点「不再显示」或「保持」各一次），开销可忽略。
// 文件缺失或不可写一律降级为全默认：丢了只会让引导多弹一次，绝不能让开窗失败。
// 只在消息线程调用；不碰音频线程。
//
// 并发：Input、Output 与进程外的宿主进程（进程外的 AU 之类）共用同一份文件，而写是「读全文件、改一个键、整体重写」，
// 两个进程各改不同的键会丢更新。所以每一次读或写都在一把有名的进程间锁里整段完成（打开、读写、落盘、关闭），
// 不是只在 JUCE 读盘和写盘的那两小步里各拿一次：那样中间的空隙仍在。
// 等锁有上限（kProcessLockWaitMs）：别的进程卡死时消息线程不能跟着卡死，等不到就不带锁照常做，代价是这一次可能丢更新。
// 一次状态快照要的标志用 readUiFlags 一次读回，只开一次文件、拿一次锁。

#include <juce_core/juce_core.h>

#include <functional>

#include "BridgeBase.h"

namespace rinne::uidefaults
{

// 进程间锁的名字（Rinne 前缀，与别的软件的锁不撞）和最长等待毫秒数
inline constexpr const char* kProcessLockName = "RinneUiDefaults";
inline constexpr int kProcessLockWaitMs = 500;

bool langChosenGlobal();
void setLangChosenGlobal(bool chosen);

// 选中的语言值本身。空串表示没存过；磁盘上不在白名单（zh、en、fr）里的值也按没存过处理
juce::String langGlobal();
// 不在白名单里的语言码直接忽略，不写盘
void setLangGlobal(const juce::String& lang);

// 用户已确认的规则页版本号；0 表示没确认过，负值与读不出来的值都按 0
int rulesAckVersion();
// 负值忽略，不写盘
void setRulesAckVersion(int version);

bool guideSeenGlobalInput();
void setGuideSeenGlobalInput(bool seen);
bool guideSeenGlobalOutput();
void setGuideSeenGlobalOutput(bool seen);

// 缩放默认的百分比；0 表示没存过，范围外的值（磁盘上可被改动）也按没存过。写入范围外的值直接忽略
int uiScalePercentInput();
void setUiScalePercentInput(int percent);
int uiScalePercentOutput();
void setUiScalePercentOutput(int percent);

bool diagLogEnabled();
void setDiagLogEnabled(bool enabled);

// 状态快照要的几个标志（语言是否选过、规则确认版本、本角色引导是否已看、诊断日志开关）一次读回：
// 只开一次文件、拿一次锁。每个字段的校验与上面逐个读的函数完全相同。role 取 "input"；其余值一律当 Output
bridge::UiFlags readUiFlags(const juce::String& role);

// 按角色取值的便捷入口，role 取 "input"；其余值一律当 Output
bool guideSeenGlobal(const juce::String& role);
void setGuideSeenGlobal(const juce::String& role, bool seen);
int uiScalePercent(const juce::String& role);
void setUiScalePercent(const juce::String& role, int percent);

// 仅供测试：把落盘目录改到临时目录，避免单测写真实用户设置。传空 File
// 恢复默认位置。生产代码不得调用（宿主测试有一条用例扫描 src/ 守这条）
void setStorageDirForTests(const juce::File& dir);

// 仅供测试：用一对替身顶替真正的进程间锁，观察每次读写是不是整段都在锁里。
// enter 收到最长等待毫秒数，返回是否拿到；exit 只在拿到之后调用，且每次 enter 返回真对应一次 exit。
// 两个参数都传空函数恢复真锁。生产代码不得调用（同上，有用例扫描 src/）
void setLockForTests(std::function<bool(int timeoutMs)> enter, std::function<void()> exit);

} // namespace rinne::uidefaults
