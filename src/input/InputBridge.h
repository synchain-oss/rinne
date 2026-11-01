// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 原生桥的纯逻辑（契约 docs/UI_BRIDGE.md 的 common 与 input 两节）：二十四个角色函数的参数校验与回执、
// rinne.state 的 Input 字段、rinne.pair、rinne.params、rinne.meters、rinne.presets 的载荷，变更检测与 1 Hz 兜底。
// 不碰任何组件，落到系统上的动作（剪贴板、浏览器、许可证）与时钟都经 Hooks 注入，所以宿主测试（不开浏览器组件）
// 直接编它、直接测。接到 WebView 上的胶水是 InputEditor（WebViewHost 的子类）。
//
// 事件：
//   - rinne.state：WebViewHost 的共用快照加上这里的 Input 字段。每次 poll 用一份廉价的签名比较（不读设置文件），
//     变了就发；另有 1 Hz 兜底，读设置文件的几项（另一个实例改了全局位）靠它带上；
//   - rinne.pair、rinne.params、rinne.presets：每次 poll 与上一帧比较，变了就发，另有 1 Hz 兜底。seq 各数各的
//     （页面按事件各自丢弃旧帧）；rinne.presets 不带 seq；
//   - rinne.meters：只在 tick（25 Hz）里读，五路峰值读走即清零，所以桥函数之后的 poll 不碰它；
//     先比较再发，另有 1 Hz 兜底；
//   - setParam 回执里的 seq 就是随后那帧回声的 seq，页面据此丢掉手势之内的旧回声；
//   - rinne.error：按需发，不属于首帧。目前只有一种：保存工程时 state 编码失败（code stateEncode，不带 detail）。
//   首帧由 firstFrames 给出（编辑器在 requestInitialState 的首帧钩子里发），同时把各事件的基线对齐到首帧，紧接着的
//   一拍不会再重发。网页不可见时 JUCE 不发事件，基线照样推进；页面重新可见后最迟 1 s 由兜底帧追平。
// 可缺省字段拿不到就不发，不编值：sampleRate 与 delay.actualMs、delay.minMs 在 prepare 之前不发、delay.bpm 没有速度时
// 不发、build 的 sha 与 fmt 各自缺省、takeoverGroup 只在有待接管的组时发、self.color 与 peer.channels /
// peer.sampleRate 宿主或对端没给就不发。
//
// 当前预设：工程里只有 preset_name 与 preset_dirty 两个键（state 冻结），存的是显示名（工厂预设取 en）。ID 在实例的
// UiSession 里，工程恢复或换机之后按名字找回来：先工厂三语、再用户预设；都找不到时 id 为空、名字照写，页面显示名字，
// 只是不能覆盖或删除。参数被界面手动改动（值真的变了）才置脏，宿主自动化不算。
//
// 安全相关的几条：panic 与 resumeLoop 只走处理器现有的入口；是否切断、是否冷却都读安全链发布的原子字，桥不另判。
// 冷却里只有「在不在」一位，剩余毫秒是估算：从观察到这一位起按消息线程时钟倒数 1.5 s，到点而这一位还在就写 1。
// 手势只在消息线程；编辑器析构或切兜底面板时，还开着的手势一律补上结束。桥从不主动写参数：只有页面调用才写，
// 所以首帧之前不会把任何默认值写给宿主。
//
// 线程：全部在消息线程。音频线程只经处理器现有的原子量交出读数，这里不往音频路径里加任何锁或分配。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <optional>

#include "BridgeBase.h"
#include "InputProcessor.h"
#include "diag/DiagReport.h"
#include "ipc/SlotObserver.h"
#include "loop/LoopStatus.h"

