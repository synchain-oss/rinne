// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <cmath>
#include <cstddef>

namespace rinne::webview
{
// 开窗占位底色。WebView2 的宿主窗口在合成出页面首帧之前，屏上露的是窗口本身的底色，
// 开窗路径上如果三层（编辑器组件的 paint、WebView2 的默认背景色、页面的根元素底色）都没有不透明底色，
// 就会看到一瞬间的白。占位取成品外壳的渐变，开窗全程与成品首屏在明暗上不跳阶。
//
// 色标表、角度与页面样式里的渐变（web/shared/tokens.css 的 --page-gradient）、三个页面 head 里内联的
// 渐变字面量是同一组数，C++ 侧以这里为准，页面侧由 web-preview 的嵌入资源回归逐项对拍，改任何一处都会判红。
// 外圈色（外壳圆角之外那一圈）是另一个角色，取值不同，两者不得写成同值。
//
// 全部色标必须完全不透明：JUCE 的 withBackgroundColour 只接受全不透明或全透明，渐变停靠点同理。
// 色值只写 24 位 RGB，alpha 由构造时补满，不在表里留可写错的高位。
struct ShellBackdropStop
{
    juce::uint32 rgb; // 0xRRGGBB
    float pos; // 沿渐变轴的比例 0 到 1（页面样式里写的百分数除以 100）
};

// 渐变角度（度）。页面样式里 0deg 向上、顺时针。只对拍色标表而不对拍角度的话，
// 角度漂了判据照绿，所以角度也是一条独立的对拍项
inline constexpr float kShellBackdropAngleDeg = 157.0f;

// 占位渐变的色标数组，C++ 侧的单一真源。表里的注释不要写逗号加数字，回归脚本按字面抽取这张表
inline constexpr ShellBackdropStop kShellBackdropStops[] = {
    {0xb5acc9, 0.00f},
    {0xccbfd5, 0.32f},
    {0xe3d2e0, 0.64f},
    {0xfde8ed, 1.00f},
};

// 占位渐变沿轴 50% 处的插值色（现值 #d9cadb）。只喂给只收纯色的地方：WebView2 的默认背景色，
// 以及空矩形守卫。定义必须在 shellBackdropGradient 之前
inline juce::Colour shellBackdropMid()
{
    constexpr auto count = sizeof(kShellBackdropStops) / sizeof(kShellBackdropStops[0]);
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        const auto& a = kShellBackdropStops[i];
        const auto& b = kShellBackdropStops[i + 1];
        // 命中条件带 b.pos > a.pos：相邻两个停靠点写成同一位置（硬边界）时除数为 0，会算出 NaN。
        // 宁可落到末尾的首色兜底，也不算出 NaN
        if (a.pos <= 0.5f && 0.5f <= b.pos && b.pos > a.pos)
        {
            const float t = (0.5f - a.pos) / (b.pos - a.pos);
            const auto mix = [t](juce::uint32 ca, juce::uint32 cb, int shift) -> juce::uint8 {
                const auto va = static_cast<float>((ca >> shift) & 0xffu);
                const auto vb = static_cast<float>((cb >> shift) & 0xffu);
                return static_cast<juce::uint8>(juce::roundToInt(va + t * (vb - va)));
            };
            return juce::Colour(mix(a.rgb, b.rgb, 16), mix(a.rgb, b.rgb, 8), mix(a.rgb, b.rgb, 0));
        }
    }
    return juce::Colour(static_cast<juce::uint8>((kShellBackdropStops[0].rgb >> 16) & 0xffu),
                        static_cast<juce::uint8>((kShellBackdropStops[0].rgb >> 8) & 0xffu),
                        static_cast<juce::uint8>(kShellBackdropStops[0].rgb & 0xffu));
}

