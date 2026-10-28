// SPDX-License-Identifier: GPL-3.0-or-later
//
// 开窗遮挡闸：什么时候挡住、什么时候放行、什么绝不放行。纯逻辑，时间由测试喂进去。
// 守四件事：放行只认首帧信号（外加超时兜底），首帧信号到后要同时满足「tick 数」与「毫秒下界」，
// 毫秒计数回绕时不会提前或永不放行，导航开始之前就挡住的那一段不起算超时、只由兜底面板或导航开始接手。
// 另带关窗诊断行的拼装。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "WebViewRevealGate.h"

namespace
{

using rinne::webview::parkedBounds;
using rinne::webview::RevealGate;

std::string reason(const RevealGate& gate)
{
    return gate.lastRevealReason();
}

} // namespace

TEST_CASE("reveal gate constants", "[webview][gate]")
{
    CHECK(RevealGate::kRevealFallbackMs == 3000);
    CHECK(RevealGate::kRevealSettleTicks == 1);
    CHECK(RevealGate::kRevealSettleMs == 64);
}

TEST_CASE("reveal gate: starts open and parks on the first navigation", "[webview][gate]")
{
    RevealGate gate;
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate).empty());

    gate.onNavigationStarted(1000);
    CHECK(gate.parked());
}

TEST_CASE("reveal gate: the first frame signal only arms, it does not release", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onFirstFrame(1100);
    CHECK(gate.parked()); // 信号到了也还挡着，放行要等 tick
}

TEST_CASE("reveal gate: release needs both the tick count and the millisecond floor", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onFirstFrame(1100);

    gate.onTick(1100 + 63); // tick 数够了，毫秒下界差一毫秒
    CHECK(gate.parked());
    gate.onTick(1100 + 64); // 刚好到下界
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "firstFrame");
}

TEST_CASE("reveal gate: a signal that lands right before a tick does not release early", "[webview][gate]")
{
    // 信号到达点相对 tick 相位是随机的：只数 tick 的话，实际等待可能只有几毫秒，比一个合成帧还短
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onFirstFrame(1236);
    gate.onTick(1240);
    CHECK(gate.parked());
    gate.onTick(1280);
    CHECK(gate.parked());
    gate.onTick(1300); // 1236 + 64
    CHECK_FALSE(gate.parked());
}

TEST_CASE("reveal gate: the page-finished callback never releases", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onNavigationFinished();
    CHECK(gate.navigationFinishedSeen());
    gate.onTick(1040);
    gate.onTick(1080);
    gate.onTick(1120);
    CHECK(gate.parked());
    CHECK(reason(gate).empty());
}

TEST_CASE("reveal gate: timeout releases at the deadline and not before", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onTick(1000 + RevealGate::kRevealFallbackMs - 1);
    CHECK(gate.parked());
    gate.onTick(1000 + RevealGate::kRevealFallbackMs);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "timeout");
}

TEST_CASE("reveal gate: the deadline is measured from navigation start, not from construction", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(50000);
    gate.onTick(3000); // 数值上小于起点，回绕安全的差值是负的
    CHECK(gate.parked());
    gate.onTick(50000 + 2999);
    CHECK(gate.parked());
    gate.onTick(50000 + 3000);
    CHECK_FALSE(gate.parked());
}

TEST_CASE("reveal gate: a millisecond counter wrap does not release early or never", "[webview][gate]")
{
    const std::uint32_t nearEnd = 0xFFFFFF00u;

    SECTION("timeout across the wrap")
    {
        RevealGate gate;
        gate.onNavigationStarted(nearEnd);
        gate.onTick(nearEnd + 2999u); // 无符号回绕后的数值很小
        CHECK(gate.parked());
        gate.onTick(nearEnd + 3000u);
        CHECK_FALSE(gate.parked());
        CHECK(reason(gate) == "timeout");
    }

    SECTION("settle floor across the wrap")
    {
        RevealGate gate;
        gate.onNavigationStarted(nearEnd);
        gate.onFirstFrame(nearEnd + 10u);
        gate.onTick(nearEnd + 10u + 63u);
        CHECK(gate.parked());
        gate.onTick(nearEnd + 10u + 64u);
        CHECK_FALSE(gate.parked());
        CHECK(reason(gate) == "firstFrame");
    }
}

