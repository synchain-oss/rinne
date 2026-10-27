// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cstdint>

namespace rinne::webview
{

// 开窗遮挡闸：「WebView 子窗口此刻该不该待在可视区之外」的唯一判定处。纯逻辑，不碰任何组件，宿主测试直接编它。
//
// 要解决的事：WebView2 的宿主窗口一旦上屏，在合成出页面首帧之前画什么，插件侧没有任何接口管得到，
// 实际表现是白、黑、白、内容的闪烁。所以不让它上屏：从导航开始到页面首帧已绘之间，把 WebView 子窗口
// 挪到本组件可视区之外，那块地方由 WebViewHost::paint 铺与外壳渐变同组色标的占位渐变。
//
// 为什么挪走，不用 setVisible(false) 或零尺寸：
//   - JUCE 在组件被隐藏时会把页面导航到空白页（Options 里 keepPageLoadedWhenBrowserIsHidden 默认 false），
//     隐藏一下就把正在加载的页面顶掉；
//   - WebView2 在不可见时会停止渲染，页内等动画帧的首帧信号可能因此永远等不到；
//   - 零尺寸会把视口压成 0，页面按 0 宽布局，放回来时整页重排。
//   挪走则尺寸不变、所属组件的可见性不变、页面仍按真实视口出帧。
//
// 已知风险（只能在真宿主上看，单测覆盖不到）：被系统裁到零可见面积的子窗口，Chromium 也可能停止出帧，
// 页内首帧信号随之不到，每次开窗都吃满兜底时长。命中时是静默降级（多占位几秒），不是崩溃或卡死，
// 所以首帧信号到达要留诊断行，兜底放行也要留诊断行，两行合起来才能看出是哪一条路。
//
// 为什么要等导航开始才挪：控制器创建完成之前的重试泵挂在 HostWebView::paint 上，挪出可视区后不再重绘，
// 泵就停了。导航开始说明控制器已建好，此后泵本来就是空调用。控制器建好之前一直可见，既保住了泵，
// 屏上那一段也是我方铺的占位，不会露白。
//
// 放行只认首帧：
//   页面加载完成回调只说明文档下载完、load 事件发了，不保证任何一帧已经合成。两个回调谁先到不稳定，
//   在不同角色的页面上顺序可以相反，拿它放行的那几次露出的正是 WebView2 首帧之前的底色。
//   所以加载完成只记账（进超时那一行诊断），放行只有三条路：
//     firstFrame  前端发的首帧信号（唯一的正常路），武装后在下一个定时 tick 上生效；
//     timeout     信号没来时的兜底，绝不允许永远不放行；
//     fallback    看门狗切了兜底面板，面板自己铺满本组件，这不算放行。
//
// 首帧信号到了为什么还要再等一拍：信号保证页内某一帧已提交给合成器，从提交到真正上屏还差一拍。
// 信号一到就立刻挪回来，仍可能在这一拍里露出底色。所以收到信号只武装，真正放行落在后面的 tick 上，
// 且要同时满足两个条件：至少再回一次消息循环（tick 数），以及自信号到达起过了一段毫秒下界。
// 只数 tick 不够：信号到达点相对 tick 相位随机，实际等待可以落在 0 到一个 tick 周期之间，下界是 0。
// 代价上界：tick 数为 1 时，最坏等待 = kRevealSettleMs + 一个 tick 周期（紧界，逼近但取不到）；
// tick 数大于 1 时通式是 max(T * 周期, kRevealSettleMs + 周期)，改 kRevealSettleTicks 前要重新推。
//
// 前端的首帧信号可能因页内宿主对象迟到而晚发。信号若在兜底放行之后才到，走 !parked_ 分支，是无害的空操作：
// 不会重复放行，但那次开窗实际走的是兜底，不是等到真的画过一帧再放行。
class RevealGate
{
public:
    // 首帧信号没来时的兜底上界。取 3 秒，比看门狗的导航后预算短，保证宁可早放行看见一点底色，
    // 也不会拖到看门狗切兜底面板那一步。「短」有两个半边：常量关系由 WebViewHost.h 里的编译期断言守；
    // 前提是两个预算同时起算，靠 beginLoadAttempt 重新武装。
    static constexpr int kRevealFallbackMs = 3000;

    // 首帧信号到达后压住的两个条件，onTick 里必须同时满足才放行
    static constexpr int kRevealSettleTicks = 1; // 至少再回一次消息循环
    static constexpr int kRevealSettleMs = 64; // 约四个 60Hz 合成帧的余量

    // 一次新的加载尝试开始（构造与重试共用）：重新武装，允许下一次导航再挪一次
    void beginLoadAttempt() noexcept
    {
        parked_ = false;
        revealed_ = false;
        settling_ = false;
        settleTicksSeen_ = 0;
        settleAtMs_ = 0;
        navFinishedSeen_ = false;
        revealReason_ = "";
    }

