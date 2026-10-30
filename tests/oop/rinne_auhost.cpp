// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/mac-ci.json
//
// 进程外 AU 宿主自测：按 AudioComponent 三元组找到已装好的 AU，用 kAudioComponentInstantiation_LoadOutOfProcess 实例化
// （插件跑在 AUHostingService 进程里），喂正弦，离线渲染，判输出有限、没有越界、峰值在、渲染调用都返回成功。
// 实例化之后核对插件真的在进程外：宿主进程自己的已加载映像里不该有 Rinne 的 .component（否则这一步其实验的是进程内）。
//   rinne_auhost --type aufx --subtype Rnni --manufacturer Snch [--in-process] [--seconds 1]
//   rinne_auhost --pair [--manufacturer Snch] [--input-subtype Rnni] [--output-subtype Rnno] [--seconds 5]
//                       [--shm-prefix SnchRinne]
//   rinne_auhost --self-test
// 配对模式（--pair）：同时以进程外方式实例化 Output 与 Input，按实时节奏渲染几秒，Output 喂 880 Hz、Input 喂 440 Hz
// 并把 反馈开到 100 %，宿主只读映射会合段与数据段去看两个槽，再判 Input 的输出里有没有出现 Output 的 880 Hz。 判据全在
// PairJudge.h（纯函数，tests/ipc/test_pair_judge.cpp 在所有平台上自测）。智能默认配对只匹配同一进程里的对端，
// 所以两个实例落在不同的 AUHostingService 进程时配不上，报告里会给出两端的 pid。
// 退出码：0 通过；1 渲染或判据不通过；2 参数错；3 找不到组件；4 实例化或配置失败；5 配对判据不通过。

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "PairJudge.h"