TEST_CASE("reveal gate: a timestamp that is not yet past the deadline stays parked when the deadline wraps",
          "[webview][gate]")
{
    // 起点 + 3000 在无符号数上回绕成一个很小的数。若用「现在 >= 起点 + 3000」直接比大小，
    // 起点之后 100 毫秒（计数还没回绕）就会被当成早已超时
    const std::uint32_t nearEnd = 0xFFFFFF00u;
    RevealGate gate;
    gate.onNavigationStarted(nearEnd);
    gate.onTick(nearEnd + 100u);
    CHECK(gate.parked());
    gate.onTick(0xFFFFFFFFu);
    CHECK(gate.parked());
}

TEST_CASE("reveal gate: a tick stamped earlier than the event it follows never releases", "[webview][gate]")
{
    // 不同线程或不同时刻取的毫秒数，tick 的时间戳可能比信号的略小。回绕安全的差值是负的，不能当成一个巨大的正数
    RevealGate signalled;
    signalled.onNavigationStarted(1000);
    signalled.onFirstFrame(1100);
    signalled.onTick(1090);
    CHECK(signalled.parked());
    signalled.onTick(1100 + 64);
    CHECK_FALSE(signalled.parked());

    RevealGate waiting;
    waiting.onNavigationStarted(5000);
    waiting.onTick(4990);
    CHECK(waiting.parked());
    CHECK(reason(waiting).empty());
}

TEST_CASE("reveal gate: a signal near the deadline is counted as first frame, not timeout", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onFirstFrame(1000 + 2990);
    gate.onTick(1000 + 3000); // 已到超时线，但信号已到，先结算那一拍：毫秒下界没到，继续挡着，也不记成超时
    CHECK(gate.parked());
    CHECK(reason(gate).empty());
    gate.onTick(1000 + 2990 + 63);
    CHECK(gate.parked());
    gate.onTick(1000 + 2990 + 64);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "firstFrame");
}

TEST_CASE("reveal gate: a signal before the first navigation prevents parking", "[webview][gate]")
{
    RevealGate gate;
    gate.onFirstFrame(100);
    gate.onNavigationStarted(200);
    CHECK_FALSE(gate.parked()); // 已经画好的页面不再挪走
}

TEST_CASE("reveal gate: after a release a second navigation does not park again", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(0);
    gate.onFirstFrame(0);
    gate.onTick(100);
    REQUIRE_FALSE(gate.parked());

    gate.onNavigationStarted(500);
    CHECK_FALSE(gate.parked());
}

TEST_CASE("reveal gate: beginLoadAttempt re-arms everything", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(0);
    gate.onNavigationFinished();
    gate.onFirstFrame(0);
    gate.onTick(100);
    REQUIRE_FALSE(gate.parked());
    REQUIRE(reason(gate) == "firstFrame");

    gate.beginLoadAttempt();
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate).empty());
    CHECK_FALSE(gate.navigationFinishedSeen());

    gate.onNavigationStarted(1000);
    CHECK(gate.parked());
    // 重新武装后，上一轮的首帧信号不再有效：不到超时线不放行
    gate.onTick(1100);
    CHECK(gate.parked());
}

TEST_CASE("reveal gate: showing the fallback panel releases with its own reason", "[webview][gate]")
{
    RevealGate gate;
    gate.onNavigationStarted(1000);
    gate.onFallbackShown();
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "fallback");
}

TEST_CASE("reveal gate: showing the fallback panel without a park leaves no reason", "[webview][gate]")
{
    RevealGate gate;
    gate.onFallbackShown();
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate).empty()); // 从未挡住过，就没有「放行」可记

    RevealGate released;
    released.onNavigationStarted(0);
    released.onFirstFrame(0);
    released.onTick(100);
    REQUIRE(reason(released) == "firstFrame");
    released.onFallbackShown();
    CHECK(reason(released) == "firstFrame"); // 已经放行过，原因不被后来的兜底覆盖
}

