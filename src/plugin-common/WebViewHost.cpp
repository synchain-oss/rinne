// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "WebViewHost.h"

#include "BridgeBase.h"
#include "PlatformLog.h"
#include "PlatformWebView.h"
#include "ResourceProvider.h"
#include "UiDefaultsStore.h"

#include <type_traits>
#include <utility>

// 品红探针。开成 1 后 HostWebView::paint 铺品红而不是占位渐变：真机上一眼就能分出
// 「白闪没了，是因为我们这层 paint 真的在跑」还是「WebView2 这次恰好起得快」。
// 默认关，只经编译定义临时打开；交付构建里它必须是 0
#ifndef RINNE_PROBE_MAGENTA
#define RINNE_PROBE_MAGENTA 0
#endif

namespace rinne::webview
{

using WBC = juce::WebBrowserComponent;

namespace
{
juce::String fallbackTitle(const juce::String& role)
{
    // 两个角色各自成句：用户开不了窗时看到的正是这张面板，标题指错插件会把排查引到另一个插件上去
    return role == "input" ? "Rinne Input" : "Rinne Output";
}

// 提到运行时名字的三条文案（缺失、过旧、环境没起来）按平台写，放在 PlatformWebView 里，这里只留与平台无关的三条。
//
// 超时兜底按「最后一次导航走到哪一步」分三态给文案。这是本面板最重要的信息：
// 同一句「加载太慢」在三种完全不同的故障上都会出现，而这三种的下一步动作互不相同。
//   notStarted  导航事件一次都没来，说明 WebView2 环境或控制器根本没建起来。
//               JUCE 把环境创建与控制器创建的返回码都吞了，所以「没有事件」是我们唯一能拿到的信号，
//               也正因如此它很可靠：环境起来了就必然有导航开始事件。常见成因：用户数据目录不可写或被别的进程占着，
//               宿主策略挡掉了浏览器子进程。
//   started     导航开始但没走完，说明资源供给或渲染进程卡住。
//   finished    页面加载完了桥仍没起来，说明前端脚本没跑到 requestInitialState。
juce::String navStalledMessage()
{
    return "The page started loading but never finished.\n"
           "Click Retry, or close and reopen this plugin window.";
}

juce::String bridgeStalledMessage()
{
    return "The page loaded, but the plugin UI never finished starting up.\n"
           "Click Retry; if it keeps failing, report the diagnostics line below.";
}

juce::String bootErrorMessage()
{
    return "The plugin UI failed to start (front-end script error).\n"
           "Click Retry; if it keeps failing, report the diagnostics line below.";
}

// 当前桥已就绪的编辑器数量（进程内）。热预算的判据只能是「此刻确有一个活着的桥」，不能是「历史上曾经起来过」：
//   - WebView2 的浏览器进程组按用户数据目录共享，只要还有一个实例活着，进程组就在，新开的编辑器只是往里加一个 WebView，
//     较短的预算绰绰有余；
//   - 可一旦所有编辑器都关掉，浏览器进程组会退出，再开就是完整冷启动。此时若因「曾经起来过」而只给短预算，
//     等于亲手造出「关窗再开必超时」这个新故障。
// 所以用计数而不是布尔：大于 0 才算热。
std::atomic<int>& readyBridgeCount()
{
    static std::atomic<int> count{0};
    return count;
}

// 新窗口该参照的显示器可用区：鼠标所在的那块（用户多半在那里点开的插件窗口），取不到退到主显示器；
// 都取不到给空矩形，调用方当作不夹取
juce::Rectangle<int> openAreaForNewWindow()
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForPoint(juce::Desktop::getMousePosition());
    if (display == nullptr)
        display = displays.getPrimaryDisplay();
    return display != nullptr ? display->userArea : juce::Rectangle<int>();
}

juce::String initialLang()
{
    const auto saved = uidefaults::langGlobal();
    return saved.isNotEmpty() ? saved : juce::String("zh");
}
} // namespace

// -----------------------------------------------------------------------------
// HostWebView：只为拿到 JUCE 的页面回调而存在的薄子类。
// 看门狗要「收到导航事件才起算」，而 pageAboutToLoad、pageFinishedLoading、pageLoadHadNetworkError
// 是 JUCE 唯一暴露导航时序的接口，只能经继承取得。
// -----------------------------------------------------------------------------
class WebViewHost::HostWebView final : public juce::WebBrowserComponent
{
public:
    HostWebView(WebViewHost& owner, Options options) : juce::WebBrowserComponent(std::move(options)), owner_(owner) {}

