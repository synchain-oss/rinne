// SPDX-License-Identifier: GPL-3.0-or-later
//
// 正式构建接线用例共用的夹具：两个真处理器（InputProcessor、OutputProcessor）经测试注入的命名作用域在同一进程里配对，
// 时钟是假的、定时器不起，tick 由测试按 25 Hz 的节拍调。另有数映射的后端、按参数表 ID 写参数、拼 state 字节。
// 用例只在正式构建里跑（探针构建里处理器不建参数层与胶水，参数与 state 在 ProbeHost 里，见 tests/host/probe_host/）；
// 要真共享内存的用例在验证宿主副本里跳过（那里 IPC 整体关闭，另有专门的用例断言它）。
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/LoopBinding.h"
#include "ipc/Naming.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/Rendezvous.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "util/RinneVersion.h"

namespace rinne::wiring_test
{

namespace ipc = rinne::ipc;
namespace state = rinne::state;

inline constexpr double kRate = 48000.0;
inline constexpr int kBlock = 480; // 10 ms
inline constexpr std::uint64_t kBlockNs = 10'000'000;
inline constexpr int kCanary = 64; // 块尾之后多查的样本数，抓越界写
inline constexpr std::uint32_t kCanaryBits = 0xa5a5a5a5u;
inline constexpr int kLongestBlock = 32769; // 比支持的最大块长多 1

// 正式构建才有参数层与胶水；探针构建里这组用例跳过（SKIP 抛出，在辅助函数里调用同样结束用例）
inline void formalOnly()
{
    if constexpr (rinne::build::kProbeBuild)
        SKIP("probe build: parameters and state live in the probe host");
}

// 本进程是不是验证宿主的换名副本（那里 IPC 整体关闭）。问一个只构造、不 prepare 的处理器，与处理器自己的识别同一份
inline bool runningAsValidationHost()
{
    static const bool value = [] {
        const OutputProcessor processor;
        return processor.isValidationHost();
    }();
    return value;
}

// 要真共享内存的用例在验证宿主副本里跳过（那里只出干声、不开段，由专门的用例断言）
inline void needsSharedMemory()
{
    if (runningAsValidationHost())
        SKIP("validation host: shared memory is off");
}

struct FakeClock
{
    std::uint64_t ns = 10'000'000'000ull;
    static std::uint64_t read(void* context) noexcept { return static_cast<FakeClock*>(context)->ns; }
};

class FakePlayHead final : public juce::AudioPlayHead
{
public:
    bool playing = false;
    std::int64_t samples = 0;
    std::uint64_t hostNs = 0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setTimeInSamples(samples);
        info.setBpm(120.0);
        info.setHostTimeNs(hostNs);
        return info;
    }
};

// 平台后端外面套一层，数这个后端名下还活着的映射（建或开成功 +1，解映射 -1）
class CountingBackend final : public ipc::ISegmentBackend
{
public:
    ipc::InitResult createOrOpen(const std::string& name, std::size_t size, ipc::SegmentView& view) override
    {
        return counted(inner_.createOrOpen(name, size, view), view);
    }
    ipc::InitResult openExisting(const std::string& name, ipc::SegmentView& view) override
    {
        return counted(inner_.openExisting(name, view), view);
    }
    ipc::InitResult openExistingReadOnly(const std::string& name, ipc::SegmentView& view) override
    {
        return counted(inner_.openExistingReadOnly(name, view), view);
    }
    void unmap(ipc::SegmentView& view) noexcept override
    {
        if (view.base != nullptr)
            live.fetch_sub(1, std::memory_order_relaxed);
        inner_.unmap(view);
    }
    ipc::OrphanUnmap orphanUnmapper() const noexcept override { return inner_.orphanUnmapper(); }
    void tryLock(const ipc::SegmentView& view) override { inner_.tryLock(view); }

    std::atomic<int> live{0};
    std::atomic<int> mapped{0}; // 累计映射过几次（非空断言用）

private:
    ipc::InitResult counted(ipc::InitResult result, const ipc::SegmentView& view)
    {
        if (view.base != nullptr)
        {
            live.fetch_add(1, std::memory_order_relaxed);
            mapped.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    ipc::PlatformSegmentBackend inner_;
};

inline bool sameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}

inline float fromBits(std::uint32_t bits)
{
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

inline float sine(std::uint64_t i, int channel)
{
    const double hz = channel == 0 ? 220.0 : 330.0;
    return 0.25f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * static_cast<double>(i) / kRate));
}

