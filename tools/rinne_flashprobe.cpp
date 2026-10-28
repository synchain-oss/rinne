// SPDX-License-Identifier: GPL-3.0-or-later
//
// rinne_flashprobe：开窗白帧检测。Windows 专用的开发者工具，不进 ctest、不进打包：输出到构建目录的 devtools-bin/，
// 打包脚本与 CI 产物只取 tools-bin/ 下的可执行文件。
//
// 加载一个本机构建出的 .vst3，把编辑器放进一个无边框的置顶窗口里开、关，重复 N 次。开窗期间另起一条抓屏线程，
// 抓编辑器所在矩形合成之后的结果：WebView2 走 DirectComposition，PrintWindow 只抓得到窗口自己画的那层，
// 抓不到它；只有从屏幕上抓合成之后的结果，才是用户真正看到的。默认走桌面复制（DXGI），合成器每出一张新桌面就取一张，
// 两次取帧之间合成了不止一张会记成「合并」，取到了却读不出来的记成「丢弃」，两者都为 0 才是一帧没漏；
// 复制反复失效、本轮抓屏中途停掉的开窗单独计数。取不到桌面复制（或 --capture gdi）时
// 退回屏幕 DC 的 BitBlt 按 --fps 轮询，实际帧率记进结果。每帧按采样点归一类：
//   backdrop     本工具铺在编辑器窗口后面的绿底：编辑器窗口还没上屏；我方首帧（占位或品红）之后再见到它记成「透底」
//                （see-through），真宿主上那一刻露出的是宿主窗口后面的东西；我方首帧之前的绿底是宿主首次上屏的空档，
//                不算透底。编辑器窗口被要求上屏之前抓到的帧一律记成底。--white-backdrop 把底换成白，透底直接记成白帧
//   black        JUCE 宿主侧插件窗容器的黑底（本工具借 JUCE 的 VST3 宿主开窗，它的容器画黑）
//   magenta      品红：插件用品红探针构建时我方占位那一层，测到它才证明抓到的确实是我方这一层
//   white        近白像素占比过阈值
//   placeholder  与占位相符：方形（旧几何）与圆角（新几何）都认，参照图用插件同一份 C++ 函数画
//   content      与本次开窗揭幕之后的最后一帧相符
//   other        以上都不是（过渡帧、半张图）
// 另记两条时间线，全部相对「编辑器创建」那一刻：插件经调试输出写的诊断行（DBWIN 通道，按本进程 pid 过滤，
// 与插件的诊断日志开关无关），以及编辑器窗口下各层子窗口的出现与位置（WebView2 的宿主窗口落在可视区里还是在外面）。
//
// 用法：
//   rinne_flashprobe --plugin <x.vst3> --out <结果.json> [--opens 20] [--max-open-ms 6000] [--after-reveal-ms 400]
//                    [--gap-ms 500] [--load <线程数>] [--fps 240] [--white 0.2] [--x 80] [--y 80]
//                    [--max-seconds 115] [--frames-dir <目录>] [--capture dxgi|gdi] [--white-backdrop] [--no-gui-lock]
//   --load    起若干条普通优先级的忙循环线程复现「机器忙」，跑完自己停，不碰别的进程。
//   范围：--max-seconds 1…3600；三个毫秒参数不超过 600000，--max-open-ms 至少 500，另两个不小于 0。越界按参数错误退出。
//   会在桌面上弹窗：默认先取本机 gates 的 GUI 锁（与 scripts/lib/Locks.psm1 的 gui 锁是同一个命名互斥体），排队等。
// 退出码：0 跑完（有没有白帧看结果文件）；1 参数、加载或抓屏失败；3 等 GUI 锁超时；
// 4 到了硬上限（--max-seconds 再加 5 秒）自己结束，已跑完的轮次只在标准输出里。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PlatformWebView.h"
#include "util/DesignBox.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitLockTimeout = 3;
constexpr int kExitDeadline = 4;

// 编辑器窗口后面那块底的颜色：纯绿，开窗路径上任何一层都不会画它
const juce::Colour kBackdropColour(0, 224, 0);

double nowMs()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

struct Options
{
    juce::String plugin, out, framesDir, capture = "dxgi";
    bool whiteBackdrop = false; // 底换成白：模拟宿主的插件窗容器是白的，透底就记成白帧
    int opens = 20, maxOpenMs = 6000, afterRevealMs = 400, gapMs = 500, load = 0, fps = 240, x = 80, y = 80;
    int maxSeconds = 115;
    float white = 0.2f;
    bool guiLock = true;
};

bool parseOptions(const juce::StringArray& args, Options& o, juce::String& error)
{
    for (int i = 1; i < args.size(); ++i)
    {
        const auto& a = args[i];
        const auto value = [&]() -> juce::String { return i + 1 < args.size() ? args[++i] : juce::String(); };
        if (a == "--plugin")
            o.plugin = value();
        else if (a == "--out")
            o.out = value();
        else if (a == "--frames-dir")
            o.framesDir = value();
        else if (a == "--opens")
            o.opens = value().getIntValue();
        else if (a == "--max-open-ms")
            o.maxOpenMs = value().getIntValue();
        else if (a == "--after-reveal-ms")
            o.afterRevealMs = value().getIntValue();
        else if (a == "--gap-ms")
            o.gapMs = value().getIntValue();
        else if (a == "--load")
            o.load = value().getIntValue();
        else if (a == "--fps")
            o.fps = value().getIntValue();
        else if (a == "--white")
            o.white = value().getFloatValue();
        else if (a == "--x")
            o.x = value().getIntValue();
        else if (a == "--y")
            o.y = value().getIntValue();
        else if (a == "--max-seconds")
            o.maxSeconds = value().getIntValue();
        else if (a == "--white-backdrop")
            o.whiteBackdrop = true;
        else if (a == "--capture")
            o.capture = value();
        else if (a == "--no-gui-lock")
            o.guiLock = false;
        else
        {
            error = "unknown argument: " + a;
            return false;
        }
    }
    if (o.plugin.isEmpty() || o.out.isEmpty())
        error = "--plugin and --out are required";
    // 硬上限由 --max-seconds 算出：不许非正（换成无符号会绕成极大值，看门狗永不触发），也不许大到加 5 秒溢出
    else if (o.opens < 1 || o.maxOpenMs < 500 || o.maxOpenMs > 600000 || o.afterRevealMs < 0 ||
             o.afterRevealMs > 600000 || o.gapMs < 0 || o.gapMs > 600000 || o.maxSeconds < 1 || o.maxSeconds > 3600 ||
             o.fps < 30 || o.load < 0 || o.white <= 0.0f || o.white > 1.0f ||
             (o.capture != "dxgi" && o.capture != "gdi"))
        error = "argument out of range";
    return error.isEmpty();
}

// ---------------------------------------------------------------- 采样点与像素分类