    // 开窗白闪的真正那一层。
    //
    // 本组件在构造里声明不透明，又铺满父组件；JUCE 画父组件前会把不透明子组件的矩形从裁剪区里剔掉，
    // 所以宿主组件的 paint 在这一段净效果为 0。控制器建好之前真正在画的是 JUCE 自己：
    // 基类的 paint 转给后端的兜底绘制，WebView2 后端那份每帧无条件把整块填成白色，白的来源就是它。
    //
    // 修法：先调基类，再把自己的占位渐变盖上去。两次填充落在同一个 Graphics、同一次 paint 里，先白后占位，
    // 中间不上屏（两条渲染路都是整帧画完才出），上屏的只有后盖的那一层。
    //
    // 为什么不是「覆写且不调基类」：基类的兜底绘制尾部还有一句「控制器没建起来就继续检查窗口关联」，
    // 那是 WebView2 控制器的重试泵。构造期就已经导航，那一刻窗口还没有原生句柄，
    // 控制器能不能建起来，靠此后一次次重入这句检查。绕过基类等于把泵拆了。
    // 在本层复刻一份泵也不行：复刻件押在 JUCE 的私有实现细节上，版本一升就可能静默变成空调用，
    // 而编译期断言守不到「那份复刻还在」。调基类之后没有可删的复刻件，泵连同它的条件一字未动留在 JUCE 里。
    // 不要把下面的断言读成「守住覆写就守住了全部」：函数体里的基类调用删掉照样编得过、断言照样绿，
    // 它只把必守面从两处减到一处，且那一处是显式的基类调用，签名一变就编译红。
    //
    // 开窗那几帧的分层，只写在这里一份：
    //   灰或白（第一段）  宿主自己的插件窗容器，在我们的窗口上屏之前就在那里，插件侧无从覆盖，只有真机能确认；
    //   控制器建起来之前  就是上面讲的那一层，由本覆写整块盖住；
    //   控制器建好、文档还没提交  WebView2 的默认背景色，由 PlatformWebView 的选项铺上。这一层可能整层不在：
    //       JUCE 取不到第二版控制器接口时会静默跳过，所以每次加载尝试会打一行诊断，说明运行时有没有这个接口；
    //   文档已提交、外链样式还没到  页面 head 里内联了一条根元素背景声明，排在外链样式之前。
    //       「排在前面」是排序事实，不是时序保证：Chromium 对 head 里的外链样式是渲染阻塞的，
    //       外链就绪之前文档整体不进正常绘制路径。所以这条内联声明确定兜住的只有一种情形：外链取不到或加载失败，
    //       阻塞随之解除，画出来的是内联背景而不是白；
    //   WebView2 自己的宿主窗口在首帧合成之前由运行时画，JUCE 不暴露它。这一段是否还看得见白，只有真机能判。
    // 光换颜色盖不住最后一段，所以主修法是不让 WebView 上屏：导航开始到页面首帧已绘之间，
    // 把它挪出可视区，那块地方由宿主组件的 paint 铺占位渐变。判定见 WebViewRevealGate.h。
    void paint(juce::Graphics& g) override
    {
        // 基类的兜底绘制：白底加控制器重试泵。这一句必须在前，它画的白由下面整块盖掉，而泵要的是每帧都被调到
        juce::WebBrowserComponent::paint(g);

        // 盖上去的是占位渐变：与成品外壳同一组色标、同一角度的线性渐变，开窗那几帧「占位到内容」不再有明暗跳变。
        // 坐标系是本组件的本地边界，与外壳铺满设计盒时的几何同形
#if RINNE_PROBE_MAGENTA
        g.fillAll(juce::Colours::magenta);
#else
        const auto area = getLocalBounds().toFloat();
        g.setFillType(juce::FillType(shellBackdropGradient(area)));
        g.fillRect(area);
#endif
    }

    // 导航闸：页面带着原生桥（会写磁盘的 setLang、setUiScale、commitUiScale），一旦被带到外站，外站的脚本就够得到桥。
    // 所以只放行资源提供者的根下面的地址，加上页面隐藏时 JUCE 借道的空白页，其余拦下并写一行日志。
    // 两个平台的回调都走这里：WebView2 的导航开始事件（主框架），WKWebView 的导航策略回调（含子框架）。
    // 判定是纯函数（ResourceProvider::isRootNavigation），放不放行的全部用例在宿主测试里；
    // 拦下的导航不算「导航开始」，不起算看门狗的导航后预算。
    // 这里要写全名：在 WebBrowserComponent 的派生类里，裸写 ResourceProvider 指的是基类里的同名函数类型
    bool pageAboutToLoad(const juce::String& url) override
    {
        if (!rinne::webview::ResourceProvider::isRootNavigation(url, WBC::getResourceProviderRoot()))
        {
            owner_.logDiag("blocked navigation: " + rinne::webview::ResourceProvider::urlForLog(url));
            return false;
        }
        owner_.onNavigationStarted(url);
        return true;
    }

    void pageFinishedLoading(const juce::String& url) override { owner_.onNavigationFinished(url); }

    // 页面里的新窗口请求（window.open、target=_blank）走这里。JUCE 的 WebView2 后端把新窗口请求接到本虚函数，
    // 并已标记为已处理，不重写它外链就是死的。
    // 本卡一律不放行：外链要经桥交给原生按白名单解析后用系统浏览器打开，白名单随那个桥函数落地。
    // 在白名单存在之前放行任意 http 地址，等于把「把一个字符串交给系统去执行」的权力交给页面
    void newWindowAttemptingToLoad(const juce::String& newURL) override
    {
        owner_.logDiag("blocked new-window request: " + rinne::webview::ResourceProvider::urlForLog(newURL.trim()));
    }

    bool pageLoadHadNetworkError(const juce::String& errorInfo) override
    {
        owner_.onNavigationError(errorInfo);
        return false; // 不显示 WebView2 内建错误页：兜底面板给的是可操作信息，内建页不是
    }