// 占位渐变：在 area 上复刻 CSS 的 linear-gradient 几何。
//   - 0deg 指向上、顺时针，方向向量（x 向右，y 向下）是 (sin θ, -cos θ)；
//   - 渐变线过矩形中心，线长 = |W·sinθ| + |H·cosθ|，首末停靠点各落在线的两端。
// 这是 CSS 规范对线性渐变的定义，不是近似。JUCE 的 ColourGradient 沿起终点插值，两端外侧延展首末色，与 CSS 同形
inline juce::ColourGradient shellBackdropGradient(juce::Rectangle<float> area)
{
    if (area.isEmpty())
    {
        // 空矩形的渐变线两端重合，ColourGradient 在两点重合时行为未定义。改走中点色的纯色退化
        const auto mid = shellBackdropMid();
        return juce::ColourGradient::vertical(mid, 0.0f, mid, 1.0f);
    }
    const auto rad = juce::degreesToRadians(kShellBackdropAngleDeg);
    const auto sinA = std::sin(rad);
    const auto cosA = std::cos(rad);
    const auto len = std::abs(area.getWidth() * sinA) + std::abs(area.getHeight() * cosA);
    const auto centre = area.getCentre();
    const juce::Point<float> dir{sinA, -cosA};
    const auto start = centre - dir * (len * 0.5f);
    const auto end = centre + dir * (len * 0.5f);

    const auto colourOf = [](const ShellBackdropStop& s) {
        return juce::Colour(static_cast<juce::uint8>((s.rgb >> 16) & 0xffu),
                            static_cast<juce::uint8>((s.rgb >> 8) & 0xffu), static_cast<juce::uint8>(s.rgb & 0xffu));
    };
    constexpr auto count = sizeof(kShellBackdropStops) / sizeof(kShellBackdropStops[0]);
    juce::ColourGradient gradient{colourOf(kShellBackdropStops[0]),
                                  start.x,
                                  start.y,
                                  colourOf(kShellBackdropStops[count - 1]),
                                  end.x,
                                  end.y,
                                  false};
    for (std::size_t i = 1; i + 1 < count; ++i)
        gradient.addColour(static_cast<double>(kShellBackdropStops[i].pos), colourOf(kShellBackdropStops[i]));
    return gradient;
}

// 平台 WebView 的分支都集中在这里：
//   Windows 用 WebView2（显式选后端、可写的用户数据目录、静态 loader 的运行时探测）；
//   macOS 与 Linux 用系统 WebKit（JUCE 内建，不需要探测，恒可用）。
// 用户看得见的、带平台名字的文案与诊断行也收在这里，宿主只调用不自己写。理由是可测：
// 宿主类依赖真实的浏览器组件，进不了宿主测试目标，而本类的实现进得去。
class PlatformWebView
{
public:
#if JUCE_WEB_BROWSER
    // 应用 Windows 专属的 WebView2 配置：显式选 webview2 后端（否则 JUCE 回退旧的 IE 控件，
    // 不支持资源提供者与原生集成），用户数据目录用调用方经 makeUserDataFolder 取到的那一个
    // （宿主安装目录通常只读，WebView2 初始化会失败，必须给可写目录）。目录由调用方持有，才进得了诊断面板。
    // 非 Windows 原样返回，走系统默认后端。
    static juce::WebBrowserComponent::Options makeWebViewOptions(juce::WebBrowserComponent::Options options,
                                                                 const juce::File& userDataFolder);
#endif

    // 两个插件共用的 WebView2 用户数据目录：本地应用数据下的 Synchain/Rinne/WebView2。
    // WebView2 的浏览器进程组按用户数据目录共享：同一目录复用同一组 msedgewebview2 进程，
    // 不同目录各起一整套。Input 与 Output 共用一个目录，第二个窗口就是热启动，
    // 「本进程已经起过桥」的热启动判定也才成立。共用的前提是两边的环境选项一致，本插件的环境选项是常量。
    //
    // 不用临时目录：磁盘清理会在会话进行中把正在用的目录删掉。也不用漫游的应用数据目录：
    // 浏览器缓存可达几百兆，漫游配置文件登录注销时会整个同步。
    // 非 Windows 返回空 File：系统 WebKit 用默认的数据仓，不消费这个目录。不拼路径：
    // juce 的空 File 拼子路径会得到根下的 "/x"
    static juce::File makeUserDataFolder();

