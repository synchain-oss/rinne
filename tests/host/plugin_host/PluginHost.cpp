// SPDX-License-Identifier: GPL-3.0-or-later

#include "host/plugin_host/PluginHost.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "util/RinneVersion.h"

namespace rinne::plugin_host
{

namespace
{

constexpr std::uint32_t kCanaryBits = 0xa5a5a5a5u;

float garbage(Garbage kind, float x) noexcept
{
    switch (kind)
    {
    case Garbage::kNan:
        return std::numeric_limits<float>::quiet_NaN();
    case Garbage::kInf:
        return x * std::numeric_limits<float>::infinity();
    case Garbage::kHuge:
        return x * 1.0e6f;
    }
    return x;
}

} // namespace

void MiddleChain::setDelay(int samples)
{
    delay_ = std::max(samples, 0);
    for (auto& ch : line_)
        ch.assign(static_cast<std::size_t>(delay_), 0.0f);
    write_ = 0;
}

void MiddleChain::process(float* left, float* right, int n) noexcept
{
    float* channels[2] = {left, right};
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t at = position_ + i;
        for (std::size_t c = 0; c < 2; ++c)
        {
            float x = channels[c][i] * gain_;
            if (delay_ > 0)
            {
                std::swap(x, line_[c][write_]);
            }
            for (const Burst& b : bursts_)
                if (at >= b.from && at < b.to)
                    x = garbage(b.kind, x);
            channels[c][i] = x;
        }
        if (delay_ > 0)
            write_ = (write_ + 1) % static_cast<std::size_t>(delay_);
    }
    position_ += n;
}

PluginHost::PluginHost(const HostOptions& options)
    : options_(options), scope_(ipc::NameScope::randomForTesting()), map_(0, false),
      inputLane_(map_, options.sampleRate), outputLane_(map_, options.sampleRate)
{
    const auto frames = static_cast<std::size_t>(kMaxFrames + kCanary);
    for (auto* group : {&data_, &dry_, &seen_})
        for (auto& ch : *group)
            ch.assign(frames, 0.0f);
    const InstanceHostConfig c = config(options.realTimer);
    input_ = std::make_unique<InputProcessor>(c);
    output_ = std::make_unique<OutputProcessor>(c);
    input_->setPlayHead(&inputLane_);
    output_->setPlayHead(&outputLane_);
}

PluginHost::~PluginHost()
{
    input_.reset();
    output_.reset();
}

InstanceHostConfig PluginHost::config(bool realTimer)
{
    InstanceHostConfig c;
    c.scope = scope_;
    c.startTimer = realTimer;
    if (!realTimer)
    {
        c.clock = &LogicalClock::read;
        c.clockContext = &clock_;
    }
    return c;
}

void PluginHost::prepare()
{
    input_->prepareToPlay(options_.sampleRate, options_.inputBlock);
    output_->prepareToPlay(options_.sampleRate, options_.outputBlock);
}

void PluginHost::prepareInput(int maxBlock)
{
    options_.inputBlock = maxBlock;
    input_->prepareToPlay(options_.sampleRate, maxBlock);
}

void PluginHost::prepareOutput(int maxBlock)
{
    options_.outputBlock = maxBlock;
    output_->prepareToPlay(options_.sampleRate, maxBlock);
}

bool PluginHost::selectGroup(int group)
{
    const bool ok = input_->selectGroup(group) && output_->selectGroup(group);
    if (!options_.realTimer)
        tick();
    return ok;
}

bool PluginHost::active() const
{
    return input_->instanceHost().rendezvousState() == ipc::RendezvousState::kActive &&
           output_->instanceHost().rendezvousState() == ipc::RendezvousState::kActive;
}

bool PluginHost::paired() const
{
    return active() && inputEngine().lastBlockResult().paired;
}

bool PluginHost::pairUp(int frames, int maxCycles)
{
    for (int i = 0; i < maxCycles && !paired(); ++i)
        cycle(frames);
    return paired();
}

