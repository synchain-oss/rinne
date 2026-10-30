// SPDX-License-Identifier: GPL-3.0-or-later
//
// 插件级的免 DAW 宿主：同一进程里一对真处理器（rinne::InputProcessor 与 rinne::OutputProcessor），像宿主那样按块调
// processBlock，中间夹一段环内插件的替身，组成一条真的反馈环（经测试作用域里的共享内存会合）。
//
// 一条轨道，一个宿主回调周期：信号源填缓冲 → Input（第一格，就地加回送）→ 中间链（增益、固定延迟、坏值突发）→
// Output（推子前最后一格，只读缓冲、写环）。块长由调用方逐周期给；某一端这一周期可以不调（宿主跳过它的块）。
// 每一端一个 LanePlayHead（共用一张 TransportMap），调插件之前 arm 一次：时间线位置按这一端自己的游标查，
// 速度、时间戳给不给与给什么由各自的脚本按块给（见 TransportMap.h）。默认走带是停着的（与 core 测试台一致）。
//
// 时间：默认是逻辑时钟（InstanceHostConfig 的时钟注入点），不起定时器：每个周期把时钟推进 n/sr，累计满 40 ms 调两端
// 的 tick（25 Hz，与插件里的定时器同频）。快、可复现，与音频同一线程。realTimer 一档起真的 25 Hz 定时器、用真单调时钟，
// 宿主本身不调 tick：只有定时器的回调能让会合往前走（用例自己泵定时器）。
//
// 配对：两端手动选组（selectGroup），不依赖智能默认。两个实例用同一个随机测试作用域，与正式作用域、别的用例都不相干。
//
// 每个周期还核对两条不变量：Output 逐位不碰宿主缓冲（outputTouched 恒为 0）、两端都不写块尾之后（canaryHits 恒为 0）。
//
// 线程：只在测试线程上用。它同时是音频线程与消息线程（进程里的 JUCE 由 tests/host/shell/JuceLifetime.cpp 起）。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "InputProcessor.h"
#include "OutputProcessor.h"
#include "engine/EngineParams.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "host/plugin_host/TransportMap.h"
#include "ipc/Naming.h"

namespace rinne::plugin_host
{

inline constexpr double kRate = 48000.0;
inline constexpr std::uint64_t kTickNs = 40'000'000; // 25 Hz
inline constexpr int kMaxFrames = 32769; // 比支持的最大块长多 1
inline constexpr int kCanary = 64; // 块尾之后多查的样本数，抓越界写

// 逻辑时钟：InstanceHost 的单调时钟注入点
struct LogicalClock
{
    std::uint64_t ns = 10'000'000'000ull;
    static std::uint64_t read(void* context) noexcept { return static_cast<LogicalClock*>(context)->ns; }
};

// 坏值突发的三种：样本换成 NaN；样本乘 +∞（正 → +Inf、负 → −Inf、恰为 0 → NaN）；样本乘 1e6（有限的超大值）
enum class Garbage
{
    kNan,
    kInf,
    kHuge,
};

// 环内插件的替身：增益 → 固定延迟 → 坏值突发。突发按中间链自己的样本号（它处理过的第几个样本）给区间，可以落在块中间。
class MiddleChain
{
public:
    void setGain(float gain) noexcept { gain_ = gain; }
    // 固定延迟（样本），0 = 直通。改延迟清空延迟线
    void setDelay(int samples);
    // [from, to) 里的样本做 kind 那种坏值
    void addBurst(Garbage kind, std::int64_t from, std::int64_t to) { bursts_.push_back(Burst{kind, from, to}); }
    void clearBursts() noexcept { bursts_.clear(); }
    std::int64_t position() const noexcept { return position_; }

    void process(float* left, float* right, int n) noexcept;

private:
    struct Burst
    {
        Garbage kind;
        std::int64_t from;
        std::int64_t to;
    };

