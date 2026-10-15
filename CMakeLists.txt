# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/build.json
#
# Rinne 的构建入口：rinne_core（不链 JUCE）、rinne_plugin_common、Rinne Input 与 Rinne Output 两个插件、测试与工具。
# 版本只有这一个真源：project(Rinne VERSION …)。配置：
#   cmake -S . -B build -G "Ninja Multi-Config" -DJUCE_PATH=<JUCE 8.0.8 的检出>
# 本机与 CI 都经 scripts/build.ps1 调用，它负责 MSVC 环境、段名前缀与构建槽。
# 后续卡片只新增文件、不改这里的扩展点：
#   cmake/RinneMac.cmake        mac 的架构、部署目标与只在 Apple 上生效的设置（project() 之前 include）
#   cmake/RinneWebAssets.cmake  web 资源嵌入的函数（插件目录之前 include）
#   src/core/<子目录>/          rinne_core 按子目录收源码，新子目录自动纳入（见 src/core/CMakeLists.txt）
#   src/<壳>/sources-*.cmake    input / output / plugin-common 各自的增量源码与链接
#   tests/CMakeLists.txt        测试入口，存在才加入
#   tools/<名字>.cmake          工具，见 tools/CMakeLists.txt
cmake_minimum_required(VERSION 3.22)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 静态 CRT 必须在 project() 之前设：两个插件与 WebView2 的静态 loader 共用 /MT，宿主里不依赖 VC 运行库的安装。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RINNE_BUILD_TESTS "构建测试（tests/CMakeLists.txt 存在时）" ON)
option(RINNE_BUILD_TOOLS "构建 tools/ 下的工具" ON)
option(RINNE_PROBE_BUILD "改用探针身份构建（插件码、bundle id 与段名前缀都与正式版不同）" OFF)
option(RINNE_CORE_ONLY "只构建 rinne_core 与它的测试，不需要 JUCE（Linux sanitizer 用）" OFF)
set(RINNE_SANITIZE "" CACHE STRING "只作用于 rinne_core 与测试：空、asan_ubsan 或 tsan")
set_property(CACHE RINNE_SANITIZE PROPERTY STRINGS "" asan_ubsan tsan)
set(RINNE_STRESS_MUTATE "" CACHE STRING "压测的变异构建：空、no_read_fence 或 no_intent；只允许与 RINNE_CORE_ONLY 同用")
set_property(CACHE RINNE_STRESS_MUTATE PROPERTY STRINGS "" no_read_fence no_intent)
set(RINNE_BUILD_LABEL "" CACHE STRING "写进构建信息的标签，例如 kit 版本加短 sha；只能有字母、数字与 . + -")

# 只在 Apple 上起作用的设置必须在 project() 之前：架构与部署目标参与编译器探测。要改目标的设置用 cmake_language(DEFER)。
include(RinneMac OPTIONAL)

project(Rinne VERSION 0.9.0)

# 共享内存段名的前缀，编译期定死，只有三个值：正式版（默认）、本机 gates 构建（带 D）、探针（带 P）。
# 本机测试因此碰不到维护者在 DAW 里装的正式版；CI 与发布构建断言用的是默认值（scripts/build.ps1 -CheckCache）。
# 探针构建与探针前缀必须同时出现，免得探针和正式版在同一个宿主里互相配对。
set(RINNE_SHM_PREFIX_FORMAL "SynchainRinne")
set(_rinne_shm_values "${RINNE_SHM_PREFIX_FORMAL}" "${RINNE_SHM_PREFIX_FORMAL}D" "${RINNE_SHM_PREFIX_FORMAL}P")
if(RINNE_PROBE_BUILD)
  set(_rinne_shm_default "${RINNE_SHM_PREFIX_FORMAL}P")
else()
  set(_rinne_shm_default "${RINNE_SHM_PREFIX_FORMAL}")