namespace rinne::input
{

// notices 的五个来源，都是现成的信号（见 InputBridge::noticeFacts）
struct NoticeFacts
{
    bool ipcDenied = false; // 会合打不开共享内存，而且后端报的是操作系统拒绝（多半是宿主沙盒）
    bool outputFirst = false; // 错序证据成立：Output 在 Input 之前被处理
    bool unstable = false; // 宿主时序不稳定：进过一次容忍模式就一直算
    std::uint32_t heldNewerAbi = 0; // 正在保留的更新版本 state 的 abi；0 = 没有
    bool decodeFailed = false; // 最近一次读入的 state 解不出来
};

// notices 数组（元素 { id, a?, b? }），顺序固定：ipcSandbox、outOfOrder、unstable、newerProject、stateDecodeFailed。
// newerProject 的 a 是工程的、b 是本构建的（页面显示时自己对调）。没有一条成立时返回空数组，调用方据此不发这个字段
juce::Array<juce::var> buildNotices(const NoticeFacts& facts);
// 五个来源里来自环路状态的两个
void applyLoopFacts(NoticeFacts& facts, const loop::LoopStatusFields& loop) noexcept;

// silentCause 的四个来源。按值发，与配对态无关
struct SilentFacts
{
    bool hostBypass = false; // 最近一块是 processBlockBypassed
    bool peerBypass = false; // 本组 Output 槽上的旁路位
    bool hostPaused = false; // 样本计数停了超过 kHostPausedMs
    bool stalled = false; // 安全状态机处在 STALLED
};

// 回送为什么静音；没有原因时是空指针（桥上写 null）。优先级：本端旁路 > 宿主暂停 > 对端旁路 > 断档。
// 宿主停了回调时对端的旁路位与断档都可能是陈旧的结论，所以排在宿主暂停之后
const char* silentCauseOf(const SilentFacts& facts) noexcept;

// 本组 Output 槽上的旁路位算不算数：槽要读稳了、是活的。休眠或正在换主人的槽上的是旧账
bool peerBypassOf(const ipc::SlotSample& peer) noexcept;

// rinne.pair 的 peer 里来自槽的两项，拿不到就不发。对端不在时槽里剩下的值是旧账
void addPeerAudioFacts(juce::DynamicObject& peer, const diag::DiagPeer& facts);

// 三盏灯的判据输入，cut 取 safety.cut 的同一个字符串（没切断是空指针）
struct LampFacts
{
    const char* cut = nullptr;
    bool dcHeld = false; // 回送直流均值超过 kDcLampLevel 已持续 kDcLampHoldMs
    bool loopNan = false; // 环内 NaN 灯（引擎按样本计的 5 s 保持）
    std::uint32_t nonfiniteCount = 0; // 读环时与写环前置零的非有限样本数之和
    float guardGainDb = 0.0f; // 失控保护当前的压低量（≤ 0）
    bool nearFullHeld = false; // 回送贴近满幅的连续时长达到失控保护的判定时长
    bool limiterOn = true; // 限制器关着时没有压限可言，也就不报持续压限
};

// rinne.meters 的 lamps：{ dc, nonfinite, guard, nonfiniteCount, guardDb }
juce::var buildLamps(const LampFacts& facts);

// 线性峰值换成 dBFS（一位小数），静音（0 或非有限）是 null
juce::var levelDb(float linearPeak);

// safety.l2：切断、组冲突、版本不匹配、限制器关而失控灯亮，四类条件有一个成立。
// 限制器关且反馈超 100 % 不必单列：那时有效上限就是 100 %，不算异常
bool isL2(const char* cut, const juce::String& pairState, bool limiterOn, bool guardLit);

// delay.reason：ok、raised（用户要的 D 低于下限，下限按块长抬起来了）、overrun（下限是宿主违约抬的）、
// capped（D 超过环容量被截）
const char* delayReasonOf(const loop::LoopStatusFields& loop, std::uint32_t sampleRate) noexcept;

// 把这一次读到的导出计数记进界面会话：计数变了就把引擎留的原因位并进来，然后更新已见值
void trackExportCauses(InputProcessor::UiSession& session, const loop::LoopStatusFields& loop) noexcept;

// rinne.state 的 build：{ sha, fmt, arch }，拿不到的项不发。
//   sha  构建标签里最后一个 + 之后的部分，要是 7 到 40 位小写十六进制（本机构建没有标签，不发）；
//   fmt  包装层名字：VST3 或 AU，别的（宿主测试里的未定义、独立程序）不发；
//   arch 平台加处理器架构，如 "Windows x64"、"macOS arm64"，编译时就定了，恒有
juce::var buildInfo(const juce::String& buildLabel, juce::AudioProcessor::WrapperType wrapper);

class InputBridge
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

    // 本次要发的帧。除 state 与 errors 外空的 var 表示这次不发。state 为真时由调用方经 WebViewHost::emitState 发
    struct Frames
    {
        bool state = false;
        juce::var pair;
        juce::var params;
        juce::var meters;
        juce::var presets;
        juce::Array<juce::var> errors;
    };

    // processor 要比本对象活得久（编辑器由处理器造，先于处理器析构）
    InputBridge(InputProcessor& processor, Hooks hooks);
    ~InputBridge();

    // ---- 两个角色共用的函数（消息线程）。回执照 docs/UI_BRIDGE.md；改了要推给页面的东西时，下一次 poll 当拍发出 ----
    juce::var setRulesAck(const juce::Array<juce::var>& args);
    juce::var setGuideSeen(const juce::Array<juce::var>& args);
    juce::var setGroup(const juce::Array<juce::var>& args);
    juce::var copyDiagnostics(const juce::Array<juce::var>& args, const UiContext& ui);
    juce::var openExternal(const juce::Array<juce::var>& args);
    juce::var setDiagLog(const juce::Array<juce::var>& args);
    juce::var resolveTakeover(const juce::Array<juce::var>& args);
    juce::var showLicense();

    // ---- Input 专用函数 ----
    juce::var beginParamGesture(const juce::Array<juce::var>& args);
    juce::var setParam(const juce::Array<juce::var>& args);
    juce::var endParamGesture(const juce::Array<juce::var>& args);
    juce::var setLimiter(const juce::Array<juce::var>& args);
    juce::var panic();
    juce::var resumeLoop();
    juce::var dismissHint(const juce::Array<juce::var>& args);
    juce::var loadPreset(const juce::Array<juce::var>& args);
    juce::var savePreset(const juce::Array<juce::var>& args);
    juce::var deletePreset(const juce::Array<juce::var>& args);
    juce::var setGuardOn(const juce::Array<juce::var>& args);
    juce::var setResetOnPlay(const juce::Array<juce::var>& args);
    juce::var setTailMode(const juce::Array<juce::var>& args);
    juce::var setFbUnit(const juce::Array<juce::var>& args);
    juce::var setDelayUnit(const juce::Array<juce::var>& args);
    juce::var moveToFreeGroup();