struct Layout
{
    int w = 0, h = 0;
    std::vector<POINT> pts; // 物理像素，相对抓屏矩形左上角
    std::size_t grid = 0; // 前 grid 个是均匀网格（算各类像素占比只用它们），其后是四角与四边的密集点
};

Layout makeLayout(int w, int h, double dpi)
{
    Layout l;
    l.w = w;
    l.h = h;
    const int step = std::max(4, std::min(w, h) / 48);
    for (int y = step / 2; y < h; y += step)
        for (int x = step / 2; x < w; x += step)
            l.pts.push_back({x, y});
    l.grid = l.pts.size();
    // 四角：圆角与外圈色只在这里看得出来，每 2 像素一点
    const int c = std::min({static_cast<int>(std::lround(36.0 * dpi)), w / 2, h / 2});
    for (int y = 0; y < c; y += 2)
        for (int x = 0; x < c; x += 2)
            for (const POINT p : {POINT{x, y}, POINT{w - 1 - x, y}, POINT{x, h - 1 - y}, POINT{w - 1 - x, h - 1 - y}})
                l.pts.push_back(p);
    // 四边：外壳的 1px 边框与内阴影落在这里
    for (int inset = 0; inset < 3; ++inset)
    {
        for (int x = c; x < w - c; x += 8)
            for (const POINT p : {POINT{x, inset}, POINT{x, h - 1 - inset}})
                l.pts.push_back(p);
        for (int y = c; y < h - c; y += 8)
            for (const POINT p : {POINT{inset, y}, POINT{w - 1 - inset, y}})
                l.pts.push_back(p);
    }
    return l;
}

bool isWhite(int r, int g, int b)
{
    return r >= 245 && g >= 245 && b >= 245;
}
bool isMagenta(int r, int g, int b)
{
    return r >= 220 && g <= 40 && b >= 220;
}
bool isBackdrop(int r, int g, int b)
{
    return g >= 200 && r <= 40 && b <= 40;
}
bool isBlack(int r, int g, int b)
{
    return r <= 10 && g <= 10 && b <= 10;
}

struct Frame
{
    double t = 0; // 相对编辑器创建
    float white = 0, magenta = 0, backdrop = 0, black = 0; // 网格点里各类像素的占比
    std::vector<std::uint8_t> rgb; // 每个采样点 3 字节
};

// 两组采样的平均逐通道绝对差（0 到 255）
double meanDiff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, std::size_t from = 0)
{
    if (a.size() != b.size() || a.size() <= from * 3)
        return 255.0;
    double sum = 0;
    for (std::size_t i = from * 3; i < a.size(); ++i)
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return sum / static_cast<double>(a.size() - from * 3);
}

std::vector<std::uint8_t> sampleImage(const juce::Image& img, const Layout& l)
{
    std::vector<std::uint8_t> out;
    out.reserve(l.pts.size() * 3);
    const juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
    for (const auto& p : l.pts)
    {
        const auto c = bd.getPixelColour(p.x, p.y);
        out.push_back(c.getRed());
        out.push_back(c.getGreen());
        out.push_back(c.getBlue());
    }
    return out;
}

// 参照图：逻辑尺寸 lw × lh 的占位按 dpi 画成物理像素。mode 0 是旧的方形占位（渐变铺满），
// mode 1 是插件现行的 paintShellPlaceholder；mode 2 是几何标定用的变体（自选半径，可加 1px 边框）
juce::Image renderPlaceholder(const Layout& l, double dpi, float lw, float lh, float designW, float designH, int mode,
                              float radiusCss = 0.0f, bool border = false)
{
    juce::Image img(juce::Image::RGB, l.w, l.h, true, juce::SoftwareImageType());
    juce::Graphics g(img);
    g.addTransform(juce::AffineTransform::scale(static_cast<float>(dpi)));
    const juce::Rectangle<float> area(0.0f, 0.0f, lw, lh);
    if (mode == 0)
    {
        g.setGradientFill(rinne::webview::shellBackdropGradient(area));
        g.fillRect(area);
    }
    else if (mode == 1)
    {
        rinne::webview::paintShellPlaceholder(g, area, designW, designH);
    }
    else
    {
        const auto rim = rinne::webview::kShellRimRgb;
        g.setColour(juce::Colour(static_cast<juce::uint8>(rim >> 16), static_cast<juce::uint8>((rim >> 8) & 0xffu),
                                 static_cast<juce::uint8>(rim & 0xffu)));
        g.fillRect(area);
        const auto shell = rinne::webview::shellPlaceholderBounds(area, designW, designH);
        const float zoom = rinne::webview::shellFitZoom(lw, lh, designW, designH);
        g.setGradientFill(rinne::webview::shellBackdropGradient(shell));
        g.fillRoundedRectangle(shell, radiusCss * zoom);
        if (border)
        {
            g.setColour(juce::Colours::white.withAlpha(0.42f));
            g.drawRoundedRectangle(shell.reduced(0.5f * zoom), (radiusCss - 0.5f) * zoom, zoom);
        }
    }
    return img;
}

// ---------------------------------------------------------------- 子窗口时间线

struct WinInfo
{
    HWND h = nullptr;
    std::string cls;
    RECT r{};
    bool visible = false;
};

struct WinEvent
{
    double t = 0;
    std::string kind, cls;
    RECT r{}; // 相对抓屏矩形左上角，物理像素
    bool visible = false, inView = false;
};

BOOL CALLBACK collectChild(HWND h, LPARAM param)
{
    auto* out = reinterpret_cast<std::vector<WinInfo>*>(param);
    WinInfo w;
    w.h = h;
    char cls[128] = {};
    GetClassNameA(h, cls, static_cast<int>(sizeof(cls)));
    w.cls = cls;
    GetWindowRect(h, &w.r);
    w.visible = IsWindowVisible(h) != FALSE;
    out->push_back(std::move(w));
    return TRUE;
}

// ---------------------------------------------------------------- 桌面复制（DXGI）