namespace
{

namespace ipc = rinne::ipc;
namespace oop = rinne::oop;

constexpr double kSampleRate = 48000.0;
constexpr UInt32 kBlock = 512;
constexpr UInt32 kMaxFrames = 4096;
constexpr UInt32 kChannels = 2;
// 干声直通加上限制器之类的处理，输出不会比输入大很多；超过这个界说明链路坏了
constexpr float kBound = 4.0f;
// 喂进去的是 0.25 的正弦，直通后的峰值应当在 0.25 左右；低于这个值说明音频根本没过去（全静音也是「有限、没越界」）
constexpr float kMinPeak = 0.2f;
// Input 的 Feedback 参数 ID 与范围（0..150 %）取自 tests/golden/params-input.json 的 au 列
constexpr AudioUnitParameterID kFeedbackParam = 1955982213u;
constexpr float kPairFeedbackPercent = 100.0f;
constexpr double kPairSeconds = 5.0;
constexpr double kPairMinSeconds = 3.0;
// 每隔这么多块去看一眼会合段（约 256 ms）
constexpr std::uint64_t kPollEveryBlocks = 24;

struct Judge
{
    bool finite = true;
    bool inBounds = true;
    float peak = 0.0f;
};

// 一块输出的判据：每个样本有限，且绝对值不超过上界
void judgeBlock(const float* samples, UInt32 n, Judge& j)
{
    for (UInt32 i = 0; i < n; ++i)
    {
        const float v = samples[i];
        if (!std::isfinite(v))
        {
            j.finite = false;
            continue;
        }
        const float a = std::fabs(v);
        if (a > j.peak)
            j.peak = a;
        if (a > kBound)
            j.inBounds = false;
    }
}

// 渲染多少块：向上取整，秒数不能整除块长时也要满足「至少渲染这么长」（1 秒 = 94 块，不是 93 块）
std::uint64_t blocksFor(double seconds)
{
    return static_cast<std::uint64_t>(std::ceil(seconds * kSampleRate / kBlock));
}

int selfTest()
{
    int failed = 0;
    const auto expect = [&failed](const char* name, bool ok) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++failed;
    };
    {
        Judge j;
        const float good[] = {0.0f, 0.5f, -0.5f};
        judgeBlock(good, 3, j);
        expect("正常的块通过", j.finite && j.inBounds);
    }
    {
        Judge j;
        const float bad[] = {0.0f, NAN};
        judgeBlock(bad, 2, j);
        expect("NaN 判红", !j.finite);
    }
    {
        Judge j;
        const float bad[] = {0.0f, 100.0f};
        judgeBlock(bad, 2, j);
        expect("越界判红", !j.inBounds);
    }
    expect("1 秒渲染 94 块（向上取整，不是 93）", blocksFor(1.0) == 94);
    expect("2 秒渲染 188 块（187.5 向上取整）", blocksFor(2.0) == 188);
    expect("5 秒渲染 469 块", blocksFor(5.0) == 469);
    {
        std::vector<float> dry(48000);
        std::vector<float> ret(48000);
        for (std::size_t i = 0; i < dry.size(); ++i)
        {
            dry[i] = static_cast<float>(
                0.25 * std::sin(2.0 * M_PI * oop::kInputToneHz * static_cast<double>(i) / kSampleRate));
            ret[i] = static_cast<float>(
                0.125 * std::sin(2.0 * M_PI * oop::kOutputToneHz * static_cast<double>(i) / kSampleRate));
        }
        const double a440 = oop::goertzelAmplitude(dry.data(), dry.size(), oop::kInputToneHz, kSampleRate);
        const double a880 = oop::goertzelAmplitude(dry.data(), dry.size(), oop::kOutputToneHz, kSampleRate);
        expect("Goertzel 在 440 Hz 看到 0.25、在 880 Hz 看不到", std::fabs(a440 - 0.25) < 0.001 && a880 < 0.001);
        for (std::size_t i = 0; i < dry.size(); ++i)
            dry[i] += ret[i];
        const double mixed880 = oop::goertzelAmplitude(dry.data(), dry.size(), oop::kOutputToneHz, kSampleRate);
        expect("音频判据：干声加回送通过", oop::judgeTones(a440, mixed880).ok());
        expect("音频判据：没有回送判红", !oop::judgeTones(a440, a880).ok());
    }
    {
        oop::GroupSnapshot early;
        early.group = 2;
        for (oop::SlotSnapshot* s : {&early.input, &early.output})
        {
            s->ctl = ipc::makeCtl(1, ipc::kCtlPhaseActive);
            s->pid = 4242;
            s->heartbeatMs = 1000;
            s->blockSeq = 10;
            s->safetyState = ipc::kSafetyFadeIn;
        }
        early.ringMapped = true;
        early.ringMagic = ipc::kRingMagic;
        early.writeHead = 5000;
        oop::GroupSnapshot late = early;
        for (oop::SlotSnapshot* s : {&late.input, &late.output})
        {
            s->heartbeatMs = 3200;
            s->blockSeq = 210;
        }
        late.input.safetyState = ipc::kSafetyRun;
        late.writeHead = 5000 + 200 * kBlock;
        oop::PairCheck check;
        check.hostPid = 99;
        check.hostBlocks = 200;
        expect("配对判据：健康的一对通过", oop::judgePairing(early, late, check).ok());
        oop::GroupSnapshot stalled = late;
        stalled.output.heartbeatMs = early.output.heartbeatMs;
        expect("配对判据：心跳不走判红", !oop::judgePairing(early, stalled, check).ok());
        oop::GroupSnapshot inHost = late;
        inHost.input.pid = 99;
        inHost.output.pid = 99;
        oop::GroupSnapshot inHostEarly = early;
        inHostEarly.input.pid = 99;
        inHostEarly.output.pid = 99;
        expect("配对判据：槽里的 pid 是宿主自己判红", !oop::judgePairing(inHostEarly, inHost, check).ok());
    }
    expect("段名：会合段", oop::posixSegmentName("SnchRinne", 501, 0) == "/SnchRinne.000001f5.rv");
    expect("段名：数据段", oop::posixSegmentName("SnchRinne", 501, 3) == "/SnchRinne.000001f5.v1.g3");
    expect("映像路径：Rinne 的 .component 是插件映像",
           oop::isRinnePluginImagePath(
               "/Users/runner/Library/Audio/Plug-Ins/Components/Rinne Input.component/Contents/MacOS/Rinne Input"));
    expect("映像路径：系统框架不是",
           !oop::isRinnePluginImagePath("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox"));
    expect("映像路径：宿主自己不是", !oop::isRinnePluginImagePath("/Users/runner/work/rinne/build-oop/rinne_auhost"));
    expect("宿主进程里没有 Rinne 的 .component 映像（自测时还没加载任何插件）", [] {
        const std::uint32_t count = _dyld_image_count();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const char* name = _dyld_get_image_name(i);
            if (name != nullptr && oop::isRinnePluginImagePath(name))
                return false;
        }
        return true;
    }());
    std::printf("rinne_auhost 自测：%s\n", failed == 0 ? "通过" : "失败");
    return failed == 0 ? 0 : 1;
}