    // 导航开始（控制器已建好）。已经放行过就不再挪：页面自己再导航一次时重新遮一下比那点底色更难看
    void onNavigationStarted(std::uint32_t nowMs) noexcept
    {
        if (revealed_ || parked_)
            return;
        parked_ = true;
        parkedAtMs_ = nowMs;
    }

    // 前端的首帧已绘信号。这里不放行，只武装；真正放行在 onTick 上。
    // 还没挪走时也要记账：记成已放行之后，后面那次导航开始就不会再把已经画好的页面挪走。
    // 取 nowMs 是为了那条毫秒下界：信号到达的时刻只有调用方知道，它在消息线程上，与 tick 不同相。
    void onFirstFrame(std::uint32_t nowMs) noexcept
    {
        if (!parked_)
        {
            revealed_ = true;
            return;
        }
        if (!settling_)
        {
            settling_ = true;
            settleTicksSeen_ = 0;
            settleAtMs_ = nowMs;
        }
    }

    // 页面加载完成回调。不放行：它只保证文档下载完，不保证任何一帧已合成。
    // 只记账，供超时那一行诊断区分「页面 load 完了但首帧信号没发」与「导航压根没走完」
    void onNavigationFinished() noexcept { navFinishedSeen_ = true; }

    // 定时 tick：先结算首帧信号的那一拍，再看超时兜底。
    // 时间差走 uint32 到 int32：毫秒计数每约 49 天回绕，直接比大小会在回绕点把「刚挪走」算成「早该放行」，
    // 或者反过来永不放行。
    void onTick(std::uint32_t nowMs) noexcept
    {
        if (!parked_)
            return;
        if (settling_)
        {
            // 结算必须先于超时判定，否则信号踩着兜底线到达时会被记成超时，诊断里凭空多出一次「信号缺席」
            const bool ticksDone = (++settleTicksSeen_ >= kRevealSettleTicks);
            const bool msDone = (static_cast<std::int32_t>(nowMs - settleAtMs_) >= kRevealSettleMs);
            if (ticksDone && msDone)
                reveal("firstFrame");
            return;
        }
        if (static_cast<std::int32_t>(nowMs - parkedAtMs_) >= kRevealFallbackMs)
            reveal("timeout");
    }

    // 切了兜底面板：面板自己铺满本组件，闸门不该再按住 WebView 的位置
    void onFallbackShown() noexcept { reveal("fallback"); }

    bool parked() const noexcept { return parked_; }
    // 最近一次放行的原因（诊断行用；从未放行过是空串）
    const char* lastRevealReason() const noexcept { return revealReason_; }
    // 加载完成回调到过没有。与放行无关，只进超时那一行诊断
    bool navigationFinishedSeen() const noexcept { return navFinishedSeen_; }

private:
    void reveal(const char* why) noexcept
    {
        revealed_ = true;
        settling_ = false;
        if (!parked_)
            return;
        parked_ = false;
        revealReason_ = why;
    }

    bool parked_ = false;
    bool revealed_ = false;
    bool settling_ = false; // 首帧信号已到、正在压那一拍
    bool navFinishedSeen_ = false;
    int settleTicksSeen_ = 0;
    std::uint32_t settleAtMs_ = 0; // 首帧信号到达的时刻（毫秒下界的起点）
    std::uint32_t parkedAtMs_ = 0;
    const char* revealReason_ = "";
};

// 遮挡期间 WebView 子窗口该占的矩形：尺寸一字不改（页面不重排，Chromium 仍按真实视口出帧），
// 整块平移到本组件右侧两个窗口宽之外。JUCE 会把它换算成宿主窗口客户区坐标喂给 put_Bounds，
// 而子窗口恒被系统裁到父窗口客户区内，所以屏上看不见。
//
// 这依赖一条没有用例守着的前提：JUCE 必须把「只改 x、宽高不变」的 setBounds 也转发到 put_Bounds。
// 读实现核过，成立（位移会触发移动回调，WebView2 后端的覆写不看移动或缩放标志，一律重设控制器边界）。
// 这一环真断了的话，闸门的标志照翻、诊断行照打、单测照绿，而 WebView 从头到尾没挪过，所以真机验收按正向指标判：
// 开窗应看见一段约 1 秒的占位渐变再切到内容；仍见白或黑，就说明位移没生效。
// 宽度为 0（还没按设计盒定尺寸）时退一步用 1，保证平移量恒为正、不会原地不动。
inline juce::Rectangle<int> parkedBounds(juce::Rectangle<int> visible) noexcept
{
    const int step = juce::jmax(1, visible.getWidth());
    return visible.withX(visible.getX() + step * 2);
}

} // namespace rinne::webview
