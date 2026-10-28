// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "PlatformWebView.h"

// 运行时探测单独成文件：这个翻译单元引用 WebView2 静态 loader 的版本查询，只有插件目标经 JUCE 的
// WebView2 支持链接了 loader。拆开后 PlatformWebView.cpp 里的纯函数不依赖 loader，可以离线单测。

#if JUCE_WINDOWS
// WebView2 静态 loader 导出的版本查询，以及 ole32 的释放函数。
// 直接前置声明，免引 WebView2 头与 windows.h（避开包含路径与宏污染）
extern "C" {
long __stdcall GetAvailableCoreWebView2BrowserVersionString(const wchar_t* browserExecutableFolder,
                                                            wchar_t** versionInfo);
void __stdcall CoTaskMemFree(void* pv);
}
#endif

namespace rinne::webview
{

PlatformWebView::RuntimeInfo PlatformWebView::runtimeInfo()
{
#if JUCE_WINDOWS
    RuntimeInfo info;

    wchar_t* version = nullptr;
    const long hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool present = hr >= 0 && version != nullptr && version[0] != L'\0';
    if (present)
        info.version = juce::String(version);
    if (version != nullptr)
        CoTaskMemFree(version);

    if (!present)
        return info; // status 保持 missing，version 为空

    // 版本串解析不出主版本号（loader 换了格式或非常规通道）时不误判成过旧：宁可放行走正常加载路径，
    // 让看门狗按超时兜底，也不要把一台能用的机器挡在「请升级」面板后面
    const int major = majorVersionOf(info.version);
    info.status = (major >= 0 && major < kMinRuntimeMajor) ? RuntimeStatus::tooOld : RuntimeStatus::ok;
    return info;
#else
    return {RuntimeStatus::ok, {}}; // 系统 WebKit 恒可用
#endif
}

bool PlatformWebView::runtimeAvailable()
{
    return runtimeInfo().status == RuntimeStatus::ok;
}

} // namespace rinne::webview
