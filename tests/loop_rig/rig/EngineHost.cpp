// SPDX-License-Identifier: GPL-3.0-or-later
//
// EngineHost 的实现。见 EngineHost.h 的说明。
//
// 宿主信号：按样本递增的编号（0, 1, 2, …），左声道写编号、右声道写编号加一个偏移。
// 这样「读到的是哪一帧」一眼能看出来，场景断言「一个冲激在 D 个样本后回到读头」时不用猜。
//
// 拓扑（每一周期）：
//   Output 侧：宿主缓冲 → 引擎清洗 → 写进环。
//   Input 侧：从环读回 → 混好的回送 + 干声 → 宿主缓冲。
//   中间链在环内：Input 的输出经中间链之后才是 Output 写进环的东西。

#include "rig/EngineHost.h"

#include <algorithm>
#include <cmath>

#include "ipc/LoopRing.h"

namespace rinne::rig
{

namespace
{
// 宿主信号的右声道偏移：让左右不一样，两声道混起来时能看出是哪一边
constexpr float kRightOffset = 0.5f;
// 宿主信号每帧的步长：0.001，48 kHz 下约 48 秒绕回一圈（够场景用，又不丢精度）
constexpr float kHostStep = 0.001f;
} // namespace

EngineHost::EngineHost() = default;
EngineHost::~EngineHost() = default;

bool EngineHost::open(std::uint32_t group)
{
    group_ = group;
    config_.group = group;
    if (!segments_.open(group))
    {
        return false;
    }

    // Output 公告自己：写权 token 与采样率。epoch 与 writer_token 由 publishWriter 写。
    auto* ring = segments_.ring(group);
    if (ring == nullptr)
    {
        return false;
    }
    ipc::LoopRing::publishWriter(*ring, outputToken_, static_cast<std::uint32_t>(sampleRate_), 2);
    return true;
}

void EngineHost::prepare(double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = maxBlock > 0 ? maxBlock : 1024;
    config_.sampleRate = sampleRate_;
    config_.maxBlock = maxBlock_;

    // 缓冲按支持的最大块长给：场景要能塞违约块与超长块。
    const std::size_t capacity = static_cast<std::size_t>(ipc::kMaxSupportedBlock) + 8u;
    hostLeft_.assign(capacity, 0.0f);
    hostRight_.assign(capacity, 0.0f);
    outLeft_.assign(capacity, 0.0f);
    outRight_.assign(capacity, 0.0f);
    fedLeft_.assign(capacity, 0.0f);
    fedRight_.assign(capacity, 0.0f);
    chainLeft_.assign(capacity, 0.0f);
    chainRight_.assign(capacity, 0.0f);
    secondLeft_.assign(capacity, 0.0f);
    secondRight_.assign(capacity, 0.0f);

    hostChannels_[0] = hostLeft_.data();
    hostChannels_[1] = hostRight_.data();
    outChannels_[0] = outLeft_.data();
    outChannels_[1] = outRight_.data();
    fedChannels_[0] = fedLeft_.data();
    fedChannels_[1] = fedRight_.data();
    secondChannels_[0] = secondLeft_.data();
    secondChannels_[1] = secondRight_.data();
    chainChannels_[0] = chainLeft_.data();
    chainChannels_[1] = chainRight_.data();

    chain_.prepare(sampleRate_, maxBlock_);

    input_.prepare(sampleRate_, maxBlock_);
    output_.prepare(sampleRate_, maxBlock_);
    if (haveSecondInput_)
    {
        secondInput_.prepare(sampleRate_, maxBlock_);
    }
    if (haveSecondOutput_)
    {
        secondOutput_.prepare(sampleRate_, maxBlock_);
    }

    prepared_ = true;
}

void EngineHost::setBlockScripts(BlockScript input, BlockScript output) noexcept
{
    inputScript_ = std::move(input);
    outputScript_ = std::move(output);
}

void EngineHost::setTransportScript(TransportScript script) noexcept
{
    transportScript_ = std::move(script);
}

void EngineHost::setParamsScript(ParamsScript script) noexcept
{
    paramsScript_ = std::move(script);
}

void EngineHost::setCommandBaseline(const engine::CommandBaseline& baseline) noexcept
{
    input_.setCommandBaseline(baseline);
    secondInput_.setCommandBaseline(baseline);
}

void EngineHost::fillHostBuffers(int n, std::uint64_t cycle) noexcept
{
    // 宿主信号：按绝对样本计编号。跨周期连续，场景可以按位置断言。
    for (int i = 0; i < n; ++i)
    {
        const auto index = static_cast<std::uint32_t>((hostCounter_ + static_cast<std::uint64_t>(i)) & 0xffffu);
        hostLeft_[static_cast<std::size_t>(i)] = static_cast<float>(index) * kHostStep;
        hostRight_[static_cast<std::size_t>(i)] = static_cast<float>(index) * kHostStep + kRightOffset;
    }
    (void)cycle;
    hostCounter_ += static_cast<std::uint64_t>(std::max(n, 0));
}

engine::TransportInfo EngineHost::transportFor(std::size_t index, std::int64_t hostNsOffset) const noexcept
{
    engine::TransportInfo info = transportScript_.at(index);
    // 两端拿同一个时间戳（错序判定靠它）；场景要造「某一端看起来更靠前」就用偏移量。
    if (hostNsOffset != 0 && info.hostNsValid)
    {
        const auto shifted = static_cast<std::int64_t>(info.hostNs) + hostNsOffset;
        info.hostNs = shifted > 0 ? static_cast<std::uint64_t>(shifted) : 0u;
    }
    return info;
}

int EngineHost::runCycle(std::size_t index)
{
    if (!prepared_)
    {
        return 0;
    }

    // 这一周期两端的块长：各自查自己的脚本。
    const int inputLength = inputScript_.lengthAt(index);
    const int outputLength = outputScript_.lengthAt(index);
    const bool inputSkip = inputScript_.skipAt(index);
    const bool outputSkip = outputScript_.skipAt(index);

    // 走带：同一周期两端拿同一份（加各自的偏移量）。
    transport_ = transportScript_.at(index);

    const bool doInput = !inputSkip && order_ != CallOrder::kOutputOnly;
    const bool doOutput = !outputSkip && order_ != CallOrder::kInputOnly;

    int processed = 0;
    switch (order_)
    {
    case CallOrder::kInputFirst:
        if (doInput)
        {
            processed = runInput(index, inputLength);
        }
        if (doOutput)
        {
            runOutput(index, outputLength);
        }
        break;
    case CallOrder::kOutputFirst:
        if (doOutput)
        {
            runOutput(index, outputLength);
        }
        if (doInput)
        {
            processed = runInput(index, inputLength);
        }
        break;
    case CallOrder::kOutputOnly:
        if (doOutput)
        {
            runOutput(index, outputLength);
        }
        break;
    case CallOrder::kInputOnly:
        if (doInput)
        {
            processed = runInput(index, inputLength);
        }
        break;
    }

    inputStatus_ = input_.loopStatus();
    if (haveSecondInput_)
    {
        secondInputStatus_ = secondInput_.loopStatus();
    }
    return processed;
}

void EngineHost::runCycles(std::size_t count, std::size_t first)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        runCycle(first + i);
    }
}

