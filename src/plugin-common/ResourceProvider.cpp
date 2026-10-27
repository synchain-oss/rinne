// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "ResourceProvider.h"

#include <vector>

namespace rinne::webview
{

ResourceProvider::ResourceProvider(Source source) : source_(source) {}

juce::String ResourceProvider::normalizePath(const juce::String& url)
{
    juce::String path = url.upToFirstOccurrenceOf("?", false, false).upToFirstOccurrenceOf("#", false, false);

    // 完整 URL（scheme://host/path）剥掉来源；否则视为已经是根相对路径
    if (const int schemeEnd = path.indexOf("://"); schemeEnd >= 0)
    {
        const int pathStart = path.indexOfChar(schemeEnd + 3, '/');
        path = (pathStart >= 0) ? path.substring(pathStart + 1) : juce::String("index.html");
    }

    if (path.startsWith("/"))
        path = path.substring(1);

    return path.isEmpty() ? juce::String("index.html") : path;
}

std::optional<ResourceProvider::Hit> ResourceProvider::lookup(const juce::String& url) const
{
    const auto path = normalizePath(url);
    const auto baseName = path.fromLastOccurrenceOf("/", false, false);
    const auto fileName = baseName.isEmpty() ? path : baseName; // 顶层资源没有斜杠，取整段

    if (source_.originalFilenames == nullptr || source_.resourceNames == nullptr || source_.getNamedResource == nullptr)
        return std::nullopt;

    for (int i = 0; i < source_.resourceCount; ++i)
    {
        if (juce::String(source_.originalFilenames[i]) != fileName)
            continue;
        int size = 0;
        if (const char* data = source_.getNamedResource(source_.resourceNames[i], size))
        {
            const auto ext = fileName.fromLastOccurrenceOf(".", false, false);
            return Hit{data, size, mimeForExtension(ext)};
        }
    }
    return std::nullopt;
}

#if JUCE_WEB_BROWSER
std::optional<juce::WebBrowserComponent::Resource> ResourceProvider::provide(const juce::String& url) const
{
    const auto hit = lookup(url);
    if (!hit)
        return std::nullopt;
    std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(hit->data),
                                 reinterpret_cast<const std::byte*>(hit->data) + hit->size);
    return juce::WebBrowserComponent::Resource{std::move(bytes), juce::String(hit->mime)};
}
#endif

const char* ResourceProvider::mimeForExtension(const juce::String& ext)
{
    const auto e = ext.toLowerCase();
    if (e == "html" || e == "htm")
        return "text/html";
    if (e == "css")
        return "text/css";
    if (e == "js" || e == "mjs")
        return "text/javascript"; // ES 模块必须是脚本类型
    if (e == "json")
        return "application/json";
    if (e == "svg")
        return "image/svg+xml";
    if (e == "woff2")
        return "font/woff2";
    if (e == "woff")
        return "font/woff";
    if (e == "ttf")
        return "font/ttf";
    if (e == "png")
        return "image/png";
    return "application/octet-stream";
}

} // namespace rinne::webview