endif()
set(RINNE_SHM_PREFIX "${_rinne_shm_default}" CACHE STRING "共享内存段名前缀：${_rinne_shm_values}")
set_property(CACHE RINNE_SHM_PREFIX PROPERTY STRINGS ${_rinne_shm_values})
# CI 断言时拿这一项比对；每次配置都覆盖，不给命令行留第二个入口
set(RINNE_SHM_PREFIX_DEFAULT "${_rinne_shm_default}" CACHE INTERNAL "本次配置的默认段名前缀")
if(NOT RINNE_SHM_PREFIX IN_LIST _rinne_shm_values)
  message(FATAL_ERROR "RINNE_SHM_PREFIX=${RINNE_SHM_PREFIX} 不在允许的三个值里：${_rinne_shm_values}")
endif()
if(RINNE_PROBE_BUILD AND NOT RINNE_SHM_PREFIX STREQUAL "${RINNE_SHM_PREFIX_FORMAL}P")
  message(FATAL_ERROR "探针构建必须用探针前缀 ${RINNE_SHM_PREFIX_FORMAL}P，现在是 ${RINNE_SHM_PREFIX}")
endif()
if(NOT RINNE_PROBE_BUILD AND RINNE_SHM_PREFIX STREQUAL "${RINNE_SHM_PREFIX_FORMAL}P")
  message(FATAL_ERROR "探针前缀只能用于探针构建（-DRINNE_PROBE_BUILD=ON）")
endif()

if(RINNE_SANITIZE AND NOT RINNE_SANITIZE MATCHES "^(asan_ubsan|tsan)$")
  message(FATAL_ERROR "RINNE_SANITIZE 只能是空、asan_ubsan 或 tsan，现在是 ${RINNE_SANITIZE}")
endif()
if(RINNE_STRESS_MUTATE AND NOT RINNE_STRESS_MUTATE MATCHES "^(no_read_fence|no_intent)$")
  message(FATAL_ERROR "RINNE_STRESS_MUTATE 只能是空、no_read_fence 或 no_intent，现在是 ${RINNE_STRESS_MUTATE}")
endif()
# 变异构建故意去掉内存序保护，只给压测证明测试会变红；插件永远不带它。
if(RINNE_STRESS_MUTATE AND NOT RINNE_CORE_ONLY)
  message(FATAL_ERROR "RINNE_STRESS_MUTATE 只能与 -DRINNE_CORE_ONLY=ON 同用")
endif()
if(NOT RINNE_BUILD_LABEL MATCHES "^[0-9A-Za-z.+-]*$")
  message(FATAL_ERROR "RINNE_BUILD_LABEL 只能有字母、数字与 . + -，现在是 ${RINNE_BUILD_LABEL}")
endif()

include(RinneWarnings)
if(NOT RINNE_CORE_ONLY)
  include(RinneJuceGuard)
  include(RinneDeps)
endif()

# 两个插件共用的格式与参数 ID 口径。格式按平台显式分支，不靠 JUCE 在非 Apple 平台上静默滤掉 AU。
set(RINNE_PLUGIN_FORMATS VST3)
if(APPLE)
  list(APPEND RINNE_PLUGIN_FORMATS AU)
endif()
# 参数 ID 的两个 JUCE 宏决定宿主工程里保存的自动化 ID，钉成 JUCE 8.0.8 的默认值：ID 由 ParamID 字符串散列得到，
# 散列清掉最高位（VST3 与 AU 同一规则）。任何一个翻转，已发布工程里的自动化就全部对不上。
# WebView2 一开始就按静态 loader 接好（NEEDS_WEBVIEW2 见各插件），编辑器换成 WebView 时不必再改构建。
set(RINNE_PLUGIN_DEFINITIONS
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
  JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1
  JUCE_WEB_BROWSER=1
)
if(WIN32)
  list(APPEND RINNE_PLUGIN_DEFINITIONS JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
endif()

if(RINNE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src/core)
if(NOT RINNE_CORE_ONLY)
  include(RinneWebAssets OPTIONAL)
  add_subdirectory(src/plugin-common)
  add_subdirectory(src/input)
  add_subdirectory(src/output)
endif()
if(RINNE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
if(RINNE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