TEST_CASE("reveal gate: ticks while not parked do nothing", "[webview][gate]")
{
    RevealGate gate;
    gate.onTick(123456);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate).empty());
}

TEST_CASE("parked bounds keep the size and move the window out of the visible area", "[webview][gate]")
{
    const juce::Rectangle<int> visible(0, 0, 460, 364);
    const auto parked = parkedBounds(visible);
    CHECK(parked.getWidth() == 460);
    CHECK(parked.getHeight() == 364);
    CHECK(parked.getY() == 0);
    CHECK(parked.getX() == 920); // 平移两个窗口宽
    CHECK_FALSE(parked.intersects(visible));

    const auto offset = parkedBounds({10, 5, 100, 50});
    CHECK(offset == juce::Rectangle<int>(210, 5, 100, 50));

    // 宽度为 0（还没定尺寸）时平移量仍然为正，不会原地不动
    const auto zero = parkedBounds({0, 0, 0, 0});
    CHECK(zero.getX() > 0);
}

TEST_CASE("reveal gate: holding before navigation parks at once and never times out on its own", "[webview][gate]")
{
    RevealGate gate;
    gate.holdBeforeNavigation();
    CHECK(gate.parked());
    CHECK(gate.holdingBeforeNavigation());

    // 导航开始之前没有超时：多久都不放行，这一段归看门狗管
    gate.onTick(0);
    gate.onTick(RevealGate::kRevealFallbackMs * 10u);
    CHECK(gate.parked());
    CHECK(reason(gate).empty());
}

TEST_CASE("reveal gate: navigation start after a hold keeps the park and starts the deadline there", "[webview][gate]")
{
    RevealGate gate;
    gate.holdBeforeNavigation();
    gate.onNavigationStarted(5000);
    CHECK(gate.parked());
    CHECK_FALSE(gate.holdingBeforeNavigation());

    gate.onTick(5000 + RevealGate::kRevealFallbackMs - 1);
    CHECK(gate.parked());
    gate.onTick(5000 + RevealGate::kRevealFallbackMs);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "timeout");
}

TEST_CASE("reveal gate: a second navigation start does not move the deadline", "[webview][gate]")
{
    RevealGate gate;
    gate.holdBeforeNavigation();
    gate.onNavigationStarted(1000);
    gate.onNavigationStarted(2500); // 页面自己再导航一次：超时仍从第一次起算
    gate.onTick(1000 + RevealGate::kRevealFallbackMs);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "timeout");
}

TEST_CASE("reveal gate: bridge ready while still held starts the deadline like a navigation start", "[webview][gate]")
{
    // 纵深防御：导航开始没送到、桥却就绪了。闸门不能永远挪在外面，补记之后照常超时放行
    RevealGate gate;
    gate.holdBeforeNavigation();
    CHECK(gate.onBridgeReady(4000));
    CHECK(gate.parked());
    CHECK_FALSE(gate.holdingBeforeNavigation());

    gate.onTick(4000 + RevealGate::kRevealFallbackMs - 1);
    CHECK(gate.parked());
    gate.onTick(4000 + RevealGate::kRevealFallbackMs);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "timeout");
}

TEST_CASE("reveal gate: bridge ready after a navigation start changes nothing", "[webview][gate]")
{
    // 正常顺序：导航开始先到。桥就绪不补记，超时仍从导航开始起算
    RevealGate gate;
    gate.holdBeforeNavigation();
    gate.onNavigationStarted(1000);
    CHECK_FALSE(gate.onBridgeReady(2500));
    gate.onTick(1000 + RevealGate::kRevealFallbackMs);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "timeout");

    // 已经放行之后也不补记，不会再挪走
    CHECK_FALSE(gate.onBridgeReady(9000));
    CHECK_FALSE(gate.parked());

    // 从没挡住过（系统 WebKit 上闸门不武装）
    RevealGate idle;
    CHECK_FALSE(idle.onBridgeReady(100));
    CHECK_FALSE(idle.parked());
}

