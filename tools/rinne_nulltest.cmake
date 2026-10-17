# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_nulltest：WAV 对拍与回声间隔测量。不依赖 JUCE 和 rinne_core，RINNE_CORE_ONLY 下也建。
# 自测 tests/tools/nulltest/selftest.cmake 用 cmake -P 驱动这个可执行文件，套件按 ctest 的一条测试注册在这里。

add_executable(rinne_nulltest rinne_nulltest.cpp)
rinne_target_warnings(rinne_nulltest PRIVATE)
rinne_warnings_as_errors(rinne_nulltest)
if(NOT MSVC)
  # 生成的测试信号要逐字节可复现：不让编译器把乘加融合成 fma
  target_compile_options(rinne_nulltest PRIVATE -ffp-contract=off)
endif()

if(RINNE_BUILD_TESTS)
  add_test(NAME rinne_nulltest_selftest
    COMMAND "${CMAKE_COMMAND}"
            "-DNULLTEST=$<TARGET_FILE:rinne_nulltest>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nulltest-selftest"
            -P "${CMAKE_SOURCE_DIR}/tests/tools/nulltest/selftest.cmake")
  set_tests_properties(rinne_nulltest_selftest PROPERTIES TIMEOUT 120)
endif()
