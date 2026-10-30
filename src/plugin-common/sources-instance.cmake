# SPDX-License-Identifier: GPL-3.0-or-later
#
# 实例接线（InstanceHost：后端、会合、引擎、tick、命令序号）：两种构建都编进每个链接 rinne_plugin_common 的插件。
# 正式构建里处理器直接持有它，探针构建里的 ProbeHost 建在它之上。测试经 tests/host/wiring/ 的包装 .cpp 编进
# rinne_host_tests。由 src/plugin-common/CMakeLists.txt 按文件名顺序 include。

target_sources(rinne_plugin_common INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/InstanceHost.h"
  "${CMAKE_CURRENT_LIST_DIR}/InstanceHost.cpp")
