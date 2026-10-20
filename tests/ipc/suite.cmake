# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_ipc_tests 的跨进程设置。套件本身由 tests/CMakeLists.txt 的 rinne_add_test_suite 建好并注册 ctest。
#
# 根 CMakeLists 先 add_subdirectory(tests) 再 add_subdirectory(tools)，所以这里执行时 TARGET rinne_ipc_peer 还
# 不存在。把「有没有 peer、peer 在哪」的接线推迟到顶层目录处理结束时再做：那时 tools/ 已经 include 完，
# TARGET 判定才是真的。没有 peer（tools 关掉或目标缺失）时定义 RINNE_IPC_NO_PEER，用例自己 SKIP 并说明原因。
function(rinne_ipc_wire_peer)
  if(TARGET rinne_ipc_peer AND TARGET rinne_ipc_tests)
    add_dependencies(rinne_ipc_tests rinne_ipc_peer)
    target_compile_definitions(rinne_ipc_tests PRIVATE RINNE_IPC_PEER_PATH="$<TARGET_FILE:rinne_ipc_peer>")
  elseif(TARGET rinne_ipc_tests)
    target_compile_definitions(rinne_ipc_tests PRIVATE RINNE_IPC_NO_PEER)
  endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL rinne_ipc_wire_peer)

# 套件已经由 rinne_add_test_suite 加上 tests/；这里显式写出来，让 support/Spawn.h 的包含路径不依赖调用方的实现
target_include_directories(rinne_ipc_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# 跨进程用例会按正式作用域探共享内存段，同一时刻只许一个套件碰这个命名空间
set_tests_properties(rinne_ipc_tests PROPERTIES RESOURCE_LOCK rinne_formal_shm)

# 与脚本 scripts/ci/mac/shm-leftover.sh 同名同标签的单独一条：残段检查直接跑它
add_test(NAME rinne_ipc_shm_leftover COMMAND rinne_ipc_tests "[shm-leftover]")
set_tests_properties(rinne_ipc_shm_leftover PROPERTIES TIMEOUT 60 RESOURCE_LOCK rinne_formal_shm)
