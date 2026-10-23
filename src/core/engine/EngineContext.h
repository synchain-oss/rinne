// SPDX-License-Identifier: GPL-3.0-or-later
//
// 一块要用的宿主侧上下文：数据段、会合段、组号、写权。两个引擎共用这一份，
// 于是 InputEngine::process 与 OutputEngine::process 的形状一致，壳只写一套调用代码。
//
// 这些指针都由壳在块首经租约（ipc::BoundBlock）拿到，整块有效，块末归还。引擎不查名字、不开段、不加锁。
//
// 角色由引擎自己决定（InputEngine 只写 Input 槽、只读 Output 槽；OutputEngine 只写 Output 槽），
// 不由调用方传：传错角色就会写到对端的槽里，那违反「环头与槽只由属主写」。
//
// 会合段只在本端确实是该槽的属主时给（BoundBlock::ok() 且 stillMine()）；不是属主就给空，
// 引擎因此不写任何端点槽。
#pragma once

#include <cstdint>

#include "ipc/SegmentLayout.h"

namespace rinne::engine
{

struct EngineContext
{
    // 数据段（环）。Input 侧读它，Output 侧写它。可以为空（没有对端 → Input 只出干声，Output 什么都不写）。
    ipc::RingSegment* ring = nullptr;
    // 会合段。Input 侧从中读对端槽并发布自己的槽；Output 侧发布自己的槽。可以为空。
    ipc::RvSegment* rendezvous = nullptr;
    // 组号 1..8。会合段按组块排布，组号 0 或越界时不碰端点槽（会越界）。
    std::uint32_t group = 0;
    // 自己的写权 token（Output 用；0 = 没有写权）。Input 侧不用。
    std::uint64_t token = 0;
    // 段的映射代号：壳在块首从绑定快照取（LoopBinding::seq，每次换段、换组、换写方、重新映射都发布一份新快照，
    // 序号随之变）。同一块映射的整个寿命里不变；换了映射就换代号，哪怕新映射恰好落在同一个地址上。
    // Output 据此判「换段」并复位写方助手（不看地址）；Input 据此（连同地址）判「换段」并重建读头，
    // 旧读头只读旧段已拉进来的数据。只比地址会漏掉同一地址上的重新映射。壳必须给；0 = 没给，
    // 那时 Output 判不出换段（只按写权与采样率起任期），Input 只能按地址判。
    std::uint64_t ringGeneration = 0;
};

} // namespace rinne::engine