void PluginHost::cycle(int frames, bool runInput, bool runOutput)
{
    REQUIRE(frames >= 0);
    REQUIRE(frames <= kMaxFrames);
    const auto count = static_cast<std::size_t>(frames);
    for (std::size_t i = 0; i < count; ++i)
    {
        float left = 0.0f;
        float right = 0.0f;
        if (source)
            source(trackPosition_ + static_cast<std::int64_t>(i), left, right);
        data_[0][i] = left;
        data_[1][i] = right;
    }
    for (auto& ch : data_)
        for (std::size_t i = count; i < count + kCanary; ++i)
            std::memcpy(&ch[i], &kCanaryBits, sizeof kCanaryBits);

    float* channels[2] = {data_[0].data(), data_[1].data()};
    juce::AudioBuffer<float> buffer(channels, 2, frames);
    juce::MidiBuffer midi;
    if (runInput)
    {
        for (std::size_t c = 0; c < 2; ++c)
            std::copy(data_[c].begin(), data_[c].begin() + static_cast<std::ptrdiff_t>(count), dry_[c].begin());
        inputLane_.arm(frames, clock_.ns);
        input_->processBlock(buffer, midi);
        if (onInputBlock)
            onInputBlock(InputBlock{inputLane_.cursor(), frames, dry_[0].data(), dry_[1].data(), data_[0].data(),
                                    data_[1].data()});
        inputLane_.advance(frames);
    }
    chain_.process(data_[0].data(), data_[1].data(), frames);
    if (runOutput)
    {
        const std::size_t bytes = (count + kCanary) * sizeof(float);
        for (std::size_t c = 0; c < 2; ++c)
            std::memcpy(seen_[c].data(), data_[c].data(), bytes);
        if (onOutputBlock)
            onOutputBlock(data_[0].data(), data_[1].data(), frames);
        outputLane_.arm(frames, clock_.ns);
        output_->processBlock(buffer, midi);
        bool touched = false;
        for (std::size_t c = 0; c < 2; ++c)
            touched = touched || std::memcmp(seen_[c].data(), data_[c].data(), bytes) != 0;
        outputTouched += touched ? 1u : 0u;
        outputLane_.advance(frames);
    }
    for (const auto& ch : data_)
        for (std::size_t i = count; i < count + kCanary; ++i)
            if (std::memcmp(&ch[i], &kCanaryBits, sizeof kCanaryBits) != 0)
                ++canaryHits;
    trackPosition_ += frames;
    advance(static_cast<std::uint64_t>(std::llround(static_cast<double>(frames) * 1.0e9 / options_.sampleRate)));
}

void PluginHost::run(std::int64_t samples, int frames)
{
    REQUIRE(frames > 0);
    for (std::int64_t done = 0; done < samples; done += frames)
        cycle(frames);
}

void PluginHost::reloadInput()
{
    juce::MemoryBlock state;
    input_->getStateInformation(state);
    input_.reset();
    input_ = std::make_unique<InputProcessor>(config(options_.realTimer));
    input_->setPlayHead(&inputLane_);
    input_->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    input_->prepareToPlay(options_.sampleRate, options_.inputBlock);
    if (!options_.realTimer)
        tick();
}

void PluginHost::tick()
{
    input_->instanceHost().tick();
    output_->instanceHost().tick();
}

void PluginHost::advance(std::uint64_t ns)
{
    if (options_.realTimer)
    {
        clock_.ns += ns;
        return;
    }
    // 逐拍走：每一拍 tick 时时钟正好在那一拍上
    for (std::uint64_t left = ns; left > 0;)
    {
        const std::uint64_t step = std::min(left, kTickNs - sinceTick_);
        clock_.ns += step;
        left -= step;
        sinceTick_ += step;
        if (sinceTick_ >= kTickNs)
        {
            sinceTick_ = 0;
            tick();
        }
    }
}

engine::EngineParams PluginHost::inputParams()
{
    engine::EngineParams params;
    if constexpr (!build::kProbeBuild)
    {
        InputProcessor::snapshotBlock(*input_->parameterLayer(), *input_->stateGlue(), *input_->presetStore(), params);
    }
    return params;
}

} // namespace rinne::plugin_host
