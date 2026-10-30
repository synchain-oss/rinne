// SPDX-License-Identifier: GPL-3.0-or-later
//
// 进程外配对自测的判据，全部是纯函数，不碰操作系统：宿主 rinne_auhost 的 --pair 模式用它判结果，
// tests/ipc/test_pair_judge.cpp 在所有平台上用手摆的数据把每一条判据的红与绿各跑一遍（宿主本身只能在 mac 上跑）。
// 只依赖标准库与 ipc/SegmentLayout.h（加 -I src/core 即可，里头只有 util/Fourcc.h），所以 tests/oop 那个独立工程
// 不必链 rinne_core。
//
// 判什么：
//   1. 同一组里 Input 与 Output 两个槽都是 Active，两端 pid 相同、都不是宿主自己的 pid（插件真的在进程外）。
//   2. 在观察窗口里两端的心跳都在走、块计数都在涨、槽没有被重新认领，没有冲突或跳闸标志。
//   3. Input 的安全状态机在 FadeIn 或 Run（配上了、没有停摆、没有跳闸）。
//   4. 数据段的写头在涨：Output 的音频真的写进了环。
//   5. Input 的输出里除了自己的 440 Hz 干声，还出现了 Output 的 880 Hz：对端的音频真的过来了。
// 段名的拼法与 ipc/Naming.h 的 composeSegmentName 对拍（tests/ipc/test_pair_judge.cpp），防两边漂移。
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ipc/SegmentLayout.h"

namespace rinne::oop
{

// 宿主喂给两端的测试音与判据的门限。Input 的反馈在宿主里设成 100 %，混合取默认 25 %：
// 干声增益 1、回送增益 0.5，所以 880 Hz 的回送理论上约 0.25 × 1.0 × 0.5 = 0.125，门限取它的八分之一左右。
inline constexpr double kInputToneHz = 440.0;
inline constexpr double kOutputToneHz = 880.0;
inline constexpr double kToneAmplitude = 0.25;
inline constexpr double kMinDryAmplitude = 0.15;
inline constexpr double kMinReturnAmplitude = 0.02;
// 观察窗口至少这么多块（48 kHz、512 帧的块约 1 秒）：窗口太短，「心跳在走」没有意义
inline constexpr std::uint64_t kMinWindowBlocks = 94;

struct SlotSnapshot
{
    ipc::u64 ctl = 0;
    ipc::u32 pid = 0;
    ipc::u64 heartbeatMs = 0;
    ipc::u64 blockSeq = 0;
    ipc::u32 safetyState = 0;
    ipc::u32 rtFlags = 0;

    bool active() const noexcept { return ipc::ctlPhase(ctl) == ipc::kCtlPhaseActive; }
};

struct GroupSnapshot
{
    ipc::u32 group = 0; // 1..8
    SlotSnapshot input;
    SlotSnapshot output;
    bool ringMapped = false;
    ipc::u32 ringMagic = 0;
    ipc::u64 writeHead = 0;

