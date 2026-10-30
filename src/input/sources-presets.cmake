# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rinne Input 的预设库：8 个工厂预设（presets/factory/*.json）经 juce_add_binary_data 编进插件二进制，
# 命名空间 RinnePresetData（PresetStore.cpp 引用它）；PresetStore 本身也在这里加进 Input。
# 由 src/input/CMakeLists.txt 在 juce_add_plugin 之后按文件名顺序 include。
# 只加这几个文件：处理器还没有接上预设库，接线时在处理器里持有 PresetStore、每块把 presetSeq() 填进引擎参数。
#
# 资源目标 RinnePresetAssets 同时链给测试（tests/params/suite-presets.cmake）：测试读到的就是插件里嵌的那一份。
# 文件按 glob 收（新增预设不会被漏掉），名字带两位序号前缀，界面上的先后就是文件名的先后；
# 个数与内容由 PresetStore 的构造校验、tests/params 的用例核对（8 个全部解析并通过校验）。

file(GLOB _rinne_factory_presets CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/presets/factory/*.json")
list(SORT _rinne_factory_presets)
if(NOT _rinne_factory_presets)
  message(FATAL_ERROR "presets/factory 下没有扫到任何工厂预设")
endif()

juce_add_binary_data(RinnePresetAssets
  NAMESPACE   RinnePresetData
  HEADER_NAME RinnePresetData.h
  SOURCES     ${_rinne_factory_presets})

target_sources(RinneInput PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/PresetStore.h"
  "${CMAKE_CURRENT_LIST_DIR}/PresetStore.cpp")
target_link_libraries(RinneInput PRIVATE RinnePresetAssets)
