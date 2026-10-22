// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路试验台的主机：同时托管一台 InputEngine 与一台 OutputEngine，中间是可配置的链，
// 两端经真实共享内存的环会合。
//
// 拓扑：
//
//   InputEngine  →  中间链  →  OutputEngine
//        ↑                            ↓
//        └──────────  环  ────────────┘
//
// 每一周期按脚本驱动：块长、跳过、走带、参数、调用顺序都由脚本给。
// 同一周期两端拿到同一份走带快照（含宿主时间戳），所以错序判据在 rig 里是可测的。
//
// 脚本能力（下游卡直接用，能力清单是契约，改动算接口事件）：
//   1. 每周期的调用顺序（Input 先 / Output 先 / 同周期 / 单端）。
//   2. 两端各自的块长脚本：恒定、随机、交替、0 样本穿插、违约块（n 大于声明值）、超长块（n 大于 32768）。
//   3. 单端跳过 k 块：这一端这一周期不被调用。
//   4. 每周期的 TransportInfo 脚本：走带状态、速度、时间戳及其有效位。同一周期两端拿到同一个时间戳
//      （错序判定靠它；两端拿不同的值由场景自己造，RigConfig 里有两端的偏移量）。
//   5. 逐块的 EngineParams 脚本。
//   6. 中间链的七种形态，逐周期可改。
//   7. 同时托管多台引擎：两个 Output（换写方）与两个 Input（复制体）。
//   8. 读两端引擎的状态对象。
//
// 线程：只在非实时线程用（建段、prepare 都要分配）。音频线程只拿这里给出的裸指针。

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/AudioView.h"
#include "engine/EngineParams.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"
#include "ipc/SegmentLayout.h"
#include "loop/LoopStatus.h"
#include "rig/MiddleChain.h"
#include "rig/RigConfig.h"
#include "rig/Schedule.h"
#include "rig/Segments.h"

namespace rinne::rig
{

// 主机：托管两端与中间链，按脚本跑周期。
class EngineHost
{
public:
    EngineHost();
    ~EngineHost();
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    // 建段、准备引擎。group 是组号（1..8）。返回 false 表示建段失败。
    bool open(std::uint32_t group = 1);

    // 非实时线程：按采样率与最大块长准备两端与中间链。可以分配。
    void prepare(double sampleRate, int maxBlock);

    // 脚本：给宿主装上要走的样子。
    void setBlockScripts(BlockScript input, BlockScript output) noexcept;
    void setTransportScript(TransportScript script) noexcept;
    void setParamsScript(ParamsScript script) noexcept;
    void setCallOrder(CallOrder order) noexcept { order_ = order; }
    void setChain(ChainKind kind) noexcept { chain_.setKind(kind); }
    void setInputBypassed(bool on) noexcept { inputBypassed_ = on; }
    void setOutputBypassed(bool on) noexcept { outputBypassed_ = on; }
    // 命令序号的基线（构造引擎后、prepare 前给一次）。场景要验「Panic 不重放」时用。
    void setCommandBaseline(const engine::CommandBaseline& baseline) noexcept;

    // 走一个周期。按调用顺序驱动两端。返回这一周期实际跑过的样本数（Input 侧）。
    int runCycle(std::size_t index);

    // 跑多个周期。
    void runCycles(std::size_t count, std::size_t first = 0);

    // 起第二个 Output（换写方场景）与第二个 Input（复制体场景）。第二个 Output 用新的写权 token。
    bool addSecondOutput();
    bool addSecondInput();

    // ---- 读出：两端引擎的状态对象 ----

    // Input 侧的环路层状态（诊断与界面要的量都在这里）
    const loop::LoopStatusFields& inputStatus() const noexcept { return inputStatus_; }
    // Output 侧的写环状态
    engine::WriteState outputState() const noexcept { return output_.state(); }
    std::uint64_t outputWrittenFrames() const noexcept { return output_.writtenFrames(); }
    std::uint32_t outputNonFiniteCount() const noexcept { return output_.nonFiniteCount(); }
    std::uint32_t outputClampCount() const noexcept { return output_.clampCount(); }
    std::uint32_t outputConflictCount() const noexcept { return output_.conflictCount(); }
    std::uint32_t outputUnsupportedCount() const noexcept { return output_.unsupportedCount(); }
    // 第二个 Output（换写方场景）的读出
    engine::WriteState secondOutputState() const noexcept { return secondOutput_.state(); }
    std::uint64_t secondOutputWrittenFrames() const noexcept { return secondOutput_.writtenFrames(); }
    // 第二个 Input（复制体场景）的读出
    const loop::LoopStatusFields& secondInputStatus() const noexcept { return secondInputStatus_; }