// 屏幕 DC 的 BitBlt 在多显卡、虚拟显示器的机器上每次要等一次合成回读，实测只有十几帧每秒，漏得掉一两帧的白。
// 桌面复制按合成器的节奏交出每一张新合成的桌面，不靠轮询频率；两次取帧之间若合成了不止一张，
// AccumulatedFrames 会大于 1，记成「合并」，用来证明这次抓屏有没有漏帧。
class Dupl
{
public:
    bool open(RECT screen, juce::String& why)
    {
        why.clear(); // 只在失败时写原因：先清掉，重开成功后不留上一次失败的旧文本
        close();
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return fail(why, "CreateDXGIFactory1 failed");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
        {
            ComPtr<IDXGIOutput> output;
            for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_OUTPUT_DESC desc{};
                output->GetDesc(&desc);
                const RECT& d = desc.DesktopCoordinates;
                if (screen.left < d.left || screen.top < d.top || screen.right > d.right || screen.bottom > d.bottom)
                    continue;
                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output.As(&output1)) ||
                    FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &device_, nullptr, &context_)))
                    return fail(why, "no D3D11 device on the adapter of this output");
                const HRESULT hr = output1->DuplicateOutput(device_.Get(), &dup_);
                if (FAILED(hr))
                    return fail(why, "DuplicateOutput failed 0x" + juce::String::toHexString(static_cast<int>(hr)));
                box_ = {};
                box_.left = static_cast<UINT>(screen.left - d.left);
                box_.top = static_cast<UINT>(screen.top - d.top);
                box_.right = static_cast<UINT>(screen.right - d.left);
                box_.bottom = static_cast<UINT>(screen.bottom - d.top);
                box_.back = 1;
                D3D11_TEXTURE2D_DESC td{};
                td.Width = box_.right - box_.left;
                td.Height = box_.bottom - box_.top;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_)))
                    return fail(why, "cannot create the staging texture");
                return true;
            }
        }
        return fail(why, "no DXGI output contains the capture rectangle");
    }

    void close()
    {
        staging_.Reset();
        dup_.Reset();
        context_.Reset();
        device_.Reset();
    }

    // 1 取到一张新桌面并已拷进 out（BGRA，紧排）；0 这段时间里没有新合成；
    // 2 有新合成却没读出来（取纹理或映射失败），accumulated 照填，调用方记成丢弃；-1 复制失效（例如切了全屏或锁屏）
    int next(UINT timeoutMs, std::uint8_t* out, double& presentMs, UINT& accumulated)
    {
        using Microsoft::WRL::ComPtr;
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return 0;
        if (FAILED(hr))
            return -1;
        int got = 0;
        if (info.LastPresentTime.QuadPart != 0) // 只动了鼠标的帧不算
        {
            accumulated = info.AccumulatedFrames;
            got = 2; // 下面读出来了才改成 1
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture)))
            {
                context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture.Get(), 0, &box_);
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map)))
                {
                    const UINT rowBytes = (box_.right - box_.left) * 4;
                    for (UINT y = 0; y < box_.bottom - box_.top; ++y)
                        std::memcpy(out + y * rowBytes, static_cast<const std::uint8_t*>(map.pData) + y * map.RowPitch,
                                    rowBytes);
                    context_->Unmap(staging_.Get(), 0);
                    LARGE_INTEGER f;
                    QueryPerformanceFrequency(&f);
                    presentMs =
                        static_cast<double>(info.LastPresentTime.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
                    got = 1;
                }
            }
        }
        dup_->ReleaseFrame();
        return got;
    }

private:
    static bool fail(juce::String& why, const juce::String& text)
    {
        why = text;
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    D3D11_BOX box_{};
};

// ---------------------------------------------------------------- 抓屏线程

// 留存的整帧（--frames-dir 时每轮最多留 keep 张）：白帧，以及抓到过别的东西之后又见绿底的帧（是不是透底由事后分类判）
struct Kept
{
    double t = 0;
    const char* kind = "";
    juce::Image img;
};

class Capture
{
public:
    ~Capture()
    {
        end();
        dupl_.close();
    }

    bool begin(HWND root, RECT screen, const Layout& layout, double t0, const Options& o, juce::String& error)
    {
        root_ = root;
        screen_ = screen;
        layout_ = &layout;
        t0_ = t0;
        interval_ = 1000.0 / o.fps;
        whiteThreshold_ = o.white;
        keepWhite_ = o.framesDir.isNotEmpty() ? 2 : 0;
        frames_.clear();
        frames_.reserve(4096);
        windows_.clear();
        kept_.clear();
        seenEditor_ = false;
        known_.clear();
        coalesced_ = 0;
        dropped_ = 0;
        lost_ = 0;
        stopped_ = false;
        note_.clear();
        const int w = screen.right - screen.left, h = screen.bottom - screen.top;
        dxgi_ = false;
        if (o.capture == "dxgi")
        {
            buffer_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
            // 桌面复制只建一次、跨轮复用：建 D3D 设备与复制对象有一次性开销，放进每轮开窗会拖慢开窗时间线
            if (!duplOpen_ || !EqualRect(&duplRect_, &screen))
            {
                duplOpen_ = dupl_.open(screen, duplNote_);
                duplRect_ = screen;
            }
            dxgi_ = duplOpen_;
            note_ = duplNote_;
        }
        if (dxgi_)
        {
            bits_ = buffer_.data();
            stop_ = false;
            thread_ = std::thread([this] { run(); });
            return true;
        }
        screenDc_ = GetDC(nullptr);
        memDc_ = CreateCompatibleDC(screenDc_);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (screenDc_ == nullptr || memDc_ == nullptr || dib_ == nullptr || bits_ == nullptr)
        {
            error = "cannot create the capture surface";
            end();
            return false;
        }
        old_ = SelectObject(memDc_, dib_);
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void end()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        if (dxgi_)
            bits_ = nullptr;
        if (memDc_ != nullptr)
        {
            SelectObject(memDc_, old_);
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
        if (dib_ != nullptr)
        {
            DeleteObject(dib_);
            dib_ = nullptr;
            bits_ = nullptr;
        }
        if (screenDc_ != nullptr)
        {
            ReleaseDC(nullptr, screenDc_);
            screenDc_ = nullptr;
        }
    }

    std::vector<Frame>& frames() { return frames_; }
    const std::vector<WinEvent>& windows() const { return windows_; }
    const std::vector<Kept>& kept() const { return kept_; }
    const char* backend() const { return dxgi_ ? "dxgi" : "gdi"; }
    // 桌面复制两次取帧之间多合成的张数之和、取到却没读出来的张数（两者大于 0 都说明有帧没看到）、复制失效次数、
    // 本轮抓屏是否中途停掉、退回 GDI 或停掉的原因。这几项都由抓屏线程写，end() 汇合之后才读
    int coalesced() const { return coalesced_; }
    int dropped() const { return dropped_; }
    int lost() const { return lost_; }
    bool stopped() const { return stopped_; }
    const juce::String& note() const { return note_; }

private:
    void run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const int w = screen_.right - screen_.left, h = screen_.bottom - screen_.top;
        if (dxgi_)
        {
            bool first = true; // 本轮第一张带着上一轮以来攒下的合并数，不算
            int lostInRow = 0;
            while (!stop_)
            {
                double present = 0;
                UINT accumulated = 0;
                const int got = dupl_.next(5, buffer_.data(), present, accumulated);
                if (got == 1)
                {
                    sample(present, w);
                    coalesced_ += (!first && accumulated > 1) ? static_cast<int>(accumulated) - 1 : 0;
                    first = false;
                    lostInRow = 0;
                }
                else if (got == 2)
                {
                    // 这一张没读出来：它盖住的合成张数都算丢弃（本轮第一张只算它自己，理由同上面的合并数）
                    dropped_ += first ? 1 : static_cast<int>(std::max<UINT>(accumulated, 1));
                    first = false;
                }
                else if (got < 0)
                {
                    // 复制失效（切到安全桌面、UAC、锁屏、显示模式变化）：歇一下再重开。重开后一直取不到帧就放弃本轮抓屏，
                    // 免得在最高优先级上反复重建、不让出 CPU；复制对象一并关掉，下一轮 begin 重开
                    ++lost_;
                    if (++lostInRow > kMaxLostInRow)
                    {
                        note_ = "desktop duplication kept getting lost; capture stopped for this open";
                        dupl_.close();
                        duplOpen_ = false;
                        stopped_ = true;
                        break;
                    }
                    Sleep(kLostRetryMs);
                    duplOpen_ = dupl_.open(screen_, duplNote_);
                    note_ = duplNote_;
                    first = true;
                    if (!duplOpen_)
                    {
                        stopped_ = true;
                        break;
                    }
                }
                watchWindows(nowMs());
            }
            return;
        }
        double next = nowMs();
        while (!stop_)
        {
            const double t = nowMs();
            if (BitBlt(memDc_, 0, 0, w, h, screenDc_, screen_.left, screen_.top, SRCCOPY) != FALSE)
            {
                GdiFlush();
                sample(t, w);
            }
            watchWindows(t);
            next += interval_;
            for (double left = next - nowMs(); left > 0 && !stop_; left = next - nowMs())
            {
                if (left > 1.5)
                    Sleep(1);
                else
                    SwitchToThread();
            }
            if (nowMs() - next > interval_ * 4)
                next = nowMs(); // 落后太多就不追，免得连拍
        }
    }

