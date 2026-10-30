// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路试验台的主机：同时托管 InputEngine 与 OutputEngine（各可再起一台），中间是可配置的链，
// 两端经真实共享内存的环会合，环路是闭合的。
//
// 拓扑（一条轨道，宿主缓冲按调用顺序依次流过各级）：
//
//   信号源 → [InputEngine：干声 + 回送] → [中间链] → [OutputEngine：写环] → 轨道输出
//                 ↑                                         ↓
//                 └──────────────────  环  ─────────────────┘
//
// 每一周期：信号源先往轨道缓冲里填这一周期的样本，再按调用顺序让缓冲依次流过各级：
//   kInputFirst   Input → 中间链 → Output（正确的摆放：回送经中间链再进环，环路闭合）
//   kOutputFirst  中间链 → Output → Input（错放：Output 在 Input 之前，环里只有干声，回送不再进环）
//   kOutputOnly   中间链 → Output
//   kInputOnly    Input
// 中间链恒在 Output 之前：它是「环内插件」的替身。
//
// 脚本能力（下游直接用，能力清单是接口，改动算接口事件）：
//   1. 每周期的调用顺序（Input 先 / Output 先 / 单端）。
//   2. 两端各自的块长脚本：恒定、交替、0 样本穿插、违约块（n 大于声明值）、超长块（n 大于 32768）。
//   3. 单端跳过 k 块：这一端这一周期不被调用。
//   4. 每周期的 TransportInfo 脚本：走带状态、速度、时间戳及其有效位。同一周期两端拿到同一个时间戳
//      （错序判定靠它；两端拿不同的值由场景自己造，RigConfig 里有两端的偏移量）。
//   5. 逐块的 EngineParams 脚本。
//   6. 中间链的七种形态，逐周期可改；Input 报出起播沿的 DSP 复位点时，中间链在同一个样本上复位。
//   7. 同时托管多台引擎：第二个 Output（换写方：switchWriter 把写权与 Output 槽交给它）
//      与第二个 Input（复制体：默认不拿环也不拿槽，只出干声；setSecondInputReadsRing 让它读环）。
//   8. 读两端引擎的状态对象、两端的端点槽、Input 的上一块结果与诊断安全段。
//   9. 信号源脚本（静音、冲激、斜坡或场景自己的函数）。
//  10. 只重新准备一端（prepareOutput / prepareInput）：两端采样率可以不同，写方按自己的采样率发布环头。
//  11. 越界哨兵：每周期在轨道缓冲的有效样本之后放一串哨兵位型，周期末核对（canaryHits）。
//  12. 两端各自的声道数（setChannels，1 或 2）：单声道的一端只看到第 0 个声道，轨道缓冲的第 1 个声道不碰。
//  13. 单端切片（setSlicing）：Input 或 Output 一端每个周期的块再切成带种子随机的小块依次调用，另一端仍整块。
//
// 线程：只在非实时线程用（建段、prepare 都要分配）。引擎的 process 在这里同步调用，模拟音频线程。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "engine/AudioView.h"
#include "engine/EngineContext.h"
#include "engine/EngineParams.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "ipc/SegmentLayout.h"
#include "loop/LoopStatus.h"
#include "rig/MiddleChain.h"
#include "rig/RigConfig.h"
#include "rig/Schedule.h"
#include "rig/Segments.h"

namespace rinne::rig
{

// 信号源：往 left / right 里填 n 个样本，第一个样本的绝对编号是 first（跨周期连续）。
using SourceFn = std::function<void(std::uint64_t first, float* left, float* right, int n)>;

// 几个常用的信号源。
SourceFn silenceSource();
// 一个冲激：绝对编号 at 处为 amplitude（左右都是），其余为 0。
SourceFn impulseSource(std::uint64_t at, float amplitude);
// 斜坡：编号 k 处左声道为 (k mod 65536)/65536，右声道取反。值都在 (−1, 1) 里，读回来能对上第几帧。
SourceFn rampSource();

// 主机：托管两端与中间链，按脚本跑周期。
class EngineHost
{
public:
    EngineHost();
    ~EngineHost();
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    // 建段、发布写权。group 是组号（1..8）。返回 false 表示建段失败。
    bool open(std::uint32_t group = 1);

    // 非实时线程：按采样率与最大块长准备两端与中间链。可以分配。
    void prepare(double sampleRate, int maxBlock);

    // 只重新准备一端（宿主只对一个插件停用再激活，可以换采样率）。另一端、中间链与信号源都不动。
    // prepareOutput 同时按新的采样率重新发布写权（生产里由会合层做）并更新 Output 槽的最大块长：
    // 环头里写方的采样率因此变成 sampleRate，Input 一侧据此判两端采样率是否一致。
    // prepareInput 只动 Input（环头里没有 Input 的采样率）。
    void prepareOutput(double sampleRate, int maxBlock);
    void prepareInput(double sampleRate, int maxBlock);