struct Instance
{
    AudioUnit unit = nullptr;
    double toneHz = 440.0;
    std::vector<float> scratch[kChannels];
    std::vector<float> out[kChannels];
};

// 输入回调：给插件的输入总线喂 toneHz、0.25 幅度（约 -12 dBFS）的正弦
OSStatus feedInput(void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 frames,
                   AudioBufferList* io)
{
    auto* in = static_cast<Instance*>(refCon);
    if (frames > kMaxFrames)
        return kAudioUnitErr_TooManyFramesToProcess;
    const auto start = static_cast<std::uint64_t>(ts->mSampleTime);
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        std::vector<float>& src = in->scratch[b < kChannels ? b : kChannels - 1];
        for (UInt32 i = 0; i < frames; ++i)
            src[i] = static_cast<float>(
                oop::kToneAmplitude * std::sin(2.0 * M_PI * in->toneHz * static_cast<double>(start + i) / kSampleRate));
        if (io->mBuffers[b].mData == nullptr)
            io->mBuffers[b].mData = src.data();
        else
            std::memcpy(io->mBuffers[b].mData, src.data(), frames * sizeof(float));
        io->mBuffers[b].mDataByteSize = frames * static_cast<UInt32>(sizeof(float));
    }
    return noErr;
}

struct InstantiateWait
{
    std::atomic<bool> done{false};
    AudioComponentInstance instance = nullptr;
    OSStatus status = noErr;
};

UInt32 fourcc(const std::string& s)
{
    UInt32 v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | static_cast<UInt32>(static_cast<unsigned char>(s[static_cast<std::size_t>(i)]));
    return v;
}

// 实例化并等回调；回调靠主线程的 run loop 转，所以边等边转
bool instantiate(AudioComponent comp, bool outOfProcess, double timeoutSeconds, AudioComponentInstance& out,
                 OSStatus& status)
{
    // 堆上分配，超时后不释放：回调可能晚到，写进已释放的内存更糟。一次性进程，泄漏可接受
    auto* wait = new InstantiateWait();
    const AudioComponentInstantiationOptions options =
        outOfProcess ? static_cast<AudioComponentInstantiationOptions>(kAudioComponentInstantiation_LoadOutOfProcess)
                     : static_cast<AudioComponentInstantiationOptions>(0);
    AudioComponentInstantiate(comp, options, ^(AudioComponentInstance instance, OSStatus error) {
      wait->instance = instance;
      wait->status = error;
      wait->done.store(true, std::memory_order_release);
    });
    double waited = 0.0;
    while (!wait->done.load(std::memory_order_acquire) && waited < timeoutSeconds)
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        waited += 0.02;
    }
    if (!wait->done.load(std::memory_order_acquire))
    {
        status = -1;
        return false;
    }
    out = wait->instance;
    status = wait->status;
    delete wait;
    return status == noErr && out != nullptr;
}

