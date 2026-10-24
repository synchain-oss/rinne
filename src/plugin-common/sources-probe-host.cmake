# SPDX-License-Identifier: GPL-3.0-or-later
#
# 探针接线（ProbeHost）：同 sources-probe.cmake，只在探针构建（RINNE_PROBE_BUILD=ON）里编进每个链接
# rinne_plugin_common 的插件。正式构建里处理器只在 `if constexpr (build::kProbeBuild)` 的另一支里提到它，
# 那一支不实例化，不会引用定义。测试经 tests/host/probe_host/ 的包装 .cpp 在两种构建里都编进 rinne_host_tests。
# 由 src/plugin-common/CMakeLists.txt 按文件名顺序 include。

if(RINNE_PROBE_BUILD)
  target_sources(rinne_plugin_common INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/ProbeHost.h"
    "${CMAKE_CURRENT_LIST_DIR}/ProbeHost.cpp")
endif()
