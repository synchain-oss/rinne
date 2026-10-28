# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rinne Input 的参数层：按参数表（ParamTable.h）展开参数树，并在块首把 11 个参数的明文值快照成引擎参数。
# 由 src/input/CMakeLists.txt 在 juce_add_plugin 之后按文件名顺序 include。
# 只加这两个文件：处理器还没有接上它，接线时在处理器里构造 InputParams、每块调用 snapshot。

target_sources(RinneInput PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/InputParams.h"
  "${CMAKE_CURRENT_LIST_DIR}/InputParams.cpp")