OSStatus setProperty(AudioUnit unit, AudioUnitPropertyID id, AudioUnitScope scope, const void* data, UInt32 size,
                     const char* what)
{
    const OSStatus st = AudioUnitSetProperty(unit, id, scope, 0, data, size);
    if (st != noErr)
        std::fprintf(stderr, "设置 %s 失败，状态 %d\n", what, static_cast<int>(st));
    return st;
}

// 宿主自己进程里有没有映射着 Rinne 的 .component 映像
bool pluginImageInHost()
{
    const std::uint32_t count = _dyld_image_count();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const char* name = _dyld_get_image_name(i);
        if (name != nullptr && oop::isRinnePluginImagePath(name))
        {
            std::fprintf(stderr, "宿主进程里映射着插件映像：%s\n", name);
            return true;
        }
    }
    return false;
}

// 找组件、实例化、核对进程外、设格式与回调、初始化。0 = 成功，其余是退出码
int startInstance(const char* label, const AudioComponentDescription& desc, bool outOfProcess, double toneHz,
                  Instance& in)
{
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr)
    {
        std::fprintf(stderr, "找不到组件 %s（没装好，或注册器还没扫到）\n", label);
        return 3;
    }
    in.toneHz = toneHz;
    for (auto& s : in.scratch)
        s.assign(kMaxFrames, 0.0f);
    for (auto& o : in.out)
        o.assign(kBlock, 0.0f);
    OSStatus status = noErr;
    if (!instantiate(comp, outOfProcess, 60.0, in.unit, status))
    {
        std::fprintf(stderr, "实例化失败（%s，%s），状态 %d\n", label, outOfProcess ? "进程外" : "进程内",
                     static_cast<int>(status));
        return 4;
    }
    std::printf("%s 已%s实例化\n", label, outOfProcess ? "以进程外选项" : "在进程内");
    if (outOfProcess && pluginImageInHost())
    {
        std::fprintf(stderr, "%s：要求进程外加载，但插件映像出现在宿主进程里，这一步验的其实是进程内\n", label);
        return 4;
    }

    AudioStreamBasicDescription format{};
    format.mSampleRate = kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    UInt32 maxFrames = kMaxFrames;
    AURenderCallbackStruct callback{&feedInput, &in};
    if (setProperty(in.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, &format, sizeof format,
                    "输入格式") != noErr ||
        setProperty(in.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, &format, sizeof format,
                    "输出格式") != noErr ||
        setProperty(in.unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, &maxFrames,
                    sizeof maxFrames, "最大帧数") != noErr ||
        setProperty(in.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, &callback, sizeof callback,
                    "渲染回调") != noErr)
        return 4;
    status = AudioUnitInitialize(in.unit);
    if (status != noErr)
    {
        std::fprintf(stderr, "AudioUnitInitialize 失败（%s），状态 %d\n", label, static_cast<int>(status));
        return 4;
    }
    return 0;
}

void stopInstance(Instance& in)
{
    if (in.unit == nullptr)
        return;
    AudioUnitUninitialize(in.unit);
    AudioComponentInstanceDispose(in.unit);
    in.unit = nullptr;
}

// 渲染一块；data[c] 指向各声道的输出（插件可能把指针换成自己的缓冲，所以取渲染后的值）
OSStatus renderBlock(Instance& in, std::uint64_t n, const float* (&data)[kChannels])
{
    // AudioBufferList 的尾部是可变长数组：按两个通道的大小自己摆一个
    struct
    {
        UInt32 count;
        AudioBuffer buffers[kChannels];
    } list{};
    list.count = kChannels;
    for (UInt32 c = 0; c < kChannels; ++c)
    {
        list.buffers[c].mNumberChannels = 1;
        list.buffers[c].mDataByteSize = kBlock * static_cast<UInt32>(sizeof(float));
        list.buffers[c].mData = in.out[c].data();
    }
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = static_cast<double>(n * kBlock);
    AudioUnitRenderActionFlags flags = 0;
    const OSStatus status = AudioUnitRender(in.unit, &flags, &ts, 0, kBlock, reinterpret_cast<AudioBufferList*>(&list));
    for (UInt32 c = 0; c < kChannels; ++c)
        data[c] = static_cast<const float*>(list.buffers[c].mData);
    return status;
}