    void sample(double t, int w)
    {
        Frame f;
        f.t = t - t0_;
        f.rgb.reserve(layout_->pts.size() * 3);
        const auto* px = static_cast<const std::uint8_t*>(bits_);
        int white = 0, magenta = 0, backdrop = 0, black = 0;
        for (std::size_t i = 0; i < layout_->pts.size(); ++i)
        {
            const auto& p = layout_->pts[i];
            const auto* q = px + (static_cast<std::size_t>(p.y) * static_cast<std::size_t>(w) + p.x) * 4; // BGRA
            const int b = q[0], g = q[1], r = q[2];
            f.rgb.push_back(static_cast<std::uint8_t>(r));
            f.rgb.push_back(static_cast<std::uint8_t>(g));
            f.rgb.push_back(static_cast<std::uint8_t>(b));
            if (i < layout_->grid)
            {
                white += isWhite(r, g, b);
                magenta += isMagenta(r, g, b);
                backdrop += isBackdrop(r, g, b);
                black += isBlack(r, g, b);
            }
        }
        const auto n = static_cast<float>(std::max<std::size_t>(1, layout_->grid));
        f.white = white / n;
        f.magenta = magenta / n;
        f.backdrop = backdrop / n;
        f.black = black / n;
        const bool backdropAgain = seenEditor_ && f.backdrop >= 0.5f;
        seenEditor_ = seenEditor_ || f.backdrop < 0.5f;
        if ((f.white >= whiteThreshold_ || backdropAgain) && static_cast<int>(kept_.size()) < keepWhite_)
        {
            const int h = screen_.bottom - screen_.top;
            juce::Image img(juce::Image::RGB, w, h, false, juce::SoftwareImageType());
            juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const auto* q = px + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4;
                    bd.setPixelColour(x, y, juce::Colour(q[2], q[1], q[0]));
                }
            kept_.push_back({f.t, backdropAgain ? "backdrop" : "white", img});
        }
        frames_.push_back(std::move(f));
    }

    // 编辑器窗口下的全部子窗口：出现、消失、位置或可见性变了各记一条
    void watchWindows(double t)
    {
        std::vector<WinInfo> now;
        EnumChildWindows(root_, collectChild, reinterpret_cast<LPARAM>(&now));
        const auto record = [&](const char* kind, const WinInfo& wi) {
            WinEvent e;
            e.t = t - t0_;
            e.kind = kind;
            e.cls = wi.cls;
            e.r = {wi.r.left - screen_.left, wi.r.top - screen_.top, wi.r.right - screen_.left,
                   wi.r.bottom - screen_.top};
            e.visible = wi.visible;
            RECT inter{};
            e.inView = IntersectRect(&inter, &wi.r, &screen_) != FALSE;
            windows_.push_back(std::move(e));
        };
        for (const auto& wi : now)
        {
            const auto it = std::find_if(known_.begin(), known_.end(), [&](const WinInfo& k) { return k.h == wi.h; });
            if (it == known_.end())
                record("appeared", wi);
            else if (!EqualRect(&it->r, &wi.r) || it->visible != wi.visible)
                record("changed", wi);
        }
        for (const auto& k : known_)
            if (std::none_of(now.begin(), now.end(), [&](const WinInfo& wi) { return wi.h == k.h; }))
                record("gone", k);
        known_ = std::move(now);
    }

    HWND root_ = nullptr;
    RECT screen_{};
    const Layout* layout_ = nullptr;
    double t0_ = 0, interval_ = 4;
    float whiteThreshold_ = 0.2f;
    int keepWhite_ = 0;
    HDC screenDc_ = nullptr, memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    // 复制失效后的重开节奏：每次先歇 50 ms，连续 20 次（约 1 秒）一帧都没取到就停掉本轮抓屏
    static constexpr int kLostRetryMs = 50;
    static constexpr int kMaxLostInRow = 20;
    Dupl dupl_;
    bool dxgi_ = false;
    bool duplOpen_ = false;
    RECT duplRect_{};
    juce::String duplNote_;
    std::vector<std::uint8_t> buffer_;
    int coalesced_ = 0, dropped_ = 0, lost_ = 0;
    bool stopped_ = false;
    juce::String note_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::vector<Frame> frames_;
    std::vector<WinEvent> windows_;
    std::vector<WinInfo> known_;
    std::vector<Kept> kept_;
    bool seenEditor_ = false; // 这一轮已经抓到过不是绿底的帧：此后再见绿底就留一张
};

// ---------------------------------------------------------------- 调试输出（DBWIN）

// 插件的诊断行经 JUCE 的日志走系统调试输出；没有调试器挂着时，系统把它写进 DBWIN 共享缓冲。
// 在这里当一个 DBWIN 读者，只留本进程的行。系统里已有别的读者（例如调试输出查看器）时让位，不抢
class Dbwin
{
public:
    ~Dbwin() { stop(); }

    bool start()
    {
        ready_ = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
        if (ready_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
            return closeAll();
        data_ = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
        if (data_ == nullptr || map_ == nullptr)
            return closeAll();
        view_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 4096);
        if (view_ == nullptr)
            return closeAll();
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        closeAll();
    }

    std::vector<std::pair<double, std::string>> lines() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    // since 之后第一条含 needle 的行的时刻，没有返回负数
    double find(const char* needle, double since) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [t, text] : lines_)
            if (t >= since && text.find(needle) != std::string::npos)
                return t;
        return -1.0;
    }

