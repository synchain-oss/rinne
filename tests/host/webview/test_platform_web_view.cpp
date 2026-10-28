// SPDX-License-Identifier: GPL-3.0-or-later
//
// 平台 WebView 的纯函数部分：占位渐变（色标、角度、几何）、开窗占位（外圈色、圆角、缩放）、用户数据目录、
// 运行时版本判定、按引擎分支的文案与诊断行。
// 真实的 WebView2 实例化与运行时探测需要 loader 与真机，不在这里。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <string>

#include <juce_graphics/juce_graphics.h>

#include "PlatformWebView.h"

namespace
{

using rinne::webview::PlatformWebView;
using Engine = PlatformWebView::Engine;
using Status = PlatformWebView::RuntimeStatus;
using Support = PlatformWebView::BackgroundColourSupport;
namespace wv = rinne::webview;

PlatformWebView::RuntimeInfo info(Status status, const char* version)
{
    PlatformWebView::RuntimeInfo i;
    i.status = status;
    i.version = version;
    return i;
}

bool isAscii(const juce::String& s)
{
    for (auto p = s.getCharPointer(); !p.isEmpty(); ++p)
        if (static_cast<unsigned>(*p) > 0x7f)
            return false;
    return true;
}

} // namespace

TEST_CASE("backdrop stops are the agreed table", "[webview][platform][backdrop]")
{
    constexpr auto count = sizeof(wv::kShellBackdropStops) / sizeof(wv::kShellBackdropStops[0]);
    REQUIRE(count == 4);

    // 与页面样式里的渐变逐项同色标；页面侧由嵌入资源回归逐项对拍，这里把 C++ 侧的值钉死
    CHECK(wv::kShellBackdropStops[0].rgb == 0xb5acc9u);
    CHECK(wv::kShellBackdropStops[1].rgb == 0xccbfd5u);
    CHECK(wv::kShellBackdropStops[2].rgb == 0xe3d2e0u);
    CHECK(wv::kShellBackdropStops[3].rgb == 0xfde8edu);
    CHECK(wv::kShellBackdropStops[0].pos == 0.00f);
    CHECK(wv::kShellBackdropStops[1].pos == 0.32f);
    CHECK(wv::kShellBackdropStops[2].pos == 0.64f);
    CHECK(wv::kShellBackdropStops[3].pos == 1.00f);
    CHECK(wv::kShellBackdropAngleDeg == 157.0f);

    // 位置严格递增，且落在 0 到 1 之间；色值只有 24 位，高位不放任何东西
    for (std::size_t i = 0; i < count; ++i)
    {
        CHECK(wv::kShellBackdropStops[i].rgb <= 0xffffffu);
        CHECK(wv::kShellBackdropStops[i].pos >= 0.0f);
        CHECK(wv::kShellBackdropStops[i].pos <= 1.0f);
        if (i > 0)
            CHECK(wv::kShellBackdropStops[i].pos > wv::kShellBackdropStops[i - 1].pos);
    }
}

TEST_CASE("backdrop mid colour is the interpolated 50 percent point and fully opaque", "[webview][platform][backdrop]")
{
    const auto mid = wv::shellBackdropMid();
    CHECK(mid.getARGB() == 0xffd9cadbu);
    CHECK(mid.isOpaque());
}

TEST_CASE("backdrop gradient follows the CSS geometry for 157 degrees", "[webview][platform][backdrop]")
{
    const juce::Rectangle<float> area(0.0f, 0.0f, 460.0f, 364.0f);
    const auto g = wv::shellBackdropGradient(area);

    CHECK_FALSE(g.isRadial);
    CHECK(g.getNumColours() == 4);

    // 方向 (sin θ, -cos θ)：157 度指向右下；线长 |W sinθ| + |H cosθ|；线过矩形中心
    const float rad = juce::degreesToRadians(157.0f);
    const float expectedLen = std::abs(460.0f * std::sin(rad)) + std::abs(364.0f * std::cos(rad));
    const float len = g.point1.getDistanceFrom(g.point2);
    CHECK(len == Catch::Approx(expectedLen).margin(0.01));
    CHECK(g.point1.x < g.point2.x);
    CHECK(g.point1.y < g.point2.y);
    const auto centre = (g.point1 + g.point2) * 0.5f;
    CHECK(centre.x == Catch::Approx(230.0f).margin(0.01));
    CHECK(centre.y == Catch::Approx(182.0f).margin(0.01));
    const float dx = (g.point2.x - g.point1.x) / len;
    const float dy = (g.point2.y - g.point1.y) / len;
    CHECK(dx == Catch::Approx(std::sin(rad)).margin(0.0005));
    CHECK(dy == Catch::Approx(-std::cos(rad)).margin(0.0005));

    // 色标位置与颜色逐项对应，且全部不透明
    for (int i = 0; i < g.getNumColours(); ++i)
    {
        const auto& stop = wv::kShellBackdropStops[i];
        CHECK(g.getColourPosition(i) == Catch::Approx(static_cast<double>(stop.pos)).margin(1e-6));
        CHECK(g.getColour(i).getARGB() == (0xff000000u | stop.rgb));
        CHECK(g.getColour(i).isOpaque());
    }
}

