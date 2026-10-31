// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#pragma once

// 需要 JUCE_WEB_BROWSER=1：宿主测试目标不开浏览器组件，不要在那里包含本头文件，
// 要测的逻辑已经拆到 BridgeBase、PlatformWebView、ResourceProvider、FallbackPanel 与 WebViewRevealGate 里。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <functional>
#include <memory>

#include "BridgeBase.h"
#include "FallbackPanel.h"
#include "PlatformWebView.h"
#include "ResourceProvider.h"
#include "WebViewRevealGate.h"
#include "util/RinneVersion.h"

namespace rinne::webview
{
// 两个插件共用的网页编辑器：装配 WebView、资源提供者、桥函数、看门狗、兜底面板与开窗遮挡闸。
// 角色（Input 或 Output）只决定设计盒尺寸、页面入口与存储的键；角色专属的桥函数与状态事件，
// 由 Config::augmentOptions 追加原生函数，并覆写 buildSnapshot、emitTick 与 emitFirstFrames 补充。
//
// 缩放：固定设计盒加页面里的缩放，窗口大小跟着档位走。
//   - 开窗时按显示器可用区夹取档位，只影响本次开窗，不写回已保存的值；
//   - 页面选档只是预览，确认后才落盘；
//   - 缩放锁定时窗口钉在 100%，选档与确认都被拒，已保存的档位不动（系统 WebKit 过旧时由页面或平台层触发）；
//     锁定时没确认的预览作废，档位退回最近一次确认的那一档。
// 兜底面板：运行时缺失、过旧、前端启动失败或看门狗超时时切出，离开它只有点重试这一条路（重新加载入口页）。
// 切面板时网页被隐藏，而 JUCE 在隐藏时默认把页面卸载（导航到空白页；本宿主不开保留页面的选项，
// 那会改变宿主隐藏窗口等所有隐藏场景的资源与重载行为），所以慢页面在看门狗超时那一刻就被卸掉了，面板不会自己收回。
// 面板在场时迟到的桥请求只回执，不当作就绪：不置位、不计入活着的桥、不发首帧（见 handleRequestInitialState）。
// 除 requestInitialState 之外的桥函数在面板在场时一律回 not-ready，先于参数校验（通则见 docs/UI_BRIDGE.md）：
// 不改窗口、不落盘、不改语言与缩放、不发状态。守卫写在每个桥函数的开头，源码扫描逐个核对（见宿主测试的源码卫生用例）；
// 新增桥函数（含经 augmentOptions 追加的角色专属函数）或覆写 handleSetLang 的子类，也要在开头加同样的守卫：
// 日志一行、回 not-ready、返回。宿主之外拿不到私有的面板成员，经受保护的 panelShown() 与 logDiag 写守卫，
// 守卫写成 if (panelShown())；源码扫描认这个写法，找不到合格守卫的角色专属桥函数会让宿主测试变红。
// 面板在场期间定时器照跑，面板上的环路开关与恢复冷却靠它刷新。
// 页面和内核交上来的文字（启动失败的阶段与细节、导航错误说明、地址）写日志、上面板之前一律先整形成一行，
// 面板只显示纯 ASCII，见 ResourceProvider::asciiForLog 与 urlForLog。
// 导航：页面带着原生桥，所以只放行资源提供者的根下面的地址（和页面隐藏时的空白页），其余导航一律拦下并写日志，
// 判定见 ResourceProvider::isRootNavigation；新窗口请求同样一律拦下。页面隐藏时 JUCE 自己导航到空白页，
// 这一次放行但不当作页面加载（不改导航状态、不写诊断，判定见 ResourceProvider::isBlankPage）。
// 全部在消息线程上，没有任何一行跑在音频线程。
class WebViewHost : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    struct Config
    {
        juce::String role; // Init::Role："input" 或 "output"
        // 首帧预置与快照里的版本串。默认取构建时的版本，两个插件不必各传一遍
        juce::String version = build::kVersion;
        ResourceProvider::Source resourceSource; // 插件自己的二进制数据；暂无可嵌资源则传空 Source
        // 在通用装配之后调用，追加角色专属的原生函数与首帧预置。空表示只用通用装配
        std::function<void(juce::WebBrowserComponent::Options&)> augmentOptions;
        // 兜底面板上三个原生动作的钩子，原样交给面板
        FallbackPanel::ControlHooks controls;
    };