    float gain_ = 1.0f;
    int delay_ = 0;
    std::array<std::vector<float>, 2> line_;
    std::size_t write_ = 0;
    std::vector<Burst> bursts_;
    std::int64_t position_ = 0;
};

struct HostOptions
{
    double sampleRate = kRate;
    int inputBlock = 512; // Input 声明的最大块长
    int outputBlock = 512; // Output 声明的最大块长
    bool realTimer = false; // 真定时器 + 真单调时钟；假 = 逻辑时钟、不起定时器，宿主每 40 ms 调 tick
};

// 一个 Input 块之后交给观察者的东西，只在这一块之内有效
struct InputBlock
{
    std::int64_t first = 0; // 这一块在 Input 游标上的第一个样本（= Input 引擎实例创建以来的样本号）
    int n = 0;
    const float* dryLeft = nullptr; // Input 之前
    const float* dryRight = nullptr;
    const float* outLeft = nullptr; // Input 之后
    const float* outRight = nullptr;
};

class PluginHost
{
public:
    explicit PluginHost(const HostOptions& options = {});
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    InputProcessor& input() noexcept { return *input_; }
    OutputProcessor& output() noexcept { return *output_; }
    const engine::InputEngine& inputEngine() const noexcept { return input_->instanceHost().inputEngine(); }
    const engine::OutputEngine& outputEngine() const noexcept { return output_->instanceHost().outputEngine(); }
    TransportMap& transport() noexcept { return map_; }
    LanePlayHead& inputLane() noexcept { return inputLane_; }
    LanePlayHead& outputLane() noexcept { return outputLane_; }
    MiddleChain& chain() noexcept { return chain_; }
    LogicalClock& clock() noexcept { return clock_; }
    const ipc::NameScope& scope() const noexcept { return scope_; }
    double sampleRate() const noexcept { return options_.sampleRate; }

    // 信号源：轨道上的第几个样本（宿主回调的绝对样本号，与哪一端这一周期跑没跑无关）→ 两个声道。空 = 静音
    std::function<void(std::int64_t index, float& left, float& right)> source;
    // 每个 Input 块之后调（可空）。这时 Input 引擎的 lastBlockResult 一类块间状态正是这一块的
    std::function<void(const InputBlock&)> onInputBlock;
    // 每个 Output 块之前调（可空）：Output 这一块看到的缓冲（中间链之后）
    std::function<void(const float* left, const float* right, int n)> onOutputBlock;

    // 两端按声明的块长 prepareToPlay；只重新 prepare 一端，并改这一端声明的块长
    void prepare();
    void prepareInput(int maxBlock);
    void prepareOutput(int maxBlock);

    // 两端手动选组；逻辑时钟一档随后 tick 一拍
    bool selectGroup(int group);
    bool active() const;
    // 只在调 cycle 的线程读：上一块 Input 引擎看到了对端、能读环
    bool paired() const;
    // 按 frames 帧一块跑到配对为止
    bool pairUp(int frames, int maxCycles = 400);

    // 一个宿主回调周期，frames 帧；runInput / runOutput 为假 = 宿主这一周期没调那一端
    void cycle(int frames, bool runInput = true, bool runOutput = true);
    // 每周期 frames 帧，跑到轨道走过 samples 个样本（末块不截）
    void run(std::int64_t samples, int frames);

    // 工程重载：存下 Input 的 state，销毁这个 Input，新建一个实例并用存下的 state 全量恢复（含组号），再按原来的
    // 块长 prepare。次序与宿主重开工程一致：Output 一直留着；新实例没有任何历史，要靠 tick 重新配上。
    // 播放头游标与轨道位置不动
    void reloadInput();

    // 两端 tick 一拍（会合与引擎事件）
    void tick();
    // 逻辑时钟走 ns，途中每满 40 ms tick 一次（不调音频回调时就是宿主把两端挂起）。真定时器一档只记时间，不 tick
    void advance(std::uint64_t ns);

    // Input 这一块会拿到的引擎参数：参数层快照、胶水快照与预设序号，调的就是 InputProcessor 每块用的 snapshotBlock
    // （正式构建）
    engine::EngineParams inputParams();

    std::int64_t trackPosition() const noexcept { return trackPosition_; }

    std::size_t outputTouched = 0; // Output 改动了缓冲的周期数（不变量 8：恒为 0）
    std::size_t canaryHits = 0; // 块尾之后被写过的样本数（恒为 0）

private:
    InstanceHostConfig config(bool realTimer);

    HostOptions options_;
    ipc::NameScope scope_;
    LogicalClock clock_;
    TransportMap map_;
    LanePlayHead inputLane_;
    LanePlayHead outputLane_;
    MiddleChain chain_;
    std::unique_ptr<InputProcessor> input_;
    std::unique_ptr<OutputProcessor> output_;
    std::array<std::vector<float>, 2> data_; // 宿主缓冲（带块尾金丝雀）
    std::array<std::vector<float>, 2> dry_; // Input 之前
    std::array<std::vector<float>, 2> seen_; // Output 之前（核对 Output 不碰缓冲）
    std::int64_t trackPosition_ = 0;
    std::uint64_t sinceTick_ = 0;
};

} // namespace rinne::plugin_host
