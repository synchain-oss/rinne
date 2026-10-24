// SPDX-License-Identifier: GPL-3.0-or-later
//
// 同 tests/host/probe/ 的两个包装：探针接线的源码只在探针构建里进插件，这里直接编进免 DAW 宿主测试，
// 两种构建里都覆盖得到。tests/host/suite.cmake 只列处理器与基类，不属于本目录，所以按文件包含。
#include "ProbeHost.cpp"
