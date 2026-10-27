// SPDX-License-Identifier: GPL-3.0-or-later
//
// 资源提供者的源码编进免 DAW 宿主测试。宿主测试目标不开浏览器组件，也不链接 WebView2，
// 所以只编这一份不依赖它们的源码，每份源码一个包装文件，避免匿名命名空间里的名字在同一个翻译单元里撞车。
#include "ResourceProvider.cpp"