    // paint 的覆写一旦被删掉，成员指针会经名字查找落到基类，类型变成基类的成员指针，本断言当场编译红。
    // 宿主类只在插件目标里编，不进任何测试目标，运行期用例够不着这一层，编译期断言是唯一守得住的判据。
    // 它守得住的范围只有「paint 由本类覆写」这一件事，函数体里的基类调用它守不到（见上）
    static_assert(std::is_same_v<decltype(&HostWebView::paint), void (HostWebView::*)(juce::Graphics&)>,
                  "HostWebView::paint must be overridden here: without it the WebView2 backend fallback paint "
                  "fills white on every frame and the opening white flash comes back.");

private:
    WebViewHost& owner_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostWebView)
};

WebViewHost::WebViewHost(juce::AudioProcessor& processor, Config config)
    : juce::AudioProcessorEditor(&processor), config_(std::move(config)), provider_(config_.resourceSource),
      scale_(config_.role, bridge::scaleFromStoredPercent(config_.role, uidefaults::uiScalePercent(config_.role))),
      lang_(initialLang())
{
    // 声明本组件完全不透明：JUCE 因此不会去画它下面的东西（宿主给的编辑器容器）。
    // 这一句挡不住开窗白闪，白闪那一段本组件的 paint 根本不执行（见 HostWebView::paint），它管的是兜底面板路径下这块底
    setOpaque(true);

    // 开窗按显示器可用区夹取档位：只影响本次开窗，已保存的值不动，也不产生待确认的预览
    const auto area = openAreaForNewWindow();
    scale_.fitToDisplay(area.getWidth(), area.getHeight());

    // 用户数据目录必须在装 Options 之前定好，并当场探一次可写性：WebView2 自己碰这个目录时的失败会被 JUCE 吞掉，
    // 等到那时候就只剩「超时」两个字了。系统 WebKit 不用这个目录：makeUserDataFolder 返回空 File，探针对空 File
    // 直接返回
    userDataFolder_ = PlatformWebView::makeUserDataFolder();
    userDataFolderIssue_ = PlatformWebView::probeUserDataFolder(userDataFolder_);
    if (userDataFolderIssue_.isNotEmpty())
        logDiag("user-data folder problem: " + userDataFolderIssue_ + " (" +
                PlatformWebView::userDataFolderDisplay(userDataFolder_, false) + ")");

    webView_ = std::make_unique<HostWebView>(*this, makeOptions());
    addAndMakeVisible(*webView_);

    setResizable(false, false); // 只经缩放档位编程改尺寸，不开自由拖角
    resizeToDesignBox();

    beginLoadAttempt();
}

// 起算一次加载尝试（构造与重试共用）：先探运行时三态，再决定加载还是直接兜底。
// 「没装」「太旧」两条都不进看门狗：用户要做的是装或升级运行时，干等一个预算窗口毫无意义
void WebViewHost::beginLoadAttempt()
{
    releaseReadyBridge(); // 重试时本实例的旧桥不再算数（计数只统计活着的桥）
    bridgeReady_ = false;
    navState_ = NavState::notStarted;
    navDetail_ = {};
    bootError_ = {};
    navBudgetApplied_ = false;
    revealGate_.beginLoadAttempt(); // 重新武装：下一次导航开始时再挪一次
    revealLogged_ = false;
    startMs_ = juce::Time::getMillisecondCounter(); // 先落起点：兜底面板的「已等待」也从这里算
    runtime_ = PlatformWebView::runtimeInfo();

    if (runtime_.status == PlatformWebView::RuntimeStatus::missing)
    {
        webView_->setVisible(false);
        showFallback(FallbackReason::MissingRuntime);
        return;
    }
    if (runtime_.status == PlatformWebView::RuntimeStatus::tooOld)
    {
        webView_->setVisible(false);
        showFallback(FallbackReason::RuntimeTooOld);
        return;
    }

    // 冷热预算见头文件的三个常量。导航事件到达后再按导航后预算顺延。
    // 热的判据是此刻另有一个桥活着（共享的浏览器进程组必然在跑），不是「曾经起来过」
    const bool warm = readyBridgeCount().load() > 0;
    deadlineMs_ = startMs_ + static_cast<juce::uint32>(warm ? kWarmLoadBudgetMs : kColdLoadBudgetMs);
    // 用户数据目录这一段只进本机日志，所以给完整路径
    logDiag(juce::String(warm ? "warm" : "cold") + " start, budget " +
            juce::String(warm ? kWarmLoadBudgetMs : kColdLoadBudgetMs) + " ms, udf " +
            PlatformWebView::userDataFolderDisplay(userDataFolder_, false));

    logBackgroundColourSupport();

    // 必须在任何事件发送之前完成首个跳转（前端脚本随后加载并注册监听）
    webView_->setVisible(true);
    webView_->goToURL(entryUrl());
    if (!isTimerRunning())
        startTimerHz(25);
}