int EngineHost::runInput(std::size_t index, int n) noexcept
{
    engine::AudioView view;
    view.channels = hostChannels_.data();
    view.numChannels = 2;
    view.length = n;

    engine::EngineContext ctx;
    ctx.ring = segments_.ring(group_);
    ctx.rendezvous = segments_.rv();
    ctx.group = group_;
    ctx.role = ipc::Role::kInput;

    if (n <= 0)
    {
        // 0 样本：引擎必须立刻返回、不推进任何计数。仍然调一次，验它真的不推进。
        input_.process(view, paramsScript_.at(index), transportFor(index, config_.inputHostNsOffset), inputBypassed_,
                       ctx);
        return 0;
    }

    fillHostBuffers(n, 0);

    // 干声逐位直通：引擎把宿主缓冲当输入、结果写进同一块缓冲（Input 侧是允许的）。
    std::copy(hostLeft_.begin(), hostLeft_.begin() + n, outLeft_.begin());
    std::copy(hostRight_.begin(), hostRight_.begin() + n, outRight_.begin());

    view.channels = hostChannels_.data();
    view.length = n;
    input_.process(view, paramsScript_.at(index), transportFor(index, config_.inputHostNsOffset), inputBypassed_, ctx);
    return n;
}

void EngineHost::runOutput(std::size_t index, int n) noexcept
{
    engine::AudioView view;
    view.channels = hostChannels_.data();
    view.numChannels = 2;
    view.length = n;

    if (n <= 0)
    {
        output_.process(view, paramsScript_.at(index), transportFor(index, config_.outputHostNsOffset), outputBypassed_,
                        segments_.ring(group_), outputToken_);
        return;
    }

    fillHostBuffers(n, 0);

    // 中间链：环内插件的替身。宿主信号先过中间链，再由 Output 写进环。
    chain_.process(hostLeft_.data(), hostRight_.data(), chainLeft_.data(), chainRight_.data(), n);

    engine::AudioView chainView;
    chainView.channels = chainChannels_.data();
    chainView.numChannels = 2;
    chainView.length = n;

    output_.process(chainView, paramsScript_.at(index), transportFor(index, config_.outputHostNsOffset), outputBypassed_,
                    segments_.ring(group_), outputToken_);

    // 引擎清洗之后真正写进环的那份：宿主缓冲可能被引擎改过（Output 侧不改宿主 buffer，
    // 所以 fed 就是宿主缓冲的副本；场景要断言「逐位直通」就看这两份相同）。
    std::copy(chainLeft_.begin(), chainLeft_.begin() + n, fedLeft_.begin());
    std::copy(chainRight_.begin(), chainRight_.begin() + n, fedRight_.begin());
}

