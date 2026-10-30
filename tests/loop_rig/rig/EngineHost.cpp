// SPDX-License-Identifier: GPL-3.0-or-later
//
// EngineHost 的实现。见 EngineHost.h 的说明。
//
// 一周期：信号源往轨道缓冲里填这一周期的样本，缓冲按调用顺序依次流过各级（Input 就地加回送、中间链、
// Output 只读并写环）。Input 先、Output 后时，Input 加上去的回送经中间链再被 Output 写进环：环路是闭合的。

#include "rig/EngineHost.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ipc/LoopRing.h"
#include "loop/LoopTiming.h"

namespace rinne::rig
{

namespace
{
// 越界哨兵：位型不是任何正常样本，也不是 NaN 的常见位型
constexpr std::uint32_t kCanaryBits = 0xa5a5a5a5u;
constexpr int kCanarySamples = 8;
} // namespace

SourceFn silenceSource()
{
    return [](std::uint64_t, float* left, float* right, int n) {
        std::fill(left, left + n, 0.0f);
        std::fill(right, right + n, 0.0f);
    };
}

SourceFn impulseSource(std::uint64_t at, float amplitude)
{
    return [at, amplitude](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const bool hit = first + static_cast<std::uint64_t>(i) == at;
            left[i] = hit ? amplitude : 0.0f;
            right[i] = hit ? amplitude : 0.0f;
        }
    };
}

SourceFn rampSource()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const auto k = static_cast<std::uint32_t>((first + static_cast<std::uint64_t>(i)) & 0xffffu);
            const float v = static_cast<float>(k) / 65536.0f;
            left[i] = v;
            right[i] = -v;
        }
    };
}

EngineHost::EngineHost() : source_(rampSource()) {}
EngineHost::~EngineHost() = default;

bool EngineHost::open(std::uint32_t group)
{
    group_ = group;
    config_.group = group;
    if (!segments_.open(group))
    {
        return false;
    }
    ++ringGeneration_;

    // 第一个 Output 拿写权：token 与采样率。epoch 与 writer_token 由 publishWriter 写。
    auto* ring = segments_.ring(group);
    if (ring == nullptr)
    {
        return false;
    }
    ipc::LoopRing::publishWriter(*ring, outputToken_, loop::publishedSampleRate(outputRate_), 2);
    writerIsSecond_ = false;
    return true;
}

void EngineHost::prepare(double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    outputRate_ = sampleRate_;
    maxBlock_ = maxBlock > 0 ? maxBlock : 1024;
    config_.sampleRate = sampleRate_;
    config_.maxBlock = maxBlock_;

    // 缓冲按支持的最大块长给：场景要能塞违约块与超长块。
    ensureHostCapacity();

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

    // 认领一侧（会合层）的事：写方按新的采样率重新发布自己，两端槽里写上 prepare 声明的最大块长。
    if (auto* ring = segments_.ring(group_))
    {
        ipc::LoopRing::publishWriter(*ring, writerIsSecond_ ? secondOutputToken_ : outputToken_,
                                     loop::publishedSampleRate(outputRate_), 2);
    }
    if (auto* rv = segments_.rv())
    {
        ipc::roleSlot(*rv, group_, ipc::Role::kInput).max_block.store(static_cast<std::uint32_t>(maxBlock_));
        ipc::roleSlot(*rv, group_, ipc::Role::kOutput).max_block.store(static_cast<std::uint32_t>(maxBlock_));
    }

    prepared_ = true;
}

void EngineHost::prepareOutput(double sampleRate, int maxBlock)
{
    outputRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int block = maxBlock > 0 ? maxBlock : maxBlock_;
    output_.prepare(outputRate_, block);
    if (haveSecondOutput_)
    {
        secondOutput_.prepare(outputRate_, block);
    }
    if (auto* ring = segments_.ring(group_))
    {
        ipc::LoopRing::publishWriter(*ring, writerIsSecond_ ? secondOutputToken_ : outputToken_,
                                     loop::publishedSampleRate(outputRate_), 2);
    }
    if (auto* rv = segments_.rv())
    {
        ipc::roleSlot(*rv, group_, ipc::Role::kOutput).max_block.store(static_cast<std::uint32_t>(block));
    }
}

void EngineHost::prepareInput(double sampleRate, int maxBlock)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int block = maxBlock > 0 ? maxBlock : maxBlock_;
    input_.prepare(rate, block);
    if (haveSecondInput_)
    {
        secondInput_.prepare(rate, block);
    }
    if (auto* rv = segments_.rv())
    {
        ipc::roleSlot(*rv, group_, ipc::Role::kInput).max_block.store(static_cast<std::uint32_t>(block));
    }
}

