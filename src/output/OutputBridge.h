// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 原生桥的纯逻辑（契约 docs/UI_BRIDGE.md 的 common 与 output 两节）：十个角色函数的参数校验与回执、
// rinne.state 的 Output 字段、rinne.pair 载荷、notices、变更检测与 1 Hz 兜底。不碰任何组件，落到系统上的动作
// （剪贴板、浏览器、许可证）与时钟都经 Hooks 注入，所以宿主测试（不开浏览器组件）直接编它、直接测。
// 接到 WebView 上的胶水是 OutputEditor（WebViewHost 的子类）。
//
// 事件：
//   - rinne.state：WebViewHost 的共用快照加上这里的 Output 字段。每拍（25 Hz）用一份廉价的签名比较（不读设置文件），
//     变了就发；另有 1 Hz 兜底，读设置文件的几项（另一个实例改了全局位）靠它带上。桥函数改了全局位时当拍补发；
//   - rinne.pair：每拍构造完整载荷与上一帧比较，变了就发，另有 1 Hz 兜底；seq 自己数，与 rinne.state 互不相干
//     （页面按事件各自丢弃旧帧）。首帧由 firstFrames 给出（编辑器在 requestInitialState 的首帧钩子里发），
//     同时把两个事件的基线对齐到首帧，紧接着的一拍不会再重发；
//   - rinne.error：按需发，不属于首帧。目前只有一种：保存工程时 state 编码失败（code stateEncode，不带 detail）。
//   网页不可见时 JUCE 不发事件，基线照样推进；页面重新可见后最迟 1 s 由兜底帧追平。
// 可缺省字段拿不到就不发，不编值：sampleRate 在 prepare 之前不发、build 的 sha 与 fmt 各自缺省、
// takeoverGroup 只在有待接管的组时发、self.color 与 peer.channels / peer.sampleRate 宿主或对端没给就不发。
//
// 接管：休眠实例占着要去的组时，一键修复先不动（回 needs-confirm），记下待接管的组，rinne.state 带 takeover 与
// takeoverGroup；resolveTakeover(true) 带确认的意图去拿那一组，false 取消；没拿成（探针构建，或处理器拒了）回
// no-pending，与已经没有待接管的同一个原因。不在冲突里了，或用户自己选了组，待接管的记录作废。
//
// 线程：全部在消息线程。音频线程只经处理器现有的原子量交出读数（样本计数、引擎状态、错序证据），
// 这里不往音频路径里加任何锁或分配。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <optional>

#include "BridgeBase.h"
#include "OutputProcessor.h"

namespace rinne::output
{

// notices 的四个来源，都是现成的信号（见 OutputBridge::noticeFacts）
struct NoticeFacts
{
    bool ipcDenied = false; // 会合打不开共享内存，而且后端报的是操作系统拒绝（多半是宿主沙盒）
    std::uint32_t heldNewerAbi = 0; // 正在保留的更新版本 state 的 abi；0 = 没有
    bool decodeFailed = false; // 最近一次读入的 state 解不出来
    bool outputFirst = false; // 错序证据成立：Output 在 Input 之前被处理
};

// notices 数组（元素 { id, a?, b? }），顺序固定：ipcSandbox、newerProject、stateDecodeFailed、outOfOrder。
// 没有一条成立时返回空数组，调用方据此不发这个字段
juce::Array<juce::var> buildNotices(const NoticeFacts& facts);

// rinne.state 的 build：{ sha, fmt, arch }，拿不到的项不发。
//   sha  构建标签里最后一个 + 之后的部分，要是 7 到 40 位小写十六进制（本机构建没有标签，不发）；
//   fmt  包装层名字：VST3 或 AU，别的（宿主测试里的未定义、独立程序）不发；
//   arch 平台加处理器架构，如 "Windows x64"、"macOS arm64"，编译时就定了，恒有
juce::var buildInfo(const juce::String& buildLabel, juce::AudioProcessor::WrapperType wrapper);

class OutputBridge
{
public:
    // 落到系统上的动作与时钟。编辑器填真的，宿主测试填替身；空的动作钩子按「做不到」回执
    // （nowMs 与 takeOverGroup 除外，空的时候用括号里的那一份）
    struct Hooks
    {
        std::function<std::uint64_t()> nowMs; // 单调毫秒；空 = juce::Time::getMillisecondCounterHiRes
        std::function<bool(const juce::String&)> writeClipboard; // 写剪贴板并回读核对，成功返回真
        std::function<bool(const juce::String&)> launchUrl; // 用系统浏览器打开（地址已过白名单）
        std::function<bool()> showLicense; // 用系统方式打开随包的许可证
        // 带确认的意图接管那一组，拿到了返回真；空 = 处理器的 takeOverGroup。宿主测试用它造出拿不到的情形
        std::function<bool(int)> takeOverGroup;
    };

