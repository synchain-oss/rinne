# SPDX-License-Identifier: GPL-3.0-or-later
#
# 诊断日志文件（PlatformLog）：两个插件共用，源码编进每个链接 rinne_plugin_common 的插件。
# 由 src/plugin-common/CMakeLists.txt 按文件名顺序 include。

target_sources(rinne_plugin_common INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/PlatformLog.h"
  "${CMAKE_CURRENT_LIST_DIR}/PlatformLog.cpp")