// 宿主缓冲的容量：容得下支持的最大块长（超长块也要送进引擎，引擎才判得出超限）。
namespace
{
std::size_t hostCapacityFor(const BlockScript& input, const BlockScript& output)
{
    std::size_t capacity = static_cast<std::size_t>(ipc::kMaxSupportedBlock) + 8u;
    // 脚本里写了更长的块长就按它给：否则宿主信号会被写越界。
    for (const BlockScript* script : {&input, &output})
    {
        for (const int length : script->lengths)
        {
            if (length > 0 && static_cast<std::size_t>(length) + 8u > capacity)
            {
                capacity = static_cast<std::size_t>(length) + 8u;
            }
        }
    }
    return capacity;
}
} // namespace

void EngineHost::ensureHostCapacity()
{
    const std::size_t capacity = hostCapacityFor(inputScript_, outputScript_);

    trackLeft_.assign(capacity, 0.0f);
    trackRight_.assign(capacity, 0.0f);
    outLeft_.assign(capacity, 0.0f);
    outRight_.assign(capacity, 0.0f);
    fedLeft_.assign(capacity, 0.0f);
    fedRight_.assign(capacity, 0.0f);
    chainLeft_.assign(capacity, 0.0f);
    chainRight_.assign(capacity, 0.0f);
    secondInLeft_.assign(capacity, 0.0f);
    secondInRight_.assign(capacity, 0.0f);

    trackChannels_.assign(2, nullptr);
    secondInChannels_.assign(2, nullptr);
    trackChannels_[0] = trackLeft_.data();
    trackChannels_[1] = trackRight_.data();
    secondInChannels_[0] = secondInLeft_.data();
    secondInChannels_[1] = secondInRight_.data();
}

void EngineHost::setBlockScripts(BlockScript input, BlockScript output)
{
    inputScript_ = std::move(input);
    outputScript_ = std::move(output);
    // 脚本里的块长可能比 prepare 时给的容量大（超长块）：重新按脚本给缓冲。
    if (prepared_)
    {
        ensureHostCapacity();
    }
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

void EngineHost::switchWriter(bool toSecond)
{
    writerIsSecond_ = toSecond;
    if (auto* ring = segments_.ring(group_))
    {
        ipc::LoopRing::publishWriter(*ring, toSecond ? secondOutputToken_ : outputToken_,
                                     loop::publishedSampleRate(outputRate_), 2);
    }
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

engine::EngineContext EngineHost::inputContext() const noexcept
{
    engine::EngineContext ctx;
    ctx.ring = segments_.ring(group_);
    ctx.ringGeneration = ctx.ring != nullptr ? ringGeneration_ : 0u;
    ctx.rendezvous = segments_.rv();
    ctx.group = group_;
    return ctx;
}

engine::EngineContext EngineHost::outputContext(bool second) const noexcept
{
    engine::EngineContext ctx;
    ctx.ring = segments_.ring(group_);
    ctx.group = group_;
    ctx.token = second ? secondOutputToken_ : outputToken_;
    // 段的映射代号：每次 open 建一份新映射，代号随之变（壳里取绑定快照的发布序号）。
    ctx.ringGeneration = ctx.ring != nullptr ? ringGeneration_ : 0u;
    // Output 槽只给现任写方（它是槽的属主）；另一个 Output 拿不到槽，不写任何端点槽。
    ctx.rendezvous = second == writerIsSecond_ ? segments_.rv() : nullptr;
    return ctx;
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
    const bool doInput = !inputScript_.skipAt(index) && order_ != CallOrder::kOutputOnly;
    const bool doOutput = !outputScript_.skipAt(index) && order_ != CallOrder::kInputOnly;

    // 信号源：给这一周期跑的那几端里最长的那一段。
    const int sourceLength =
        std::max({doInput ? inputLength : 0, doOutput ? outputLength : 0, secondInputActive_ ? inputLength : 0});
    const auto capacity = static_cast<int>(trackLeft_.size());
    const int filled = std::clamp(sourceLength, 0, capacity);
    if (filled > 0)
    {
        source_(sourceCounter_, trackLeft_.data(), trackRight_.data(), filled);
        std::copy(trackLeft_.begin(), trackLeft_.begin() + filled, secondInLeft_.begin());
        std::copy(trackRight_.begin(), trackRight_.begin() + filled, secondInRight_.begin());
        sourceCounter_ += static_cast<std::uint64_t>(filled);
    }
    // 哨兵紧跟在有效样本之后（缓冲比最长的块多留了 8 个样本）
    canaryAt_ = filled > 0 && filled + kCanarySamples <= capacity ? filled : 0;
    if (canaryAt_ > 0)
    {
        for (int i = 0; i < kCanarySamples; ++i)
        {
            std::memcpy(&trackLeft_[static_cast<std::size_t>(canaryAt_ + i)], &kCanaryBits, sizeof kCanaryBits);
            std::memcpy(&trackRight_[static_cast<std::size_t>(canaryAt_ + i)], &kCanaryBits, sizeof kCanaryBits);
        }
    }

    switch (order_)
    {
    case CallOrder::kInputFirst:
        if (doInput)
        {
            runInput(index, inputLength);
        }
        if (secondInputActive_)
        {
            runSecondInput(index, inputLength);
        }
        if (doOutput)
        {
            runChain(outputLength);
            runOutput(index, outputLength);
        }
        if (secondOutputActive_)
        {
            runSecondOutput(index, outputLength);
        }
        break;
    case CallOrder::kOutputFirst:
        if (doOutput)
        {
            runChain(outputLength);
            runOutput(index, outputLength);
        }
        if (secondOutputActive_)
        {
            runSecondOutput(index, outputLength);
        }
        if (doInput)
        {
            runInput(index, inputLength);
        }
        if (secondInputActive_)
        {
            runSecondInput(index, inputLength);
        }
        break;
    case CallOrder::kOutputOnly:
        if (doOutput)
        {
            runChain(outputLength);
            runOutput(index, outputLength);
        }
        if (secondOutputActive_)
        {
            runSecondOutput(index, outputLength);
        }
        break;
    case CallOrder::kInputOnly:
        if (doInput)
        {
            runInput(index, inputLength);
        }
        if (secondInputActive_)
        {
            runSecondInput(index, inputLength);
        }
        break;
    }

    if (canaryAt_ > 0)
    {
        for (int i = 0; i < kCanarySamples; ++i)
        {
            std::uint32_t left = 0;
            std::uint32_t right = 0;
            std::memcpy(&left, &trackLeft_[static_cast<std::size_t>(canaryAt_ + i)], sizeof left);
            std::memcpy(&right, &trackRight_[static_cast<std::size_t>(canaryAt_ + i)], sizeof right);
            canaryHits_ += (left != kCanaryBits ? 1u : 0u) + (right != kCanaryBits ? 1u : 0u);
        }
    }

    inputStatus_ = input_.loopStatus();
    if (haveSecondInput_)
    {
        secondInputStatus_ = secondInput_.loopStatus();
    }
    return doInput ? std::max(inputLength, 0) : 0;
}

void EngineHost::runCycles(std::size_t count, std::size_t first)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        runCycle(first + i);
    }
}