private:
    bool closeAll()
    {
        if (view_ != nullptr)
            UnmapViewOfFile(view_);
        for (HANDLE* h : {&ready_, &data_, &map_})
            if (*h != nullptr)
            {
                CloseHandle(*h);
                *h = nullptr;
            }
        view_ = nullptr;
        return false;
    }

    void run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        const DWORD self = GetCurrentProcessId();
        SetEvent(ready_);
        while (!stop_)
        {
            if (WaitForSingleObject(data_, 50) != WAIT_OBJECT_0)
                continue;
            const auto* p = static_cast<const char*>(view_);
            DWORD pid = 0;
            std::memcpy(&pid, p, sizeof(pid));
            if (pid == self)
            {
                std::string text(p + sizeof(DWORD), strnlen(p + sizeof(DWORD), 4096 - sizeof(DWORD)));
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                const std::lock_guard<std::mutex> lock(mutex_);
                lines_.emplace_back(nowMs(), std::move(text));
            }
            SetEvent(ready_);
        }
    }

    HANDLE ready_ = nullptr, data_ = nullptr, map_ = nullptr;
    void* view_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::pair<double, std::string>> lines_;
};

// ---------------------------------------------------------------- 负载

class BusyLoad
{
public:
    ~BusyLoad() { stop(); }
    void start(int threads)
    {
        for (int i = 0; i < threads; ++i)
            threads_.emplace_back([this] {
                double x = 1.0;
                while (!stop_.load(std::memory_order_relaxed))
                    x = x * 1.0000001 + 1e-9;
                sink_.store(x, std::memory_order_relaxed);
            });
    }
    void stop()
    {
        stop_ = true;
        for (auto& t : threads_)
            t.join();
        threads_.clear();
    }

private:
    std::atomic<bool> stop_{false};
    std::atomic<double> sink_{0.0};
    std::vector<std::thread> threads_;
};

// ---------------------------------------------------------------- 窗口

class SolidWindow final : public juce::Component
{
public:
    explicit SolidWindow(juce::Colour c) : colour_(c) { setOpaque(true); }
    void paint(juce::Graphics& g) override { g.fillAll(colour_); }

private:
    juce::Colour colour_;
};

int refreshRateOf(HWND h)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) == FALSE)
        return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    return EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) != FALSE
               ? static_cast<int>(dm.dmDisplayFrequency)
               : 0;
}

juce::var rectVar(const RECT& r)
{
    juce::Array<juce::var> a{static_cast<int>(r.left), static_cast<int>(r.top), static_cast<int>(r.right - r.left),
                             static_cast<int>(r.bottom - r.top)};
    return a;
}

// ---------------------------------------------------------------- 驱动：开窗、等揭幕、关窗、分析

class Driver final : private juce::Timer
{
public:
    explicit Driver(Options o) : opts_(std::move(o)) {}

    bool prepare(juce::String& error)
    {
        juce::OwnedArray<juce::PluginDescription> types;
        juce::VST3PluginFormat vst3;
        vst3.findAllTypesForFile(types, opts_.plugin);
        if (types.isEmpty())
        {
            error = "no VST3 plug-in found in " + opts_.plugin;
            return false;
        }
        manager_.addFormat(new juce::VST3PluginFormat());
        instance_ = manager_.createPluginInstance(*types[0], 48000.0, 512, error);
        if (instance_ == nullptr)
            return false;
        const bool input = instance_->getName().containsIgnoreCase("input");
        designW_ = static_cast<float>(input ? rinne::design::kInputDesignW : rinne::design::kOutputDesignW);
        designH_ = static_cast<float>(input ? rinne::design::kInputDesignH : rinne::design::kOutputDesignH);
        dbwinOk_ = dbwin_.start();
        if (opts_.load > 0)
            load_.start(opts_.load);
        runStart_ = nowMs();
        startTimer(10);
        return true;
    }

    int exitCode() const { return exitCode_; }

private:
    enum class State
    {
        next,
        open,
        gap
    };

    struct OpenResult
    {
        juce::var json;
        bool whiteAfterOurs = false, whiteBeforeOurs = false, dxgi = false, captureStopped = false;
        int whiteFrames = 0, coalesced = 0, dropped = 0, seeThrough = 0;
        double fps = 0;
    };

    void timerCallback() override
    {
        const double t = nowMs();
        if (state_ == State::next)
        {
            const double expected = (opts_.maxOpenMs + opts_.gapMs) / 1000.0;
            if (index_ >= opts_.opens || (t - runStart_) / 1000.0 + expected > opts_.maxSeconds)
                return finish();
            if (!openOne())
                return finish();
            state_ = State::open;
        }
        else if (state_ == State::open)
        {
            const double revealed = dbwin_.find("webview revealed", t0_);
            const double fallback = dbwin_.find("fallback ", t0_);
            const double settled = std::max(revealed, fallback);
            if ((settled >= 0 && t - settled >= opts_.afterRevealMs) || t - tVisible_ >= opts_.maxOpenMs)
            {
                closeOne();
                state_ = State::gap;
                gapUntil_ = std::max(nowMs() + opts_.gapMs, tClose_ + kCloseLineMs);
            }
        }
        else if (t >= gapUntil_)
        {
            analysePending();
            state_ = State::next;
        }
    }

    bool openOne()
    {
        t0_ = nowMs();
        editor_.reset(instance_->createEditorIfNeeded());
        if (editor_ == nullptr)
        {
            fatal_ = "the plug-in has no editor";
            return false;
        }
        holder_ = std::make_unique<SolidWindow>(juce::Colours::black);
        holder_->setSize(editor_->getWidth(), editor_->getHeight());
        holder_->addAndMakeVisible(*editor_);
        holder_->setTopLeftPosition(opts_.x, opts_.y);
        holder_->setVisible(false);
        holder_->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        holder_->setAlwaysOnTop(true);
        if (backdrop_ == nullptr)
        {
            backdrop_ = std::make_unique<SolidWindow>(opts_.whiteBackdrop ? juce::Colours::white : kBackdropColour);
            backdrop_->addToDesktop(juce::ComponentPeer::windowIsTemporary);
            backdrop_->setAlwaysOnTop(true);
            backdrop_->setVisible(true);
        }
        backdrop_->setBounds(holder_->getBounds().expanded(24)); // 每轮跟着编辑器的尺寸走，底始终盖住编辑器外圈
        const auto hwnd = static_cast<HWND>(holder_->getPeer()->getNativeHandle());
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        screen_ = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
        logicalW_ = static_cast<float>(holder_->getWidth());
        logicalH_ = static_cast<float>(holder_->getHeight());
        dpi_ = logicalW_ > 0 ? client.right / logicalW_ : 1.0;
        if (client.right <= 0 || client.bottom <= 0)
        {
            fatal_ = "the editor window has no area";
            return false;
        }
        if (layout_.w != client.right || layout_.h != client.bottom)
        {
            layout_ = makeLayout(client.right, client.bottom, dpi_);
            refSquare_ =
                sampleImage(renderPlaceholder(layout_, dpi_, logicalW_, logicalH_, designW_, designH_, 0), layout_);
            refRounded_ =
                sampleImage(renderPlaceholder(layout_, dpi_, logicalW_, logicalH_, designW_, designH_, 1), layout_);
        }
        refreshHz_ = refreshRateOf(hwnd);
        juce::String error;
        if (!capture_.begin(hwnd, screen_, layout_, t0_, opts_, error))
        {
            fatal_ = error;
            return false;
        }
        Sleep(30); // 先抓几帧底：编辑器窗口还没上屏时应当全是绿底
        holder_->setVisible(true);
        holder_->toFront(false);
        tVisible_ = nowMs();
        return true;
    }