    WebViewHost(juce::AudioProcessor& processor, Config config);
    ~WebViewHost() override;

    void resized() override;
    // 铺满占位（外圈色加与页面外壳同几何的圆角渐变）。WebView 落在本组件里时它铺满本组件且声明不透明，
    // JUCE 会把它的矩形从父组件的裁剪区里剔掉，本函数净效果为 0。它真正会画的有两条路：兜底面板路径
    // （WebView 被隐藏），以及遮挡期（WebView 被挪到可视区之外；WebView2 上从加载尝试一开头就是）。
    // 后者正是用户在内容出来之前看到的占位
    void paint(juce::Graphics& g) override;

    // 当前窗口实际使用的缩放档位（锁定时是 1.0）
    float uiScale() const { return scale_.scale(); }
    const juce::String& lang() const { return lang_; }
    bool scaleLocked() const { return scale_.locked(); }

    // 锁定或解锁缩放。锁定把窗口钉回 100%，不改已保存的值
    void setScaleLocked(bool locked);

    // 定义在实现里：HostWebView 在本头文件里只前置声明，派生到基类的转换需要完整类型
    juce::WebBrowserComponent& webView();

    // 看门狗预算。旧做法从跳转那一刻起算一个固定预算，把「WebView2 环境冷启动」和「页面加载」压在同一个
    // 预算里，冷启动被误判成加载失败。现在分两段：导航事件到达前用冷或热启动预算，
    // 导航一开始就从那一刻重新起算。三个常量都对外可见，便于单测与文档引用。

    // 冷启动预算：进程内第一个 WebView2 实例要先拉起浏览器进程组（浏览器、渲染、GPU），建用户数据目录，
    // 编译首屏脚本。机械盘、杀毒软件实时扫描或首次建目录的机器上这一段常态就超过 5 秒，
    // 用较短的预算会把「慢」判成「坏」
    static constexpr int kColdLoadBudgetMs = 15000;
    // 热启动预算：此刻另有一个活着的桥，说明浏览器进程组常驻，再开编辑器只是新建一个 WebView，
    // 不再有进程组冷启动成本。继续用冷预算只会让真失败晚 10 秒才给兜底面板
    static constexpr int kWarmLoadBudgetMs = 5000;
    // 导航开始后的预算：首个导航事件到达就证明 WebView2 环境已就绪，剩下只是资源加载加前端启动。
    // 从该时刻重新起算，避免环境启动慢吃掉页面加载的额度
    static constexpr int kAfterNavBudgetMs = 5000;

    // 这条不等式有两个半边。常量关系：遮挡闸在首帧信号缺席时的超时兜底必须早于看门狗切兜底面板，
    // 否则按住占位的那几秒会被面板顶掉，形态不是「多按一会儿占位」而是直接进兜底；这一半由下面的编译期断言在每次编译时守。
    // 前提：「早于」比较的是同一时刻起算的两个预算。看门狗起算点是 beginLoadAttempt，
    // 导航事件到达时的顺延只在本次加载尝试的首次导航事件上生效，靠 beginLoadAttempt 复位 navBudgetApplied_。
    // 复位一旦被删或挪走，第二个导航事件之后看门狗就会早于闸门到期，而断言照样绿（它只看两个常量）。
    // 这一半没有用例守：宿主类不进宿主测试目标。把看门狗的预算算术抽成不碰组件的纯逻辑再钉复位语义，留待后续
    static_assert(RevealGate::kRevealFallbackMs < kAfterNavBudgetMs,
                  "the reveal gate timeout must fire before the watchdog budget, otherwise the placeholder "
                  "phase is replaced by the fallback panel");

