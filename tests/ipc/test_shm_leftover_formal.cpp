// SPDX-License-Identifier: GPL-3.0-or-later
//
// 残段检查（正式作用域）：按名模块生成正式族的全部段名（rv 与 g1..8），逐个只读探一遍，一个都不许存在。
// 这条用例永远不建任何东西，mac CI 的 shm-leftover.sh 在验证前后各跑一次 `rinne_ipc_tests "[shm-leftover]"`。
// 探法与各平台的判据见 SegmentProbe.h；Linux 上没有操作系统级的段可探，用例按「不适用」通过。
// 跨进程用例自己的测试作用域在每条用例结束时另外探一遍（IpcTestKit.h 的 expectNoLeftovers），那里的段名是随机的，
// 正式作用域的这条看不到。
#include <catch2/catch_test_macros.hpp>

#include "SegmentProbe.h"
#include "ipc/Naming.h"

TEST_CASE("the formal scope leaves no shared memory behind", "[shm-leftover]")
{
#if defined(_WIN32) || defined(__APPLE__)
    const rinne::ipc::Naming naming(rinne::ipc::NameScope::formal());
    const std::vector<std::string> leftovers = rinne::test::existingSegments(naming);
    for (const std::string& name : leftovers)
    {
        UNSCOPED_INFO("leftover segment: " << name);
    }
    CHECK(leftovers.empty());
#else
    // 进程内平台没有操作系统级的段可探：正式作用域的名字只在这个进程的映射表里用，套件里也没有别的用例建它
    SUCCEED("in-process platform: the formal scope never creates OS-level segments");
#endif
}