TEST_CASE("backdrop gradient scales its line with the area", "[webview][platform][backdrop]")
{
    const auto small = wv::shellBackdropGradient({0.0f, 0.0f, 230.0f, 182.0f});
    const auto large = wv::shellBackdropGradient({0.0f, 0.0f, 460.0f, 364.0f});
    CHECK(large.point1.getDistanceFrom(large.point2) ==
          Catch::Approx(2.0 * small.point1.getDistanceFrom(small.point2)).margin(0.05));
}

TEST_CASE("backdrop gradient of an empty area degrades to the flat mid colour", "[webview][platform][backdrop]")
{
    const auto g = wv::shellBackdropGradient({0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(g.getColour(0) == wv::shellBackdropMid());
    CHECK(g.getColour(g.getNumColours() - 1) == wv::shellBackdropMid());
    CHECK(g.point1 != g.point2); // 两端不重合，渐变线有长度
}

TEST_CASE("shell geometry constants are the agreed values", "[webview][platform][placeholder]")
{
    // 与 tokens.css 的 --r-shell、--page-backdrop 同值；页面侧由嵌入资源回归对拍，这里把 C++ 侧钉死
    CHECK(wv::kShellCornerRadiusPx == 20.0f);
    CHECK(wv::kShellRimRgb == 0x191820u);
}

TEST_CASE("shell fit zoom is the page formula", "[webview][platform][placeholder]")
{
    CHECK(wv::shellFitZoom(800.0f, 580.0f, 800.0f, 580.0f) == 1.0f);
    CHECK(wv::shellFitZoom(1200.0f, 870.0f, 800.0f, 580.0f) == 1.5f);
    // 取两维里小的那个
    CHECK(wv::shellFitZoom(1600.0f, 580.0f, 800.0f, 580.0f) == 1.0f);
    CHECK(wv::shellFitZoom(460.0f, 728.0f, 460.0f, 364.0f) == 1.0f);
    // 向下量化到 1e-4：不会因为四舍五入把外壳放大出窗口
    const float z = wv::shellFitZoom(1000.0f, 1000.0f, 3.0f * 1000.0f, 3.0f * 1000.0f);
    CHECK(z <= 1.0f / 3.0f);
    CHECK(z == Catch::Approx(0.3333).margin(1e-6));
    // 任一维不是正数回落 1
    CHECK(wv::shellFitZoom(0.0f, 580.0f, 800.0f, 580.0f) == 1.0f);
    CHECK(wv::shellFitZoom(800.0f, -1.0f, 800.0f, 580.0f) == 1.0f);
    CHECK(wv::shellFitZoom(800.0f, 580.0f, 0.0f, 580.0f) == 1.0f);
    CHECK(wv::shellFitZoom(800.0f, 580.0f, 800.0f, std::nanf("")) == 1.0f);
}

TEST_CASE("shell placeholder bounds are the design box times the zoom, centred", "[webview][platform][placeholder]")
{
    const auto exact = wv::shellPlaceholderBounds({0.0f, 0.0f, 1200.0f, 870.0f}, 800.0f, 580.0f);
    CHECK(exact.getWidth() == Catch::Approx(1200.0f).margin(0.01));
    CHECK(exact.getHeight() == Catch::Approx(870.0f).margin(0.01));
    CHECK(exact.getCentreX() == Catch::Approx(600.0f).margin(0.01));
    CHECK(exact.getCentreY() == Catch::Approx(435.0f).margin(0.01));

    // 窗口比设计盒宽：外壳按高缩放，左右留出外圈色
    const auto wide = wv::shellPlaceholderBounds({0.0f, 0.0f, 1000.0f, 364.0f}, 460.0f, 364.0f);
    CHECK(wide.getWidth() == Catch::Approx(460.0f).margin(0.01));
    CHECK(wide.getX() == Catch::Approx(270.0f).margin(0.01));
    CHECK(wide.getY() == Catch::Approx(0.0f).margin(0.01));
}

TEST_CASE("shell placeholder paints the rim outside the rounded corner and the gradient inside",
          "[webview][platform][placeholder]")
{
    // 100% 档的输出端窗口：外壳铺满，圆角半径 20 像素
    const int w = 460, h = 364;
    juce::Image img(juce::Image::RGB, w, h, true, juce::SoftwareImageType());
    {
        juce::Graphics g(img);
        wv::paintShellPlaceholder(g, {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, 460.0f, 364.0f);
    }
    const auto rim = juce::Colour(0xff000000u | wv::kShellRimRgb);
    // 四个角上的像素在圆角之外：外圈色
    CHECK(img.getPixelAt(0, 0) == rim);
    CHECK(img.getPixelAt(w - 1, 0) == rim);
    CHECK(img.getPixelAt(0, h - 1) == rim);
    CHECK(img.getPixelAt(w - 1, h - 1) == rim);
    // 离角 3 像素的对角线点仍在 20 像素圆角之外（圆角在对角线上离角约 5.9 像素才开始）
    CHECK(img.getPixelAt(3, 3) == rim);
    // 边的中点与对角线上离角 8 像素处已在圆角之内：不是外圈色，是浅色渐变
    for (const auto p : {juce::Point<int>(w / 2, 0), juce::Point<int>(0, h / 2), juce::Point<int>(8, 8),
                         juce::Point<int>(w / 2, h / 2)})
    {
        const auto c = img.getPixelAt(p.x, p.y);
        CHECK(c != rim);
        CHECK(c.getBrightness() > 0.7f);
    }
    // 中心就是渐变的 50% 点
    const auto centre = img.getPixelAt(w / 2, h / 2);
    const auto mid = wv::shellBackdropMid();
    CHECK(std::abs(centre.getRed() - mid.getRed()) <= 3);
    CHECK(std::abs(centre.getGreen() - mid.getGreen()) <= 3);
    CHECK(std::abs(centre.getBlue() - mid.getBlue()) <= 3);
}

TEST_CASE("shell placeholder radius follows the zoom", "[webview][platform][placeholder]")
{
    // 200% 档：半径 40 像素。离角 8 像素的对角线点在 20 像素圆角里会是渐变，在 40 像素圆角里仍是外圈色
    const int w = 920, h = 728;
    juce::Image img(juce::Image::RGB, w, h, true, juce::SoftwareImageType());
    {
        juce::Graphics g(img);
        wv::paintShellPlaceholder(g, {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, 460.0f, 364.0f);
    }
    const auto rim = juce::Colour(0xff000000u | wv::kShellRimRgb);
    CHECK(img.getPixelAt(8, 8) == rim);
    CHECK(img.getPixelAt(16, 16) != rim);
}

TEST_CASE("user data folder is shared by both roles and never used by system WebKit", "[webview][platform][udf]")
{
    const auto folder = PlatformWebView::makeUserDataFolder();
#if JUCE_WINDOWS
    // 本地应用数据下的 Synchain/Rinne/WebView2，没有角色后缀：两个窗口共用一个目录才能复用浏览器进程组
    CHECK(folder.getFileName() == "WebView2");
    CHECK(folder.getParentDirectory().getFileName() == "Rinne");
    CHECK(folder.getParentDirectory().getParentDirectory().getFileName() == "Synchain");
    CHECK(folder.getParentDirectory().getParentDirectory().getParentDirectory() ==
          juce::File::getSpecialLocation(juce::File::windowsLocalAppData));
#else
    CHECK(folder == juce::File());
#endif
    CHECK(PlatformWebView::makeUserDataFolder() == folder); // 每次调用同一个目录
}

TEST_CASE("user data folder display never leaks the full path when asked for the base name", "[webview][platform][udf]")
{
    CHECK(PlatformWebView::userDataFolderDisplay(juce::File(), true) == "(not used)");
    CHECK(PlatformWebView::userDataFolderDisplay(juce::File(), false) == "(not used)");

    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("rinne-display-check")
                            .getChildFile("WebView2");
    const auto base = PlatformWebView::userDataFolderDisplay(folder, true);
    CHECK(base == "WebView2");
    CHECK_FALSE(base.contains("/"));
    CHECK_FALSE(base.contains("\\"));
    CHECK_FALSE(base.contains("rinne-display-check"));

    const auto full = PlatformWebView::userDataFolderDisplay(folder, false);
    CHECK(full == folder.getFullPathName());
}

TEST_CASE("user data probe: empty folder is not a problem and writes nothing", "[webview][platform][udf]")
{
    CHECK(PlatformWebView::probeUserDataFolder(juce::File()).isEmpty());
}

TEST_CASE("user data probe: writable folder is created and the probe file is removed", "[webview][platform][udf]")
{
    const auto folder =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("rinne-udf-probe", "", false);
    REQUIRE_FALSE(folder.exists());

    CHECK(PlatformWebView::probeUserDataFolder(folder).isEmpty());
    CHECK(folder.isDirectory());
    // 探针文件写完就删，目录里不留东西
    juce::Array<juce::File> left;
    folder.findChildFiles(left, juce::File::findFilesAndDirectories, false);
    CHECK(left.isEmpty());

    folder.deleteFile(); // 此时是空目录，不做递归删除
}

TEST_CASE("user data probe: a folder that cannot be created is reported without the path", "[webview][platform][udf]")
{
    // 父路径是一个普通文件，目录建不出来
    const auto blocker = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("rinne-udf-blocker", ".txt", false);
    REQUIRE(blocker.replaceWithText("x"));
    const auto folder = blocker.getChildFile("child");

    const auto issue = PlatformWebView::probeUserDataFolder(folder);
    CHECK(issue.startsWith("user-data folder not creatable"));
    // 这段文字会进兜底面板被截图转发，不能带路径或用户名
    CHECK_FALSE(issue.contains(blocker.getFullPathName()));
    CHECK_FALSE(issue.contains(blocker.getFileName()));
    CHECK(isAscii(issue));

    blocker.deleteFile();
}

TEST_CASE("error details are only passed on when they are plain single-line ASCII", "[webview][platform][udf]")
{
    CHECK(PlatformWebView::plainAsciiDetail("The system cannot find the path specified.") ==
          "The system cannot find the path specified.");
    CHECK(PlatformWebView::plainAsciiDetail("  padded  ") == "padded");

    // 本地化成汉字的系统说明、带换行的、空的、过长的，一律丢掉
    const char cjk[] = "\xe7\xb3\xbb\xe7\xbb\x9f";
    CHECK(PlatformWebView::plainAsciiDetail(juce::String(juce::CharPointer_UTF8(cjk))).isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail("ascii then " + juce::String(juce::CharPointer_UTF8(cjk))).isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail("line one\nline two").isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail("tab\tinside").isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail("").isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail("   ").isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail(juce::String::repeatedString("a", 120)) ==
          juce::String::repeatedString("a", 120));
    CHECK(PlatformWebView::plainAsciiDetail(juce::String::repeatedString("a", 121)).isEmpty());
    CHECK(PlatformWebView::plainAsciiDetail(juce::String::repeatedString("a", 9), 8).isEmpty());
}

TEST_CASE("runtime version parsing", "[webview][platform][runtime]")
{
    CHECK(PlatformWebView::majorVersionOf("137.0.3296.83") == 137);
    CHECK(PlatformWebView::majorVersionOf("  86.0.1 ") == 86);
    CHECK(PlatformWebView::majorVersionOf("152") == 152);
    CHECK(PlatformWebView::majorVersionOf("137.0.3296.83 dev") == 137);
    CHECK(PlatformWebView::majorVersionOf("") == -1);
    CHECK(PlatformWebView::majorVersionOf("abc") == -1);
    CHECK(PlatformWebView::majorVersionOf(".5") == -1);
    CHECK(PlatformWebView::majorVersionOf("v137.0") == -1);
    CHECK(PlatformWebView::majorVersionOf("-1.0") == -1);
    CHECK(PlatformWebView::kMinRuntimeMajor == 86);
}

TEST_CASE("default background support is decided from the runtime version only", "[webview][platform][runtime]")
{
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::missing, "")) == Support::unknown);
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::ok, "garbage")) == Support::unknown);
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::ok, "")) == Support::unknown);

    const int floorMajor = PlatformWebView::kBackgroundColourMinRuntimeMajor;
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::ok, juce::String(floorMajor).toRawUTF8())) ==
          Support::available);
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::ok, juce::String(floorMajor - 1).toRawUTF8())) ==
          Support::unavailable);
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::ok, "152.0.4191.66")) == Support::available);
    CHECK(PlatformWebView::backgroundColourSupport(info(Status::tooOld, "80.0.1")) == Support::unavailable);
}

