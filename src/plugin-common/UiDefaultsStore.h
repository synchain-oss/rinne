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

#include <juce_core/juce_core.h>

namespace rinne::uidefaults
{

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

// 按角色取值的便捷入口，role 取 "input"；其余值一律当 Output
bool guideSeenGlobal(const juce::String& role);
void setGuideSeenGlobal(const juce::String& role, bool seen);
int uiScalePercent(const juce::String& role);
void setUiScalePercent(const juce::String& role, int percent);

// 仅供测试：把落盘目录改到临时目录，避免单测写真实用户设置。传空 File
// 恢复默认位置。生产代码不得调用（宿主测试有一条用例扫描 src/ 守这条）
void setStorageDirForTests(const juce::File& dir);

} // namespace rinne::uidefaults
