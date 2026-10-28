# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rinne Input 的 state 胶水：宿主存取工程 state 经它读写（契约 docs/contracts/STATE_SCHEMA.md），两端共用的判定在
# core 的 src/core/state/StateGlueCommon.h。由 src/input/CMakeLists.txt 在 juce_add_plugin 之后按文件名顺序 include。
# 只加这两个文件：处理器还没有接上它，接线时由处理器的 get/setStateInformation 调用。

target_sources(RinneInput PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/InputStateGlue.h"
  "${CMAKE_CURRENT_LIST_DIR}/InputStateGlue.cpp")
