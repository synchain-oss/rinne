# SPDX-License-Identifier: GPL-3.0-or-later
#
# 探针面板与支持件：只在探针构建（RINNE_PROBE_BUILD=ON）里编进每个链接 rinne_plugin_common 的插件，
# 正式版的二进制里没有这些代码，也没有它们的字符串。
# 正式构建里处理器接线只需要头文件里的声明（`if constexpr (build::kProbeBuild)` 的另一支不实例化，不会引用定义）。
# 测试经 tests/host/probe/ 下的两个包装 .cpp 直接把这两个源文件编进 rinne_host_tests，两种构建里都覆盖得到。
# 由 src/plugin-common/CMakeLists.txt 按文件名顺序 include。

if(RINNE_PROBE_BUILD)
  target_sources(rinne_plugin_common INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/ProbeSupport.h"
    "${CMAKE_CURRENT_LIST_DIR}/ProbeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ProbeEditor.h"
    "${CMAKE_CURRENT_LIST_DIR}/ProbeEditor.cpp")
endif()