// 只读映射一个共享内存段。段是插件创建的，宿主只看，不建、不写、不删。
class Mapping
{
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { release(); }

    bool mapped() const { return base_ != nullptr; }
    const void* data() const { return base_; }

    bool open(const std::string& name, std::size_t bytes)
    {
        release();
        const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0)
            return false;
        struct stat info
        {
        };
        if (::fstat(fd, &info) != 0 || static_cast<std::size_t>(info.st_size) < bytes)
        {
            ::close(fd);
            return false;
        }
        void* p = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED)
            return false;
        base_ = p;
        bytes_ = bytes;
        return true;
    }

private:
    void release()
    {
        if (base_ != nullptr)
            ::munmap(base_, bytes_);
        base_ = nullptr;
        bytes_ = 0;
    }

    void* base_ = nullptr;
    std::size_t bytes_ = 0;
};

// 盯着会合段与配上的那一组的数据段
class SegmentWatcher
{
public:
    explicit SegmentWatcher(std::string prefix) : prefix_(std::move(prefix)), uid_(static_cast<ipc::u32>(::geteuid()))
    {
    }

    // 取一轮八个组的快照；会合段还没出现（或还没初始化完）时返回 false
    bool poll(std::array<oop::GroupSnapshot, ipc::kGroupCount>& groups)
    {
        if (!rv_.mapped() && !rv_.open(oop::posixSegmentName(prefix_, uid_, 0), ipc::kRvBytes))
            return false;
        const auto* rv = static_cast<const ipc::RvSegment*>(rv_.data());
        if (rv->header.magic.load(std::memory_order_acquire) != ipc::kRvMagic ||
            rv->header.abi.load(std::memory_order_relaxed) != ipc::kIpcAbi)
            return false;
        for (ipc::u32 g = 1; g <= ipc::kGroupCount; ++g)
            groups[g - 1] = oop::snapshotGroup(*rv, g);
        return true;
    }

    // 给 g 补上数据段的写头；数据段还没出现时 ringMapped 保持 false
    void attachRing(oop::GroupSnapshot& g)
    {
        if (ringGroup_ != g.group)
        {
            ring_.open(oop::posixSegmentName(prefix_, uid_, g.group), ipc::kRingBytes);
            ringGroup_ = g.group;
        }
        else if (!ring_.mapped())
        {
            ring_.open(oop::posixSegmentName(prefix_, uid_, g.group), ipc::kRingBytes);
        }
        if (ring_.mapped())
            oop::attachRing(g, static_cast<const ipc::RingSegment*>(ring_.data())->header);
    }

    bool rendezvousMapped() const { return rv_.mapped(); }
    ipc::u32 uid() const { return uid_; }
    const std::string& prefix() const { return prefix_; }

private:
    std::string prefix_;
    ipc::u32 uid_;
    Mapping rv_;
    Mapping ring_;
    ipc::u32 ringGroup_ = 0;
};

void report(const char* what, const oop::Verdict& v)
{
    if (v.ok())
    {
        std::printf("%s：通过\n", what);
        return;
    }
    for (const std::string& f : v.failures)
        std::printf("%s：不通过：%s\n", what, f.c_str());
}

struct Options
{
    std::string type = "aufx";
    std::string subtype;
    std::string manufacturer;
    std::string inputSubtype = "Rnni";
    std::string outputSubtype = "Rnno";
    std::string shmPrefix = "SnchRinne";
    bool outOfProcess = true;
    bool pair = false;
    bool secondsGiven = false;
    double seconds = 1.0;
};

