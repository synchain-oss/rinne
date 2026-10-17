# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/tests.json
#
# 测试入口（RINNE_BUILD_TESTS 打开且本文件存在时由根 CMakeLists.txt 加入）。
# 套件按目录收，一个目录一个可执行文件，目录里没有 .cpp 就不建：
#   tests/smoke/**    rinne_smoke        冒烟：构建信息、Catch2 与线程基本盘；所有配置都跑（含 sanitizer）
#   tests/core/**     rinne_core_tests   rinne_core 的单测
#   tests/loop_rig/** rinne_loop_rig     环路试验台
#   tests/stress/**   rinne_stress       压测（RINNE_STRESS_MUTATE 变异构建只作用于它）
#   tests/ipc/**      rinne_ipc_tests    跨进程契约
#   tests/params/**   rinne_params_tests 参数层（需要 JUCE）
#   tests/host/**     rinne_host_tests   免 DAW 宿主里的处理器（需要 JUCE）
# 新增测试只往对应目录里放 .cpp，不改这里。套件要额外的源码、链接或 ctest 设置，写在 tests/<套件>/suite.cmake 里：
# 建好目标之后 include，用套件的目标名（上表第二列）加源码、链接和 ctest 设置。
# 每条 ctest 都必须有显式 TIMEOUT：rinne_add_test_suite 统一设，别处自己 add_test 的要自己写；
# 末尾的 ctest_timeouts_scan 向 ctest 要它会跑的全部测试（含后注册的），缺 TIMEOUT 或 TIMEOUT 不为正的会让它变红。
# sanitizer 构建（RINNE_SANITIZE，只在 Linux 上用）的选项在 src/core/CMakeLists.txt 里经 rinne_core 传下来。

include(FetchContent)

# Catch2 只给测试，不进插件。钉 tag v3.5.4 所指的提交（注解 tag 剥开后的 40 位提交）：tag 可以被移动，提交不会。
# 不用 GIT_SHALLOW：浅克隆只认分支和 tag 名。升版本时 tag 与提交一起改，并同步 THIRD-PARTY-NOTICES.md 的 catch2 小节。
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG b5373dadca40b7edc8570cf9470b9b1cb1934d40) # v3.5.4
FetchContent_MakeAvailable(Catch2)
# 第三方头文件当系统头文件：第一方的告警即错误不该被 Catch2 的宏牵连
get_target_property(_rinne_catch2_inc Catch2 INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(Catch2 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_rinne_catch2_inc}")

# sanitizer 的运行时选项：遇到第一个问题就失败，不要让它只打印
set(_rinne_test_env "")
if(RINNE_SANITIZE STREQUAL "asan_ubsan")
  list(APPEND _rinne_test_env
    "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1:halt_on_error=1"
    "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
elseif(RINNE_SANITIZE STREQUAL "tsan")
  list(APPEND _rinne_test_env
    "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp:halt_on_error=1:second_deadlock_stack=1")
endif()

# rinne_add_test_suite(<目标名> <目录> [TIMEOUT <秒>] [NEEDS_JUCE] [SOURCES <额外源码>...])
#   <目录> 是 tests/ 下的相对目录，其中递归收 .cpp；一个都没有就不建。目标默认链 rinne_core 与 Catch2WithMain。
#   NEEDS_JUCE 的套件在 RINNE_CORE_ONLY 下不建。
function(rinne_add_test_suite name dir)
  cmake_parse_arguments(ARG "NEEDS_JUCE" "TIMEOUT" "SOURCES" ${ARGN})
  if(ARG_NEEDS_JUCE AND RINNE_CORE_ONLY)
    return()
  endif()
  file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
  if(NOT _sources)
    return()
  endif()
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 120)
  endif()
  add_executable(${name} ${_sources} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE rinne_core Catch2::Catch2WithMain)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  rinne_target_warnings(${name} PRIVATE)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
  if(_rinne_test_env)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_rinne_test_env}")
  endif()
endfunction()