    // 关窗只收窗口，分析推到间隔结束：关窗诊断行在编辑器析构时才写，经 DBWIN 读者线程异步进来，
    // 紧接着分析会漏掉它。间隔至少留 kCloseLineMs，分析取的诊断行也截到关窗后 kCloseLineMs
    void closeOne()
    {
        capture_.end();
        tClose_ = nowMs();
        releaseEditor();
        pending_ = true;
    }

    // 分析上一轮（抓到的帧与窗口时间线留在 capture_ 里，下一轮 begin 才清）。没有待分析的一轮时是空操作
    void analysePending()
    {
        if (!pending_)
            return;
        pending_ = false;
        results_.push_back(analyse(tClose_));
        ++index_;
    }

    // 收掉当前这一轮的窗口：抓屏先停，编辑器从窗口上摘下来再析构，然后才是窗口。插件实例必须比它的编辑器活得久，
    // 所以 finish() 在析构实例之前也调这里：开窗中途失败（窗口没有面积、抓屏起不来）时编辑器和窗口都还在。
    // 没有在开的一轮时是空操作
    void releaseEditor()
    {
        capture_.end();
        if (holder_ != nullptr && editor_ != nullptr)
            holder_->removeChildComponent(editor_.get());
        editor_.reset(); // 插件侧的编辑器在这里析构，关窗诊断行随之写出
        holder_.reset();
    }

    OpenResult analyse(double tClose)
    {
        auto& frames = capture_.frames();
        OpenResult r;
        auto* o = new juce::DynamicObject();
        juce::var json(o);
        o->setProperty("index", index_);
        o->setProperty("visibleAtMs", tVisible_ - t0_);
        o->setProperty("closedAtMs", tClose - t0_);

        // 诊断行：本次开窗起点之后、关窗后 kCloseLineMs 之内（关窗行在关窗之后才到）
        juce::Array<juce::var> diag;
        double revealT = -1, navT = -1;
        for (const auto& [t, text] : dbwin_.lines())
        {
            if (t < t0_ - 5 || t > tClose + kCloseLineMs)
                continue;
            auto* d = new juce::DynamicObject();
            d->setProperty("t", juce::roundToInt(t - t0_));
            d->setProperty("line", juce::String(text));
            diag.add(juce::var(d));
            if (revealT < 0 && text.find("webview revealed") != std::string::npos)
                revealT = t - t0_;
            if (navT < 0 && text.find("navigation started") != std::string::npos)
                navT = t - t0_;
        }
        o->setProperty("diag", diag);

        // 揭幕之后的最后一帧当内容参照；没揭幕就不认内容
        std::vector<std::uint8_t> refContent;
        if (revealT >= 0 && !frames.empty() && frames.back().t > revealT)
            refContent = frames.back().rgb;
        if (refContent.size() > 0 && geometry_.isVoid())
            geometry_ = calibrate(refContent);

        const char* names[] = {"backdrop", "black", "magenta", "white", "placeholder", "content", "other"};
        int counts[7] = {};
        juce::Array<juce::var> segments;
        int segClass = -1, segFrames = 0;
        double segStart = 0, segEnd = 0, firstOurs = -1, maxWhiteRun = 0, whiteRunStart = -1;
        int whiteBefore = 0, whiteAfter = 0, seeThrough = 0;
        const auto flush = [&] {
            if (segClass < 0)
                return;
            auto* s = new juce::DynamicObject();
            s->setProperty("class", names[segClass]);
            s->setProperty("from", juce::roundToInt(segStart));
            s->setProperty("to", juce::roundToInt(segEnd));
            s->setProperty("frames", segFrames);
            segments.add(juce::var(s));
        };
        // 编辑器窗口被要求上屏之前抓到的帧里不可能有它：一律记成底（白底时不让它们冒充「我方首帧前的白」，
        // 冷启动第一轮也不让底铺好之前的桌面残影记成过渡帧）
        const double visibleAt = tVisible_ - t0_;
        for (const auto& f : frames)
        {
            int c = 6;
            const double dPh = std::min(meanDiff(f.rgb, refSquare_), meanDiff(f.rgb, refRounded_));
            const double dCt = refContent.empty() ? 255.0 : meanDiff(f.rgb, refContent);
            if (f.t < visibleAt || f.backdrop >= 0.5f)
                c = 0;
            else if (f.magenta >= 0.5f)
                c = 2;
            else if (f.white >= opts_.white)
                c = 3;
            else if (f.black >= 0.5f)
                c = 1;
            else if (dPh <= kMatch && dCt <= kMatch)
                c = (revealT >= 0 && f.t >= revealT) ? 5 : 4;
            else if (dPh <= kMatch)
                c = 4;
            else if (dCt <= kMatch)
                c = 5;
            ++counts[c];
            // 透底：我方占位已经上过屏，又露出后面的绿底。真宿主上露出来的是宿主窗口后面的东西，常见是白或灰。
            // 我方首帧之前的绿底不算：那是宿主把窗口摆上屏、我方还没画第一笔的空档（冷启动第一轮最常见）
            if (c == 0 && firstOurs >= 0)
                ++seeThrough;
            if ((c == 2 || c == 4) && firstOurs < 0)
                firstOurs = f.t;
            if (c == 3)
            {
                (firstOurs < 0 ? whiteBefore : whiteAfter)++;
                if (whiteRunStart < 0)
                    whiteRunStart = f.t;
                maxWhiteRun = std::max(maxWhiteRun, f.t - whiteRunStart);
            }
            else
            {
                whiteRunStart = -1;
            }
            if (c != segClass)
            {
                flush();
                segClass = c;
                segStart = f.t;
                segFrames = 0;
            }
            segEnd = f.t;
            ++segFrames;
        }
        flush();

        auto* cnt = new juce::DynamicObject();
        for (int i = 0; i < 7; ++i)
            cnt->setProperty(names[i], counts[i]);
        o->setProperty("counts", juce::var(cnt));
        o->setProperty("segments", segments);
        o->setProperty("firstOursMs", juce::roundToInt(firstOurs));
        o->setProperty("revealMs", juce::roundToInt(revealT));
        o->setProperty("navStartedMs", juce::roundToInt(navT));
        o->setProperty("whiteBeforeOurs", whiteBefore);
        o->setProperty("whiteAfterOurs", whiteAfter);
        o->setProperty("seeThroughFrames", seeThrough);
        o->setProperty("longestWhiteMs", juce::roundToInt(maxWhiteRun));
        const double span = frames.size() > 1 ? frames.back().t - frames.front().t : 0.0;
        r.fps = span > 0 ? (static_cast<double>(frames.size()) - 1.0) * 1000.0 / span : 0.0;
        o->setProperty("frames", static_cast<int>(frames.size()));
        o->setProperty("fps", juce::roundToInt(r.fps));
        o->setProperty("capture", juce::String(capture_.backend()));
        o->setProperty("coalescedFrames", capture_.coalesced());
        o->setProperty("droppedFrames", capture_.dropped());
        o->setProperty("duplicationLost", capture_.lost());
        o->setProperty("captureStopped", capture_.stopped());
        if (capture_.note().isNotEmpty())
            o->setProperty("captureNote", capture_.note());
        r.coalesced = capture_.coalesced();
        r.dropped = capture_.dropped();
        r.captureStopped = capture_.stopped();
        r.dxgi = std::strcmp(capture_.backend(), "dxgi") == 0;

        juce::Array<juce::var> wins;
        for (const auto& e : capture_.windows())
        {
            auto* w = new juce::DynamicObject();
            w->setProperty("t", juce::roundToInt(e.t));
            w->setProperty("kind", juce::String(e.kind));
            w->setProperty("class", juce::String(e.cls));
            w->setProperty("rect", rectVar(e.r));
            w->setProperty("visible", e.visible);
            w->setProperty("inView", e.inView);
            wins.add(juce::var(w));
        }
        o->setProperty("windows", wins);

        if (opts_.framesDir.isNotEmpty())
        {
            const juce::File dir(opts_.framesDir);
            dir.createDirectory();
            for (const auto& k : capture_.kept())
            {
                juce::FileOutputStream out(dir.getChildFile("open" + juce::String(index_) + "-" +
                                                            juce::String(juce::roundToInt(k.t)) + "ms-" + k.kind +
                                                            ".png"));
                if (out.openedOk())
                {
                    out.setPosition(0);
                    out.truncate();
                    juce::PNGImageFormat().writeImageToStream(k.img, out);
                }
            }
        }

        r.json = json;
        r.whiteBeforeOurs = whiteBefore > 0;
        r.whiteAfterOurs = whiteAfter > 0;
        r.whiteFrames = counts[3];
        r.seeThrough = seeThrough;
        std::printf("open %d: %s %d frames %.0f fps coalesced %d dropped %d%s | white %d (before ours %d, after ours "
                    "%d) see-through %d black %d placeholder %d content %d magenta %d other %d | nav %d ms reveal %d "
                    "ms\n",
                    index_, capture_.backend(), static_cast<int>(frames.size()), r.fps, r.coalesced, r.dropped,
                    r.captureStopped ? " capture stopped" : "", counts[3], whiteBefore, whiteAfter, seeThrough,
                    counts[1], counts[4], counts[5], counts[2], counts[6], juce::roundToInt(navT),
                    juce::roundToInt(revealT));
        std::fflush(stdout);
        return r;
    }

