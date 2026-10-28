# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_params_tests：参数层的用例，要链 JUCE。由 tests/CMakeLists.txt 在建好目标后 include，目标的源码是
# tests/params/** 下的全部 .cpp。
# 决定宿主参数 ID 的两个 JUCE 宏只在根 CMakeLists.txt 定义一次、只挂在两个插件目标上（contract-static-identity 守着），
# 这里不另设：用例从 tests/golden/identity.json 读它们的值，交给 JUCE 的运行期函数。
# 不建编辑器、不链 WebView2，所以关掉 WebView 与 curl。

target_link_libraries(rinne_params_tests PRIVATE juce::juce_audio_processors)
target_compile_definitions(rinne_params_tests PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

# 其余的套件片段放在同目录的 suite-<名字>.cmake 里，按文件名顺序 include：以后各部分往里加自己的片段，不必再改本文件。
file(GLOB _rinne_params_parts CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/suite-*.cmake")
list(SORT _rinne_params_parts)
foreach(_part IN LISTS _rinne_params_parts)
  include("${_part}")
endforeach()
