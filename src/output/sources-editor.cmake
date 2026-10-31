# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rinne Output 的原生桥（OutputBridge，纯逻辑）与网页编辑器（OutputEditor，WebViewHost 的子类，只做胶水）。
# 由 src/output/CMakeLists.txt 在 juce_add_plugin 之后按文件名顺序 include。OutputPluginEntry.cpp 的 createEditor 经
# 编辑器工厂的构造缝造编辑器；页面资源在 sources-webview.cmake 里嵌入，编辑器从同一份资源取「查看许可证」的 LICENSE。
#
# 宿主测试不链接插件目标：OutputBridge.cpp 经 tests/host/output_bridge/OutputBridgeSource.cpp 编进 rinne_host_tests，
# 编辑器要浏览器组件，不进宿主测试。

target_sources(RinneOutput PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/OutputBridge.h"
  "${CMAKE_CURRENT_LIST_DIR}/OutputBridge.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/OutputEditor.h"
  "${CMAKE_CURRENT_LIST_DIR}/OutputEditor.cpp")