    // 几何标定：揭幕后的内容帧只看四角与四边的采样点，与不同半径、有无 1px 边框的占位逐一比，
    // 半径 0 就是旧的方形占位。现行占位（mode 1）一并列出
    juce::var calibrate(const std::vector<std::uint8_t>& content)
    {
        auto* o = new juce::DynamicObject();
        juce::var json(o);
        const auto edgeDiff = [&](const juce::Image& img) {
            return meanDiff(sampleImage(img, layout_), content, layout_.grid);
        };
        o->setProperty("current",
                       edgeDiff(renderPlaceholder(layout_, dpi_, logicalW_, logicalH_, designW_, designH_, 1)));
        o->setProperty("square",
                       edgeDiff(renderPlaceholder(layout_, dpi_, logicalW_, logicalH_, designW_, designH_, 0)));
        juce::Array<juce::var> table;
        for (const float radius : {0.0f, 12.0f, 16.0f, 18.0f, 20.0f, 22.0f, 24.0f, 28.0f})
            for (const bool border : {false, true})
            {
                auto* v = new juce::DynamicObject();
                v->setProperty("radius", radius);
                v->setProperty("border", border);
                v->setProperty("diff", edgeDiff(renderPlaceholder(layout_, dpi_, logicalW_, logicalH_, designW_,
                                                                  designH_, 2, radius, border)));
                table.add(juce::var(v));
            }
        o->setProperty("variants", table);
        if (opts_.framesDir.isNotEmpty())
        {
            const juce::File dir(opts_.framesDir);
            dir.createDirectory();
            juce::FileOutputStream out(dir.getChildFile("ref-current-placeholder.png"));
            if (out.openedOk())
            {
                out.setPosition(0);
                out.truncate();
                juce::PNGImageFormat().writeImageToStream(
                    renderPlaceholder(layout_, dpi_, logicalW_, logicalH_, designW_, designH_, 1), out);
            }
        }
        return json;
    }