// 单个 AU 的进程外自测：喂一个 440 Hz，渲染 seconds 秒（离线，不卡实时节奏），判输出有限、没越界、峰值在
int runSingle(const Options& opt)
{
    AudioComponentDescription desc{};
    desc.componentType = fourcc(opt.type);
    desc.componentSubType = fourcc(opt.subtype);
    desc.componentManufacturer = fourcc(opt.manufacturer);
    const std::string label = opt.type + " " + opt.subtype + " " + opt.manufacturer;
    Instance in;
    if (const int rc = startInstance(label.c_str(), desc, opt.outOfProcess, oop::kInputToneHz, in); rc != 0)
        return rc;

    Judge judge;
    const std::uint64_t blocks = blocksFor(opt.seconds);
    std::uint64_t rendered = 0;
    int result = 0;
    for (std::uint64_t n = 0; n < blocks; ++n)
    {
        const float* data[kChannels] = {};
        const OSStatus status = renderBlock(in, n, data);
        if (status != noErr)
        {
            std::fprintf(stderr, "第 %llu 块渲染失败，状态 %d\n", static_cast<unsigned long long>(n),
                         static_cast<int>(status));
            result = 1;
            break;
        }
        for (UInt32 c = 0; c < kChannels; ++c)
            judgeBlock(data[c], kBlock, judge);
        ++rendered;
    }
    stopInstance(in);

    std::printf("渲染 %llu / %llu 块，峰值 %.4f，有限=%d，未越界=%d\n", static_cast<unsigned long long>(rendered),
                static_cast<unsigned long long>(blocks), static_cast<double>(judge.peak), judge.finite ? 1 : 0,
                judge.inBounds ? 1 : 0);
    if (result == 0 && rendered != blocks)
        result = 1;
    if (result == 0 && (!judge.finite || !judge.inBounds))
        result = 1;
    if (result == 0 && judge.peak < kMinPeak)
    {
        std::fprintf(stderr, "输出峰值 %.4f 低于 %.2f：喂进去的 0.25 正弦没有过去（全静音也算有限、没越界）\n",
                     static_cast<double>(judge.peak), static_cast<double>(kMinPeak));
        result = 1;
    }
    std::printf("%s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}

// 配对自测：见文件头
int runPair(const Options& opt)
{
    AudioComponentDescription outDesc{};
    outDesc.componentType = fourcc(opt.type);
    outDesc.componentSubType = fourcc(opt.outputSubtype);
    outDesc.componentManufacturer = fourcc(opt.manufacturer);
    AudioComponentDescription inDesc = outDesc;
    inDesc.componentSubType = fourcc(opt.inputSubtype);

    Instance output;
    Instance input;
    if (const int rc = startInstance("Output", outDesc, true, oop::kOutputToneHz, output); rc != 0)
        return rc;
    if (const int rc = startInstance("Input", inDesc, true, oop::kInputToneHz, input); rc != 0)
    {
        stopInstance(output);
        return rc;
    }
    if (AudioUnitSetParameter(input.unit, kFeedbackParam, kAudioUnitScope_Global, 0, kPairFeedbackPercent, 0) != noErr)
    {
        std::fprintf(stderr, "设置 Input 的反馈参数失败\n");
        stopInstance(input);
        stopInstance(output);
        return 4;
    }

    SegmentWatcher watcher(opt.shmPrefix);
    std::array<oop::GroupSnapshot, ipc::kGroupCount> groups{};
    oop::GroupSnapshot early;
    oop::GroupSnapshot latest;
    std::uint64_t earlyBlock = 0;
    bool haveEarly = false;
    ipc::u32 activeWithoutRing = 0; // 配上过但数据段始终没映射上的组号，只用于报告
    // 每轮把所有组的快照存下来：配不上时报告里要列出各组的占用情况
    const auto pollOnce = [&](std::uint64_t block) {
        if (!watcher.poll(groups))
            return;
        if (!haveEarly)
        {
            const ipc::u32 g = oop::findPairedGroup(groups);
            if (g == 0)
                return;
            // 写头的增量要有起点：属主在槽变成 Active 之后才由维护步骤创建数据段，这一轮还没映射上就不记起点，
            // 下一轮再试；记了一个没有写头的起点，判据里的「写头在涨」就只剩「写过」
            oop::GroupSnapshot candidate = groups[g - 1];
            watcher.attachRing(candidate);
            if (!candidate.ringMapped)
            {
                activeWithoutRing = g;
                return;
            }
            early = candidate;
            earlyBlock = block;
            haveEarly = true;
        }
        latest = groups[early.group - 1];
        watcher.attachRing(latest);
    };

    Judge inJudge;
    Judge outJudge;
    std::vector<float> inputLeft;
    std::vector<float> outputLeft;
    const std::uint64_t blocks = blocksFor(opt.seconds);
    inputLeft.reserve(static_cast<std::size_t>(blocks) * kBlock);
    outputLeft.reserve(static_cast<std::size_t>(blocks) * kBlock);
    int result = 0;
    std::uint64_t rendered = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t n = 0; n < blocks; ++n)
    {
        // 实时节奏：智能默认配对要「距 prepare 300 ms」「累计 8 个实时块」，离线狂渲染会让这两条永远不满足
        std::this_thread::sleep_until(
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(n * kBlock) / kSampleRate)));
        const float* outData[kChannels] = {};
        const float* inData[kChannels] = {};
        const OSStatus outStatus = renderBlock(output, n, outData);
        const OSStatus inStatus = renderBlock(input, n, inData);
        if (outStatus != noErr || inStatus != noErr)
        {
            std::fprintf(stderr, "第 %llu 块渲染失败：Output 状态 %d，Input 状态 %d\n",
                         static_cast<unsigned long long>(n), static_cast<int>(outStatus), static_cast<int>(inStatus));
            result = 1;
            break;
        }
        for (UInt32 c = 0; c < kChannels; ++c)
        {
            judgeBlock(outData[c], kBlock, outJudge);
            judgeBlock(inData[c], kBlock, inJudge);
        }
        outputLeft.insert(outputLeft.end(), outData[0], outData[0] + kBlock);
        inputLeft.insert(inputLeft.end(), inData[0], inData[0] + kBlock);
        ++rendered;
        if (n % kPollEveryBlocks == 0)
            pollOnce(n);
    }
    pollOnce(rendered);
    // 结束时再取一轮全组快照，诊断用
    watcher.poll(groups);

    std::printf("渲染 %llu / %llu 块（实时节奏），Input 峰值 %.4f，Output 峰值 %.4f\n",
                static_cast<unsigned long long>(rendered), static_cast<unsigned long long>(blocks),
                static_cast<double>(inJudge.peak), static_cast<double>(outJudge.peak));
    if (result == 0 && (!inJudge.finite || !inJudge.inBounds || !outJudge.finite || !outJudge.inBounds))
    {
        std::fprintf(stderr, "输出里有非有限值或越界\n");
        result = 1;
    }

    oop::Verdict pairing;
    if (result == 0)
    {
        if (!haveEarly)
        {
            if (activeWithoutRing != 0)
            {
                pairing.failures.push_back("组 " + std::to_string(activeWithoutRing) +
                                           " 的 Input 与 Output 都是 Active，但数据段始终没有映射到：Output "
                                           "没有创建这一组的环，或段名前缀与构建不一致（见 --shm-prefix）");
            }
            else
            {
                pairing.failures.push_back(watcher.rendezvousMapped()
                                               ? "整个渲染期间没有任何一组出现 Input 与 Output 同时 Active：没有配上"
                                               : "没有找到会合段 " +
                                                     oop::posixSegmentName(watcher.prefix(), watcher.uid(), 0) +
                                                     "：插件没有创建它（沙箱不许进程外的插件用 POSIX "
                                                     "共享内存，或段名前缀与构建不一致，见 --shm-prefix）");
            }
        }
        else
        {
            oop::PairCheck check;
            check.hostPid = static_cast<ipc::u32>(::getpid());
            check.hostBlocks = rendered > earlyBlock ? rendered - earlyBlock : 0;
            pairing = oop::judgePairing(early, latest, check);
        }
        report("配对", pairing);
    }

    oop::Verdict tones;
    oop::Verdict passthrough;
    if (result == 0)
    {
        const std::size_t window = static_cast<std::size_t>(kSampleRate); // 1 秒，440 与 880 Hz 都是整数个周期
        if (inputLeft.size() < window || outputLeft.size() < window)
        {
            tones.failures.push_back("渲染不足 1 秒，没法做音频判据");
        }
        else
        {
            const float* inTail = inputLeft.data() + (inputLeft.size() - window);
            const float* outTail = outputLeft.data() + (outputLeft.size() - window);
            const double dry = oop::goertzelAmplitude(inTail, window, oop::kInputToneHz, kSampleRate);
            const double ret = oop::goertzelAmplitude(inTail, window, oop::kOutputToneHz, kSampleRate);
            std::printf("Input 输出末 1 秒：440 Hz 幅度 %.4f，880 Hz 幅度 %.4f\n", dry, ret);
            tones = oop::judgeTones(dry, ret);
            passthrough =
                oop::judgePassthrough(oop::goertzelAmplitude(outTail, window, oop::kOutputToneHz, kSampleRate));
        }
        report("音频", tones);
        report("Output 直通", passthrough);
    }

    if (!pairing.ok() || !tones.ok() || !passthrough.ok())
    {
        std::printf("各组占用情况（宿主 pid %d）：\n", static_cast<int>(::getpid()));
        bool any = false;
        for (const oop::GroupSnapshot& g : groups)
        {
            if (g.group != 0 &&
                (ipc::ctlPhase(g.input.ctl) != ipc::kCtlPhaseFree || ipc::ctlPhase(g.output.ctl) != ipc::kCtlPhaseFree))
            {
                std::printf("  %s\n", oop::describeGroup(g).c_str());
                any = true;
            }
        }
        if (!any)
            std::printf("  （没有任何一组被占用）\n");
        result = 5;
    }

    stopInstance(input);
    stopInstance(output);
    std::printf("%s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--self-test")
            return selfTest();
        if (a == "--type")
            opt.type = next();
        else if (a == "--subtype")
            opt.subtype = next();
        else if (a == "--manufacturer")
            opt.manufacturer = next();
        else if (a == "--input-subtype")
            opt.inputSubtype = next();
        else if (a == "--output-subtype")
            opt.outputSubtype = next();
        else if (a == "--shm-prefix")
            opt.shmPrefix = next();
        else if (a == "--pair")
            opt.pair = true;
        else if (a == "--in-process")
            opt.outOfProcess = false;
        else if (a == "--seconds")
        {
            opt.seconds = std::atof(next().c_str());
            opt.secondsGiven = true;
        }
        else
        {
            std::fprintf(stderr, "不认识的参数 %s\n", a.c_str());
            return 2;
        }
    }
    if (opt.pair)
    {
        if (!opt.secondsGiven)
            opt.seconds = kPairSeconds;
        if (opt.manufacturer.empty())
            opt.manufacturer = "Snch";
        if (!opt.outOfProcess || opt.type.size() != 4 || opt.manufacturer.size() != 4 || opt.inputSubtype.size() != 4 ||
            opt.outputSubtype.size() != 4 || opt.shmPrefix.empty() || opt.seconds < kPairMinSeconds)
        {
            std::fprintf(stderr,
                         "用法：rinne_auhost --pair [--type aufx] [--manufacturer <4 字符>] [--input-subtype <4 字符>] "
                         "[--output-subtype <4 字符>] [--seconds n（至少 3）] [--shm-prefix <POSIX "
                         "前缀>]；配对模式只做进程外\n");
            return 2;
        }
        return runPair(opt);
    }
    if (opt.type.size() != 4 || opt.subtype.size() != 4 || opt.manufacturer.size() != 4 || opt.seconds <= 0.0)
    {
        std::fprintf(stderr, "用法：rinne_auhost --type aufx --subtype <4 字符> --manufacturer <4 字符> [--in-process] "
                             "[--seconds n]\n       rinne_auhost --pair [...]\n       rinne_auhost --self-test\n");
        return 2;
    }
    return runSingle(opt);
}
