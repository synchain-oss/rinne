# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_params_tests 里参数层（src/input/InputParams.*）的部分，由 suite.cmake 按文件名顺序 include。
# 参数层的源码直接编进测试，不链接插件目标（不带 VST3 / AU 包装层，也不带 WebView2）。
# 用例要驱动 core 里的 SafetyChain（限制器关时的反馈上限），目标已经链了 rinne_core。
# 音频线程不分配的用例借 tests/host/shell 的分配计数器：它只改测试可执行文件自己的导入表（Windows）或 malloc zone（mac）。

target_sources(rinne_params_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/src/input/InputParams.h"
  "${CMAKE_SOURCE_DIR}/src/input/InputParams.cpp"
  "${CMAKE_SOURCE_DIR}/tests/host/shell/AllocationCounter.h"
  "${CMAKE_SOURCE_DIR}/tests/host/shell/AllocationCounter.cpp")
