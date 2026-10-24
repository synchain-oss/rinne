# SPDX-License-Identifier: GPL-3.0-or-later
#
# 探针面板与支持件：两个插件共用，源码编进每个链接 rinne_plugin_common 的插件。
# 代码在正式构建里也编译（测试覆盖得到），只在探针构建里由处理器实例化。
# 由 src/plugin-common/CMakeLists.txt 按文件名顺序 include。

target_sources(rinne_plugin_common INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/ProbeSupport.h"
  "${CMAKE_CURRENT_LIST_DIR}/ProbeSupport.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/ProbeEditor.h"
  "${CMAKE_CURRENT_LIST_DIR}/ProbeEditor.cpp")