// 首页地址是 <资源根>/<角色>/index.html，不是裸的资源根。
//
// 这不是风格问题，它决定 ES 模块的身份。浏览器按地址认模块：同一个文件被两个不同地址取到，就会被实例化两次，
// 两份各持一套模块级状态。而资源提供者按文件名扁平反查，同一个文件在多个地址下都取得到，
// 于是「服务路径」与「磁盘路径」一旦不一致，就会凭空多出模块副本。
// 从根目录进入时正是不一致的：页面在根上，它的相对引用落在根下；而共享目录里的脚本按磁盘写的上溯引用落在另一条路径下，
// 同一个文件，两个地址，两份状态。
// 改从 /<角色>/index.html 进入后，服务地址空间与 web/ 的磁盘布局逐段对齐：
// 每个相对引用解析出的路径都与浏览器预览（按真实目录服务）完全相同，模块身份天然唯一。
// 这也让「预览里能跑就是插件里能跑」重新成立。
juce::String WebViewHost::entryUrl() const
{
    return WBC::getResourceProviderRoot() + config_.role + "/index.html";
}

WebViewHost::~WebViewHost()
{
    stopTimer();
    releaseReadyBridge(); // 编辑器关闭后，本实例不再为「热启动」判定背书
}

// 计数只统计活着的桥：重试与析构都要还回去，否则关掉全部编辑器后计数仍大于 0，
// 下一次开窗会按热预算跑一次真冷启动，那正是「关窗再开必超时」
void WebViewHost::releaseReadyBridge()
{
    if (countedAsReady_)
    {
        countedAsReady_ = false;
        readyBridgeCount().fetch_sub(1);
    }
}

juce::WebBrowserComponent& WebViewHost::webView()
{
    return *webView_;
}

void WebViewHost::paint(juce::Graphics& g)
{
    // 见头文件注：WebView 落在本组件里时本函数净效果为 0，控制器建好之前那一段由 HostWebView::paint 接住。
    // 本函数真正会画的有两条路：兜底面板路径（那时 WebView 被隐藏），以及遮挡期（WebView 被挪到可视区之外，
    // 见 WebViewRevealGate.h）。后者整个窗口露的就是这一层，它就是用户在内容出来之前看到的那块占位底，
    // 画的是与成品外壳同一组色标的占位渐变，所以从占位切到内容不跳阶、明暗走向也一致
    const auto area = getLocalBounds().toFloat();
    g.setFillType(juce::FillType(shellBackdropGradient(area)));
    g.fillRect(area);
}

void WebViewHost::resized()
{
    // WebView 的落点由遮挡闸决定：遮挡期间整块挪到可视区之外（尺寸不变），那块地方由上面的 paint 铺占位渐变。
    // 几何与理由见 WebViewRevealGate.h。系统 WebKit 上闸门从不武装，恒走原位分支
    if (webView_ != nullptr)
        webView_->setBounds(revealGate_.parked() ? parkedBounds(getLocalBounds()) : getLocalBounds());
    if (fallback_ != nullptr)
        fallback_->setBounds(getLocalBounds());
}

// 把闸门的判定落到组件几何上。只经这一个函数改 WebView 的边界，别在各个触发点各写一次，那样闸门就有了第二个真源
void WebViewHost::applyRevealGate()
{
    if (webView_ == nullptr)
        return;
    resized();
    repaint(); // 挪走的那一刻要让占位底色立刻补上，别等下一次自然重绘
}

// 「默认背景色这一层在不在」的诊断行。行的拼装与措辞理由在 PlatformWebView 里。
// 这一层是 WebView2 专有的：系统 WebKit 上那个函数返回空串，这里就不打
void WebViewHost::logBackgroundColourSupport() const
{
    const auto line = PlatformWebView::backgroundColourDiagnostics(runtime_);
    if (line.isNotEmpty())
        logDiag(line);
}

// 放行诊断行。读表的人要知道的三件事：
//   - 放行原因有三种：firstFrame 是正常路；timeout 是首帧信号没来、到点兜底；fallback 是遮挡期内被兜底面板顶掉。
//     fallback 不是放行，是被面板顶掉，数表时把它单独归一类。它实际只由前端启动失败那一条路打得出来
//     （看门狗的预算恒大于闸门的超时，闸门早已自己记了 timeout；运行时缺失时闸门从未挡住），
//     所以表里没有 fallback 是正常的，别据此去查接线；
//   - timeout 必须当异常读：正常开窗一次都不该出现它。所以它带后缀，说明加载完成回调到过没有：
//     到过说明页面加载完了但前端没发信号（查前端启动与动画帧那一段），
//     没到过说明导航压根没走完（查环境与网络那一段）。
//     措辞是「到点之前没来」而不是「从没来过」：信号落在超时那个 tick 已经放行、下一个 tick 之前的窗口里时，
//     它随后仍会到，并自己打一行「已放行」。把这两行对齐读，就能把这一档与真正的信号缺席分开；
//   - 用时一律从本次加载尝试的起点算，而闸门的超时从导航开始算，所以 timeout 那条打出来是「超时加导航前耗时」。
//     两个起点不同是有意的：这一行是给人看「从点开窗口算起等了多久」
void WebViewHost::noteRevealed()
{
    // 放行原因为空说明这一轮从来没挪走过（例如运行时缺失直接切了兜底面板），那就没有「放行」这回事
    if (revealLogged_ || revealGate_.parked() || juce::String(revealGate_.lastRevealReason()).isEmpty())
        return;
    revealLogged_ = true;
    const juce::String reason(revealGate_.lastRevealReason());
    juce::String line = "webview revealed (" + reason + ") after " +
                        juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) + " ms";
    if (reason == "timeout")
        line << " -- no first-frame signal before the 3s deadline (navFinished "
             << (revealGate_.navigationFinishedSeen() ? "seen" : "not seen") << ")";
    logDiag(line);
}

