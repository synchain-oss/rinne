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
// 等锁有上限：别的进程卡死时消息线程不能跟着卡死，等不到就不带锁照常做，代价是这一次可能丢更新。
// 上限分两档，且超时一次之后几秒内只等很短：状态快照每秒至少一帧、每帧都读这个文件，
// 若每次都等满，别的进程卡住锁时每个打开的编辑器每帧都会顿一次。
//   - 读比写等得短（kProcessLockReadWaitMs < kProcessLockWriteWaitMs）：JUCE 写盘是先写临时文件再整体替换，
//     不带锁读到的要么是旧的整份、要么是新的整份，最坏读到默认值；写不带锁才会丢更新，值得多等；
//   - 任何一次拿锁超时，就在 kProcessLockBackoffMs（单调时钟）里把等待降到 kProcessLockBackoff* 那两个很短的值，
//     期间拿到锁就立刻恢复；窗口结束后下一次再等满一回，仍超时就再进入下一个窗口。窗口里再超时不顺延窗口，
//     所以卡住的锁最多每个窗口让消息线程多等一次满时长，而不是每帧一次。
// 一次状态快照要的标志用 readUiFlags 一次读回，只开一次文件、拿一次锁。

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>

#include "BridgeBase.h"

namespace rinne::uidefaults
{

// 进程间锁的名字（Rinne 前缀，与别的软件的锁不撞）和各档最长等待毫秒数
inline constexpr const char* kProcessLockName = "RinneUiDefaults";
inline constexpr int kProcessLockWriteWaitMs = 500;
inline constexpr int kProcessLockReadWaitMs = 100;
// 超时之后的避让窗口（毫秒，单调时钟）和窗口里的等待：读不等，写只等一小会儿
inline constexpr int kProcessLockBackoffMs = 5000;
inline constexpr int kProcessLockBackoffWriteWaitMs = 50;
inline constexpr int kProcessLockBackoffReadWaitMs = 0;

bool langChosenGlobal();
void setLangChosenGlobal(bool chosen);

// 选中的语言值本身。空串表示没存过；磁盘上不在白名单（zh、en、fr）里的值也按没存过处理
juce::String langGlobal();
// 不在白名单里的语言码直接忽略，不写盘
void setLangGlobal(const juce::String& lang);
// 用户显式选了一种语言：语言值和「选过」位在同一次拿锁、同一次开文件里写完（别的进程拿锁时看不到只写了一半的状态）。
// 不在白名单里的语言码两样都不写（只写「选过」不写语言，重载后就是「不再问、语言却回到默认」）
void chooseLangGlobal(const juce::String& lang);

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
// nowMs 顶替避让窗口用的单调毫秒时钟，空表示用真的单调时钟。
// 每次调用都把避让状态清零。两个锁参数都传空函数恢复真锁。生产代码不得调用（同上，有用例扫描 src/）
void setLockForTests(std::function<bool(int timeoutMs)> enter, std::function<void()> exit,
                     std::function<std::int64_t()> nowMs = {});

} // namespace rinne::uidefaults
