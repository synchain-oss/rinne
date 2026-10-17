// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/mac-ci.json
//
// 进程外 AU 宿主自测：按 AudioComponent 三元组找到已装好的 AU，用 kAudioComponentInstantiation_LoadOutOfProcess 实例化
// （插件跑在 AUHostingService 进程里），喂 1 秒的正弦，离线渲染，判输出有限、没有越界、渲染调用都返回成功。
//   rinne_auhost --type aufx --subtype Rnni --manufacturer Snch [--in-process] [--seconds 1]
//   rinne_auhost --self-test
// 退出码：0 通过；1 渲染或判据不通过；2 参数错；3 找不到组件；4 实例化或配置失败。
// 配对判据（两端各自的音频在对端出现）由后续接手本目录的工作加上，这里只验「进程外加载并处理音频不崩溃」。

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr UInt32 kBlock = 512;
constexpr UInt32 kMaxFrames = 4096;
constexpr UInt32 kChannels = 2;
// 干声直通加上限制器之类的处理，输出不会比输入大很多；超过这个界说明链路坏了
constexpr float kBound = 4.0f;

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
    std::printf("rinne_auhost 自测：%s\n", failed == 0 ? "通过" : "失败");
    return failed == 0 ? 0 : 1;
}

struct Instance
{
    AudioUnit unit = nullptr;
    std::vector<float> scratch[kChannels];
};

// 输入回调：给插件的输入总线喂 440 Hz、约 -12 dBFS 的正弦
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
            src[i] =
                0.25f * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * static_cast<double>(start + i) / kSampleRate));
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

} // namespace

int main(int argc, char** argv)
{
    std::string type = "aufx";
    std::string subtype;
    std::string manufacturer;
    bool outOfProcess = true;
    double seconds = 1.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--self-test")
            return selfTest();
        if (a == "--type")
            type = next();
        else if (a == "--subtype")
            subtype = next();
        else if (a == "--manufacturer")
            manufacturer = next();
        else if (a == "--in-process")
            outOfProcess = false;
        else if (a == "--seconds")
            seconds = std::atof(next().c_str());
        else
        {
            std::fprintf(stderr, "不认识的参数 %s\n", a.c_str());
            return 2;
        }
    }
    if (type.size() != 4 || subtype.size() != 4 || manufacturer.size() != 4 || seconds <= 0.0)
    {
        std::fprintf(stderr, "用法：rinne_auhost --type aufx --subtype <4 字符> --manufacturer <4 字符> [--in-process] "
                             "[--seconds n]\n");
        return 2;
    }

    AudioComponentDescription desc{};
    desc.componentType = fourcc(type);
    desc.componentSubType = fourcc(subtype);
    desc.componentManufacturer = fourcc(manufacturer);
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr)
    {
        std::fprintf(stderr, "找不到组件 %s %s %s（没装好，或注册器还没扫到）\n", type.c_str(), subtype.c_str(),
                     manufacturer.c_str());
        return 3;
    }

    Instance in;
    for (auto& s : in.scratch)
        s.assign(kMaxFrames, 0.0f);
    OSStatus status = noErr;
    if (!instantiate(comp, outOfProcess, 60.0, in.unit, status))
    {
        std::fprintf(stderr, "实例化失败（%s），状态 %d\n", outOfProcess ? "进程外" : "进程内",
                     static_cast<int>(status));
        return 4;
    }
    std::printf("%s %s %s 已%s实例化\n", type.c_str(), subtype.c_str(), manufacturer.c_str(),
                outOfProcess ? "在进程外" : "在进程内");

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
        std::fprintf(stderr, "AudioUnitInitialize 失败，状态 %d\n", static_cast<int>(status));
        return 4;
    }

    // AudioBufferList 的尾部是可变长数组：按两个通道的大小自己摆一个
    struct
    {
        UInt32 count;
        AudioBuffer buffers[kChannels];
    } list{};
    std::vector<float> out[kChannels];
    for (UInt32 c = 0; c < kChannels; ++c)
        out[c].assign(kBlock, 0.0f);

    Judge judge;
    const auto blocks = static_cast<std::uint64_t>(seconds * kSampleRate / kBlock);
    std::uint64_t rendered = 0;
    int result = 0;
    for (std::uint64_t n = 0; n < blocks; ++n)
    {
        list.count = kChannels;
        for (UInt32 c = 0; c < kChannels; ++c)
        {
            list.buffers[c].mNumberChannels = 1;
            list.buffers[c].mDataByteSize = kBlock * static_cast<UInt32>(sizeof(float));
            list.buffers[c].mData = out[c].data();
        }
        AudioTimeStamp ts{};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        ts.mSampleTime = static_cast<double>(n * kBlock);
        AudioUnitRenderActionFlags flags = 0;
        status = AudioUnitRender(in.unit, &flags, &ts, 0, kBlock, reinterpret_cast<AudioBufferList*>(&list));
        if (status != noErr)
        {
            std::fprintf(stderr, "第 %llu 块渲染失败，状态 %d\n", static_cast<unsigned long long>(n),
                         static_cast<int>(status));
            result = 1;
            break;
        }
        for (UInt32 c = 0; c < kChannels; ++c)
            judgeBlock(static_cast<const float*>(list.buffers[c].mData), kBlock, judge);
        ++rendered;
    }
    AudioUnitUninitialize(in.unit);
    AudioComponentInstanceDispose(in.unit);

    std::printf("渲染 %llu / %llu 块，峰值 %.4f，有限=%d，未越界=%d\n", static_cast<unsigned long long>(rendered),
                static_cast<unsigned long long>(blocks), static_cast<double>(judge.peak), judge.finite ? 1 : 0,
                judge.inBounds ? 1 : 0);
    if (result == 0 && (!judge.finite || !judge.inBounds))
        result = 1;
    std::printf("%s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}