    // 本进程 PID，只进诊断行，便于与任务管理器或活动监视器对照
    static int processId();

    // 建目录，写一个探针文件再删。返回空串表示可写；否则是可直接进诊断面板的原因，原因里不含路径。
    // WebView2 自己碰这个目录时的失败会被 JUCE 吞掉，所以必须我们先测一次。
    // 传空 File（本平台不用这个目录）时直接返回空串：不建目录、不写探针
    static juce::String probeUserDataFolder(const juce::File& folder);

    // 把系统给的错误说明收成可进诊断行的一小段：去掉首尾空白后必须是单行、纯 ASCII、不超过 maxLength 个字符，
    // 否则返回空串。系统说明会随显示语言变化（中文系统上是汉字，还可能带换行），而诊断行要求稳定、可读、可转发
    static juce::String plainAsciiDetail(const juce::String& text, int maxLength = 120);

    // 运行时三态。「装了但太旧」要与「没装」分开：两者用户要做的事都是装 Evergreen 运行时，
    // 但太旧的机器上 WebView2 能创建，只是缺 JUCE 用到的接口，坐等看门狗超时会把它误报成「加载慢」
    enum class RuntimeStatus
    {
        ok,
        missing, // loader 报告本机没有任何 WebView2 运行时
        tooOld // 有运行时，但主版本低于 kMinRuntimeMajor
    };

    struct RuntimeInfo
    {
        RuntimeStatus status = RuntimeStatus::missing;
        juce::String version; // loader 原样返回的版本串；missing 时为空
    };

    // WebView2 运行时主版本下限 = max(JUCE 接口下限, 前端语法下限) = 86。
    //   - JUCE 接口下限 86：JUCE 的 WebView2 后端硬依赖的最高接口是首发的那一版契约（网络请求过滤、文档创建时脚本、
    //     网页消息、控制器创建），随 Edge 86 首发。更高的默认背景色与 UserAgent 接口 JUCE 只经接口查询取，
    //     取不到就跳过，不构成下限；
    //   - 前端语法下限 80：web 下用到的最新语法只有空值合并（Chromium 80）和数组 flatMap（69）。
    //     web-preview 的语法下限回归持续把关，前端一旦用了更新的语法会红，提醒同批抬高本常量；
    //     否则旧运行时上是整页语法错误，脚本一行都不执行、桥永不就绪，表现与「加载超时」一模一样，极难归因。
    // 低于它，资源提供者与原生集成必然接不上，再等也不会好
    static constexpr int kMinRuntimeMajor = 86;

    // Evergreen 运行时的引导下载页（缺失与过旧两条分支共用）
    static const char* runtimeDownloadUrl();

    // 运行时探测：Windows 走 WebView2 loader 的版本查询；非 Windows 恒 ok 且 version 为空
    static RuntimeInfo runtimeInfo();

    // 等价于 runtimeInfo().status == ok
    static bool runtimeAvailable();

    // 版本串取主版本号，解析不出返回 -1。纯函数，不碰 loader。
    // loader 可能返回 "137.0.3296.83"，也可能带通道后缀，只取首段数字
    static int majorVersionOf(const juce::String& version);

    // 「默认背景色这一层在不在」。makeWebViewOptions 设的背景色最终落到 JUCE 的 WebView2 后端：
    // 它先查询控制器的第二版接口，查不到就静默跳过，没有日志也不看返回码，
    // 于是「控制器建好到首帧之间我方铺没铺上底色」在真机上不可观测，本函数把它变成一行可抓的诊断。
    //
    // 它判的是运行时有没有这个接口，不是「JUCE 那次接口查询真的成功了」，更不是「那一帧屏上真是这个颜色」。
    // 插件侧拿不到 JUCE 私有的控制器，做不到直接观测；接口在场是查询成功的必要条件，
    // 接口标识一旦随 SDK 发布就不再变，所以「运行时够新则接口在」成立，反向不成立。
    // 纯函数，只吃 RuntimeInfo，便于离线单测
    enum class BackgroundColourSupport
    {
        available, // 运行时够新：接口在，JUCE 那句不会静默跳过
        unavailable, // 运行时太旧：接口不在，这一层整层缺席，控制器建好到首帧之间露的是白
        unknown // 没探到运行时，或版本串解析不出：不猜，如实说不知道
    };