// -----------------------------------------------------------------------------
// 缩放：固定设计盒加页面缩放，窗口大小同步
// -----------------------------------------------------------------------------
void WebViewHost::resizeToDesignBox()
{
    const auto r = bridge::designBoxWindowSize(config_.role, scale_.scale());
    setSize(r.width, r.height);
}

void WebViewHost::setScaleLocked(bool locked)
{
    scale_.setLocked(locked);
    // 兜底面板在场时窗口已被放大到不小于设计盒，不要在这里缩回去；重试回到网页时会按当前档位恢复
    if (fallback_ == nullptr)
        resizeToDesignBox();
    if (bridgeReady_)
        emitState();
}

void WebViewHost::persistUiScale(float scale)
{
    uidefaults::setUiScalePercent(config_.role, bridge::scaleToPercent(scale));
}

juce::var WebViewHost::buildSnapshot()
{
    bridge::UiSeed seed;
    seed.role = config_.role;
    seed.version = config_.version;
    seed.lang = lang_;
    seed.uiScale = scale_.scale();

    // 四个标志一次读回：快照每秒至少一帧，逐个读会在消息线程上每帧开关四次设置文件
    return bridge::buildUiSnapshot(seed, uidefaults::readUiFlags(config_.role));
}

void WebViewHost::emitEvent(const char* eventId, const juce::var& payload)
{
    if (webView_ != nullptr)
        webView_->emitEventIfBrowserIsVisible(juce::Identifier(eventId), payload);
}

void WebViewHost::emitState()
{
    auto snapshot = buildSnapshot();
    if (auto* obj = snapshot.getDynamicObject())
        obj->setProperty("seq", static_cast<int>(++stateSeq_));
    emitEvent(bridge::Event::State, snapshot);
}

// -----------------------------------------------------------------------------
// 兜底面板
// -----------------------------------------------------------------------------
juce::String WebViewHost::buildDiagnostics(bool basenameOnly) const
{
    const char* nav = "notStarted";
    switch (navState_)
    {
    case NavState::started:
        nav = "started";
        break;
    case NavState::finished:
        nav = "finished";
        break;
    case NavState::networkError:
        nav = "networkError";
        break;
    case NavState::notStarted:
    default:
        break;
    }

    juce::String d;
    d << "waited " << juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) << " ms"
      << "  |  nav " << nav;
    if (navDetail_.isNotEmpty())
        d << " (" << navDetail_ << ")";
    d << "  |  " << PlatformWebView::runtimeDiagnosticsField(runtime_);
    d << "  |  host " << juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName() << " pid "
      << juce::String(PlatformWebView::processId());
    // 用户数据目录是「环境没起来」这一路的头号嫌疑，必须显示。面板上的字会被用户截图转发，完整路径带用户名，
    // 所以面板只给末级目录名，完整路径只进本机日志
    d << "\n" << "udf " << PlatformWebView::userDataFolderDisplay(userDataFolder_, basenameOnly);
    if (userDataFolderIssue_.isNotEmpty())
        d << "  [" << userDataFolderIssue_ << "]";
    if (bootError_.isNotEmpty())
        d << "\n" << bootError_;
    return d;
}

void WebViewHost::logDiag(const juce::String& line) const
{
    // 既有日志通道 juce::Logger。调试输出在发行构建里被编掉，而兜底面板恰恰只在用户的发行包上出现；
    // 写日志在没有日志器时也会落系统调试输出，宿主设了日志器则进宿主日志。诊断不能只活在调试构建里
    const juce::String message = "Rinne " + config_.role + ": " + line;
    juce::Logger::writeToLog(message);
    // 同一行再交给平台文件日志（macOS 上调试输出落在标准错误，宿主下用户基本看不到）。
    // 只有装上了文件日志才会真的落盘，没装时什么都不做。本函数只在消息线程调用，与文件日志的约定一致
    platformlog::write(message);
}