void EngineHost::runSecondOutput(std::size_t index, int n) noexcept
{
    if (!haveSecondOutput_ || n <= 0)
    {
        return;
    }
    engine::AudioView view;
    view.channels = secondChannels_.data();
    view.numChannels = 2;
    view.length = n;
    secondOutput_.process(view, paramsScript_.at(index), transportFor(index, config_.outputHostNsOffset), outputBypassed_,
                          segments_.ring(group_), secondOutputToken_);
}

void EngineHost::runSecondInput(std::size_t index, int n) noexcept
{
    if (!haveSecondInput_ || n <= 0)
    {
        return;
    }
    engine::AudioView view;
    view.channels = secondChannels_.data();
    view.numChannels = 2;
    view.length = n;

    engine::EngineContext ctx;
    ctx.ring = segments_.ring(group_);
    ctx.rendezvous = segments_.rv();
    ctx.group = group_;
    ctx.role = ipc::Role::kInput;
    secondInput_.process(view, paramsScript_.at(index), transportFor(index, config_.inputHostNsOffset), inputBypassed_,
                         ctx);
}

bool EngineHost::addSecondOutput()
{
    if (haveSecondOutput_)
    {
        return false;
    }
    // 第二个 Output：新的写权 token。第一个写方还持有写权时，第二个拿不到写权（冲突场景）。
    secondOutput_.prepare(sampleRate_, maxBlock_);
    haveSecondOutput_ = true;
    config_.outputCount = 2;
    return true;
}

bool EngineHost::addSecondInput()
{
    if (haveSecondInput_)
    {
        return false;
    }
    secondInput_.prepare(sampleRate_, maxBlock_);
    haveSecondInput_ = true;
    config_.inputCount = 2;
    return true;
}

std::uint64_t EngineHost::writeHead() const noexcept
{
    auto* ring = segments_.ring(group_);
    return ring == nullptr ? 0u : ipc::LoopRing::writeHead(*ring);
}

std::uint32_t EngineHost::ringFrames() const noexcept
{
    auto* ring = segments_.ring(group_);
    return ring == nullptr ? 0u : ring->header.ring_frames.load(std::memory_order_relaxed);
}

} // namespace rinne::rig