# 套件建好后统一收尾：include 它自己的 suite.cmake，再把告警即错误标在最终的源码清单上
macro(rinne_finish_test_suite name dir)
  if(TARGET ${name})
    include("${CMAKE_CURRENT_SOURCE_DIR}/${dir}/suite.cmake" OPTIONAL)
    rinne_warnings_as_errors(${name})
  endif()
endmacro()

rinne_add_test_suite(rinne_smoke smoke TIMEOUT 60)
rinne_finish_test_suite(rinne_smoke smoke)
rinne_add_test_suite(rinne_core_tests core TIMEOUT 120)
rinne_finish_test_suite(rinne_core_tests core)
rinne_add_test_suite(rinne_loop_rig loop_rig TIMEOUT 300)
rinne_finish_test_suite(rinne_loop_rig loop_rig)
rinne_add_test_suite(rinne_stress stress TIMEOUT 600)
rinne_finish_test_suite(rinne_stress stress)
rinne_add_test_suite(rinne_ipc_tests ipc TIMEOUT 300)
rinne_finish_test_suite(rinne_ipc_tests ipc)
rinne_add_test_suite(rinne_params_tests params NEEDS_JUCE TIMEOUT 120)
rinne_finish_test_suite(rinne_params_tests params)
rinne_add_test_suite(rinne_host_tests host NEEDS_JUCE TIMEOUT 300)
rinne_finish_test_suite(rinne_host_tests host)

# UBSan 金丝雀，用工程自己的编译选项构建（继承 rinne_core 的 PUBLIC 选项），不是脚本带着 -fsanitize 另编的那份：
# 把工程配置里的 undefined 去掉，它就不再报告，ctest 判红。UBSan 没有 ASan / TSan 那样可查询的编译器特性宏，只能这样证明。
# 与 sanitizer-canary/run.sh 共用同一个源文件；PASS_REGULAR_EXPRESSION 下 ctest 只看输出里有没有报告，不看退出码。
if(RINNE_SANITIZE STREQUAL "asan_ubsan")
  add_executable(rinne_ubsan_canary sanitizer-canary/signed_overflow.cpp)
  target_link_libraries(rinne_ubsan_canary PRIVATE rinne_core)
  add_test(NAME rinne_ubsan_canary COMMAND rinne_ubsan_canary)
  set_tests_properties(rinne_ubsan_canary PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "runtime error:")
  if(_rinne_test_env)
    set_tests_properties(rinne_ubsan_canary PROPERTIES ENVIRONMENT "${_rinne_test_env}")
  endif()
endif()

# ctest 的 TIMEOUT 门禁：自测加实扫，都在 ctest 里跑，本机 gates、CI 的 build-win 与 core-sanitizers 因此同吃一份。
# 实扫向 ctest 要它真正会跑的测试全集（--show-only=json-v1），含 discover_tests 之类后注册的，逐条要求 TIMEOUT 为正数。
# 本机 gates 与两种 CI runner 都有 pwsh；找不到就是环境坏了，直接停下，不让门禁悄悄消失。
find_program(RINNE_PWSH NAMES pwsh)
if(NOT RINNE_PWSH)
  message(FATAL_ERROR "找不到 pwsh：ctest 的 TIMEOUT 门禁（scripts/check-ctest-timeouts.ps1）跑不起来。装 PowerShell 7 再配置")
endif()
set(_rinne_timeout_script "${CMAKE_SOURCE_DIR}/scripts/check-ctest-timeouts.ps1")
add_test(NAME ctest_timeouts_selftest
  COMMAND "${RINNE_PWSH}" -NoProfile -NonInteractive -File "${_rinne_timeout_script}" -SelfTest)
add_test(NAME ctest_timeouts_scan
  COMMAND "${RINNE_PWSH}" -NoProfile -NonInteractive -File "${_rinne_timeout_script}"
          -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>" -Ctest "${CMAKE_CTEST_COMMAND}")
set_tests_properties(ctest_timeouts_selftest ctest_timeouts_scan PROPERTIES TIMEOUT 60)
