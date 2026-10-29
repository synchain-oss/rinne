// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对层用例共用的夹具：同一进程、同一测试作用域里放任意多个正式处理器（Input、Output 混着来），时钟是假的、定时器
// 不起，测试按 10 ms 一块、40 ms 一拍推进。诊断日志、日志安装与剪贴板都换成记录用的替身，诊断里的宿主名可注入。
// 只在正式构建里跑（探针构建没有配对层，见 tests/host/probe_host/）；要真共享内存的用例在验证宿主副本里跳过。
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../wiring/WiringTestKit.h"
#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/LabelSeqlock.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "pairing/PairStateModel.h"

namespace rinne::pairing_test
{

namespace ipc = rinne::ipc;
namespace state = rinne::state;
using rinne::pairing::PairState;
using wiring_test::FakeClock;
using wiring_test::FakePlayHead;
using wiring_test::formalOnly;
using wiring_test::kBlock;
using wiring_test::kBlockNs;
using wiring_test::kRate;
using wiring_test::needsSharedMemory;
using wiring_test::runningAsValidationHost;

// 替身记下的东西（用例单线程；每个 Bench 构造时清空）
struct Captured
{
    std::vector<juce::String> logLines;
    std::vector<juce::String> installs;
    std::vector<juce::String> clipboard;
};

inline Captured& captured()
{
    static Captured c;
    return c;
}

inline void captureLog(const juce::String& line)
{
    captured().logLines.push_back(line);
}

inline bool captureInstall(const juce::String& role)
{
    captured().installs.push_back(role);
    return true;
}

inline void captureClipboard(const juce::String& text)
{
    captured().clipboard.push_back(text);
}

// 包一层平台后端，数只读打开（对端视图）的次数；会合照常经它走真后端。它要比用它的处理器活得久：用例里先声明它、
// 再声明 Bench（Bench 先拆，它后拆）
class CountingBackend final : public ipc::ISegmentBackend
{
public:
    ~CountingBackend() override { forgetRetired(); }

    ipc::InitResult createOrOpen(const std::string& name, std::size_t size, ipc::SegmentView& view) override
    {
        return inner_.createOrOpen(name, size, view);
    }
    ipc::InitResult openExisting(const std::string& name, ipc::SegmentView& view) override
    {
        return inner_.openExisting(name, view);
    }
    ipc::InitResult openExistingReadOnly(const std::string& name, ipc::SegmentView& view) override
    {
        ++readOnlyOpens;
        return inner_.openExistingReadOnly(name, view);
    }
    void unmap(ipc::SegmentView& view) noexcept override { inner_.unmap(view); }
    ipc::OrphanUnmap orphanUnmapper() const noexcept override { return inner_.orphanUnmapper(); }
    void tryLock(const ipc::SegmentView& view) override { inner_.tryLock(view); }

    int readOnlyOpens = 0;

private:
    ipc::PlatformSegmentBackend inner_;
};

class Bench
{
public:
    Bench() : scope_(ipc::NameScope::randomForTesting())
    {
        captured() = Captured{};
        for (auto& ch : data_)
            ch.assign(static_cast<std::size_t>(kBlock), 0.0f);
    }

    // 后建的先拆：会合段由最后一个离开的实例收掉
    ~Bench()
    {
        while (!instances_.empty())
            instances_.pop_back();
    }

    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    InstanceHostConfig config()
    {
        InstanceHostConfig c;
        c.scope = scope_;
        c.startTimer = false;
        c.clock = &FakeClock::read;
        c.clockContext = &clock;
        c.logLine = &captureLog;
        c.installLog = &captureInstall;
        c.copyToClipboard = &captureClipboard;
        c.hostDescription = hostDescription;
        return c;
    }

    // 新建一个实例；prepare 为真时立刻 prepare（宿主插入后马上激活的常见顺序）
    InputProcessor& addInput(bool prepare = true) { return add(std::make_unique<InputProcessor>(config()), prepare); }
    OutputProcessor& addOutput(bool prepare = true)
    {
        return add(std::make_unique<OutputProcessor>(config()), prepare);
    }

    template<typename Processor>
    Processor& add(std::unique_ptr<Processor> processor, bool prepare)
    {
        Processor& ref = *processor;
        ref.setPlayHead(&playHead);
        if (prepare)
            ref.prepareToPlay(kRate, kBlock);
        instances_.push_back(Entry{std::move(processor)});
        return ref;
    }

    void remove(const RinneProcessorBase& processor)
    {
        for (auto it = instances_.begin(); it != instances_.end(); ++it)
        {
            if (it->processor.get() == &processor)
            {
                instances_.erase(it);
                return;
            }
        }
    }

    // 音频回调开关：宿主停了某个实例的回调（或者还没 prepare）
    void setRunning(const RinneProcessorBase& processor, bool running) { entryOf(processor).running = running; }
    void setBypassed(const RinneProcessorBase& processor, bool bypassed) { entryOf(processor).bypassed = bypassed; }