    bool bothActive() const noexcept { return input.active() && output.active(); }
};

inline SlotSnapshot snapshotSlot(const ipc::RoleSlot& slot) noexcept
{
    SlotSnapshot s;
    s.ctl = slot.ctl.load(std::memory_order_acquire);
    s.pid = slot.pid.load(std::memory_order_relaxed);
    s.heartbeatMs = slot.heartbeat_ms.load(std::memory_order_relaxed);
    s.blockSeq = slot.block_seq.load(std::memory_order_relaxed);
    s.safetyState = slot.safety_state.load(std::memory_order_relaxed);
    s.rtFlags = slot.rt_flags.load(std::memory_order_relaxed);
    return s;
}

// group 在 1..8 之外时返回全零的快照（group = 0）
inline GroupSnapshot snapshotGroup(const ipc::RvSegment& rv, ipc::u32 group) noexcept
{
    GroupSnapshot g;
    if (group < 1 || group > ipc::kGroupCount)
    {
        return g;
    }
    const ipc::GroupBlock& block = rv.groups[group - 1];
    g.group = group;
    g.input = snapshotSlot(block.input);
    g.output = snapshotSlot(block.output);
    return g;
}

inline void attachRing(GroupSnapshot& g, const ipc::RingHeader& header) noexcept
{
    g.ringMapped = true;
    g.ringMagic = header.magic.load(std::memory_order_relaxed);
    g.writeHead = header.write_head.load(std::memory_order_acquire);
}

// 第一个 Input 与 Output 都是 Active 的组号；没有返回 0
inline ipc::u32 findPairedGroup(const std::array<GroupSnapshot, ipc::kGroupCount>& groups) noexcept
{
    for (const GroupSnapshot& g : groups)
    {
        if (g.group != 0 && g.bothActive())
        {
            return g.group;
        }
    }
    return 0;
}

inline std::string describeSlot(const char* role, const SlotSnapshot& s)
{
    char text[160];
    std::snprintf(text, sizeof text, "%s 阶段=%u pid=%u 心跳=%llu 块计数=%llu 安全状态=%u 运行标志=0x%x", role,
                  static_cast<unsigned>(ipc::ctlPhase(s.ctl)), static_cast<unsigned>(s.pid),
                  static_cast<unsigned long long>(s.heartbeatMs), static_cast<unsigned long long>(s.blockSeq),
                  static_cast<unsigned>(s.safetyState), static_cast<unsigned>(s.rtFlags));
    return text;
}

inline std::string describeGroup(const GroupSnapshot& g)
{
    char head[64];
    std::snprintf(head, sizeof head, "组 %u：", static_cast<unsigned>(g.group));
    std::string text = head;
    text += describeSlot("Input", g.input);
    text += "；";
    text += describeSlot("Output", g.output);
    if (g.ringMapped)
    {
        char tail[96];
        std::snprintf(tail, sizeof tail, "；环 写头=%llu", static_cast<unsigned long long>(g.writeHead));
        text += tail;
    }
    return text;
}

struct PairCheck
{
    ipc::u32 hostPid = 0;
    bool requireOutOfProcess = true;
    std::uint64_t hostBlocks = 0; // 两次取样之间宿主渲染的块数
    std::uint32_t blockFrames = 512;
    double sampleRate = 48000.0;
};

// 一次判据的结论：failures 为空才算通过，每一条写清楚判了什么、看到了什么
struct Verdict
{
    std::vector<std::string> failures;

