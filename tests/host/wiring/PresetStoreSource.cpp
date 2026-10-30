// SPDX-License-Identifier: GPL-3.0-or-later
//
// 同 InstanceHostSource.cpp：正式构建的 Input 处理器持有预设库，插件里由 src/input/sources-presets.cmake 编进，
// 这里按文件包含编进免 DAW 宿主测试；工厂预设经 suite.cmake 链进来的同一个资源目标嵌入。
#include "PresetStore.cpp"
