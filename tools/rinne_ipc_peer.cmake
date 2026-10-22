# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_ipc_peer：跨进程测试用的对端进程，只链 rinne_core。tests/ipc/suite.cmake 通过 RINNE_IPC_PEER_PATH
# 把它的绝对路径编进 rinne_ipc_tests。
add_executable(rinne_ipc_peer rinne_ipc_peer.cpp)
target_link_libraries(rinne_ipc_peer PRIVATE rinne_core)
rinne_target_warnings(rinne_ipc_peer PRIVATE)
rinne_warnings_as_errors(rinne_ipc_peer)