    // 一个 10 ms 的周期：每个在跑、已 prepare 的实例各处理一块，时间前进 10 ms，每 40 ms 全体 tick 一次
    void step()
    {
        for (auto& entry : instances_)
        {
            if (!entry.running)
                continue;
            for (auto& ch : data_)
                std::fill(ch.begin(), ch.end(), 0.0f);
            float* channels[2] = {data_[0].data(), data_[1].data()};
            juce::AudioBuffer<float> buffer(channels, 2, kBlock);
            juce::MidiBuffer midi;
            if (entry.bypassed)
                entry.processor->processBlockBypassed(buffer, midi);
            else
                entry.processor->processBlock(buffer, midi);
        }
        advance(kBlockNs);
    }

    void run(std::uint64_t ms)
    {
        for (std::uint64_t t = 0; t < ms; t += 10)
            step();
    }

    // 时间流过、不调音频回调；照常每 40 ms tick
    void idle(std::uint64_t ms)
    {
        for (std::uint64_t t = 0; t < ms; t += 10)
            advance(kBlockNs);
    }

    void tick()
    {
        for (auto& entry : instances_)
            entry.processor->instanceHost().tick();
    }

    std::uint64_t nowMs() const noexcept { return clock.ns / 1'000'000u; }

    FakeClock clock;
    FakePlayHead playHead;
    const char* hostDescription = "Test Host 1.0";

private:
    struct Entry
    {
        std::unique_ptr<RinneProcessorBase> processor;
        bool running = true;
        bool bypassed = false;
    };

    Entry& entryOf(const RinneProcessorBase& processor)
    {
        for (auto& entry : instances_)
            if (entry.processor.get() == &processor)
                return entry;
        FAIL("not an instance of this bench");
        return instances_.front();
    }

    void advance(std::uint64_t ns)
    {
        clock.ns += ns;
        sinceTick_ += ns;
        if (sinceTick_ >= 4 * kBlockNs)
        {
            sinceTick_ = 0;
            tick();
        }
    }

    ipc::NameScope scope_;
    std::vector<Entry> instances_;
    std::array<std::vector<float>, 2> data_;
    std::uint64_t sinceTick_ = 0;
};

// ---- 读出 ----

inline std::uint32_t groupOf(const RinneProcessorBase& p)
{
    return p.instanceHost().group();
}

inline ipc::RendezvousState stateOf(const RinneProcessorBase& p)
{
    return p.instanceHost().rendezvousState();
}

inline bool holds(const RinneProcessorBase& p, std::uint32_t group)
{
    return groupOf(p) == group && stateOf(p) == ipc::RendezvousState::kActive;
}

// 持有的槽带不带 provisional 位（智能默认的认领还没转正）
inline bool provisional(const RinneProcessorBase& p)
{
    const std::uint64_t ctl = p.instanceHost().rendezvous().myCtl();
    return ctl != 0 && (ipc::ctlState(ctl) & ipc::kCtlProvisional) != 0;
}

inline PairState pairStateOf(const RinneProcessorBase& p)
{
    return p.pairState().state;
}

inline state::Configs configsOf(InputProcessor& p)
{
    return p.stateGlue()->configs();
}

inline state::Configs configsOf(OutputProcessor& p)
{
    return p.stateGlue()->configs();
}

inline bool settled(InputProcessor& p)
{
    return p.stateGlue()->isSettled();
}

inline bool settled(OutputProcessor& p)
{
    return p.stateGlue()->isSettled();
}

// 宿主送轨道信息：名字、颜色都可以不给（AU 只给名字）
inline void setTrack(RinneProcessorBase& p, std::optional<juce::String> name,
                     std::optional<juce::Colour> colour = std::nullopt)
{
    juce::AudioProcessor::TrackProperties properties;
    properties.name = std::move(name);
    properties.colour = colour;
    p.updateTrackProperties(properties);
}

inline std::vector<std::uint8_t> savedState(RinneProcessorBase& p)
{
    juce::MemoryBlock block;
    p.getStateInformation(block);
    const auto* bytes = static_cast<const std::uint8_t*>(block.getData());
    return std::vector<std::uint8_t>(bytes, bytes + block.getSize());
}

inline void restore(RinneProcessorBase& p, const std::vector<std::uint8_t>& bytes)
{
    p.setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
}

// 一份组号已定的 state（来源默认为用户选的）
inline std::vector<std::uint8_t> groupState(state::Role role, std::uint8_t group,
                                            state::GroupOrigin origin = state::GroupOrigin::User)
{
    return wiring_test::stateBytes(role, [group, origin](state::StateDocument& d) {
        d.configs.group = group;
        d.configs.groupOrigin = origin;
    });
}

// 标脏的次数（nonParameterStateChanged 与 programChanged 一起来）
class DirtyListener final : public juce::AudioProcessorListener
{
public:
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged && details.programChanged)
            ++dirtyMarks;
    }

    int dirtyMarks = 0;
};

// 跑到条件成立为止（最多 maxMs），成立返回真
template<typename Condition>
bool runUntil(Bench& bench, Condition condition, std::uint64_t maxMs = 5000)
{
    for (std::uint64_t t = 0; t <= maxMs; t += 10)
    {
        if (condition())
            return true;
        bench.step();
    }
    return condition();
}

} // namespace rinne::pairing_test