void WebViewHost::showFallback(FallbackReason reason)
{
    if (fallback_ != nullptr)
        return;
    // 面板自己铺满本组件，闸门不该再按住 WebView 的位置（否则重试回来时边界还停在可视区外，
    // 而那条路上不一定再有导航事件把它推回来）。
    // 放行诊断要在这里调：拿掉加载完成放行之后，唯一还会调它的地方只剩首帧信号的处理，
    // 而「走到兜底面板」的典型场景恰恰是首帧信号根本不会来，放行行会从此消失。这里本来也是更自然的落点：面板一切，
    // 就该当场记下闸门是被谁顶掉的。
    // 但它真正打得出来的只有一条路：运行时缺失或过旧发生在加载尝试开头，闸门从未挡住，不打；
    // 加载超时的预算恒大于闸门超时，闸门早已自己放行并写过行，不打；前端启动失败可以在闸门超时之内到，只有这一条打得出
    revealGate_.onFallbackShown();
    noteRevealed();
    webView_->setVisible(false);

    const bool missing = (reason == FallbackReason::MissingRuntime);
    const bool tooOld = (reason == FallbackReason::RuntimeTooOld);

    juce::String message;
    const char* tag = "loadTimeout";
    if (missing)
    {
        message = PlatformWebView::missingRuntimeMessage();
        tag = "missingRuntime";
    }
    else if (tooOld)
    {
        message = PlatformWebView::tooOldRuntimeMessage();
        tag = "runtimeTooOld";
    }
    else if (reason == FallbackReason::BootError)
    {
        message = bootErrorMessage();
        tag = "bootError";
    }
    else if (navState_ == NavState::notStarted)
    {
        // 导航一次都没开始，说明环境或控制器没建起来
        message = PlatformWebView::envNotStartedMessage();
        tag = "envNotStarted";
    }
    else if (navState_ == NavState::finished)
    {
        message = bridgeStalledMessage();
        tag = "bridgeStalled";
    }
    else
    {
        message = navStalledMessage();
        tag = "navStalled";
    }

    // 本机日志给完整路径，面板上的字只给末级目录名
    logDiag(juce::String("fallback ") + tag + " - " + buildDiagnostics(false).replaceCharacter('\n', ' '));

    FallbackPanel::Options options;
    options.title = fallbackTitle(config_.role);
    options.message = message;
    options.details = buildDiagnostics(true);
    // 两条的用户动作都是装或升级运行时。按钮文案与链接都是 WebView2 的，系统 WebKit 没有可单独下载的运行时，
    // 所以只在提供下载的平台上给
    options.showInstall = (missing || tooOld) && PlatformWebView::offersRuntimeDownload();
    // 这个按钮是面板自己的原生按钮，直接用系统浏览器打开微软的下载页；它不经桥，也不经页面里的外链通道
    options.onInstall = [] { juce::URL(PlatformWebView::runtimeDownloadUrl()).launchInDefaultBrowser(); };
    // 延后到消息线程执行：面板的重试回调里同步销毁面板，会销毁正在执行回调的按钮。
    // 安全指针兜底宿主先于回调被销毁的情况
    options.onRetry = [safeThis = juce::Component::SafePointer<WebViewHost>(this)] {
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->retryWebView();
        });
    };
    options.controls = config_.controls;

    fallback_ = std::make_unique<FallbackPanel>(std::move(options));
    addAndMakeVisible(*fallback_);

    // 兜底面板是固定像素布局，不随缩放档位变化。小档位下窗口过小会挤压或裁掉按钮，
    // 所以切兜底时放大到至少设计盒尺寸；更大的窗口保持不变。重试回到网页时按档位恢复
    const auto design = bridge::designBoxWindowSize(config_.role, 1.0f);
    if (getWidth() < design.width || getHeight() < design.height)
        setSize(juce::jmax(getWidth(), design.width), juce::jmax(getHeight(), design.height));
    resized();
}

void WebViewHost::retryWebView()
{
    fallback_.reset();
    resizeToDesignBox();
    beginLoadAttempt(); // 重探运行时三态并重置看门狗；若又是缺失或过旧会就地再切兜底
    resized();
}

// -----------------------------------------------------------------------------
// 加载时序回调（消息线程，来自 HostWebView）
// -----------------------------------------------------------------------------
void WebViewHost::onNavigationStarted(const juce::String& url)
{
    navState_ = NavState::started;
    navDetail_ = {};

    // 「收到首个导航事件才重新起算」：导航开始证明 WebView2 环境已就绪，页面加载该有自己的完整额度。
    // 只顺延一次，且只许延后不许提前，否则冷启动刚用掉的时间会被抹掉，或者一个反复导航的页面能把看门狗无限推后
    if (!navBudgetApplied_)
    {
        navBudgetApplied_ = true;
        const auto extended = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(kAfterNavBudgetMs);
        if (static_cast<juce::int32>(extended - deadlineMs_) > 0) // 回绕安全的「更晚吗」
            deadlineMs_ = extended;
    }

    // 导航开始说明控制器已建好，挂在 paint 上的重试泵此刻已是空调用，才可以把 WebView 挪出可视区
    // （挪走之后 JUCE 就不再画它，泵也就不再被驱动）。
    // 挪窗只在 WebView2 上启用：系统 WebKit 上不武装闸门，边界恒走原位，放行段与放行诊断都是空调用。
    // 不这样做的话，WKWebView 上首帧信号一旦没到（挪出可视区后动画帧是否照跑没验证过），每次开窗都要白等兜底时长。
    // 闸门的其余记账调用照常，不改状态
    if (PlatformWebView::revealGateEnabled())
    {
        revealGate_.onNavigationStarted(juce::Time::getMillisecondCounter());
        applyRevealGate();
    }

    logDiag("navigation started: " + url);
}

void WebViewHost::onNavigationFinished(const juce::String& url)
{
    // 页面已下载完不等于桥已就绪：前端还要跑模块图并调 requestInitialState。看门狗继续跑，但状态记下来：
    // 「加载完成却超时」精确指向前端启动失败，与「压根没导航」判然不同。
    // 这一条不放行，只记账（定谳见 WebViewRevealGate.h）。记下的这一位只进放行诊断里 timeout 的那一行
    navState_ = NavState::finished;
    revealGate_.onNavigationFinished();
    logDiag("navigation finished: " + url);
}

