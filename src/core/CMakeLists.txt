# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_core：C++17 静态库，承载 IPC、环路、DSP、安全状态机、state 与诊断。不 include、不链接 JUCE，能脱离宿主单测。
# 源码按子目录收（ipc/、loop/、dsp/……），新文件、新子目录不必改这里；文件名约定决定平台：
#   *Win32.cpp / *Win32.h  只在 Windows 上编译
#   *Posix.cpp / *Posix.h  只在 Apple 与 Linux 上编译
#   *.mm                   只在 Apple 上编译
# 构建期常量（版本、构建标签、段名前缀、是否探针构建）由 util/RinneVersion.h.in 生成到构建目录，
# 以 #include "util/RinneVersion.h" 取用。

file(GLOB_RECURSE _rinne_core_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.mm")
if(NOT WIN32)
  list(FILTER _rinne_core_sources EXCLUDE REGEX "Win32\\.(cpp|h)$")
endif()
if(NOT UNIX)
  list(FILTER _rinne_core_sources EXCLUDE REGEX "Posix\\.(cpp|h)$")
endif()
if(NOT APPLE)
  list(FILTER _rinne_core_sources EXCLUDE REGEX "\\.mm$")
endif()

set(RINNE_PROBE_BUILD_VALUE false)
if(RINNE_PROBE_BUILD)
  set(RINNE_PROBE_BUILD_VALUE true)
endif()
configure_file(util/RinneVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/util/RinneVersion.h" @ONLY)
set_source_files_properties(util/RinneVersion.cpp PROPERTIES COMPILE_DEFINITIONS "RINNE_BUILD_LABEL_STRING=\"${RINNE_BUILD_LABEL}\"")

add_library(rinne_core STATIC ${_rinne_core_sources} "${CMAKE_CURRENT_BINARY_DIR}/generated/util/RinneVersion.h")
target_include_directories(rinne_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(rinne_core PUBLIC cxx_std_17)
rinne_target_warnings(rinne_core PRIVATE)

# 不同编译器、不同块长下的结果要逐位一致：非 MSVC 不许把乘加合并成 FMA（MSVC 默认就不合并）。
if(NOT MSVC)
  target_compile_options(rinne_core PUBLIC -ffp-contract=off)
endif()

# sanitizer 只作用于 core 与链接它的测试，PUBLIC 传给测试目标。
if(RINNE_SANITIZE STREQUAL "asan_ubsan")
  target_compile_options(rinne_core PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(rinne_core PUBLIC -fsanitize=address,undefined)
elseif(RINNE_SANITIZE STREQUAL "tsan")
  target_compile_options(rinne_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(rinne_core PUBLIC -fsanitize=thread)
endif()

# 压测的变异构建：RINNE_STRESS_MUTATE_NO_READ_FENCE / RINNE_STRESS_MUTATE_NO_INTENT，PUBLIC 让压测里内联的代码也变。
if(RINNE_STRESS_MUTATE)
  string(TOUPPER "${RINNE_STRESS_MUTATE}" _rinne_mutate)
  target_compile_definitions(rinne_core PUBLIC RINNE_STRESS_MUTATE_${_rinne_mutate}=1)
endif()

rinne_warnings_as_errors(rinne_core)
