// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "PlatformWebView.h"

#if JUCE_WINDOWS
// 前置声明，免引 <windows.h>：这个翻译单元还要能离线单测，不该把整个 Win32 头拖进来
extern "C" unsigned long __stdcall GetCurrentProcessId();
#else
#include <unistd.h> // getpid
#endif

namespace rinne::webview
{

namespace
{
// 本进程 PID，只进诊断行。用户在诊断行里看到的数字能直接和任务管理器或活动监视器对上，
// 排查「谁占着 WebView2」时有用。进程外 AU 时插件跑在系统托管进程里，PID 就是区分进程内外的那一项
int currentProcessId()
{
#if JUCE_WINDOWS
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}
} // namespace

int PlatformWebView::processId()
{
    return currentProcessId();
}

juce::File PlatformWebView::makeUserDataFolder()
{
#if JUCE_WINDOWS
    // 本地应用数据下的 Synchain/Rinne/WebView2。
    // 不用临时目录与漫游目录的理由见头文件声明处。
    // 两个角色共用这一个固定目录（不再每实例一个）：WebView2 的浏览器进程组按目录共享，
    // 给每个实例拼唯一后缀会让进程组永不复用，热启动判定形同虚设，还让每开一个编辑器就多一整套浏览器进程。
    // 同进程内多个 WebView2 实例共用目录是受支持的用法，跨进程共用也受支持，前提是环境选项一致；
    // 失败条件是选项不一致，而不是来自另一个进程。万一仍然创建失败（企业策略、目录被锁等），
    // 构造期的可写性探针和「导航事件从未到达」的三态面板会如实说出来，不会表现成一句「加载太慢」
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
        .getChildFile("Synchain")
        .getChildFile("Rinne")
        .getChildFile("WebView2");
#else
    return {};
#endif
}

juce::String PlatformWebView::plainAsciiDetail(const juce::String& text, int maxLength)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty() || trimmed.length() > maxLength)
        return {};
    for (const auto ch : trimmed)
    {
        if (ch < 0x20 || ch > 0x7e)
            return {};
    }
    return trimmed;
}

juce::String PlatformWebView::probeUserDataFolder(const juce::File& folder)
{
    if (folder == juce::File())
        return {};

    // 原因文本里不放路径：它会进兜底面板，面板上的字会被用户截图转发。完整路径由调用方写本机日志
    const auto result = folder.createDirectory();
    if (result.failed())
    {
        // 系统的说明按显示语言本地化，中文系统上是汉字：只在它是干净的单行 ASCII 时才带上
        const auto detail = plainAsciiDetail(result.getErrorMessage());
        return detail.isEmpty() ? juce::String("user-data folder not creatable")
                                : "user-data folder not creatable: " + detail;
    }

    const auto probe = folder.getChildFile(".rinne-write-probe");
    if (!probe.replaceWithText("ok"))
        return "user-data folder not writable";
    probe.deleteFile();
    return {};
}

#if JUCE_WEB_BROWSER
juce::WebBrowserComponent::Options PlatformWebView::makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                       const juce::File& userDataFolder)
{
#if JUCE_WINDOWS
    using WBC = juce::WebBrowserComponent;
    WBC::Options::WinWebView2 wv2;
    wv2 = wv2.withUserDataFolder(userDataFolder);
    // WebView2 在任何网页内容之下铺的那一层。不设的话它是默认构造的全透明色，JUCE 会把它原样交给 WebView2，
    // 于是从控制器建好到页面样式解析完为止，这一层什么都不挡，露的是窗口的白。
    // 这一层只能收纯色，所以取占位渐变沿轴 50% 处的插值色，与渐变占位不跳阶
    wv2 = wv2.withBackgroundColour(shellBackdropMid());
    options = options.withBackend(WBC::Options::Backend::webview2).withWinWebView2Options(wv2);
#else
    juce::ignoreUnused(userDataFolder);
#endif
    return options;
}
#endif

const char* PlatformWebView::runtimeDownloadUrl()
{
    return "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
}

int PlatformWebView::majorVersionOf(const juce::String& version)
{
    const auto head = version.trim().upToFirstOccurrenceOf(".", false, false).trim();
    if (head.isEmpty() || !head.containsOnly("0123456789"))
        return -1;
    return head.getIntValue();
}

PlatformWebView::BackgroundColourSupport PlatformWebView::backgroundColourSupport(const RuntimeInfo& info)
{
    // 没探到运行时：这一层无从谈起（这条路上根本不会去建控制器，调用方直接切兜底面板）
    if (info.status == RuntimeStatus::missing)
        return BackgroundColourSupport::unknown;

    // 版本串解析不出：不猜。runtimeInfo 在这种情况下刻意放行走正常加载路径（宁可让看门狗兜底，也不误判过旧），
    // 这里同样不许拿一个猜来的结论顶替「不知道」
    const int major = majorVersionOf(info.version);
    if (major < 0)
        return BackgroundColourSupport::unknown;

    return major >= kBackgroundColourMinRuntimeMajor ? BackgroundColourSupport::available
                                                     : BackgroundColourSupport::unavailable;
}