    // 脚本：给宿主装上要走的样子。
    // setBlockScripts 会按脚本里最大的块长重新给宿主缓冲（超长块也要送得进去），所以它会分配。
    void setBlockScripts(BlockScript input, BlockScript output);
    void setTransportScript(TransportScript script) noexcept;
    void setParamsScript(ParamsScript script) noexcept;
    void setSource(SourceFn source) { source_ = std::move(source); }
    void setCallOrder(CallOrder order) noexcept { order_ = order; }
    void setChain(ChainKind kind) noexcept { chain_.setKind(kind); }
    // 两端各自的声道数（1 = 单声道，2 = 立体声），默认都是 2。单声道的一端只拿到轨道缓冲的第 0 个声道
    // （AudioView 的约定：左右都取第 0 个）；缓冲的第 1 个声道它不碰，中间链照常处理两个声道。
    void setChannels(int inputChannels, int outputChannels) noexcept
    {
        inputChannels_ = inputChannels == 1 ? 1 : 2;
        outputChannels_ = outputChannels == 1 ? 1 : 2;
    }
    // 单端切片：这一端每个周期的块再切成长度在 [1, maxSlice] 的带种子随机小块依次调用（块长之和不变，另一端仍整块）。
    // maxSlice 为 0 = 不切。小块拿到同一份走带信息，所以只用于走带不变（没有起播沿）的场景；
    // 切片之后 inputLastBlock() 是最后一个小块的结果，不再对应整个周期，要逐周期喂参照的场景不要切。
    void setSlicing(bool inputSide, int maxSlice, std::uint64_t seed) noexcept
    {
        (inputSide ? inputSlice_ : outputSlice_) = maxSlice > 0 ? maxSlice : 0;
        (inputSide ? inputSliceState_ : outputSliceState_) = seed;
    }
    void setInputBypassed(bool on) noexcept { inputBypassed_ = on; }
    void setOutputBypassed(bool on) noexcept { outputBypassed_ = on; }
    // 命令序号的基线（构造引擎后、prepare 前给一次）。场景要验「Panic 不重放」时用。
    void setCommandBaseline(const engine::CommandBaseline& baseline) noexcept;

    // 走一个周期。按调用顺序驱动两端。返回这一周期 Input 侧实际跑过的样本数。
    int runCycle(std::size_t index);

    // 跑多个周期（下标从 first 起）。
    void runCycles(std::size_t count, std::size_t first = 0);

    // 起第二个 Output（换写方场景）与第二个 Input（复制体场景）。第二个 Output 用自己的写权 token。
    bool addSecondOutput();
    bool addSecondInput();

    // 第二个 Output / 第二个 Input 是否参与每一周期。默认不参与：场景自己决定它什么时候跑。
    void setSecondOutputActive(bool on) noexcept { secondOutputActive_ = on; }
    void setSecondInputActive(bool on) noexcept { secondInputActive_ = on; }
    // 复制体读不读环（默认不读：复制体拿不到槽，冲突者不写也不读）。复制体永远不拿 Input 槽。
    void setSecondInputReadsRing(bool on) noexcept { secondInputReadsRing_ = on; }
    // 换写方：把环的写权与 Output 槽交给第二个 Output（toSecond 为真）或交回第一个。
    // 之后另一个 Output 再写就是冲突（不写环、置冲突状态）。
    void switchWriter(bool toSecond);

    // ---- 读出：两端引擎的状态对象 ----

    // Input 侧的环路层状态（诊断与界面要的量都在这里）
    const loop::LoopStatusFields& inputStatus() const noexcept { return inputStatus_; }
    // Input 侧的诊断安全段
    engine::SafetyDiag inputSafetyDiag() const noexcept { return input_.safetyDiag(); }
    // Input 侧上一块环路层的结果（复位点、补 0 个数、逐样本有效位）
    const loop::BlockResult& inputLastBlock() const noexcept { return input_.lastBlockResult(); }
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
    std::uint32_t secondOutputConflictCount() const noexcept { return secondOutput_.conflictCount(); }
    // 第二个 Input（复制体场景）的读出
    const loop::LoopStatusFields& secondInputStatus() const noexcept { return secondInputStatus_; }
    // 两端的端点槽（会合段里本组的 Input 槽与 Output 槽）。没建段时是空。
    const ipc::RoleSlot* slot(ipc::Role role) const noexcept;

    // 两端引擎本身（场景要读别的量时用）
    engine::InputEngine& input() noexcept { return input_; }
    engine::OutputEngine& output() noexcept { return output_; }

    // 中间链（场景要读它的形态与状态）
    MiddleChain& chain() noexcept { return chain_; }
    const MiddleChain& chain() const noexcept { return chain_; }

    // 本周期 Input 处理之后的轨道缓冲（干声 + 回送），场景用来断言。长度是本周期 Input 的块长。
    const std::vector<float>& inputOutputLeft() const noexcept { return outLeft_; }
    const std::vector<float>& inputOutputRight() const noexcept { return outRight_; }
    // 本周期 Output 看到的轨道缓冲（中间链之后、Output 之前；Output 不改宿主 buffer，处理之后逐位相同）。
    const std::vector<float>& outputFedLeft() const noexcept { return fedLeft_; }
    const std::vector<float>& outputFedRight() const noexcept { return fedRight_; }
    // 信号源已经给出的样本数（下一周期的第一个样本编号）
    std::uint64_t sourcePosition() const noexcept { return sourceCounter_; }

