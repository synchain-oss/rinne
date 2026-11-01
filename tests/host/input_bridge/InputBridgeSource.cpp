// SPDX-License-Identifier: GPL-3.0-or-later
//
// 同 output_bridge/OutputBridgeSource.cpp：Input 原生桥的纯逻辑，插件里由 src/input/sources-editor.cmake 编进，
// 这里按文件包含编进免 DAW 宿主测试，用例直接驱动 InputBridge（不开浏览器组件，编辑器不参与编译）。
#include "InputBridge.cpp"