void EngineHost::runInput(std::size_t index, int n) noexcept
{
    engine::AudioView view;
    view.channels = trackChannels_.data();
    view.numChannels = inputChannels_;
    view.length = n;

    // 0 样本：引擎必须立刻返回、不推进任何计数。仍然调一次，验它真的不推进。
    // 超长块：缓冲按脚本给足了，引擎自己判超限。
    if (inputSlice_ > 0 && n > 0)
    {
        // 单端切片：整个周期的块切成小块依次调用，每个小块是缓冲里的一段
        for (int offset = 0; offset < n;)
        {
            const int length = nextSlice(inputSliceState_, inputSlice_, n - offset);
            float* channels[2] = {trackLeft_.data() + offset, trackRight_.data() + offset};
            engine::AudioView slice;
            slice.channels = channels;
            slice.numChannels = inputChannels_;
            slice.length = length;
            input_.process(slice, paramsScript_.at(index), transportFor(index, config_.inputHostNsOffset),
                           inputBypassed_, inputContext());
            offset += length;
        }
    }
    else
    {
        input_.process(view, paramsScript_.at(index), transportFor(index, config_.inputHostNsOffset), inputBypassed_,
                       inputContext());
    }
    if (n <= 0)
    {
        return;
    }
    const auto count = static_cast<std::size_t>(n);
    std::copy(trackLeft_.begin(), trackLeft_.begin() + static_cast<std::ptrdiff_t>(count), outLeft_.begin());
    std::copy(trackRight_.begin(), trackRight_.begin() + static_cast<std::ptrdiff_t>(count), outRight_.begin());

    // 起播沿的 DSP 复位点落在这一块：中间链（环内插件的替身）在同一个样本上复位。
    // Input 先跑时中间链紧接着跑同一块，复位点就是块内偏移；Output 先跑时中间链这一周期已经跑过，下一次开头复位。
    const loop::BlockResult& result = input_.lastBlockResult();
    if (result.resetDsp && result.resetAtSample >= result.blockStart)
    {
        chainResetAt_ = order_ == CallOrder::kInputFirst ? result.resetAtSample - result.blockStart : 0;
    }
}

int EngineHost::nextSlice(std::uint64_t& state, int maxSlice, int left) noexcept
{
    // splitmix64
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    const int length = 1 + static_cast<int>(z % static_cast<std::uint64_t>(maxSlice));
    return std::min(length, left);
}