void WebViewHost::onNavigationError(const juce::String& errorInfo)
{
    navState_ = NavState::networkError;
    navDetail_ = errorInfo.substring(0, 200);
    logDiag("navigation error: " + navDetail_);

    // 这里刻意不切兜底面板，由看门狗统一裁决。
    //
    // 原因是「重试自锁」：点重试会先销毁面板再重新跳转，而这个跳转会中止上一次仍在飞的导航；
    // 被中止的那次随后异步回调过来报错，此时面板恰好不在、桥也还没起来，于是上一次的错误把刚开始的这一次就地判死，
    // 面板瞬间又贴回来，用户看到的就是「点重试没反应」。
    //
    // 不用「按错误码放行」来修：JUCE 只帮忙过滤了操作取消，连接中止之类的错误码照样漏下来。
    // 与其维护一张「哪些码不算数」的名单，不如根本不让任何单次导航错误拥有直接判死的权力：
    // 真失败会在预算耗尽时照常进兜底面板，诊断行里带着这里记下的错误；而中止与瞬时错误则被后续的导航事件自然覆盖掉。
    // 代价只是「必然失败的情形要多等一个预算窗口」，换来的是重试真的能用
}

// 前端启动失败上报。
//
// 为什么不是桥函数：桥函数全部要求桥已就绪，而这里要报的恰恰是「桥还没起来」。
// 所以走 JUCE 内建的上行通道（页面的宿主对象的 postMessage 对应选项里的事件监听）。
// 该通道由 JUCE 在文档创建前注入，不经桥脚本，不占用桥契约的名表，名字加 __rinne__ 前缀标明它是诊断面
void WebViewHost::handleBootError(const juce::var& payload)
{
    const auto stage = payload.getProperty("stage", juce::var("unknown")).toString();
    const auto detail = payload.getProperty("detail", juce::var("")).toString();

    bootError_ = "boot " + stage + ": " + detail.substring(0, 300);
    logDiag("front-end " + bootError_);

    if (bridgeReady_ || fallback_ != nullptr)
        return; // 桥已起来后的运行期错误归界面自己处理，不砸掉一个能用的界面
    showFallback(FallbackReason::BootError);
}

// 前端「首帧已绘」上报。通道与启动失败上报是同一条，理由见上。
// 载荷只读一个诊断字段：页面那一侧量到的「信号时刻减首次绘制时刻」，只进日志，不参与任何放行判定，判定全在闸门里。
//
// 为什么要它：用户机上的这个余量从来没有被量过，只能从前后对比反推。有了这一格，用户下次抓一份日志就能直接读出来。
// 为什么送差值而不是绝对时刻：页面的性能时间轴与这里的起算点不共享原点，绝对值无法与任何东西相减。
//
// 「没有绘制记录」按字面读，别读成「走了兜底路」：它只说明这一次的载荷里没有这个字段。
// 页面在没有绘制记录时不带它，所以「没有记录就打这一行」成立；反过来不成立：
// 保险定时器的回调可以带着一个真实的差值发出。判「走的是不是保险路」要看 after N ms 的量级，不是看这个字段在不在。
// 字段名的真源是头文件里的常量，页面逐字引用，由嵌入资源回归逐字对拍
void WebViewHost::handleFirstFrame(const juce::var& payload)
{
    // 先记「信号到了」，再谈放行：这是两件必须分开数的事。信号可能在超时兜底或兜底面板之后才姗姗来迟，
    // 那时放行诊断一个字都不写；只看放行原因就会把「信号来晚了」误读成「信号没来」，
    // 而后者正是闸门唯一那条静默降级。真机验收数的就是这一行与放行行的条数比。
    // 闸门没启用的平台（系统 WebKit）上挡住状态恒为假，写「已放行」会让人以为闸门放过一次行，那里写 reveal gate off
    logDiag(juce::String("first-frame signal after ") +
            juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) + " ms" +
            (revealGate_.parked()                   ? " (still parked)"
             : PlatformWebView::revealGateEnabled() ? " (already revealed)"
                                                    : " (reveal gate off)") +
            bridge::paintDeltaNote(payload, kPaintDeltaField));
    // 武装只记账不放行，真正挪回可视区在后面的定时 tick 上。所以下面两句在正常那条路上是空调用，
    // 留着是为了「闸门状态一变就落地」只有 applyRevealGate 一个出口。
    // 传当前毫秒数，是因为毫秒下界要从信号到达那一刻起算，不是从下一个 tick 起算
    revealGate_.onFirstFrame(juce::Time::getMillisecondCounter());
    applyRevealGate();
    noteRevealed();
}

