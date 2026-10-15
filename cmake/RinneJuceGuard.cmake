# SPDX-License-Identifier: GPL-3.0-or-later
#
# JUCE 检出的硬性守卫。JUCE 只经 -DJUCE_PATH 传入，检出必须恰好是仓根 .juce-commit 记的提交（唯一真源）；
# JUCE 自己 CMakeLists.txt 的版本行再与 .juce-version 对一遍，作为第二道检查。对不上一律 FATAL_ERROR：
# 同一台机器上可能放着别的 JUCE 版本，只给警告挡不住误用。两个钉版文件改了会自动重新配置。

set(JUCE_PATH "" CACHE PATH "JUCE 检出的目录，必须恰好是 .juce-commit 记的提交")
if(NOT JUCE_PATH)
  message(FATAL_ERROR "没有设 JUCE_PATH：-DJUCE_PATH=<JUCE 检出的目录>，检出必须是 .juce-commit 记的提交")
endif()
get_filename_component(_rinne_juce_dir "${JUCE_PATH}" ABSOLUTE)

set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/.juce-commit" "${CMAKE_SOURCE_DIR}/.juce-version")
file(READ "${CMAKE_SOURCE_DIR}/.juce-commit" _rinne_juce_want)
string(STRIP "${_rinne_juce_want}" _rinne_juce_want)
if(NOT _rinne_juce_want MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR ".juce-commit 必须是一行 40 位小写十六进制提交号")
endif()
string(LENGTH "${_rinne_juce_want}" _rinne_len)
if(NOT _rinne_len EQUAL 40)
  message(FATAL_ERROR ".juce-commit 必须是一行 40 位小写十六进制提交号")
endif()

find_package(Git REQUIRED QUIET)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${_rinne_juce_dir}" rev-parse HEAD
  OUTPUT_VARIABLE _rinne_juce_have
  ERROR_VARIABLE _rinne_juce_err
  RESULT_VARIABLE _rinne_juce_rc
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _rinne_juce_rc EQUAL 0)
  message(FATAL_ERROR "读不出 ${_rinne_juce_dir} 的 git HEAD（${_rinne_juce_err}）：JUCE_PATH 必须是 JUCE 的 git 检出")
endif()
if(NOT _rinne_juce_have STREQUAL _rinne_juce_want)
  message(FATAL_ERROR "JUCE_PATH 的 HEAD 是 ${_rinne_juce_have}，.juce-commit 要求 ${_rinne_juce_want}")
endif()

file(READ "${CMAKE_SOURCE_DIR}/.juce-version" _rinne_juce_version)
string(STRIP "${_rinne_juce_version}" _rinne_juce_version)
file(STRINGS "${_rinne_juce_dir}/CMakeLists.txt" _rinne_juce_project REGEX "^project\\(JUCE VERSION ")
if(NOT _rinne_juce_project MATCHES "^project\\(JUCE VERSION ([0-9.]+)")
  message(FATAL_ERROR "${_rinne_juce_dir}/CMakeLists.txt 里找不到 project(JUCE VERSION …) 这一行")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL _rinne_juce_version)
  message(FATAL_ERROR "JUCE_PATH 的版本行是 ${CMAKE_MATCH_1}，.juce-version 要求 ${_rinne_juce_version}")
endif()
message(STATUS "JUCE ${_rinne_juce_version}（${_rinne_juce_want}）")
