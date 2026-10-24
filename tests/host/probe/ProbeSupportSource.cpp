// SPDX-License-Identifier: GPL-3.0-or-later
//
// 免 DAW 宿主测试不链接插件目标，探针支持件的源码直接编进测试目标。tests/host/suite.cmake 只列处理器与基类，
// 不属于本目录，所以这里按文件包含，不改那份清单。
#include "ProbeSupport.cpp"
