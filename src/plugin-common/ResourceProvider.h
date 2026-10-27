// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <optional>

namespace rinne::webview
{
// 资源提供者与 MIME 映射，平台无关。
// 页面资源由构建扁平地编进二进制数据，这里按二进制数据的原始文件名反查（避免手工复刻 JUCE 的符号名改写）。
// 目录层级不进包，页面里的 ../shared/x.js 靠 URL 归一加文件名命中；
// 所以打包集合内文件名必须全局唯一，由构建期的检查守着。
// 每个插件各有一个 juce_add_binary_data 生成的命名空间，经 Source 注入本类，本类不感知具体命名空间。
class ResourceProvider
{
public:
    // juce_add_binary_data 生成命名空间的最小描述，调用方把自己的 BinaryData 静态成员填进来
    struct Source
    {
        int resourceCount = 0;
        const char* const* originalFilenames = nullptr;
        const char* const* resourceNames = nullptr;
        const char* (*getNamedResource)(const char* resourceName, int& dataSizeInBytes) = nullptr;
    };

    // 一次命中：指向二进制数据的指针（不拥有）、长度和 MIME。纯数据，不依赖浏览器组件，宿主测试直接用
    struct Hit
    {
        const char* data = nullptr;
        int size = 0;
        const char* mime = "application/octet-stream";
    };

    explicit ResourceProvider(Source source);

    // 把请求 URL 归一成资源路径：去掉来源、查询串和片段；"/" 或空归一成 "index.html"。
    // JUCE 的后端在调提供者之前已经去掉了来源，这里仍防御性地处理完整 URL，保证跨后端口径一致
    static juce::String normalizePath(const juce::String& url);

    // 页面（或页面里被注入的东西）发起的导航能不能放行：只放行资源提供者的根下面的地址，加上页面隐藏时 JUCE
    // 借道的空白页。
    // 页面带着原生桥，导航到外站等于把桥交给外站的脚本，所以其余一律不放。纯函数，两个平台的根都能在宿主测试里直接喂。
    //   - resourceRoot 取 WebBrowserComponent::getResourceProviderRoot()（Windows 是 https://juce.backend/，
    //     macOS 是 juce://juce.backend/）；空串（不认识的系统）时只放空白页；
    //   - 放行的是「协议与主机和根逐字相同」的地址，路径、查询串、片段随意；协议与主机按 ASCII 不分大小写比较
    //     （浏览器交上来的本来就是规范化后的小写，大小写不同的写法指向同一个地方，拦下没有好处）；
    //   - 一切别的都拦：形似的主机（多一段、少一段、末尾带点、非 ASCII 的同形字符）、用户信息（host@evil）、
    //     端口、反斜杠、别的协议（http 降级、file、javascript、data、ftp）、带控制字符或空白的地址、空串；
    //   - 空白页只认 "about:blank" 这一个整串。
    static bool isRootNavigation(const juce::String& url, const juce::String& resourceRoot);

    // 把来路不明的地址整理成能写进日志的一行：最多 maxChars 个字符，控制字符换成问号（防止伪造日志行）。
    static juce::String urlForLog(const juce::String& url, int maxChars = 120);

    // url 形如 "/" 或 "/index.html"。命中返回数据与 MIME，否则 nullopt。
    // 源为空（没有可嵌入的资源）时恒 nullopt，不造假资源
    std::optional<Hit> lookup(const juce::String& url) const;

#if JUCE_WEB_BROWSER
    // lookup 的结果拷成 JUCE 的资源对象
    std::optional<juce::WebBrowserComponent::Resource> provide(const juce::String& url) const;
#endif

    // 扩展名对应的 MIME。mjs 与 js 都给 text/javascript（ES 模块必须是脚本类型，否则浏览器拒载）
    static const char* mimeForExtension(const juce::String& ext);

    const Source& source() const { return source_; }

private:
    Source source_;
};

} // namespace rinne::webview
