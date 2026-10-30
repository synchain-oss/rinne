# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_params_tests 里预设库（src/input/PresetStore.*）的部分，由 suite.cmake 按文件名顺序 include。
# 预设库的源码直接编进测试，不链接插件目标；8 个工厂预设经同一个资源目标 RinnePresetAssets 嵌进来
# （src/input/sources-presets.cmake 建它），测试读到的就是插件里嵌的那一份。

target_sources(rinne_params_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/src/input/PresetStore.h"
  "${CMAKE_SOURCE_DIR}/src/input/PresetStore.cpp")
target_link_libraries(rinne_params_tests PRIVATE RinnePresetAssets)
