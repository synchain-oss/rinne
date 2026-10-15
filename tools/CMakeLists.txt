# SPDX-License-Identifier: GPL-3.0-or-later
#
# 随包发给用户或给 CI 用的命令行工具。每个工具一个 tools/<名字>.cmake，按文件名顺序 include，新工具不改这里。
# 可执行文件一律输出到 <构建目录>/tools-bin/<配置>/：scripts/build.ps1 -Stage 只从那里取，CI 有就上传、没有就跳过。
# 需要 JUCE 的工具自己判断 TARGET juce::juce_core（RINNE_CORE_ONLY 下没有 JUCE）。

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools-bin/$<CONFIG>")

file(GLOB _rinne_tools CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cmake")
list(SORT _rinne_tools)
foreach(_tool IN LISTS _rinne_tools)
  include("${_tool}")
endforeach()
