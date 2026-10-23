// SPDX-License-Identifier: GPL-3.0-or-later
//
// 试验台（loop rig）的配置：一台试验台托管几台引擎、采样率与块长、组号与共享内存后端。
//
// 作用域不在这里：EngineHost 每次自己随机取一个（ipc::NameScope::randomForTesting），
// 于是用例之间天然隔离，并行跑不撞段名。测试的命名空间只经 API 注入，发布版不读环境变量。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rinne::rig
{

// 一台试验台的配置。默认值就是「最普通的一台」：48 kHz、4096 帧的最大块长、组 1、一台 Input 与一台 Output。
struct RigConfig
{
    // 采样率（Hz）：两端都用它 prepare。
    double sampleRate = 48000.0;
    // 本端 prepare 声明的最大块长（N_prep）。块长脚本给出比它大的块就是宿主违约（补 0 并抬升延迟下限）。
    int maxBlock = 4096;
    // 组号 1..8：会合段与数据段按它取名。
    std::uint32_t group = 1;
    // 托管几台 Input 引擎（复制体场景要两台以上）。
    std::size_t inputCount = 1;
    // 托管几台 Output 引擎（换写方场景要两台以上）。
    std::size_t outputCount = 1;
    // 中间链接在哪一台 Input 的后面（宿主链上的顺序）：0 = 第一台。
    std::size_t chainSourceInput = 0;
    // POSIX 后端的锁目录；空 = 用平台默认（真实 home 下的应用支持目录）。
    std::string posixLockDir;
    // 写进两端端点槽里的 hostNs 相对本周期值的偏移：给错序场景用（正数 = 那一端看起来更靠前）。
    std::int64_t inputHostNsOffset = 0;
    std::int64_t outputHostNsOffset = 0;
};

} // namespace rinne::rig
