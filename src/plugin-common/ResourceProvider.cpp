// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "ResourceProvider.h"

#include <string>
#include <vector>

namespace rinne::webview
{

namespace
{

struct Origin
{
    std::string scheme;
    std::string authority;
};

char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string asciiLower(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
        out.push_back(asciiLower(c));
    return out;
}

bool isSchemeChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

// 取 scheme://authority 两段（统一成小写）。看不懂的、带控制字符或空白的、协议不合文法的、
// 主机段为空或含非 ASCII、用户信息、反斜杠的，一律返回空：宁可多拦
std::optional<Origin> parseOrigin(const std::string& text)
{
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7f)
            return std::nullopt;
    }

    const auto sep = text.find("://");
    if (sep == std::string::npos || sep == 0)
        return std::nullopt;

    const auto authorityStart = sep + 3;
    const auto authorityEnd = text.find_first_of("/?#", authorityStart);

    Origin origin;
    origin.scheme = asciiLower(text.substr(0, sep));
    origin.authority = asciiLower(text.substr(
        authorityStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart));

    if (origin.authority.empty() || origin.scheme.front() < 'a' || origin.scheme.front() > 'z')
        return std::nullopt;
    for (const char c : origin.scheme)
        if (!isSchemeChar(c))
            return std::nullopt;
    for (const char ch : origin.authority)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80 || c == '@' || c == '\\')
            return std::nullopt;
    }
    return origin;
}

} // namespace

ResourceProvider::ResourceProvider(Source source) : source_(source) {}

bool ResourceProvider::isRootNavigation(const juce::String& url, const juce::String& resourceRoot)
{
    const std::string target = url.toStdString();
    if (target == "about:blank")
        return true;

    const auto root = parseOrigin(resourceRoot.toStdString());
    const auto wanted = parseOrigin(target);
    return root.has_value() && wanted.has_value() && root->scheme == wanted->scheme &&
           root->authority == wanted->authority;
}

namespace
{

// 会被日志查看器或终端当作断行、或能把光标带走的字符：C0 控制字符（含换行、回车、制表、退格）、删除符、
// C1 控制字符（0x80 到 0x9F，其中 U+0085 是 NEL，很多查看器当换行）、U+2028 行分隔符与 U+2029 段分隔符。
// 日志里的任何一行都不许由来路不明的文字决定在哪里结束，所以这些一律换成问号
bool breaksLogLine(juce::juce_wchar c)
{
    return c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f) || c == 0x2028 || c == 0x2029;
}

// urlForLog 与 asciiForLog 共用的整形：最多 maxChars 个字符，断行类字符换成问号。
// asciiOnly 时再把所有非 ASCII 字符换成问号，两个函数的口径只差这一步
juce::String shapeUntrusted(const juce::String& text, int maxChars, bool asciiOnly)
{
    juce::String out;
    int kept = 0;
    for (auto it = text.begin(); it != text.end() && kept < maxChars; ++it, ++kept)
    {
        const auto c = *it;
        const bool replace = breaksLogLine(c) || (asciiOnly && c > 0x7e);
        out += juce::String::charToString(replace ? static_cast<decltype(c)>('?') : c);
    }
    return out;
}

} // namespace

juce::String ResourceProvider::urlForLog(const juce::String& url, int maxChars)
{
    return shapeUntrusted(url, maxChars, false);
}

juce::String ResourceProvider::asciiForLog(const juce::String& text, int maxChars)
{
    return shapeUntrusted(text, maxChars, true);
}

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
