# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_host_tests：在不开 DAW 的进程里直接构造两个处理器、手推 processBlock，验证外壳对宿主的约定
# （逐位直通、延迟恒 0、tail、旁路、验证宿主、0 样本）。由 tests/CMakeLists.txt 在建好目标后 include，
# 目标的源码是 tests/host/** 下的全部 .cpp。
# 处理器与基类的源码直接编进测试，不链接插件目标：不带 VST3 / AU 包装层，也不带 WebView2。
# createEditor 在这里由 tests/host/shell/EditorStubs.cpp 补成无头实现。

set(_rinne_src "${CMAKE_SOURCE_DIR}/src")
target_sources(rinne_host_tests PRIVATE
  "${_rinne_src}/plugin-common/RinneProcessorBase.h"
  "${_rinne_src}/plugin-common/RinneProcessorBase.cpp"
  "${_rinne_src}/input/InputProcessor.h"
  "${_rinne_src}/input/InputProcessor.cpp"
  "${_rinne_src}/output/OutputProcessor.h"
  "${_rinne_src}/output/OutputProcessor.cpp")
target_include_directories(rinne_host_tests PRIVATE
  "${_rinne_src}/plugin-common" "${_rinne_src}/input" "${_rinne_src}/output")
target_link_libraries(rinne_host_tests PRIVATE juce::juce_audio_processors)
# RinneProcessorBase::getName 读 JucePlugin_Name；真正的插件由 juce_add_plugin 定义它，这里没有插件目标
target_compile_definitions(rinne_host_tests PRIVATE
  "JucePlugin_Name=\"Rinne Host Test\""
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

# 验证宿主（pluginval、auval 等）按宿主可执行文件的名字识别。同一个测试再以带 pluginval 字样的文件名跑一遍，
# 处理器就会认出自己在验证宿主里：既覆盖「验证宿主下只出干声」，也证明识别真的在起作用。
# 副本只是换名字的拷贝，每次构建后覆盖，测试里用环境变量断言识别结果，拷贝没生效时它会变红。
set(_rinne_host_copy "$<TARGET_FILE_DIR:rinne_host_tests>/pluginval-rinne-host-tests${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(TARGET rinne_host_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:rinne_host_tests> "${_rinne_host_copy}"
  VERBATIM)
add_test(NAME rinne_host_tests_validation_host COMMAND "${_rinne_host_copy}")
set_tests_properties(rinne_host_tests_validation_host PROPERTIES TIMEOUT 300
  ENVIRONMENT "RINNE_EXPECT_VALIDATION_HOST=1")