TEST_CASE("engine defaults match the platform", "[webview][platform][engine]")
{
#if JUCE_WINDOWS
    CHECK(PlatformWebView::kEngine == Engine::webView2);
#else
    CHECK(PlatformWebView::kEngine == Engine::systemWebKit);
#endif
    CHECK(PlatformWebView::revealGateEnabled(Engine::webView2));
    CHECK_FALSE(PlatformWebView::revealGateEnabled(Engine::systemWebKit));
    CHECK(PlatformWebView::offersRuntimeDownload(Engine::webView2));
    CHECK_FALSE(PlatformWebView::offersRuntimeDownload(Engine::systemWebKit));
    CHECK(PlatformWebView::revealGateEnabled() == PlatformWebView::revealGateEnabled(PlatformWebView::kEngine));
}

TEST_CASE("runtime messages name the right engine and stay ASCII", "[webview][platform][text]")
{
    for (const Engine e : {Engine::webView2, Engine::systemWebKit})
    {
        for (const auto& text : {PlatformWebView::missingRuntimeMessage(e), PlatformWebView::tooOldRuntimeMessage(e),
                                 PlatformWebView::envNotStartedMessage(e)})
        {
            CHECK(text.isNotEmpty());
            CHECK(isAscii(text));
        }
    }

    for (const auto& text : {PlatformWebView::missingRuntimeMessage(Engine::webView2),
                             PlatformWebView::tooOldRuntimeMessage(Engine::webView2),
                             PlatformWebView::envNotStartedMessage(Engine::webView2)})
        CHECK(text.contains("WebView2"));

    // 系统 WebKit 上不能出现 WebView2 的名字，也不能让用户去装一个不存在的运行时
    for (const auto& text : {PlatformWebView::missingRuntimeMessage(Engine::systemWebKit),
                             PlatformWebView::tooOldRuntimeMessage(Engine::systemWebKit),
                             PlatformWebView::envNotStartedMessage(Engine::systemWebKit)})
    {
        CHECK_FALSE(text.contains("WebView2"));
        CHECK_FALSE(text.contains("msedgewebview2"));
        CHECK_FALSE(text.contains("Edge"));
        CHECK(text.contains("WebKit"));
    }
}