// -----------------------------------------------------------------------------
// WebView 装配
// -----------------------------------------------------------------------------
juce::WebBrowserComponent::Options WebViewHost::makeOptions()
{
    auto options = PlatformWebView::makeWebViewOptions(WBC::Options{}, userDataFolder_);

    options = options.withNativeIntegrationEnabled().withResourceProvider(
        [this](const juce::String& url) { return provider_.provide(url); },
        juce::URL(WBC::getResourceProviderRoot()).getOrigin());

    // 首帧同步预置：角色、版本、语言、缩放。键值对与快照同源
    bridge::UiSeed seed;
    seed.role = config_.role;
    seed.version = config_.version;
    seed.lang = lang_;
    seed.uiScale = scale_.scale();
    for (const auto& seedPair : bridge::buildUiSeedPairs(seed))
        options = options.withInitialisationData(seedPair.first, seedPair.second);

    // 页面调原生：通用的几个；角色专属的由 augmentOptions 追加
    options = options
                  .withNativeFunction(juce::Identifier(bridge::Fn::RequestInitialState),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleRequestInitialState(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::SetLang),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetLang(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::SetUiScale),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetUiScale(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::CommitUiScale),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleCommitUiScale(a, std::move(c));
                                      });

    // 诊断面（不属于桥契约）：前端启动失败上行
    options = options.withEventListener(juce::Identifier(kBootErrorEventId),
                                        [this](const juce::var& payload) { handleBootError(payload); });

    // 时序面（不属于桥契约）：前端「首帧已绘」上行，遮挡闸的正常放行路
    options = options.withEventListener(juce::Identifier(kFirstFrameEventId),
                                        [this](const juce::var& payload) { handleFirstFrame(payload); });

    if (config_.augmentOptions)
        config_.augmentOptions(options);

    return options;
}

// -----------------------------------------------------------------------------
// 桥函数处理（消息线程）
// -----------------------------------------------------------------------------
void WebViewHost::handleRequestInitialState(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    bridgeReady_ = true; // 前端确认就绪，此后定时器才允许发送事件
    // 本实例的桥活着，共享的 WebView2 进程组必在跑，其它编辑器可按热预算起看门狗。
    // requestInitialState 可能被同一页面多次调用（页面重载后要重来一次），故用标志防重复计数
    if (!countedAsReady_)
    {
        countedAsReady_ = true;
        readyBridgeCount().fetch_add(1);
    }
    logDiag("bridge ready after " + juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) +
            " ms");
    complete(bridge::okResponse());
    emitState(); // 首帧必发
}

void WebViewHost::handleSetLang(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    bool accepted = false;
    complete(bridge::respondSetLang(args, [this, &accepted](const juce::String& code) {
        lang_ = code;
        uidefaults::setLangGlobal(code);
        uidefaults::setLangChosenGlobal(true); // 用户显式选过语言，新工程不再重复问
        accepted = true;
    }));
    if (accepted)
        emitState();
}

void WebViewHost::handleSetUiScale(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    bool windowChanged = false;
    auto response = bridge::respondSetUiScale(scale_, config_.role, args, windowChanged);
    if (windowChanged)
        resizeToDesignBox(); // 只实时预览，不落盘
    complete(std::move(response));
    if (windowChanged)
        emitState();
}

void WebViewHost::handleCommitUiScale(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    // 确认保留后才落盘；没有待确认的预览，或处于锁定时，回执里带原因，不落盘
    complete(bridge::respondCommitUiScale(scale_, [this](float scale) { persistUiScale(scale); }));
}

// -----------------------------------------------------------------------------
// 25Hz 定时器（消息线程）：看门狗、就绪门控、变更检测与发送
// -----------------------------------------------------------------------------
void WebViewHost::timerCallback()
{
    // 遮挡闸在这个 tick 上做两件事，都收在 onTick 里：
    //   - 首帧信号已到时结算那一拍：信号表示帧已提交，提交到上屏还差一拍，所以放行落在这里而不是信号的处理函数里；
    //   - 信号没来时到点强制放行，绝不允许出现「永远挪在外面」，那会是一块彻底不动的占位板，比白闪坏得多。
    // 放行现在只可能发生在这个 tick 上，这个 if 是唯一会把闸门从挡住翻过来的地方（兜底面板那条除外），
    // 别把它读成「只是超时兜底」。
    // 下面第二个 if 是看门狗，方向相反：那里到点是换成兜底面板，这里到点是把界面放出来。两段不要读串
    if (revealGate_.parked())
    {
        revealGate_.onTick(juce::Time::getMillisecondCounter());
        if (!revealGate_.parked())
        {
            applyRevealGate();
            noteRevealed();
        }
    }

    if (fallback_ != nullptr)
        fallback_->refreshControls();

    // 看门狗：预算由 beginLoadAttempt 定（冷或热），首个导航事件到达后按导航后预算顺延。超时判定加载失败，切兜底面板
    // （可重试，也可重开窗口），文案不误报「运行时缺失」。
    // 比较走无符号差值再转有符号：毫秒计数每约 49 天回绕一次，直接比大小会在回绕点把「还没到点」算成「早就超时」，
    // 把好好的窗口砸成兜底面板
    if (!bridgeReady_ && fallback_ == nullptr &&
        static_cast<juce::int32>(juce::Time::getMillisecondCounter() - deadlineMs_) > 0)
    {
        showFallback(FallbackReason::LoadTimeout);
        return;
    }
    if (!bridgeReady_)
        return;

    emitTick();
}

void WebViewHost::emitTick()
{
    // 默认空实现；各角色在此追加自己各类事件的变更检测与发送
}

} // namespace rinne::webview