// 按引擎分支的用户可见文案与诊断行。文案一律 ASCII：运行期字面量含非 ASCII 会在中文代码页的机器上触发
// MSVC 的字符集警告，而第一方代码零警告
juce::String PlatformWebView::missingRuntimeMessage(Engine engine)
{
    if (engine == Engine::webView2)
        return "Microsoft Edge WebView2 Runtime was not found, so the full UI cannot load.\n"
               "Install the runtime once, then reopen this plugin window.";
    return "The system web view (WebKit) is not available, so the full UI cannot load.\n"
           "Update macOS, then reopen this plugin window.";
}

juce::String PlatformWebView::tooOldRuntimeMessage(Engine engine)
{
    if (engine == Engine::webView2)
        return "The installed Microsoft Edge WebView2 Runtime is too old for this plugin.\n"
               "Update to the Evergreen runtime, then reopen this plugin window.";
    return "The system web view (WebKit) is too old for this plugin.\n"
           "Update macOS, then reopen this plugin window.";
}

juce::String PlatformWebView::envNotStartedMessage(Engine engine)
{
    if (engine == Engine::webView2)
        return "The WebView2 environment did not start (no navigation ever began).\n"
               "This is usually the user-data folder being unwritable or already in use by another\n"
               "process, or the host blocking the msedgewebview2.exe child process.";
    // 系统 WebKit 没有用户数据目录这一环，剩下的常见成因是宿主挡了 WebKit 的网页内容子进程，或系统内存吃紧
    return "The system web view (WebKit) did not start (no navigation ever began).\n"
           "This is usually the host blocking the WebKit web content process, or the\n"
           "system running low on memory. Click Retry, or close and reopen this plugin window.";
}

juce::String PlatformWebView::runtimeDiagnosticsField(const RuntimeInfo& info, Engine engine)
{
    if (engine == Engine::webView2)
        return "WebView2 " + (info.version.isNotEmpty() ? info.version : juce::String("not found"));
    // 系统 WebKit 的版本跟着系统走，runtimeInfo 不探测（version 恒空）。
    // 写成 "not found" 会把一台正常的机器误报成缺运行时
    return "WebKit (system)";
}

// 四条形态措辞互不相同，便于在日志里直接检索：
//   available   正常；这一层按版本推断在，控制器建好到首帧之间铺的是我方色值；
//   UNAVAILABLE 运行时过旧；那一段露的是 WebView2 默认的白。遮挡闸已经让那一段不上屏，这里只如实说出来；
//   unknown 两条  版本串没解析出来，或压根没探到运行时。两种原因分开写，不猜。
// 措辞用 "inferred present|absent (from runtime ..., not directly observed)"：这一行是按运行时主版本推断出来的，
// 不是对 JUCE 那次接口查询的直接观测。用户会把日志片段整段贴回来，贴回来的人多半不会同时读头文件的注释，
// 所以「这是推断」必须写在行里，不能只写在注释里
juce::String PlatformWebView::backgroundColourDiagnostics(const RuntimeInfo& info, Engine engine)
{
    if (engine != Engine::webView2)
        return {};

    using Support = BackgroundColourSupport;
    const auto support = backgroundColourSupport(info);
    const juce::String version = info.version.isNotEmpty() ? info.version : juce::String("unknown");
    // 默认背景色仍只收纯色：占位渐变的轴中点色
    const juce::String argb =
        juce::String::toHexString(static_cast<int>(shellBackdropMid().getARGB())).paddedLeft('0', 8);

    const juce::String floor = juce::String(kBackgroundColourMinRuntimeMajor);
    if (support == Support::available)
        return "webview2 default background: available -- ICoreWebView2Controller2 inferred present "
               "(from runtime " +
               version + " >= major " + floor + ", not directly observed), JUCE puts argb " + argb;
    if (support == Support::unavailable)
        return "webview2 default background: UNAVAILABLE -- ICoreWebView2Controller2 inferred absent "
               "(from runtime " +
               version + " < major " + floor + ", not directly observed), JUCE drops argb " + argb + " silently";
    if (info.status == RuntimeStatus::missing)
        // 当前调用点在缺失时已提前返回，走不到这里。写对它是为了将来挪调用点的人：
        // 否则这条会打成 "runtime version unknown not parsable"，把原因指错
        return "webview2 default background: unknown (no WebView2 runtime detected)";
    return "webview2 default background: unknown (runtime version " + version + " not parsable)";
}

juce::String PlatformWebView::userDataFolderDisplay(const juce::File& folder, bool basenameOnly)
{
    // 空 File 表示本平台不用这个目录。WebView2 上恒非空
    if (folder == juce::File())
        return "(not used)";
    return basenameOnly ? folder.getFileName() : folder.getFullPathName();
}

} // namespace rinne::webview
