# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_plugin_common：两个插件共用的 JUCE 胶水（处理器基类，之后还有 WebView 宿主、桥、Fallback 面板）。
# INTERFACE 库：源码编进每个链接它的插件，JUCE 模块不会在两个静态库里各编一份。
# 增量源码与链接放在本目录的 sources-<名字>.cmake 里，按文件名顺序 include，不改这里。

add_library(rinne_plugin_common INTERFACE)
target_sources(rinne_plugin_common INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/RinneProcessorBase.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/RinneProcessorBase.cpp")
target_include_directories(rinne_plugin_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rinne_plugin_common INTERFACE
  rinne_core
  juce::juce_audio_processors   # AudioProcessor、PluginHostType、GenericAudioProcessorEditor
  juce::juce_gui_extra)         # WebBrowserComponent

file(GLOB _rinne_common_parts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sources-*.cmake")
list(SORT _rinne_common_parts)
foreach(_part IN LISTS _rinne_common_parts)
  include("${_part}")
endforeach()
