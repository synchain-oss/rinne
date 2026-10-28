// SPDX-License-Identifier: GPL-3.0-or-later
//
// 同 tests/host/probe_host/ 的包装：实例接线的源码在插件里经 rinne_plugin_common 编进，这里直接编进免 DAW 宿主测试
// （两种构建都要：正式构建的处理器直接持有它，探针构建的 ProbeHost 建在它之上）。tests/host/suite.cmake 只列处理器与
// 基类，不属于本目录，所以按文件包含。
#include "InstanceHost.cpp"