    // 宿主那边的界面状态：WebViewHost 管着语言与缩放，运行时描述只在 ui 段里用
    struct UiContext
    {
        juce::String lang = "zh";
        float uiScale = rinne::bridge::plugin::DefaultUiScale;
        juce::String webview;
    };

    // 本拍要发的帧。pair 与 errors 已带 seq；state 为真时由调用方经 WebViewHost::emitState 发
    struct Frames
    {
        bool state = false;
        juce::var pair;
        juce::Array<juce::var> errors;
    };

    // processor 要比本对象活得久（编辑器由处理器造，先于处理器析构）
    OutputBridge(OutputProcessor& processor, Hooks hooks);

    // ---- 角色函数（消息线程）。回执照 docs/UI_BRIDGE.md；改了要推给页面的东西时，下一次 poll 当拍发出 ----
    juce::var setRulesAck(const juce::Array<juce::var>& args);
    juce::var setGuideSeen(const juce::Array<juce::var>& args);
    juce::var setGroup(const juce::Array<juce::var>& args);
    juce::var copyDiagnostics(const juce::Array<juce::var>& args, const UiContext& ui);
    juce::var openExternal(const juce::Array<juce::var>& args);
    juce::var setDiagLog(const juce::Array<juce::var>& args);
    juce::var resolveTakeover(const juce::Array<juce::var>& args);
    juce::var showLicense();
    juce::var repairPair();
    juce::var setTailMode(const juce::Array<juce::var>& args);

    // ---- 事件 ----
    // rinne.state 的 Output 字段，补进 WebViewHost 的共用快照（含读设置文件的 inputGuideSeen）
    void addStateFields(juce::DynamicObject& snapshot);
    // 每拍调用：变更检测加 1 Hz 兜底，返回本拍要发的帧
    Frames poll(const UiContext& ui);
    // requestInitialState 发完首帧 rinne.state 之后调用：把 rinne.state 的基线对齐到刚发的那一帧，返回首帧 rinne.pair
    juce::var firstFrames(const UiContext& ui);

    // ---- 读出（测试） ----
    NoticeFacts noticeFacts() const;
    std::optional<std::uint32_t> pendingTakeover() const noexcept { return takeover_; }

    static constexpr std::uint64_t kHeartbeatMs = 1000; // 两个状态类事件的兜底间隔
    // 样本计数这么久没前进算宿主暂停：与会合判两端一起挂起的口径相同（250 ms）
    static constexpr std::uint64_t kHostPausedMs = 250;

private:
    std::uint64_t now() const;
    // rinne.state 里不读设置文件的那些字段；签名与快照共用这一份
    juce::DynamicObject::Ptr cheapStateFields();
    juce::String stateSignature(const UiContext& ui);
    // rinne.pair 载荷（不含 seq）
    juce::var pairBody() const;
    juce::var withSeq(const juce::var& body, std::uint32_t seq) const;
    void trackHostProgress(std::uint64_t nowMs);
    void dropStaleTakeover();

    OutputProcessor& processor_;
    Hooks hooks_;

    std::optional<std::uint32_t> takeover_; // 等用户确认接管的组（1..8）
    bool forceState_ = false; // 桥函数改了全局位：签名看不见，下一次 poll 照发

    juce::String lastStateSig_;
    std::uint64_t lastStateMs_ = 0;
    bool stateSent_ = false;
    juce::String lastPairJson_;
    std::uint64_t lastPairMs_ = 0;
    bool pairSent_ = false;
    std::uint32_t pairSeq_ = 0;
    std::uint32_t errorSeq_ = 0;
    std::uint32_t encodeFailuresSeen_ = 0;

    std::int64_t lastSamples_ = 0;
    std::uint64_t lastAdvanceMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputBridge)
};

} // namespace rinne::output