    // 前端启动失败上行的事件名。走 JUCE 内建的页面到原生通道（页面里的宿主对象的 postMessage 对应
    // 选项里的事件监听），不经桥脚本，不属于桥契约；带 __rinne__ 前缀标明这是诊断面。
    // 真源在此，页面的启动守卫逐字引用同一个名字，由嵌入资源回归逐字对拍
    static constexpr const char* kBootErrorEventId = "__rinne__bootError";

    // 前端「首帧已绘」上行信号的事件名，同样是诊断与时序面，不属于桥契约，与上一条共用同一条通道。
    // 真源在此，三个页面的 head 内联脚本逐字引用同一个名字。它守的是什么、为什么不能改成隐藏或零尺寸，
    // 只写在 WebViewRevealGate.h 一处
    static constexpr const char* kFirstFrameEventId = "__rinne__firstFrame";

    // 上面那条信号载荷里唯一被读的字段名：页面量到的「信号时刻减首次绘制时刻」的毫秒差，只进日志。
    // 它必须与首帧事件名同一档纪律，真源在此，页面逐字引用。单独立常量而不是就地写字面量，
    // 是因为这个名字跨了页面到原生两侧，任一侧打错一个字母，失败形态是日志里打「没有绘制记录」，
    // 而那与「页面确实走了回落路」在日志里逐字同形，分不出是哪一种
    static constexpr const char* kPaintDeltaField = "paintDeltaMs";

protected:
    // 确认缩放后落盘。默认写进全局默认存储（按角色分键）。子类可覆写，例如同时写别处
    virtual void persistUiScale(float scale);

    // 状态快照的共用部分：角色、版本、语言、缩放，加上全局默认里的语言是否选过、规则确认版本、
    // 本角色引导是否已看、诊断日志开关。子类在此基础上补自己的字段。seq 由发送方统一加
    virtual juce::var buildSnapshot();

    // 定时器（25Hz）：先跑看门狗与遮挡闸，再在桥就绪且未切兜底时调用 emitTick
    void timerCallback() override;

    // 子类在定时器里追加自己的变更检测与事件发送。默认空实现
    virtual void emitTick();

    // 向页面推一个事件。页面不可见时不发（由 JUCE 判定）
    void emitEvent(const char* eventId, const juce::var& payload);

    // 推一帧 rinne.state 全量快照，帧序号单调递增
    void emitState();

    // 首帧钩子：消息线程上，handleRequestInitialState 回执并发完首帧 rinne.state（emitState）之后紧接着调用一次，
    // 每次 requestInitialState 一次（页面重载后重来一次就再调一次）。子类在这里补发本角色其余的状态类事件
    // （桥契约时序：收到请求立刻把本角色所有状态类事件各发一帧）。默认空实现，基类自己的首帧顺序与揭幕门不受影响；
    // 兜底面板在场时那次请求只回执，不发首帧，也不调用它
    virtual void emitFirstFrames() {}

    // 兜底面板是否在场。角色专属桥函数开头的守卫写成 if (panelShown())（见文件头注释）
    bool panelShown() const noexcept { return fallback_ != nullptr; }

    // 既有日志通道，加一份平台文件日志。角色专属桥函数的守卫用它记一行
    void logDiag(const juce::String& line) const;