TEST_CASE("runtime diagnostics field per engine", "[webview][platform][text]")
{
    CHECK(PlatformWebView::runtimeDiagnosticsField(info(Status::ok, "137.0.1"), Engine::webView2) ==
          "WebView2 137.0.1");
    CHECK(PlatformWebView::runtimeDiagnosticsField(info(Status::missing, ""), Engine::webView2) ==
          "WebView2 not found");
    // 系统 WebKit 的版本不探测，不能把正常的机器报成「not found」
    CHECK(PlatformWebView::runtimeDiagnosticsField(info(Status::ok, ""), Engine::systemWebKit) == "WebKit (system)");
}

TEST_CASE("background colour diagnostics: one line per case, none for system WebKit", "[webview][platform][text]")
{
    CHECK(PlatformWebView::backgroundColourDiagnostics(info(Status::ok, "152.0.4191.66"), Engine::systemWebKit)
              .isEmpty());

    const auto available =
        PlatformWebView::backgroundColourDiagnostics(info(Status::ok, "152.0.4191.66"), Engine::webView2);
    CHECK(available.startsWith("webview2 default background: available"));
    CHECK(available.contains("inferred present"));
    CHECK(available.contains("not directly observed"));
    CHECK(available.contains("ffd9cadb")); // 交给 WebView2 的就是占位渐变的中点色

    const auto absent = PlatformWebView::backgroundColourDiagnostics(info(Status::ok, "80.0.1"), Engine::webView2);
    CHECK(absent.startsWith("webview2 default background: UNAVAILABLE"));
    CHECK(absent.contains("inferred absent"));

    const auto unparsable = PlatformWebView::backgroundColourDiagnostics(info(Status::ok, "garbage"), Engine::webView2);
    CHECK(unparsable.startsWith("webview2 default background: unknown"));
    CHECK(unparsable.contains("not parsable"));

    const auto missing = PlatformWebView::backgroundColourDiagnostics(info(Status::missing, ""), Engine::webView2);
    CHECK(missing.startsWith("webview2 default background: unknown"));
    CHECK(missing.contains("no WebView2 runtime"));

    for (const auto& line : {available, absent, unparsable, missing})
        CHECK(isAscii(line));
}

TEST_CASE("runtime download url is an https page", "[webview][platform][text]")
{
    const std::string url = PlatformWebView::runtimeDownloadUrl();
    CHECK(url.rfind("https://", 0) == 0);
    CHECK(url.find("go.microsoft.com") != std::string::npos);
}

TEST_CASE("process id is positive", "[webview][platform]")
{
    CHECK(PlatformWebView::processId() > 0);
}
