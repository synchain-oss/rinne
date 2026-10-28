# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_params_tests 里两端 state 胶水（src/input/InputStateGlue.*、src/output/OutputStateGlue.*）的部分，由 suite.cmake
# 按文件名顺序 include。胶水的源码直接编进测试，不链接插件目标。
# Output 的胶水也编在这个目标里：它只要 juce_audio_processors 与 rinne_core，这两样目标都已经链了；
# 两个壳互不 include 的规矩管的是插件源码，测试目标同时编两端不违反它。

target_sources(rinne_params_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/src/input/InputStateGlue.h"
  "${CMAKE_SOURCE_DIR}/src/input/InputStateGlue.cpp"
  "${CMAKE_SOURCE_DIR}/src/output/OutputStateGlue.h"
  "${CMAKE_SOURCE_DIR}/src/output/OutputStateGlue.cpp")
