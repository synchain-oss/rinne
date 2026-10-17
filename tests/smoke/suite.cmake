# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_smoke 的额外设置：sanitizer 配置下把「期望哪个 sanitizer 生效」传给测试，
# test_sanitizer_active.cpp 据此断言编译器真的插了桩。否则哪天 rinne_core 的 PUBLIC 编译选项改成 PRIVATE，
# core-sanitizers 照样是绿的，只是什么都没检查。

if(RINNE_SANITIZE STREQUAL "asan_ubsan")
  target_compile_definitions(rinne_smoke PRIVATE RINNE_EXPECT_ASAN=1)
elseif(RINNE_SANITIZE STREQUAL "tsan")
  target_compile_definitions(rinne_smoke PRIVATE RINNE_EXPECT_TSAN=1)
endif()