    // 默认背景色接口所在的运行时主版本下限。
    // 这个数字是本条判定里唯一没有机检、也无法离线核实的一环：它来自该接口首发的 SDK 版本对应的 Edge 通道，
    // 仓里没有任何东西能把这条映射钉住。影响面被两头夹得很小：主版本 86 以下的运行时已被挡在兜底面板之后，
    // 而 Evergreen 运行时会自动升级，现实中不存在停在 86 或 87 这一档的机器。判错只影响一行诊断的措辞，不改变行为
    static constexpr int kBackgroundColourMinRuntimeMajor = 87;

    static BackgroundColourSupport backgroundColourSupport(const RuntimeInfo& info);

    // 本平台的 WebView 引擎。Windows 是 WebView2（运行时要单独装，有用户数据目录）；
    // 其余是系统 WebKit。下面带 engine 参数的函数都按它分支：默认取本平台，显式传另一个值只给单测用，
    // 这样 Windows 上的单测能把 mac 那一份文案逐条判过
    enum class Engine
    {
        webView2,
        systemWebKit
    };
#if JUCE_WINDOWS
    static constexpr Engine kEngine = Engine::webView2;
#else
    static constexpr Engine kEngine = Engine::systemWebKit;
#endif

    // 兜底面板正文里提到运行时的三条（其余几条与平台无关，留在宿主里）。
    // 缺失与过旧在系统 WebKit 上实际走不到（runtimeInfo 恒 ok），仍按平台写，
    // 免得哪天调用点挪动时把 WebView2 的名字端到 mac 用户面前
    static juce::String missingRuntimeMessage(Engine engine = kEngine);
    static juce::String tooOldRuntimeMessage(Engine engine = kEngine);
    // 「导航一次都没开始」那一态。系统 WebKit 上走得到
    static juce::String envNotStartedMessage(Engine engine = kEngine);

    // 诊断行里「运行时」那一段：WebView2 是 "WebView2 <版本>" 或 "WebView2 not found"；
    // 系统 WebKit 是 "WebKit (system)"（版本跟着系统走，插件侧不探测）
    static juce::String runtimeDiagnosticsField(const RuntimeInfo& info, Engine engine = kEngine);

    // 「默认背景色这一层在不在」的诊断行（每次加载尝试打一条）。这一层是 WebView2 专有的，
    // 系统 WebKit 返回空串，调用方据此不打这一行
    static juce::String backgroundColourDiagnostics(const RuntimeInfo& info, Engine engine = kEngine);

    // 开窗遮挡闸（把 WebView 挪出可视区等首帧，见 WebViewRevealGate.h）启不启用。
    // 只在 WebView2 上启用：闸门为 WebView2 宿主窗口首帧之前那段白而设；系统 WebKit 上没有验证过，
    // 而首帧信号一旦不到，每次开窗都会白等兜底时长。系统 WebKit 上闸门保持未武装
    static constexpr bool revealGateEnabled(Engine engine = kEngine) { return engine == Engine::webView2; }

    // 兜底面板要不要给「下载运行时」按钮（文案与链接都是 WebView2 的）。系统 WebKit 没有可单独下载的运行时，恒 false
    static constexpr bool offersRuntimeDownload(Engine engine = kEngine) { return engine == Engine::webView2; }

    // 诊断里用户数据目录那一段。系统 WebKit 不用这个目录，显示 "(not used)" 而不是一个空路径。
    // basenameOnly 为真只给末级目录名，进兜底面板的文本用它：面板上的字会被用户截图转发，
    // 完整路径带用户名，只进本机日志
    static juce::String userDataFolderDisplay(const juce::File& folder, bool basenameOnly);
};

} // namespace rinne::webview
