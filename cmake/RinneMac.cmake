# SPDX-License-Identifier: GPL-3.0-or-later
#
# 只在 Apple 上起作用的设置。根 CMakeLists.txt 在 project() 之前 include 它：架构与部署目标参与编译器探测，project() 之后再设就晚了。
# 这时 APPLE 还没有定义，所以按宿主系统名判断。
#   正式构建     arm64 单架构，部署目标 macOS 11.0（ADR-0005）；Intel 上无法实测，不出 universal 的正式包
#   探针构建     universal（arm64 + x86_64），借来的机器可能是 Intel
# 命令行或调用方已经给了值就不覆盖（缓存里有就是有）：CI 不在命令行传这两项，证明的就是这里的默认值在起作用。
# RINNE_HAS_POSIX_SHM：shm_open 可用（Apple 与 Linux），IPC 的 POSIX 后端按它决定编不编；Windows 上不设。

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    if(RINNE_PROBE_BUILD)
      set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS 目标架构（探针构建为 universal）")
    else()
      set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS 目标架构（正式构建只出 arm64）")
    endif()
  endif()
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS 最低系统版本")
  endif()
endif()

if(CMAKE_HOST_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
  set(RINNE_HAS_POSIX_SHM ON)
else()
  set(RINNE_HAS_POSIX_SHM OFF)
endif()