    // 中间链（场景要读它的形态与状态）
    MiddleChain& chain() noexcept { return chain_; }
    const MiddleChain& chain() const noexcept { return chain_; }

    // 本周期 Input 侧的输出缓冲（干声 + 回送），场景用来断言
    const std::vector<float>& inputOutputLeft() const noexcept { return outLeft_; }
    const std::vector<float>& inputOutputRight() const noexcept { return outRight_; }
    // 本周期 Output 侧送进环的信号（宿主缓冲经引擎清洗之后）
    const std::vector<float>& outputFedLeft() const noexcept { return fedLeft_; }
    const std::vector<float>& outputFedRight() const noexcept { return fedRight_; }

    // 环的写头（诊断用）
    std::uint64_t writeHead() const noexcept;
    // 环的帧数（诊断用）
    std::uint32_t ringFrames() const noexcept;

    // 排空两端的事件（诊断用）。回调收到 (code, a, b, sample)。
    template<typename Sink>
    void drainEvents(Sink&& sink) noexcept
    {
        input_.drainEvents(sink);
        output_.drainEvents(sink);
    }

    // 配置（读写）
    RigConfig& config() noexcept { return config_; }
    const RigConfig& config() const noexcept { return config_; }

private:
    void fillHostBuffers(int n, std::uint64_t cycle) noexcept;
    int runInput(std::size_t index, int n) noexcept;
    void runOutput(std::size_t index, int n) noexcept;
    void runSecondOutput(std::size_t index, int n) noexcept;
    void runSecondInput(std::size_t index, int n) noexcept;
    // 本周期两端各自的走带快照（同一份 + 各自的偏移量）。
    engine::TransportInfo transportFor(std::size_t index, std::int64_t hostNsOffset) const noexcept;

    RigConfig config_;
    Segments segments_;
    MiddleChain chain_;

    engine::InputEngine input_;
    engine::OutputEngine output_;
    engine::InputEngine secondInput_;
    engine::OutputEngine secondOutput_;
    bool haveSecondOutput_ = false;
    bool haveSecondInput_ = false;

    // 写权 token：Output 与第二个 Output 各一个（换写方场景靠这个分出「谁在写」）
    std::uint64_t outputToken_ = 1;
    std::uint64_t secondOutputToken_ = 2;

    std::uint32_t group_ = 1;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 1024;
    bool prepared_ = false;

    BlockScript inputScript_;
    BlockScript outputScript_;
    TransportScript transportScript_;
    ParamsScript paramsScript_;
    CallOrder order_ = CallOrder::kInputFirst;
    bool inputBypassed_ = false;
    bool outputBypassed_ = false;

    // 宿主侧缓冲（预分配，音频线程上不再分配）
    std::vector<float> hostLeft_;
    std::vector<float> hostRight_;
    std::vector<float> outLeft_;
    std::vector<float> outRight_;
    std::vector<float> fedLeft_;
    std::vector<float> fedRight_;
    std::vector<float> chainLeft_;
    std::vector<float> chainRight_;
    std::vector<float> secondLeft_;
    std::vector<float> secondRight_;

    // 每声道的指针数组（AudioView 要的就是这个形状）
    std::vector<float*> hostChannels_{nullptr, nullptr};
    std::vector<float*> outChannels_{nullptr, nullptr};
    std::vector<float*> fedChannels_{nullptr, nullptr};
    std::vector<float*> secondChannels_{nullptr, nullptr};
    std::vector<float*> chainChannels_{nullptr, nullptr};

    // 走带快照：同一周期两端拿同一份（加各自的偏移量）
    engine::TransportInfo transport_;

    loop::LoopStatusFields inputStatus_;
    loop::LoopStatusFields secondInputStatus_;

    // 宿主信号：按周期递增的斜坡（每个样本一个可辨认的值，便于断言「读到的是哪一帧」）
    std::uint64_t hostCounter_ = 0;
};

} // namespace rinne::rig