    // 越界哨兵：每周期信号源填完之后，在轨道缓冲的有效样本之后放 kCanarySamples 个哨兵位型，周期末核对它们还在。
    // 两端引擎或中间链写过有效样本之外的位置就会被抓到。累计被改动的样本数（左右声道合计），正常应恒为 0。
    std::size_t canaryHits() const noexcept { return canaryHits_; }

    // 环的写头（诊断用）
    std::uint64_t writeHead() const noexcept;
    // 环的写公告（诊断用）：写方在动样本之前公告的目标。只有一个写方、每块都写完时恒等于写头
    std::uint64_t writeIntent() const noexcept;
    // 环的帧数（诊断用）
    std::uint32_t ringFrames() const noexcept;

    // 排空两端的事件（诊断用）。回调收到 diag::Event。
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
    void runInput(std::size_t index, int n) noexcept;
    // 下一个小块的长度（1…maxSlice，不超过 left）
    static int nextSlice(std::uint64_t& state, int maxSlice, int left) noexcept;
    void runChain(int n) noexcept;
    void runOutput(std::size_t index, int n) noexcept;
    void runSecondOutput(std::size_t index, int n) noexcept;
    void runSecondInput(std::size_t index, int n) noexcept;
    // 本周期两端各自的走带快照（同一份 + 各自的偏移量）。
    engine::TransportInfo transportFor(std::size_t index, std::int64_t hostNsOffset) const noexcept;
    // 宿主缓冲要容得下脚本里最大的块长：超长块（大于 32768）也要送进引擎，
    // 引擎才判得出「块长超限」。缓冲按这个长度给，宿主信号才不会被写越界。
    void ensureHostCapacity();
    engine::EngineContext inputContext() const noexcept;
    engine::EngineContext outputContext(bool second) const noexcept;

    RigConfig config_;
    Segments segments_;
    MiddleChain chain_;
    SourceFn source_;

    engine::InputEngine input_;
    engine::OutputEngine output_;
    engine::InputEngine secondInput_;
    engine::OutputEngine secondOutput_;
    bool haveSecondOutput_ = false;
    bool haveSecondInput_ = false;
    bool secondOutputActive_ = false;
    bool secondInputActive_ = false;
    bool secondInputReadsRing_ = false;
    // 写权（与 Output 槽）现在在谁手里：false = 第一个 Output，true = 第二个
    bool writerIsSecond_ = false;

    // 写权 token：Output 与第二个 Output 各一个（换写方场景靠这个分出「谁在写」）
    std::uint64_t outputToken_ = 1;
    std::uint64_t secondOutputToken_ = 2;

    std::uint32_t group_ = 1;
    // 段的映射代号（Output 判「换段」用）：每次 open 加一
    std::uint64_t ringGeneration_ = 0;
    double sampleRate_ = 48000.0;
    // 写方发布的采样率：prepare 时等于 sampleRate_，prepareOutput 之后可以不同
    double outputRate_ = 48000.0;
    int maxBlock_ = 1024;
    bool prepared_ = false;

    BlockScript inputScript_;
    BlockScript outputScript_;
    TransportScript transportScript_;
    ParamsScript paramsScript_;
    CallOrder order_ = CallOrder::kInputFirst;
    bool inputBypassed_ = false;
    bool outputBypassed_ = false;
    int inputChannels_ = 2;
    int outputChannels_ = 2;
    int inputSlice_ = 0;
    int outputSlice_ = 0;
    std::uint64_t inputSliceState_ = 0;
    std::uint64_t outputSliceState_ = 0;

    // 中间链在起播沿复位点上要复位：本周期 Input 报出的块内偏移（-1 = 没有）
    std::int64_t chainResetAt_ = -1;

    // 宿主侧缓冲（预分配）
    std::vector<float> trackLeft_;
    std::vector<float> trackRight_;
    std::vector<float> outLeft_;
    std::vector<float> outRight_;
    std::vector<float> fedLeft_;
    std::vector<float> fedRight_;
    std::vector<float> chainLeft_;
    std::vector<float> chainRight_;
    std::vector<float> secondInLeft_;
    std::vector<float> secondInRight_;

    // 每声道的指针数组（AudioView 要的就是这个形状）
    std::vector<float*> trackChannels_;
    std::vector<float*> secondInChannels_;

    loop::LoopStatusFields inputStatus_;
    loop::LoopStatusFields secondInputStatus_;

    // 信号源已经给出的样本数（跨周期连续）
    std::uint64_t sourceCounter_ = 0;

    // 越界哨兵：本周期哨兵起点（= 信号源填的样本数；0 = 本周期没有）与累计命中
    int canaryAt_ = 0;
    std::size_t canaryHits_ = 0;
};

} // namespace rinne::rig
