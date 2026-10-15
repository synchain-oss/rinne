# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/build.json
#
# Rinne Output：推子前区最后一格的发送端，只做分组与配对，宿主的音频逐位直通。VST3，Apple 上另出 AU。
# 插件身份（产品名、插件码、bundle id、AU 三元组 aufx/<插件码>/Snch）在首个对外发出的构建产物里就永久冻结，
# 正式版与探针版各一套，都写成字面量，一行一项，契约守卫按行认得出改动。
# 增量源码与链接放在本目录的 sources-<名字>.cmake 里，按文件名顺序 include，不改这里。

if(RINNE_PROBE_BUILD)
  set(_rinne_output_identity
    PRODUCT_NAME "Rinne Probe Output"
    PLUGIN_CODE Rnpo
    BUNDLE_ID com.synchain.rinne.probe.output)
else()
  set(_rinne_output_identity
    PRODUCT_NAME "Rinne Output"
    PLUGIN_CODE Rnno
    BUNDLE_ID com.synchain.rinne.output)
endif()

juce_add_plugin(RinneOutput
  ${_rinne_output_identity}
  PLUGIN_MANUFACTURER_CODE Snch
  COMPANY_NAME "Synchain"
  COMPANY_COPYRIGHT "Copyright 2026 Synchain"
  COMPANY_WEBSITE "https://synchain.ca"
  COMPANY_EMAIL "contact@synchain.ca"
  FORMATS ${RINNE_PLUGIN_FORMATS}
  VST3_CATEGORIES Fx Delay
  # 类型写死成效果器：默认推导哪天因为加了 MIDI 输入之类漂成别的类型，就等于换了 AU 身份
  AU_MAIN_TYPE kAudioUnitType_Effect
  # 不进 AU 沙盒：插件要建、要附着跨进程的共享内存段，沙盒里会被拒
  AU_SANDBOX_SAFE FALSE
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  # 构建不往系统的插件目录里拷：本机构建绝不能替换维护者在 DAW 里用的那份
  COPY_PLUGIN_AFTER_BUILD FALSE
  NEEDS_WEB_BROWSER TRUE
  NEEDS_WEBVIEW2 TRUE)

target_sources(RinneOutput PRIVATE
  OutputProcessor.h
  OutputProcessor.cpp
  OutputPluginEntry.cpp)   # createPluginFilter 与 createEditor 单独放：宿主测试编处理器时不必链 WebView2
target_include_directories(RinneOutput PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(RinneOutput PRIVATE rinne_plugin_common)
# PUBLIC：VST3 / AU 包装层的编译单元也要看到这些宏，参数 ID 是在那里算的
target_compile_definitions(RinneOutput PUBLIC ${RINNE_PLUGIN_DEFINITIONS})
rinne_target_warnings(RinneOutput PRIVATE)

file(GLOB _rinne_output_parts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sources-*.cmake")
list(SORT _rinne_output_parts)
foreach(_part IN LISTS _rinne_output_parts)
  include("${_part}")
endforeach()

# 放在最后：sources-*.cmake 加进来的源文件也要算在内
rinne_warnings_as_errors(RinneOutput)