void EngineHost::runChain(int n) noexcept
{
    if (n <= 0)
    {
        return;
    }
    // 中间链：环内插件的替身。轨道缓冲经中间链，再由 Output 读走。
    const std::int64_t split = chainResetAt_ >= 0 && chainResetAt_ < n ? chainResetAt_ : -1;
    if (split >= 0)
    {
        const auto k = static_cast<int>(split);
        if (k > 0)
        {
            chain_.process(trackLeft_.data(), trackRight_.data(), chainLeft_.data(), chainRight_.data(), k);
        }
        chain_.reset();
        chain_.process(trackLeft_.data() + k, trackRight_.data() + k, chainLeft_.data() + k, chainRight_.data() + k,
                       n - k);
        chainResetAt_ = -1;
    }
    else
    {
        chain_.process(trackLeft_.data(), trackRight_.data(), chainLeft_.data(), chainRight_.data(), n);
        if (chainResetAt_ >= n)
        {
            chainResetAt_ -= n; // 复位点在下一段里
        }
    }
    std::copy(chainLeft_.begin(), chainLeft_.begin() + n, trackLeft_.begin());
    std::copy(chainRight_.begin(), chainRight_.begin() + n, trackRight_.begin());
}

void EngineHost::runOutput(std::size_t index, int n) noexcept
{
    engine::AudioView view;
    view.channels = trackChannels_.data();
    view.numChannels = outputChannels_;
    view.length = n;

    if (outputSlice_ > 0 && n > 0)
    {
        for (int offset = 0; offset < n;)
        {
            const int length = nextSlice(outputSliceState_, outputSlice_, n - offset);
            float* channels[2] = {trackLeft_.data() + offset, trackRight_.data() + offset};
            engine::AudioView slice;
            slice.channels = channels;
            slice.numChannels = outputChannels_;
            slice.length = length;
            output_.process(slice, paramsScript_.at(index), transportFor(index, config_.outputHostNsOffset),
                            outputBypassed_, outputContext(false));
            offset += length;
        }
    }
    else
    {
        output_.process(view, paramsScript_.at(index), transportFor(index, config_.outputHostNsOffset), outputBypassed_,
                        outputContext(false));
    }
    if (n <= 0)
    {
        return;
    }
    // Output 看到的那一份（它不改宿主 buffer：处理之后逐位相同，场景断言「逐位直通」就看这一份）。
    std::copy(trackLeft_.begin(), trackLeft_.begin() + n, fedLeft_.begin());
    std::copy(trackRight_.begin(), trackRight_.begin() + n, fedRight_.begin());
}

void EngineHost::runSecondOutput(std::size_t index, int n) noexcept
{
    if (!haveSecondOutput_)
    {
        return;
    }
    // 第二个 Output 读同一条轨道缓冲（复制体或接管者插在同一处）。
    engine::AudioView view;
    view.channels = trackChannels_.data();
    view.numChannels = 2;
    view.length = n;
    secondOutput_.process(view, paramsScript_.at(index), transportFor(index, config_.outputHostNsOffset),
                          outputBypassed_, outputContext(true));
}

void EngineHost::runSecondInput(std::size_t index, int n) noexcept
{
    if (!haveSecondInput_)
    {
        return;
    }
    // 复制体在另一条轨道上：自己的一份信号源，拿不到 Input 槽；默认也不读环（冲突者不写也不读）。
    engine::AudioView view;
    view.channels = secondInChannels_.data();
    view.numChannels = 2;
    view.length = n;

    engine::EngineContext ctx;
    ctx.ring = secondInputReadsRing_ ? segments_.ring(group_) : nullptr;
    ctx.group = group_;
    secondInput_.process(view, paramsScript_.at(index), transportFor(index, config_.inputHostNsOffset), inputBypassed_,
                         ctx);
}

bool EngineHost::addSecondOutput()
{
    if (haveSecondOutput_)
    {
        return false;
    }
    // 第二个 Output：自己的写权 token。第一个写方还持有写权时，第二个写就是冲突（不写环）。
    secondOutput_.prepare(outputRate_, maxBlock_);
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

const ipc::RoleSlot* EngineHost::slot(ipc::Role role) const noexcept
{
    auto* rv = segments_.rv();
    return rv == nullptr ? nullptr : &ipc::roleSlot(*rv, group_, role);
}

std::uint64_t EngineHost::writeHead() const noexcept
{
    auto* ring = segments_.ring(group_);
    return ring == nullptr ? 0u : ipc::LoopRing::writeHead(*ring);
}

std::uint64_t EngineHost::writeIntent() const noexcept
{
    auto* ring = segments_.ring(group_);
    return ring == nullptr ? 0u : ipc::LoopRing::writeIntent(*ring);
}

std::uint32_t EngineHost::ringFrames() const noexcept
{
    auto* ring = segments_.ring(group_);
    return ring == nullptr ? 0u : ring->header.ring_frames.load(std::memory_order_relaxed);
}

} // namespace rinne::rig
