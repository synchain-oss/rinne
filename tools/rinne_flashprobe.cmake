# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_flashprobe：开窗白帧检测，开发者本机用（用法写在源文件开头）。只在 Windows 上建：抓屏走 DXGI 桌面复制
# （取不到时退回 GDI 的屏幕 DC），编辑器经 JUCE 的 VST3 宿主打开。不注册 ctest：它会在桌面上弹窗、要取 GUI 锁，只在人手跑的时候用。
# 不进打包：可执行文件放在 <构建目录>/devtools-bin/<配置>/，而 scripts/build.ps1 -Stage（CI 产物）只拷 tools-bin/ 下的
# 可执行文件，scripts/package.ps1 只认 kit 清单里的 rinne_nulltest。
# 占位的几何与色标直接用插件同一份头文件（PlatformWebView.h），参照图与插件画的是同一个函数。

if(WIN32 AND TARGET juce::juce_audio_processors)
  add_executable(rinne_flashprobe rinne_flashprobe.cpp)
  set_target_properties(rinne_flashprobe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/devtools-bin/$<CONFIG>")
  target_include_directories(rinne_flashprobe PRIVATE "${CMAKE_SOURCE_DIR}/src/plugin-common")
  # rinne_core 只为设计盒常量的头文件（util/DesignBox.h）
  target_link_libraries(rinne_flashprobe PRIVATE
    rinne_core
    juce::juce_audio_processors
    juce::juce_gui_extra)
  target_compile_definitions(rinne_flashprobe PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
  rinne_target_warnings(rinne_flashprobe PRIVATE)
  rinne_warnings_as_errors(rinne_flashprobe)
endif()
