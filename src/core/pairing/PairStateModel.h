// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对状态模型：把「会合状态、对端快照、宿主事实」折成界面要显示的一份只读模型。纯函数，不读段、不读时钟。
// 15 个状态按颜色分组：红 5、琥珀 3、灰 4、绿 3（connectedStill 与 connected 同色同文案，只是绿点不脉冲）。
// 判定顺序固定（先到先得），与设计稿的优先级表一致：
//   validator, ipcUnavailable, abiMismatch, srMismatch, conflict, unassigned, stale, bypassed,
//   crossProcess, crossTrack, waitingAudio, searching, connectedUnverified, connected / connectedStill。
// 其中 stale 与 bypassed 之后的各项都以「有对端」为前提（searching 之前），bypassed 看的是本端（Input 或
// Output）被宿主旁路。
#pragma once

#include <cstdint>
#include <string>

#include "ipc/LoopBinding.h"
#include "ipc/PairingPolicy.h"
#include "ipc/SegmentLayout.h"

namespace rinne::pairing
{

using rinne::ipc::Role;
using rinne::ipc::u32;

enum class PairState : std::uint8_t
{
    kValidator,
    kIpcUnavailable,
    kAbiMismatch,
    kSrMismatch,
    kConflict,
    kUnassigned,
    kStale,
    kBypassed,
    kCrossProcess,
    kCrossTrack,
    kWaitingAudio,
    kSearching,
    kConnectedUnverified,
    kConnected,
    kConnectedStill,
};
inline constexpr int kPairStateCount = 15;

enum class PairColour : std::uint8_t
{
    kRed,
    kAmber,
    kGrey,
    kGreen,
};

PairColour colourOf(PairState s) noexcept;
// 稳定的机读标识（桥与诊断共用），例如 "connectedStill"
const char* idOf(PairState s) noexcept;
// 只有绿色两态（connectedUnverified / connected）在音频流动时脉冲；connectedStill 不脉冲
bool pulses(PairState s) noexcept;

// 名字核验：两端哈希都已知且相等 = same；都已知但不等 = different；有一端未知 = unknown
enum class NameCheck : std::uint8_t
{
    kUnknown,
    kSame,
    kDifferent,
};
NameCheck checkNames(u32 selfHash, u32 peerHash) noexcept;

// 对端的快照（同组里与本角色相反的那一端）
struct PeerFacts
{
    bool present = false; // 槽被占着（active；休眠不算「有对端」）
    bool lost = false; // 观察者判定的对端丢失
    u32 pid = 0;
    u32 version = 0; // packVersion
    u32 sampleRate = 0;
    u32 channels = 0;
    u32 nameHash = 0;
    std::string name; // 显示前缀，已经过 UTF-8 清洗；可空
    u32 colourArgb = 0; // 0 = 未知
};

// 所有输入。布尔量都是调用方已经算好的事实
struct PairInputs
{
    Role role = Role::kInput;
    u32 selfPid = 0;
    u32 selfVersion = 0;
    u32 selfSampleRate = 0;
    u32 selfChannels = 0;
    u32 selfNameHash = 0;
    u32 group = 0; // 0 = 没有组
    bool validator = false; // 验证工具中（IPC 关闭）
    bool ipcUnavailable = false;
    bool abiMismatch = false;
    bool conflict = false; // 槽被别人占着，或者我们的被接管了
    bool allGroupsFull = false; // 智能默认找不到组
    bool realtimeAudioSeen = false; // 已经收到过实时块
    bool selfBypassed = false; // 宿主旁路了本端
    bool loopSilent = false; // 回送静音：切断、锁定、环路关、任一端旁路、宿主暂停、断档
    PeerFacts peer;
    std::string occupantName; // 冲突时占着本角色槽的实例的轨道名；可空
};

struct PairModel
{
    PairState state = PairState::kSearching;
    PairColour colour = PairColour::kGrey;
    u32 group = 0;
    u32 occupiedMask = 0; // bit0 = 组 A：本角色一端已被他人占着（active 或 dormant）
    u32 peerMask = 0; // bit0 = 组 A：同进程的对端活着，而本角色一端还空着（有未配对的对端在等）
    NameCheck nameCheck = NameCheck::kUnknown;
    std::string peerName;
    u32 peerColourArgb = 0;
    std::string occupantName; // 只在 conflict 时有值
    bool channelMismatch = false; // 两端声道数都已知且不同（只提示，不改状态）
    std::string selfVersion; // "主.次.修"
    std::string peerVersion; // 对端不在时为空
};

std::string formatVersion(u32 packed);

PairModel buildPairModel(const PairInputs& in, const rinne::ipc::GroupViews& groups);

} // namespace rinne::pairing
