# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rinne Output 的页面资源：web/output、web/shared、web/i18n、web/js/juce、web/fonts 与随包的许可证文本，
# 经 juce_add_binary_data 编进插件二进制，命名空间 RinneOutputWebData（OutputPluginEntry.cpp 引用它）。
# 由 src/output/CMakeLists.txt 在 juce_add_plugin 之后按文件名顺序 include。
#
# 没有这一步，页面资源提供者什么都取不到：窗口空白，前端永远调不到 requestInitialState，
# 表现是看门狗超时后落到兜底面板，而真因是资源根本没进包。打包集合的口径与文件名唯一性断言在
# cmake/RinneWebAssets.cmake，web-preview 的嵌入资源回归逐项核对这里和那里。
#
# 探针构建也嵌：入口代码对两种身份是同一份，不为身份另起编译定义；探针路径从不读这份资源，
# 代价只是探针包多出同一份资源（约 1.4 MB，其中中文字体占 1 MB 出头）。

if(NOT COMMAND rinne_add_web_assets)
  message(FATAL_ERROR "找不到 rinne_add_web_assets：根 CMakeLists.txt 应当在插件目录之前 include(RinneWebAssets)")
endif()

rinne_add_web_assets(
  TARGET RinneOutputWebAssets
  NAMESPACE RinneOutputWebData
  ROLE_DIR output
  LICENSE_FILE "${CMAKE_SOURCE_DIR}/LICENSE")

target_link_libraries(RinneOutput PRIVATE RinneOutputWebAssets)
