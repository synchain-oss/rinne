# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/mac-ci.json
#
# rinne_auhost：进程外 AU 宿主自测。独立的 CMake 工程，不进根工程：根 CMakeLists.txt 与 tests/CMakeLists.txt 都不 add_subdirectory
# 本目录，Windows 构建与本机 gates 不受影响。只在 build.yml 的 mac 构建里单独配置：
#   cmake -S tests/oop -B build-oop -DCMAKE_BUILD_TYPE=Release
# 宿主只链 AudioToolbox 与 CoreFoundation，去加载同一个 run 里已经构建、已经装好的 AU，所以这里不编任何插件。
cmake_minimum_required(VERSION 3.22)

# 必须在 project() 之前：参与编译器探测。与根工程的默认值一致，命令行给了就不覆盖。
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS 目标架构")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS 最低系统版本")
endif()

project(RinneAuHost VERSION 0.0.1 LANGUAGES CXX)

if(NOT APPLE)
  message(FATAL_ERROR "tests/oop 只在 macOS 上构建：它加载 AudioUnit，并经 AUHostingService 把插件放到进程外")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rinne_auhost rinne_auhost.cpp)
# 警告即错误，与根工程对第一方代码的口径一致；-fblocks：AudioComponentInstantiate 的完成回调是 block
target_compile_options(rinne_auhost PRIVATE -fblocks -Wall -Wextra -Werror)
target_link_libraries(rinne_auhost PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
