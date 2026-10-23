// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对状态模型：把调用方已经算好的事实折成界面要显示的一份只读模型。纯函数，不读段、不读时钟。
// 判定顺序固定、先到先得：越靠前的状态越需要用户动作，不能被后面更「温和」的状态盖掉；
// 例如失联的对端仍要先报 stale，而不是因为名字对得上就显示已连接。
#include "pairing/PairStateModel.h"

#include <cstddef>
#include <string>

namespace rinne::pairing
{

PairColour colourOf(PairState s) noexcept
{
    switch (s)
    {
    case PairState::kIpcUnavailable:
    case PairState::kAbiMismatch:
    case PairState::kSrMismatch:
    case PairState::kConflict:
    case PairState::kUnassigned:
        return PairColour::kRed;
    case PairState::kStale:
    case PairState::kCrossProcess:
    case PairState::kCrossTrack:
        return PairColour::kAmber;
    case PairState::kSearching:
    case PairState::kWaitingAudio:
    case PairState::kValidator:
    case PairState::kBypassed:
        return PairColour::kGrey;
    case PairState::kConnectedUnverified:
    case PairState::kConnected:
    case PairState::kConnectedStill:
        return PairColour::kGreen;
    }
    // 不认识的编号按灰处理：界面宁可显示离线，也不要把未知状态显示成已连接
    return PairColour::kGrey;
}

const char* idOf(PairState s) noexcept
{
    // 桥与诊断都按这些字面量对表，不能从枚举名推导：改名会同时改变外部契约
    switch (s)
    {
    case PairState::kValidator:
        return "validator";
    case PairState::kIpcUnavailable:
        return "ipcUnavailable";
    case PairState::kAbiMismatch:
        return "abiMismatch";
    case PairState::kSrMismatch:
        return "srMismatch";
    case PairState::kConflict:
        return "conflict";
    case PairState::kUnassigned:
        return "unassigned";
    case PairState::kStale:
        return "stale";
    case PairState::kBypassed:
        return "bypassed";
    case PairState::kCrossProcess:
        return "crossProcess";
    case PairState::kCrossTrack:
        return "crossTrack";
    case PairState::kWaitingAudio:
        return "waitingAudio";
    case PairState::kSearching:
        return "searching";
    case PairState::kConnectedUnverified:
        return "connectedUnverified";
    case PairState::kConnected:
        return "connected";
    case PairState::kConnectedStill:
        return "connectedStill";
    }
    return "";
}

bool pulses(PairState s) noexcept
{
    // connectedStill 是「连上了但回送静音」，绿点不该动；只有音频真的在流动的两态才脉冲
    return s == PairState::kConnectedUnverified || s == PairState::kConnected;
}

NameCheck checkNames(u32 selfHash, u32 peerHash) noexcept
{
    // 0 表示宿主没给轨道名：未知不能被当成「不同」，否则每一对匿名轨都会报 crossTrack
    if (selfHash == 0 || peerHash == 0)
    {
        return NameCheck::kUnknown;
    }
    return selfHash == peerHash ? NameCheck::kSame : NameCheck::kDifferent;
}

std::string formatVersion(u32 packed)
{
    // 主版本不截到 8 位：packVersion 的入参就是这么定义的，截了会把将来大于 255 的主版本显示错
    const u32 major = packed >> 16;
    const u32 minor = (packed >> 8) & 0xffu;
    const u32 patch = packed & 0xffu;
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

PairModel buildPairModel(const PairInputs& in, const rinne::ipc::GroupViews& groups)
{
    PairModel model;
    const NameCheck nameCheck = in.peer.present ? checkNames(in.selfNameHash, in.peer.nameHash) : NameCheck::kUnknown;

    // 顺序就是优先级：每一档只在前面的档位都没有命中时才看
    PairState state = PairState::kConnected;
    if (in.validator)
    {
        state = PairState::kValidator;
    }
    else if (in.ipcUnavailable)
    {
        state = PairState::kIpcUnavailable;
    }
    else if (in.abiMismatch)
    {
        state = PairState::kAbiMismatch;
    }
    else if (in.peer.present && in.selfSampleRate != 0 && in.peer.sampleRate != 0 &&
             in.selfSampleRate != in.peer.sampleRate)
    {
        state = PairState::kSrMismatch;
    }
    else if (in.conflict)
    {
        state = PairState::kConflict;
    }
    else if (in.allGroupsFull)
    {
        state = PairState::kUnassigned;
    }
    else if (in.peer.present && in.peer.lost)
    {
        state = PairState::kStale;
    }
    else if (in.role == Role::kOutput && in.selfBypassed)
    {
        state = PairState::kBypassed;
    }
    else if (in.peer.present && in.peer.pid != in.selfPid)
    {
        state = PairState::kCrossProcess;
    }
    else if (in.peer.present && nameCheck == NameCheck::kDifferent)
    {
        state = PairState::kCrossTrack;
    }
    else if (!in.realtimeAudioSeen)
    {
        state = PairState::kWaitingAudio;
    }
    else if (!in.peer.present)
    {
        state = PairState::kSearching;
    }
    else if (nameCheck == NameCheck::kUnknown)
    {
        state = PairState::kConnectedUnverified;
    }
    else
    {
        state = in.loopSilent ? PairState::kConnectedStill : PairState::kConnected;
    }

    model.state = state;
    model.colour = colourOf(state);
    model.group = in.group;
    model.nameCheck = nameCheck;
    // 对端不在时这些字段保持空/0：界面据此不显示上一轮残留的名字与颜色
    if (in.peer.present)
    {
        model.peerName = in.peer.name;
        model.peerColourArgb = in.peer.colourArgb;
        model.peerVersion = formatVersion(in.peer.version);
    }
    // 占着本角色槽的实例只在冲突时有意义，其余状态一律清空
    if (state == PairState::kConflict)
    {
        model.occupantName = in.occupantName;
    }
    model.channelMismatch =
        in.peer.present && in.selfChannels != 0 && in.peer.channels != 0 && in.selfChannels != in.peer.channels;
    model.selfVersion = formatVersion(in.selfVersion);

    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        const u32 bit = static_cast<u32>(1u) << static_cast<u32>(i);
        const bool selfIsInput = in.role == Role::kInput;
        const rinne::ipc::EndView& own = selfIsInput ? groups[i].input : groups[i].output;
        const rinne::ipc::EndView& peer = selfIsInput ? groups[i].output : groups[i].input;
        // 自己那一组的本角色端就是自己，不算「被他人占着」；冲突时恰恰相反，本组的本角色端是别人占着的
        if ((group != in.group || state == PairState::kConflict) && !own.isFree())
        {
            model.occupiedMask |= bit;
        }
        // 对端掩码：同进程、活着、而本角色一端还空着，即「有未配对的对端在等」
        if (peer.isLive() && peer.pid == in.selfPid && own.isFree())
        {
            model.peerMask |= bit;
        }
    }
    return model;
}

} // namespace rinne::pairing
