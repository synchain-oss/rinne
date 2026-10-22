# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_loop_rig 套件的额外设置（rinne_add_test_suite 建好目标之后被 include）。
#
# 套件的源码由 tests/CMakeLists.txt 的 GLOB 收（tests/loop_rig/**/*.cpp），
# 这里只做三件事：
#   1. 给 rig/ 与 scenarios/ 加包含路径：源文件用相对路径「rig/xxx.h」互相找头文件，
#      所以包含根要是 tests/loop_rig/（rig/ 的父目录），这样 "rig/MiddleChain.h" 才找得到。
#      rig/ 与 scenarios/ 各自的目录也加进去：场景文件之间用 "xxx.h" 互相找。
#      注意用 CMAKE_CURRENT_LIST_DIR（是本文件所在的目录），不用 CMAKE_CURRENT_SOURCE_DIR
#      （那是 include 它的 tests/CMakeLists.txt 所在的目录）。
#   2. 钉住场景 id 的前缀（场景要按 id 分组，测试台按前缀过滤）。
#   3. 注册额外的 ctest：每条都要带显式 TIMEOUT（ctest 的 TIMEOUT 门禁会实扫，缺 TIMEOUT 判红）。

target_include_directories(rinne_loop_rig PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(rinne_loop_rig PRIVATE "${CMAKE_CURRENT_LIST_DIR}/rig")
target_include_directories(rinne_loop_rig PRIVATE "${CMAKE_CURRENT_LIST_DIR}/scenarios")

# 场景 id 的前缀：场景文件里的 id 必须以它开头（测试台按前缀挑自己那批场景）。
target_compile_definitions(rinne_loop_rig PRIVATE RINNE_RIG_SCENARIO_PREFIX="s")

# 框架自身的用例已经落在 rinne_loop_rig 这个可执行文件里（GLOB 收的 .cpp）。
# 这里不再单独注册 ctest：同一套件里已有的用例已经随目标一起跑。