    void finish()
    {
        stopTimer();
        releaseEditor();
        analysePending(); // 正常路径上间隔结束时已经分析过；这里只兜住还没分析的最后一轮
        load_.stop();
        backdrop_.reset();
        auto* o = new juce::DynamicObject();
        juce::var json(o);
        o->setProperty("tool", "rinne_flashprobe");
        o->setProperty("plugin", juce::File(opts_.plugin).getFileName());
        o->setProperty("pluginName", instance_ != nullptr ? instance_->getName() : juce::String());
        o->setProperty("loadThreads", opts_.load);
        o->setProperty("logicalCpus", static_cast<int>(std::thread::hardware_concurrency()));
        // 进程优先级类从启动它的 shell 继承（WebView2 的子进程再从这里继承），负载下的结果要连着它一起看
        o->setProperty("priorityClass", static_cast<int>(GetPriorityClass(GetCurrentProcess())));
        o->setProperty("captureRequested", opts_.capture);
        o->setProperty("whiteBackdrop", opts_.whiteBackdrop);
        o->setProperty("targetFps", opts_.fps); // 只对 gdi 轮询有意义
        o->setProperty("whiteThreshold", opts_.white);
        o->setProperty("refreshHz", refreshHz_);
        o->setProperty("dpiScale", dpi_);
        o->setProperty("captureRect", rectVar({0, 0, layout_.w, layout_.h}));
        o->setProperty("dbwin", dbwinOk_);
        o->setProperty("seconds", (nowMs() - runStart_) / 1000.0);
        o->setProperty("error", fatal_);
        int opens = 0, withWhite = 0, withWhiteBefore = 0, withWhiteAfter = 0, whiteFrames = 0, coalesced = 0,
            dropped = 0, captureStopped = 0, dxgiOpens = 0, withSeeThrough = 0, seeThroughFrames = 0;
        double fpsSum = 0, fpsMin = 1e9;
        juce::Array<juce::var> list;
        for (const auto& r : results_)
        {
            ++opens;
            withWhite += r.whiteFrames > 0;
            withWhiteBefore += r.whiteBeforeOurs;
            withWhiteAfter += r.whiteAfterOurs;
            whiteFrames += r.whiteFrames;
            coalesced += r.coalesced;
            dropped += r.dropped;
            captureStopped += r.captureStopped;
            dxgiOpens += r.dxgi;
            withSeeThrough += r.seeThrough > 0;
            seeThroughFrames += r.seeThrough;
            fpsSum += r.fps;
            fpsMin = std::min(fpsMin, r.fps);
            list.add(r.json);
        }
        auto* s = new juce::DynamicObject();
        s->setProperty("opens", opens);
        s->setProperty("opensWithWhite", withWhite);
        s->setProperty("opensWithWhiteBeforeOurs", withWhiteBefore);
        s->setProperty("opensWithWhiteAfterOurs", withWhiteAfter);
        s->setProperty("whiteFrames", whiteFrames);
        s->setProperty("fpsMean", opens > 0 ? juce::roundToInt(fpsSum / opens) : 0);
        s->setProperty("fpsMin", opens > 0 ? juce::roundToInt(fpsMin) : 0);
        s->setProperty("opensCapturedByDuplication", dxgiOpens);
        s->setProperty("coalescedFrames", coalesced);
        s->setProperty("droppedFrames", dropped);
        // 抓屏中途停掉的开窗：这几轮的白帧与透底统计只覆盖停掉之前，读汇总时要把它们扣出来看
        s->setProperty("opensCaptureStopped", captureStopped);
        s->setProperty("opensWithSeeThrough", withSeeThrough);
        s->setProperty("seeThroughFrames", seeThroughFrames);
        o->setProperty("summary", juce::var(s));
        o->setProperty("geometry", geometry_);
        o->setProperty("opensDetail", list);
        const juce::File out(opts_.out);
        out.getParentDirectory().createDirectory();
        if (!out.replaceWithText(juce::JSON::toString(json, false)))
            fatal_ = fatal_.isNotEmpty() ? fatal_ : juce::String("cannot write the result file");
        std::printf("summary: %d opens (%d by duplication, %d coalesced frames, %d dropped frames, capture stopped "
                    "in %d), white in %d (before ours %d, after ours %d), %d white frames, see-through in %d (%d "
                    "frames), fps mean %d min %d%s%s\n",
                    opens, dxgiOpens, coalesced, dropped, captureStopped, withWhite, withWhiteBefore, withWhiteAfter,
                    whiteFrames, withSeeThrough, seeThroughFrames, opens > 0 ? juce::roundToInt(fpsSum / opens) : 0,
                    opens > 0 ? juce::roundToInt(fpsMin) : 0, fatal_.isNotEmpty() ? " | error: " : "",
                    fatal_.toRawUTF8());
        std::fflush(stdout);
        exitCode_ = fatal_.isNotEmpty() ? kExitError : kExitOk;
        dbwin_.stop();
        instance_.reset();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }

    // 采样平均差不超过它就算「相符」（逐通道 0 到 255）：两份 JUCE 的渲染差与抗锯齿都在这之内
    static constexpr double kMatch = 6.0;
    // 关窗诊断行经 DBWIN 异步进来的等待：间隔至少这么长，分析取的诊断行也截到关窗后这么久
    static constexpr double kCloseLineMs = 300.0;

    Options opts_;
    juce::AudioPluginFormatManager manager_;
    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    std::unique_ptr<SolidWindow> holder_, backdrop_;
    Dbwin dbwin_;
    BusyLoad load_;
    Capture capture_;
    Layout layout_;
    std::vector<std::uint8_t> refSquare_, refRounded_;
    std::vector<OpenResult> results_;
    juce::var geometry_;
    RECT screen_{};
    State state_ = State::next;
    int index_ = 0, refreshHz_ = 0, exitCode_ = kExitOk;
    float designW_ = 0, designH_ = 0, logicalW_ = 0, logicalH_ = 0;
    double dpi_ = 1.0, t0_ = 0, tVisible_ = 0, tClose_ = 0, gapUntil_ = 0, runStart_ = 0;
    bool dbwinOk_ = false, pending_ = false;
    juce::String fatal_;
};

} // namespace

int main(int argc, char* argv[])
{
    // 物理像素口径：抓屏矩形、窗口矩形都按物理像素取
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    Options opts;
    juce::String error;
    if (!parseOptions(juce::StringArray(argv, argc), opts, error))
    {
        std::fprintf(stderr, "rinne_flashprobe: %s\n", error.toRawUTF8());
        return kExitError;
    }

    HANDLE guiLock = nullptr;
    if (opts.guiLock)
    {
        guiLock = CreateMutexW(nullptr, FALSE, L"Local\\Rinne-gui");
        std::printf("waiting for the gui lock...\n");
        std::fflush(stdout);
        const DWORD got = guiLock != nullptr ? WaitForSingleObject(guiLock, 30 * 60 * 1000) : WAIT_FAILED;
        if (got != WAIT_OBJECT_0 && got != WAIT_ABANDONED)
        {
            std::fprintf(stderr, "rinne_flashprobe: gui lock not acquired\n");
            return kExitLockTimeout;
        }
    }
    std::printf("gui lock acquired\n");
    std::fflush(stdout);

    // 硬上限：机器被压满时整条消息线程都可能停住，轮次之间的时长检查就不起作用了。到点直接结束进程，
    // 不让本工具占着 GUI 锁超过约定时长（内核在进程结束时交出互斥体，下一个等待者按「被遗弃」接手）。
    // 已经跑完的轮次在标准输出里各有一行，结果文件这时不写。
    // 这条线程自己也会被压住：按墙钟判到点（不数 Sleep 的次数），并提到最高优先级，否则负载一重它也跟着拖
    std::atomic<bool> done{false};
    std::thread deadline([&done, limit = opts.maxSeconds + 5] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const ULONGLONG start = GetTickCount64();
        while (!done && GetTickCount64() - start < static_cast<ULONGLONG>(limit) * 1000u)
            Sleep(100);
        if (!done)
        {
            std::printf("hard deadline (%d s) reached, terminating\n", limit);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(kExitDeadline));
        }
    });

    int code = kExitOk;
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        Driver driver(opts);
        if (driver.prepare(error))
        {
            juce::MessageManager::getInstance()->runDispatchLoop();
            code = driver.exitCode();
        }
        else
        {
            std::fprintf(stderr, "rinne_flashprobe: %s\n", error.toRawUTF8());
            code = kExitError;
        }
    }

    done = true;
    deadline.join();
    if (guiLock != nullptr)
    {
        ReleaseMutex(guiLock);
        CloseHandle(guiLock);
    }
    timeEndPeriod(1);
    return code;
}