    // 还开着的参数手势一律补上结束（编辑器析构、切兜底面板）。页面已经没了，等不到它自己结束
    void endAllGestures();

    // ---- 事件 ----
    // rinne.state 的 Input 字段，补进 WebViewHost 的共用快照
    void addStateFields(juce::DynamicObject& snapshot);
    // 桥函数之后调用：变更检测加 1 Hz 兜底，返回这次要发的帧（不含电平）
    Frames poll(const UiContext& ui);
    // 定时器每拍（25 Hz）调用：poll 再加上 rinne.meters
    Frames tick(const UiContext& ui);
    // requestInitialState 发完首帧 rinne.state
    // 之后调用：把各事件的基线对齐到首帧，返回首帧的其余事件（重复请求每次都给）
    Frames firstFrames(const UiContext& ui);

    // ---- 读出（测试） ----
    NoticeFacts noticeFacts() const;
    SilentFacts silentFacts();
    std::optional<std::uint32_t> pendingTakeover() const noexcept { return takeover_; }

    static constexpr std::uint64_t kHeartbeatMs = 1000; // 状态类事件的兜底间隔
    // 样本计数这么久没前进算宿主暂停：与会合判两端一起挂起的口径相同（250 ms）
    static constexpr std::uint64_t kHostPausedMs = 250;
    // 直流灯的判据（core 只发布均值）：均值超过 −40 dBFS，持续 1 s
    static constexpr float kDcLampLevel = 0.01f;
    static constexpr std::uint64_t kDcLampHoldMs = 1000;
    // 恢复的冷却时长，只用来估算剩余毫秒；在不在冷却看安全链的那一位
    static constexpr std::uint64_t kCooldownMs = 1500;

private:
    std::uint64_t now() const;
    engine::Meters& meters() noexcept;
    void discardPeaks();
    // rinne.state 里不读设置文件的那些字段；签名与快照共用这一份
    juce::DynamicObject::Ptr cheapStateFields();
    juce::String stateSignature(const UiContext& ui);
    juce::var pairBody() const;
    juce::var paramsBody() const;
    juce::var metersBody();
    juce::var presetsBody();
    juce::var delayBody(const loop::LoopStatusFields& loop, std::uint32_t sampleRate) const;
    juce::var safetyBody(std::uint32_t safetyWord, const char* cut, const juce::String& pairState, std::uint64_t nowMs);
    juce::var presetField();
    bool limiterOn() const;
    LampFacts lampFacts(std::uint32_t safetyWord, std::uint64_t nowMs);
    juce::var withSeq(const juce::var& body, std::uint32_t seq) const;
    void trackHostProgress(std::uint64_t nowMs);
    void trackCooldown(bool cooling, std::uint64_t nowMs);
    std::uint64_t cooldownLeftMs(std::uint64_t nowMs) const;
    void dropStaleTakeover();
    void resolveCurrentPreset(const juce::String& name);
    void setCurrentPreset(const juce::String& id, const juce::String& name);
    juce::var writeParam(const juce::String& id, const juce::var& value);
    void markPresetDirty();
    void refreshPresetList();

    InputProcessor& processor_;
    Hooks hooks_;

    std::optional<std::uint32_t> takeover_; // 等用户确认接管的组（1..8）
    bool forceState_ = false; // 桥函数改了全局位：签名看不见，下一次 poll 照发
    juce::StringArray gestures_; // 还开着的参数手势

    juce::String lastStateSig_;
    std::uint64_t lastStateMs_ = 0;
    bool stateSent_ = false;

    juce::String lastPairJson_;
    std::uint64_t lastPairMs_ = 0;
    bool pairSent_ = false;
    std::uint32_t pairSeq_ = 0;

    juce::String lastParamsJson_;
    std::uint64_t lastParamsMs_ = 0;
    bool paramsSent_ = false;
    bool echoPending_ = false; // setParam 已经发出 seq，这个 seq 的回声帧还没发
    std::uint32_t paramSeq_ = 0;

    juce::String lastMetersJson_;
    std::uint64_t lastMetersMs_ = 0;
    bool metersSent_ = false;
    std::uint32_t meterSeq_ = 0;
    bool dcOver_ = false;
    std::uint64_t dcOverSinceMs_ = 0;

    juce::var userPresets_; // 用户预设列表的缓存：读目录只在首帧与保存、删除之后
    std::uint64_t lastPresetsMs_ = 0;
    bool presetsPending_ = true;

    std::uint32_t errorSeq_ = 0;
    std::uint32_t encodeFailuresSeen_ = 0;

    std::int64_t lastSamples_ = 0;
    std::uint64_t lastAdvanceMs_ = 0;
    bool cooling_ = false;
    std::uint64_t coolingSinceMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputBridge)
};

} // namespace rinne::input