    // 通用桥函数：两个角色共用
    void handleRequestInitialState(const juce::Array<juce::var>& args,
                                   juce::WebBrowserComponent::NativeFunctionCompletion complete);
    virtual void handleSetLang(const juce::Array<juce::var>& args,
                               juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleSetUiScale(const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleCommitUiScale(const juce::Array<juce::var>& args,
                             juce::WebBrowserComponent::NativeFunctionCompletion complete);

private:
    enum class FallbackReason
    {
        MissingRuntime,
        RuntimeTooOld,
        LoadTimeout,
        BootError
    };

    // 最后一次导航的状态。诊断口径：区分「WebView2 根本没动」和「导航起来了但页面或脚本没起来」
    enum class NavState
    {
        notStarted,
        started,
        finished,
        networkError
    };

    // 装 juce::WebBrowserComponent 的薄子类：只把页面回调转给宿主。看门狗要靠导航事件起算，
    // 而这几个回调是 JUCE 唯一暴露导航时序的地方，只能经继承拿到
    class HostWebView;

    juce::WebBrowserComponent::Options makeOptions(); // 装配通用 Options，再交给 augmentOptions
    void showFallback(FallbackReason reason);
    void retryWebView();
    void resizeToDesignBox();

    // 加载时序（消息线程）：由 HostWebView 的页面回调驱动
    void onNavigationStarted(const juce::String& url);
    void onNavigationFinished(const juce::String& url);
    void onNavigationError(const juce::String& errorInfo);
    void handleBootError(const juce::var& payload); // 前端启动失败上报（诊断面，不属于桥契约）
    // 前端「首帧已绘」上报（同上）。载荷只读一个诊断字段，只进日志，不参与任何放行判定（判定全在闸门里）
    void handleFirstFrame(const juce::var& payload);

    // 遮挡闸的两个动作面。判定全在 revealGate_ 里（纯逻辑，可单测），这两个函数只负责把判定落到组件几何上并写诊断行
    void applyRevealGate(); // 把闸门是否挡住落到 WebView 的边界上
    void noteRevealed(); // 放行时写一行诊断（原因加用时）
    // 两层 paint（本组件与 HostWebView）共用的占位。品红探针也只在这里分叉
    void paintPlaceholder(juce::Graphics& g, juce::Rectangle<float> area) const;

    // 「默认背景色这一层在不在」的诊断行（每次加载尝试一条）
    void logBackgroundColourSupport() const;

    // 首页地址 = <资源根>/<角色>/index.html。让服务地址空间与 web/ 的磁盘布局逐段对齐，
    // 从而保证 ES 模块身份唯一（同一文件不会被两个地址各实例化一份）。完整理由见实现
    juce::String entryUrl() const;

    void beginLoadAttempt(); // 起算看门狗（构造与重试共用）
    void releaseReadyBridge(); // 把本实例从「活着的桥」计数里摘掉（重试与析构）
    // 兜底面板的诊断文本。basenameOnly 为真时用户数据目录只给末级目录名（面板上的字会被截图转发），
    // 为假时给完整路径（只进本机日志）
    juce::String buildDiagnostics(bool basenameOnly) const;

    Config config_;
    ResourceProvider provider_;
    bridge::UiScaleModel scale_;
    juce::String lang_;

    std::atomic<bool> bridgeReady_{false};
    juce::uint32 startMs_ = 0;
    juce::uint32 deadlineMs_ = 0; // startMs_ 加冷或热预算；首个导航事件到达时按 kAfterNavBudgetMs 顺延
    bool navBudgetApplied_ = false; // 只顺延一次：页面若反复导航，不允许无限推迟看门狗
    NavState navState_ = NavState::notStarted;
    juce::String navDetail_; // 最后一次导航错误信息（网络错误时非空）
    juce::String bootError_; // 前端上报的启动失败摘要（启动失败分支用）
    PlatformWebView::RuntimeInfo runtime_; // 本次加载尝试开始时的运行时探测结果（诊断用）
    juce::File userDataFolder_; // 两个角色共用的 WebView2 用户数据目录（进程组据此复用）；系统 WebKit 上为空 File
    juce::String userDataFolderIssue_; // 构造期可写性探针的结果（不含路径）；空表示没问题
    bool countedAsReady_ = false; // 本实例是否已计入活着的桥的计数（防重复加减）
    RevealGate revealGate_; // 「WebView 该不该在可视区外」的唯一判定处
    bool revealLogged_ = false; // 本次加载尝试是否已写过放行诊断行（只写第一次）
    juce::uint32 stateSeq_ = 0; // rinne.state 的帧序号

    std::unique_ptr<HostWebView> webView_;
    std::unique_ptr<FallbackPanel> fallback_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebViewHost)
};

} // namespace rinne::webview