TEST_CASE("reveal gate: a first frame signal during the hold is ignored", "[webview][gate]")
{
    RevealGate gate;
    gate.holdBeforeNavigation();
    gate.onFirstFrame(100); // 这一次还没有文档，信号不属于它
    gate.onTick(1000);
    CHECK(gate.parked());

    gate.onNavigationStarted(2000);
    gate.onTick(2000 + RevealGate::kRevealSettleMs + 10);
    CHECK(gate.parked()); // 被忽略的信号没有武装放行

    gate.onFirstFrame(2100);
    gate.onTick(2100 + RevealGate::kRevealSettleMs);
    CHECK_FALSE(gate.parked());
    CHECK(reason(gate) == "firstFrame");
}

TEST_CASE("reveal gate: the fallback panel releases a hold that never saw a navigation", "[webview][gate]")
{
    RevealGate gate;
    gate.holdBeforeNavigation();
    gate.onFallbackShown();
    CHECK_FALSE(gate.parked());
    CHECK_FALSE(gate.holdingBeforeNavigation());
    CHECK(reason(gate) == "fallback");

    // 放行过就不再挡：迟到的导航开始不会把面板后面的 WebView 再挪走
    gate.onNavigationStarted(100);
    CHECK_FALSE(gate.parked());
}

TEST_CASE("reveal gate: hold is a no-op once parked or released, and beginLoadAttempt clears it", "[webview][gate]")
{
    RevealGate parkedByNav;
    parkedByNav.onNavigationStarted(1000);
    parkedByNav.holdBeforeNavigation();
    CHECK_FALSE(parkedByNav.holdingBeforeNavigation()); // 已经按导航挡住的，不改回「导航之前」
    parkedByNav.onTick(1000 + RevealGate::kRevealFallbackMs);
    CHECK(reason(parkedByNav) == "timeout"); // 超时照常

    RevealGate released;
    released.onNavigationStarted(0);
    released.onFirstFrame(0);
    released.onTick(100);
    REQUIRE_FALSE(released.parked());
    released.holdBeforeNavigation();
    CHECK_FALSE(released.parked());

    RevealGate retry;
    retry.holdBeforeNavigation();
    retry.beginLoadAttempt();
    CHECK_FALSE(retry.parked());
    CHECK_FALSE(retry.holdingBeforeNavigation());
    retry.holdBeforeNavigation(); // 重试的加载尝试照样能再挡一次
    CHECK(retry.holdingBeforeNavigation());
}

TEST_CASE("window closed line names the gate state and the elapsed time", "[webview][gate]")
{
    using rinne::webview::windowClosedLine;
    RevealGate gate;
    CHECK(windowClosedLine(1500, 1000, gate, true, false) == "window closed after 500 ms (revealed)");

    gate.holdBeforeNavigation();
    CHECK(windowClosedLine(1500, 1000, gate, true, false) == "window closed after 500 ms (parked before navigation)");
    gate.onNavigationStarted(1200);
    CHECK(windowClosedLine(1500, 1000, gate, true, false) == "window closed after 500 ms (parked)");

    // 兜底面板在场时先报面板；闸门不启用的平台报 gate off，不管闸门对象里记着什么
    CHECK(windowClosedLine(1500, 1000, gate, true, true) == "window closed after 500 ms (fallback)");
    CHECK(windowClosedLine(1500, 1000, gate, false, false) == "window closed after 500 ms (reveal gate off)");

    gate.onFirstFrame(1300);
    gate.onTick(1300 + RevealGate::kRevealSettleMs);
    REQUIRE_FALSE(gate.parked());
    CHECK(windowClosedLine(1500, 1000, gate, true, false) == "window closed after 500 ms (revealed)");

    // 毫秒计数回绕：起点在回绕前、关窗在回绕后，用时仍是正的小数
    CHECK(windowClosedLine(100u, 0xFFFFFF00u, gate, true, false) == "window closed after 356 ms (revealed)");
}
