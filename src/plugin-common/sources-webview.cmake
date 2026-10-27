# SPDX-License-Identifier: GPL-3.0-or-later
#
# WebView 宿主族：网页编辑器、桥常量与回执、平台 WebView 分支、资源提供者、兜底面板、界面全局默认存储、
# 开窗遮挡闸与编辑器工厂。源码编进每个链接 rinne_plugin_common 的插件。
# 由 src/plugin-common/CMakeLists.txt 按文件名顺序 include。
#
# 宿主测试不链接这些源码，而是经 tests/host/ 下的包装 .cpp 把其中不依赖浏览器组件的几份直接编进 rinne_host_tests
# （宿主测试目标没有 WebView2 的 loader，也不开浏览器组件）。WebViewHost.cpp 与 PlatformWebViewRuntime.cpp 只在插件目标里编。
# 页面资源的二进制数据（cmake/RinneWebAssets.cmake 的函数）与编辑器的接线不在这里，由各插件目标自己的 CMakeLists 在接线时补上。
# 设计盒常量来自 rinne_core 的公开包含目录，不需要另加包含路径。

target_sources(rinne_plugin_common INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/BridgeBase.h"
  "${CMAKE_CURRENT_LIST_DIR}/BridgeBase.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/FallbackPanel.h"
  "${CMAKE_CURRENT_LIST_DIR}/FallbackPanel.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/PlatformWebView.h"
  "${CMAKE_CURRENT_LIST_DIR}/PlatformWebView.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/PlatformWebViewRuntime.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/ResourceProvider.h"
  "${CMAKE_CURRENT_LIST_DIR}/ResourceProvider.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/RinneEditorFactory.h"
  "${CMAKE_CURRENT_LIST_DIR}/RinneEditorFactory.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/UiDefaultsStore.h"
  "${CMAKE_CURRENT_LIST_DIR}/UiDefaultsStore.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/WebViewHost.h"
  "${CMAKE_CURRENT_LIST_DIR}/WebViewHost.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/WebViewRevealGate.h")