    bool ok() const noexcept { return failures.empty(); }
};

namespace detail
{

inline std::string hex(ipc::u32 value)
{
    char text[16];
    std::snprintf(text, sizeof text, "%x", static_cast<unsigned>(value));
    return text;
}

// 运行标志里出现这几个就不算健康的配对：冲突、跳闸、不支持的块长
inline constexpr ipc::u32 kBadRtFlags = ipc::kRtConflict | ipc::kRtTripped | ipc::kRtUnsupportedBlock;

inline void checkSlotWindow(Verdict& v, const char* role, const SlotSnapshot& early, const SlotSnapshot& late,
                            const PairCheck& check)
{
    const std::string name = role;
    if (early.pid != late.pid)
    {
        v.failures.push_back(name + " 的 pid 在观察窗口里变了（" + std::to_string(early.pid) + " -> " +
                             std::to_string(late.pid) + "）：槽被别的进程接管过");
    }
    if (ipc::ctlGen(early.ctl) != ipc::ctlGen(late.ctl))
    {
        v.failures.push_back(name + " 的槽在观察窗口里被重新认领过（代数 " + std::to_string(ipc::ctlGen(early.ctl)) +
                             " -> " + std::to_string(ipc::ctlGen(late.ctl)) + "）");
    }
    const double windowMs = static_cast<double>(check.hostBlocks) * check.blockFrames * 1000.0 / check.sampleRate;
    const std::uint64_t heartbeatGain =
        late.heartbeatMs >= early.heartbeatMs ? late.heartbeatMs - early.heartbeatMs : 0;
    if (static_cast<double>(heartbeatGain) < windowMs / 2.0)
    {
        v.failures.push_back(name + " 的心跳在 " + std::to_string(static_cast<long long>(windowMs)) +
                             " ms 的窗口里只前进了 " + std::to_string(heartbeatGain) +
                             " ms（要求至少一半）：属主没有在跑");
    }
    const std::uint64_t blockGain = late.blockSeq >= early.blockSeq ? late.blockSeq - early.blockSeq : 0;
    if (blockGain < check.hostBlocks / 2)
    {
        v.failures.push_back(name + " 的块计数在宿主渲染 " + std::to_string(check.hostBlocks) + " 块的窗口里只涨了 " +
                             std::to_string(blockGain) + "（要求至少一半）：音频没有送进插件");
    }
    if ((late.rtFlags & kBadRtFlags) != 0)
    {
        v.failures.push_back(name + " 的运行标志里有冲突、跳闸或不支持的块长（0x" + hex(late.rtFlags & kBadRtFlags) +
                             "）");
    }
}

} // namespace detail

// 配对判据。early 是第一次看到该组两端都 Active 时的快照，late 是渲染结束时的快照，两次必须是同一组。
inline Verdict judgePairing(const GroupSnapshot& early, const GroupSnapshot& late, const PairCheck& check)
{
    Verdict v;
    if (early.group == 0 || early.group != late.group)
    {
        v.failures.push_back("两次取样不是同一个组：判据无从比较");
        return v;
    }
    if (check.hostBlocks < kMinWindowBlocks)
    {
        v.failures.push_back("观察窗口只有 " + std::to_string(check.hostBlocks) + " 块，不足 " +
                             std::to_string(kMinWindowBlocks) + " 块（约 1 秒）：心跳与块计数的判据没有意义");
        return v;
    }
    if (!early.bothActive() || !late.bothActive())
    {
        v.failures.push_back("该组 Input 与 Output 没有同时保持 Active 阶段");
        return v;
    }

    if (late.input.pid == 0 || late.output.pid == 0)
    {
        v.failures.push_back("槽里没有 pid：属主身份没写进去");
    }
    else if (late.input.pid != late.output.pid)
    {
        v.failures.push_back("Input 与 Output 的 pid 不同（" + std::to_string(late.input.pid) + " 与 " +
                             std::to_string(late.output.pid) + "）：智能默认只匹配同一进程里的对端");
    }
    if (check.requireOutOfProcess && check.hostPid != 0 &&
        (late.input.pid == check.hostPid || late.output.pid == check.hostPid))
    {
        v.failures.push_back("槽里的 pid 就是宿主自己（" + std::to_string(check.hostPid) + "）：插件没有在进程外");
    }

    detail::checkSlotWindow(v, "Input", early.input, late.input, check);
    detail::checkSlotWindow(v, "Output", early.output, late.output, check);

    const ipc::u32 safety = late.input.safetyState & ((1u << ipc::kTripShift) - 1u);
    if (safety != ipc::kSafetyFadeIn && safety != ipc::kSafetyRun)
    {
        v.failures.push_back("Input 的安全状态是 " + std::to_string(safety) + "，不在 FadeIn(" +
                             std::to_string(ipc::kSafetyFadeIn) + ") 或 Run(" + std::to_string(ipc::kSafetyRun) +
                             ")：没有配上、已停摆或已跳闸");
    }

    if (!late.ringMapped)
    {
        v.failures.push_back("数据段没有映射到：Output 没有创建这一组的环");
    }
    else
    {
        if (late.ringMagic != ipc::kRingMagic)
        {
            v.failures.push_back("数据段的 magic 不对（0x" + detail::hex(late.ringMagic) + "）");
        }
        const std::uint64_t headGain = late.writeHead >= early.writeHead ? late.writeHead - early.writeHead : 0;
        const std::uint64_t wantFrames = check.hostBlocks * check.blockFrames / 2;
        if (headGain < wantFrames)
        {
            v.failures.push_back("环的写头在窗口里只前进了 " + std::to_string(headGain) + " 帧（要求至少 " +
                                 std::to_string(wantFrames) + "）：Output 的音频没有写进环");
        }
    }
    return v;
}

// 单一频率的幅度（Goertzel）。样本窗口里放整数个周期时是准确值：宿主取 1 秒、48 kHz，440 与 880 Hz 都满足。
inline double goertzelAmplitude(const float* samples, std::size_t count, double frequencyHz, double sampleRate) noexcept
{
    if (samples == nullptr || count == 0 || sampleRate <= 0.0)
    {
        return 0.0;
    }
    const double pi = 3.14159265358979323846;
    const double coefficient = 2.0 * std::cos(2.0 * pi * frequencyHz / sampleRate);
    double s1 = 0.0;
    double s2 = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const double s0 = static_cast<double>(samples[i]) + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(power > 0.0 ? power : 0.0) / static_cast<double>(count);
}

// 音频判据：Input 的输出里要有自己的 440 Hz 干声，也要有 Output 的 880 Hz（经环路送过来的对端音频）
inline Verdict judgeTones(double dryAmplitude, double returnAmplitude)
{
    Verdict v;
    char text[200];
    if (!(dryAmplitude >= kMinDryAmplitude))
    {
        std::snprintf(text, sizeof text, "Input 输出里 440 Hz 的干声只有 %.4f（要求至少 %.2f）：干声没有直通",
                      dryAmplitude, kMinDryAmplitude);
        v.failures.push_back(text);
    }
    if (!(returnAmplitude >= kMinReturnAmplitude))
    {
        std::snprintf(text, sizeof text,
                      "Input 输出里没有出现 Output 的 880 Hz（幅度 %.4f，要求至少 %.2f）：对端的音频没有送过来",
                      returnAmplitude, kMinReturnAmplitude);
        v.failures.push_back(text);
    }
    return v;
}

// Output 的输出必须逐位直通宿主送来的 880 Hz：幅度在 0.25 的 10 % 以内
inline Verdict judgePassthrough(double outputAmplitude)
{
    Verdict v;
    if (!(std::fabs(outputAmplitude - kToneAmplitude) <= kToneAmplitude * 0.1))
    {
        char text[160];
        std::snprintf(text, sizeof text, "Output 输出里 880 Hz 的幅度是 %.4f，应当等于宿主送来的 %.2f：直通被改动了",
                      outputAmplitude, kToneAmplitude);
        v.failures.push_back(text);
    }
    return v;
}

// 宿主进程已加载的映像里是不是 Rinne 的 .component：进程外加载时宿主里不该有它
inline bool isRinnePluginImagePath(std::string_view path)
{
    return path.find(".component/") != std::string_view::npos && path.find("Rinne") != std::string_view::npos;
}

// POSIX 的正式作用域段名：group 0 是会合段，1..8 是数据段。prefix 是 POSIX 短写族名（如 SnchRinne），uid 是有效用户号。
// 格式与 ipc/Naming.h 的 composeSegmentName(kPosix, …, 正式作用域, uid, 段) 一致，tests/ipc/test_pair_judge.cpp
// 逐个对拍。
inline std::string posixSegmentName(std::string_view prefix, ipc::u32 uid, ipc::u32 group, ipc::u32 abi = ipc::kIpcAbi)
{
    static const char kHex[] = "0123456789abcdef";
    std::string name = "/";
    name.append(prefix);
    name.push_back('.');
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        name.push_back(kHex[(uid >> shift) & 0xfu]);
    }
    name.push_back('.');
    if (group == 0)
    {
        name += "rv";
    }
    else
    {
        name += "v" + std::to_string(abi) + ".g" + std::to_string(group);
    }
    return name;
}

} // namespace rinne::oop