// 按参数表 ID 写一个参数的明文值，宿主自动化的写法（不经界面手势，不定型）
inline void hostSetParam(InputProcessor& processor, const char* id, float plain)
{
    auto* parameter = processor.parameterLayer()->tree().getParameter(id);
    REQUIRE(parameter != nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
}

inline float paramValue(InputProcessor& processor, const char* id)
{
    const auto* raw = processor.parameterLayer()->tree().getRawParameterValue(id);
    REQUIRE(raw != nullptr);
    return raw->load(std::memory_order_relaxed);
}

// 一份能被这一端成功读入（Ok）的 state：从默认文档起，由调用方改设置与参数
inline std::vector<std::uint8_t> stateBytes(state::Role role, const std::function<void(state::StateDocument&)>& edit)
{
    state::StateDocument document = state::makeDefaultDocument(role);
    edit(document);
    std::vector<std::uint8_t> out;
    REQUIRE(state::encodeDocument(document, out));
    return out;
}

template<typename Processor>
void loadState(Processor& processor, const std::vector<std::uint8_t>& bytes)
{
    processor.setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
}

// 同一进程、同一测试作用域里的一对正式处理器，外加假时钟、假播放头与一份带金丝雀的宿主缓冲。
// cycle 的顺序同一条插入链：信号源填缓冲 → Input（第一格）→ Output（推子前最后一格，看到的是 Input 的输出）。
class Rig
{
public:
    explicit Rig(int declaredBlock = kBlock, ipc::ISegmentBackend* backend = nullptr)
        : declared(declaredBlock), backend_(backend), scope_(ipc::NameScope::randomForTesting())
    {
        for (auto& ch : data_)
            ch.assign(static_cast<std::size_t>(kLongestBlock + kCanary), 0.0f);
        for (auto& ch : afterInput_)
            ch.assign(static_cast<std::size_t>(kLongestBlock + kCanary), 0.0f);
    }

    ~Rig()
    {
        input.reset();
        output.reset();
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    InstanceHostConfig config()
    {
        InstanceHostConfig c;
        c.scope = scope_;
        c.backend = backend_;
        c.startTimer = false;
        c.clock = &FakeClock::read;
        c.clockContext = &clock;
        return c;
    }

    std::unique_ptr<InputProcessor> makeInput()
    {
        auto p = std::make_unique<InputProcessor>(config());
        p->setPlayHead(&playHead);
        return p;
    }

    std::unique_ptr<OutputProcessor> makeOutput()
    {
        auto p = std::make_unique<OutputProcessor>(config());
        p->setPlayHead(&playHead);
        return p;
    }

    // 建两端（已有的不重建）、prepare、界面选组，再 tick 一拍
    void open(int group = 1)
    {
        if (input == nullptr)
            input = makeInput();
        if (output == nullptr)
            output = makeOutput();
        input->prepareToPlay(kRate, declared);
        output->prepareToPlay(kRate, declared);
        REQUIRE(input->selectGroup(group));
        REQUIRE(output->selectGroup(group));
        tick();
    }

    bool active() const
    {
        return input != nullptr && output != nullptr &&
               input->instanceHost().rendezvousState() == ipc::RendezvousState::kActive &&
               output->instanceHost().rendezvousState() == ipc::RendezvousState::kActive;
    }

    // 两端都持有槽，绑定里都已经有环：之后的音频从第一块起就是配好对的（不等哪一拍 tick 才换绑定）
    bool ringsBound() const
    {
        if (!active())
            return false;
        const ipc::LoopBinding* in = input->instanceHost().rendezvous().bindings().current();
        const ipc::LoopBinding* out = output->instanceHost().rendezvous().bindings().current();
        return in != nullptr && out != nullptr && in->ring != nullptr && out->ring != nullptr;
    }

    // 只在调 cycle 的线程（音频线程）读：上一块 Input 引擎看到了对端、能读环
    bool paired() const { return active() && input->instanceHost().inputEngine().lastBlockResult().paired; }

    // 跑到配对为止（每块照常推进时间与 tick）
    bool pairUp(int maxCycles = 400)
    {
        for (int i = 0; i < maxCycles; ++i)
        {
            if (paired())
                return true;
            cycle();
        }
        return paired();
    }

    // 一个周期，frames 帧。录下 Input 的输出与干声（record 为真时），并核对 Output 没有改动缓冲、两端都没写过块尾
    void cycle(int frames = kBlock, bool bypassInput = false, bool bypassOutput = false)
    {
        REQUIRE(frames >= 0);
        REQUIRE(frames <= kLongestBlock);
        const auto count = static_cast<std::size_t>(frames);
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = data_[static_cast<std::size_t>(c)];
            for (std::size_t i = 0; i < count; ++i)
                ch[i] = source ? source(position_ + i, c) : 0.0f;
            for (std::size_t i = count; i < count + kCanary; ++i)
                std::memcpy(&ch[i], &kCanaryBits, sizeof kCanaryBits);
        }
        if (record)
            dryLeft.insert(dryLeft.end(), data_[0].begin(), data_[0].begin() + frames);

        float* channels[2] = {data_[0].data(), data_[1].data()};
        juce::AudioBuffer<float> buffer(channels, 2, frames);
        juce::MidiBuffer midi;
        playHead.hostNs = clock.ns;
        if (input != nullptr)
        {
            if (bypassInput)
                input->processBlockBypassed(buffer, midi);
            else
                input->processBlock(buffer, midi);
        }
        if (record)
            outLeft.insert(outLeft.end(), data_[0].begin(), data_[0].begin() + frames);
        if (output != nullptr)
        {
            const std::size_t bytes = (count + kCanary) * sizeof(float);
            for (std::size_t c = 0; c < 2; ++c)
                std::memcpy(afterInput_[c].data(), data_[c].data(), bytes);
            if (bypassOutput)
                output->processBlockBypassed(buffer, midi);
            else
                output->processBlock(buffer, midi);
            for (std::size_t c = 0; c < 2; ++c)
                if (std::memcmp(afterInput_[c].data(), data_[c].data(), bytes) != 0)
                    ++outputTouched;
        }
        for (const auto& ch : data_)
            for (std::size_t i = count; i < count + kCanary; ++i)
                if (std::memcmp(&ch[i], &kCanaryBits, sizeof kCanaryBits) != 0)
                    ++canaryHits;
        advance(static_cast<std::uint64_t>(std::llround(static_cast<double>(frames) * 1.0e9 / kRate)));
        position_ += count;
        if (playHead.playing)
            playHead.samples += frames;
    }

    void cycles(int count, int frames = kBlock)
    {
        for (int i = 0; i < count; ++i)
            cycle(frames);
    }

    // 时间流过（不调音频回调时就是宿主把两端挂起），每 40 ms tick 一次
    void advance(std::uint64_t ns)
    {
        for (std::uint64_t left = ns; left > 0;)
        {
            const std::uint64_t step = std::min<std::uint64_t>(left, kBlockNs);
            clock.ns += step;
            left -= step;
            sinceTick_ += step;
            if (sinceTick_ >= 4 * kBlockNs)
            {
                sinceTick_ = 0;
                tick();
            }
        }
    }

    void tick()
    {
        if (input != nullptr)
            input->instanceHost().tick();
        if (output != nullptr)
            output->instanceHost().tick();
        for (auto* extra : extraTicks)
            extra->instanceHost().tick();
    }

    const ipc::NameScope& scope() const noexcept { return scope_; }
    // 上一个周期缓冲左声道的前 n 帧（Input 处理之后、也就是 Output 看到的）
    const float* left() const noexcept { return data_[0].data(); }
    const float* right() const noexcept { return data_[1].data(); }

    const int declared;
    FakeClock clock;
    FakePlayHead playHead;
    std::function<float(std::uint64_t, int)> source;
    std::unique_ptr<InputProcessor> input;
    std::unique_ptr<OutputProcessor> output;
    std::vector<RinneProcessorBase*> extraTicks; // 本作用域里另外要一起 tick 的实例（不拥有）
    bool record = false;
    std::vector<float> dryLeft; // record 为真时：Input 处理之前的左声道
    std::vector<float> outLeft; // record 为真时：Input 处理之后的左声道
    std::size_t outputTouched = 0; // Output 改动了缓冲的声道块数（不变量 8：恒为 0）
    std::size_t canaryHits = 0; // 块尾之后被写过的样本数（恒为 0）

private:
    ipc::ISegmentBackend* backend_;
    ipc::NameScope scope_;
    std::array<std::vector<float>, 2> data_;
    std::array<std::vector<float>, 2> afterInput_;
    std::uint64_t position_ = 0;
    std::uint64_t sinceTick_ = 0;
};

} // namespace rinne::wiring_test
