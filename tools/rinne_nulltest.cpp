// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/nulltest.json
//
// rinne_nulltest：WAV 对拍与回声间隔测量的命令行工具。三个子命令：
//   make-signal  生成测试信号（冲激、冲激加 200 ms 粉噪突发、等间隔冲激串），同参数逐字节一致
//   compare      两份 WAV 对比：是否逐位相同、对齐偏移（lag）、残差 dBFS，可单列开头一段
//   echo         量回声间隔：在录音里找冲激回声，判相邻间隔是否都等于期望的 D，直接给 PASS / FAIL
// 零外部依赖：WAV 读写、互相关与统计都在本文件里，不链接 JUCE 和 rinne_core。
// 退出码：0 通过，2 判定未通过（FAIL），1 参数或文件错误。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
constexpr int kExitPass = 0;
constexpr int kExitError = 1;
constexpr int kExitFail = 2;

constexpr double kInf = std::numeric_limits<double>::infinity();

struct WavData
{
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    size_t frames = 0;
    std::vector<float> samples; // 交错存放，大小 = frames * channels
    uint16_t fmtTag = 0; // 1 整数 PCM，3 浮点
    uint16_t bits = 0;
    std::vector<uint8_t> raw; // data 块里的原始字节：32 位整数和 64 位浮点解码成 float 会丢位，逐位比对要看它

    float at(size_t frame, size_t ch) const { return samples[frame * channels + ch]; }
};

// ---------------------------------------------------------------- WAV 读写

uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool tagIs(const uint8_t* p, const char* tag)
{
    return std::memcmp(p, tag, 4) == 0;
}

void putU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
}

void putTag(std::vector<uint8_t>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}

// 支持 16 / 24 / 32 位整数 PCM、32 / 64 位浮点，含 WAVE_FORMAT_EXTENSIBLE
bool readWav(const std::string& path, WavData& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "打不开文件：" + path + "（检查路径是否写对、文件是否存在）";
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || !tagIs(bytes.data(), "RIFF") || !tagIs(bytes.data() + 8, "WAVE"))
    {
        err = "这不是 WAV 文件：" + path;
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    uint16_t fmtTag = 0;
    uint16_t channels = 0;
    uint16_t blockAlign = 0;
    uint16_t bits = 0;
    uint32_t sampleRate = 0;
    size_t dataOffset = 0;
    size_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        const uint8_t* head = bytes.data() + pos;
        const size_t size = readU32(head + 4);
        const uint8_t* body = head + 8;
        if (tagIs(head, "data") && !haveData)
        {
            // 录音软件中途停止时 data 块可能比声明的短，取实际剩下的部分
            dataOffset = pos + 8;
            dataSize = std::min(size, bytes.size() - dataOffset);
            haveData = true;
        }
        else if (pos + 8 + size > bytes.size())
        {
            err = "WAV 文件被截断：" + path;
            return false;
        }
        else if (tagIs(head, "fmt ") && !haveFmt)
        {
            if (size < 16)
            {
                err = "WAV 的 fmt 块太短：" + path;
                return false;
            }
            fmtTag = readU16(body);
            channels = readU16(body + 2);
            sampleRate = readU32(body + 4);
            blockAlign = readU16(body + 12);
            bits = readU16(body + 14);
            if (fmtTag == 0xFFFE) // EXTENSIBLE：子格式 GUID 的前两个字节是真正的格式码
            {
                if (size < 40)
                {
                    err = "WAV 的扩展 fmt 块太短：" + path;
                    return false;
                }
                fmtTag = readU16(body + 24);
            }
            haveFmt = true;
        }
        pos += 8 + size + (size & 1u); // 块按偶数字节对齐
    }

    if (!haveFmt || !haveData)
    {
        err = "WAV 里缺少 fmt 或 data 块：" + path;
        return false;
    }
    if (channels == 0 || blockAlign == 0 || bits == 0 || blockAlign < channels * (bits / 8))
    {
        err = "WAV 的格式信息不合法：" + path;
        return false;
    }
    const bool isFloat = fmtTag == 3;
    const bool isPcm = fmtTag == 1;
    const bool okBits = isFloat ? (bits == 32 || bits == 64) : (isPcm && (bits == 16 || bits == 24 || bits == 32));
    if (!okBits)
    {
        err = "不支持的 WAV 格式（只支持 16/24/32 位整数和 32/64 位浮点）：" + path;
        return false;
    }

    const size_t bytesPerSample = bits / 8u;
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.frames = dataSize / blockAlign;
    out.samples.assign(out.frames * channels, 0.0f);
    out.fmtTag = isFloat ? 3 : 1;
    out.bits = bits;

    const uint8_t* d = bytes.data() + dataOffset;
    out.raw.assign(d, d + out.frames * blockAlign);
    for (size_t fr = 0; fr < out.frames; ++fr)
    {
        for (size_t ch = 0; ch < channels; ++ch)
        {
            const uint8_t* p = d + fr * blockAlign + ch * bytesPerSample;
            float v = 0.0f;
            if (isFloat && bits == 32)
            {
                const uint32_t u = readU32(p);
                std::memcpy(&v, &u, sizeof(v));
            }
            else if (isFloat)
            {
                uint64_t u = static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
                double dv = 0.0;
                std::memcpy(&dv, &u, sizeof(dv));
                v = static_cast<float>(dv);
            }
            else if (bits == 16)
            {
                v = static_cast<float>(static_cast<int16_t>(readU16(p))) / 32768.0f;
            }
            else if (bits == 24)
            {
                int32_t s =
                    static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) | (static_cast<int32_t>(p[2]) << 16);
                if ((s & 0x00800000) != 0)
                    s -= 0x01000000;
                v = static_cast<float>(s) / 8388608.0f;
            }
            else
            {
                v = static_cast<float>(static_cast<double>(static_cast<int32_t>(readU32(p))) / 2147483648.0);
            }
            out.samples[fr * channels + ch] = v;
        }
    }
    return true;
}

// 格式名 -> (格式码, 位深)；make-signal 的 --format 用，默认 f32
bool parseFormat(const std::string& name, uint16_t& tag, uint16_t& bits)
{
    if (name == "f32")
    {
        tag = 3;
        bits = 32;
    }
    else if (name == "f64")
    {
        tag = 3;
        bits = 64;
    }
    else if (name == "i16")
    {
        tag = 1;
        bits = 16;
    }
    else if (name == "i24")
    {
        tag = 1;
        bits = 24;
    }
    else if (name == "i32")
    {
        tag = 1;
        bits = 32;
    }
    else
        return false;
    return true;
}

// 样本按位原样写出（浮点）或四舍五入到整数（PCM），所以同样的样本得到逐字节相同的文件。
bool writeWav(const std::string& path, uint32_t sampleRate, uint16_t channels, const std::vector<float>& samples,
              uint16_t fmtTag, uint16_t bits, size_t lsbFlipAt, std::string& err)
{
    const uint32_t bytesPerSample = bits / 8u;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bytesPerSample);
    const uint64_t dataBytes64 = static_cast<uint64_t>(samples.size()) * bytesPerSample;
    if (dataBytes64 > std::numeric_limits<uint32_t>::max() - 36u)
    {
        err = "生成的文件超过 WAV 4 GiB 的上限，请减小 --seconds、--sr 或 --channels";
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);

    std::vector<uint8_t> h;
    putTag(h, "RIFF");
    putU32(h, 36u + dataBytes);
    putTag(h, "WAVE");
    putTag(h, "fmt ");
    putU32(h, 16);
    putU16(h, fmtTag);
    putU16(h, channels);
    putU32(h, sampleRate);
    putU32(h, sampleRate * blockAlign);
    putU16(h, blockAlign);
    putU16(h, bits);
    putTag(h, "data");
    putU32(h, dataBytes);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        err = "无法创建文件：" + path + "（检查目录是否存在、有没有写入权限）";
        return false;
    }
    f.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    std::vector<uint8_t> frame;
    for (size_t idx = 0; idx < samples.size(); ++idx)
    {
        const float s = samples[idx];
        frame.clear();
        if (fmtTag == 3 && bits == 32)
        {
            uint32_t u = 0;
            std::memcpy(&u, &s, sizeof(u));
            putU32(frame, u);
        }
        else if (fmtTag == 3)
        {
            const double dv = static_cast<double>(s);
            uint64_t u = 0;
            std::memcpy(&u, &dv, sizeof(u));
            putU32(frame, static_cast<uint32_t>(u & 0xFFFFFFFFu));
            putU32(frame, static_cast<uint32_t>(u >> 32));
        }
        else
        {
            const double scale = static_cast<double>(1ull << (bits - 1));
            const double q = std::max(-scale, std::min(scale - 1.0, std::nearbyint(static_cast<double>(s) * scale)));
            uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(q));
            if (idx == lsbFlipAt)
                u += 1u; // 自测用：整数码差 1，解码成 float 后看不出来
            for (uint32_t i = 0; i < bytesPerSample; ++i)
                frame.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
        }
        f.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    }
    f.flush();
    if (!f)
    {
        err = "写入失败：" + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 参数解析

struct Args
{
    std::map<std::string, std::string> values;
    std::vector<std::string> flags;
};

// 只认 --名字 值 的形式；列在 boolFlags 里的名字不带值
bool parseArgs(int argc, char** argv, int first, const std::vector<std::string>& known,
               const std::vector<std::string>& boolFlags, Args& out, std::string& err)
{
    for (int i = first; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a.rfind("--", 0) != 0)
        {
            err = "不认识的参数：" + a + "（参数都以 -- 开头，用 --help 看用法）";
            return false;
        }
        const std::string name = a.substr(2);
        if (std::find(boolFlags.begin(), boolFlags.end(), name) != boolFlags.end())
        {
            out.flags.push_back(name);
            continue;
        }
        if (std::find(known.begin(), known.end(), name) == known.end())
        {
            err = "不认识的参数：" + a + "（用 --help 看用法）";
            return false;
        }
        if (i + 1 >= argc)
        {
            err = "参数 " + a + " 后面缺少取值";
            return false;
        }
        out.values[name] = argv[++i];
    }
    return true;
}

bool hasFlag(const Args& a, const std::string& name)
{
    return std::find(a.flags.begin(), a.flags.end(), name) != a.flags.end();
}

bool parseNumber(const std::string& s, double& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

// 取一个数值参数；没给就用默认值。取值不合法时写 err 并返回 false。
bool getNumber(const Args& a, const std::string& name, double def, double& out, std::string& err)
{
    const auto it = a.values.find(name);
    if (it == a.values.end())
    {
        out = def;
        return true;
    }
    if (!parseNumber(it->second, out))
    {
        err = "--" + name + " 需要一个数字，收到：" + it->second;
        return false;
    }
    return true;
}

bool getInt(const Args& a, const std::string& name, int64_t def, int64_t lo, int64_t hi, int64_t& out, std::string& err)
{
    double v = 0.0;
    if (!getNumber(a, name, static_cast<double>(def), v, err))
        return false;
    if (v != std::floor(v) || v < static_cast<double>(lo) || v > static_cast<double>(hi))
    {
        err = "--" + name + " 需要 " + std::to_string(lo) + " 到 " + std::to_string(hi) + " 之间的整数";
        return false;
    }
    out = static_cast<int64_t>(v);
    return true;
}

bool getString(const Args& a, const std::string& name, std::string& out, std::string& err)
{
    const auto it = a.values.find(name);
    if (it == a.values.end())
    {
        err = "缺少必需的参数 --" + name;
        return false;
    }
    out = it->second;
    return true;
}

std::string fixed(double v, int digits)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

double toDb(double linear)
{
    return linear <= 0.0 ? -kInf : 20.0 * std::log10(linear);
}

std::string dbText(double db)
{
    if (db == -kInf)
        return "-inf dBFS";
    if (db == kInf)
        return "无法计算（含非数值）";
    return fixed(db, 2) + " dBFS";
}

// ---------------------------------------------------------------- make-signal

// 固定算法的伪随机数（xorshift64*），不用标准库的分布：同种子在任何平台、任何编译器上给出同一串数
class Rng
{
public:
    explicit Rng(uint64_t seed) : state_(seed ^ 0x9E3779B97F4A7C15ull)
    {
        if (state_ == 0)
            state_ = 1;
    }

    // 均匀分布在 [-1, 1)
    double next()
    {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        const uint64_t r = state_ * 2685821657736338717ull;
        return static_cast<double>(r >> 11) / 4503599627370496.0 - 1.0; // 2^52
    }

private:
    uint64_t state_;
};

// 粉噪突发：Paul Kellet 的 7 极点近似，淡入淡出各 2 ms，峰值归一到 peak
std::vector<double> pinkBurst(size_t frames, uint32_t sampleRate, uint64_t seed, double peak)
{
    Rng rng(seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    std::vector<double> v(frames, 0.0);
    for (size_t n = 0; n < frames; ++n)
    {
        const double w = rng.next();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        v[n] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
    }
    const size_t fade = std::min(frames / 2, static_cast<size_t>(sampleRate) / 500);
    for (size_t n = 0; n < fade; ++n)
    {
        const double g =
            0.5 - 0.5 * std::cos(3.14159265358979323846 * (static_cast<double>(n) + 0.5) / static_cast<double>(fade));
        v[n] *= g;
        v[frames - 1 - n] *= g;
    }
    double maxAbs = 0.0;
    for (const double x : v)
        maxAbs = std::max(maxAbs, std::fabs(x));
    if (maxAbs > 0.0)
    {
        for (double& x : v)
            x = x / maxAbs * peak;
    }
    return v;
}

constexpr double kImpulseLevel = 0.9;
constexpr double kBurstPeak = 0.2;

int runMakeSignal(int argc, char** argv)
{
    Args a;
    std::string err;
    if (!parseArgs(argc, argv, 2,
                   {"out", "kind", "format", "sr", "channels", "seconds", "seed", "offset-samples", "d", "count"},
                   {"lsb-flip"}, a, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    std::string outPath;
    if (!getString(a, "out", outPath, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    const std::string kind = a.values.count("kind") ? a.values["kind"] : "impulse-noise";
    const std::string formatName = a.values.count("format") ? a.values["format"] : "f32";
    uint16_t fmtTag = 0, fmtBits = 0;
    if (!parseFormat(formatName, fmtTag, fmtBits))
    {
        std::cerr << "出错：--format 只能是 f32、f64、i16、i24 或 i32，收到：" << formatName << "\n";
        return kExitError;
    }
    int64_t sr = 0, channels = 0, seed = 0, offset = 0, dSamples = 0, count = 0;
    double seconds = 0.0;
    if (!getInt(a, "sr", 48000, 8000, 384000, sr, err) || !getInt(a, "channels", 2, 1, 8, channels, err) ||
        !getNumber(a, "seconds", 4.0, seconds, err) || !getInt(a, "seed", 1, 0, 1000000000, seed, err) ||
        !getInt(a, "offset-samples", 0, 0, 100000000, offset, err) ||
        !getInt(a, "d", 2048, 1, 100000000, dSamples, err) || !getInt(a, "count", 6, 2, 1000, count, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    if (seconds <= 0.0 || seconds > 600.0)
    {
        std::cerr << "出错：--seconds 需要在 0 到 600 之间\n";
        return kExitError;
    }
    if (kind != "impulse" && kind != "impulse-noise" && kind != "impulse-train")
    {
        std::cerr << "出错：--kind 只能是 impulse、impulse-noise 或 impulse-train，收到：" << kind << "\n";
        return kExitError;
    }

    const size_t total = static_cast<size_t>(std::llround(seconds * static_cast<double>(sr)));
    const size_t impulseAt = static_cast<size_t>(sr) / 2; // 0.5 s
    const size_t burstAt = static_cast<size_t>(sr); // 1.0 s
    const size_t burstLen = static_cast<size_t>(sr) / 5; // 200 ms
    const size_t shift = static_cast<size_t>(offset);
    const size_t ch = static_cast<size_t>(channels);

    // 分配之前先查：数据超过 WAV 的 4 GiB 上限就直接报错，不先去申请几个 GB 内存
    const uint64_t dataBytes = static_cast<uint64_t>(total + shift) * ch * (fmtBits / 8u);
    if (dataBytes > std::numeric_limits<uint32_t>::max() - 36u)
    {
        std::cerr
            << "出错：生成的文件会超过 WAV 4 GiB 的上限，请减小 --seconds、--sr、--channels 或 --offset-samples\n";
        return kExitError;
    }
    if (kind == "impulse-train" && impulseAt + static_cast<size_t>(count - 1) * static_cast<size_t>(dSamples) >= total)
    {
        std::cerr << "出错：冲激串放不下：" << count << " 个冲激、间隔 " << dSamples
                  << " 样本，超出了 --seconds 的长度。请加大 --seconds 或减小 --count / --d\n";
        return kExitError;
    }
    // 冲激在 0.5 s，粉噪突发占 1.0 s 到 1.2 s；--seconds 太短会把它们丢掉或截短，也要报错
    if (kind == "impulse" && impulseAt >= total)
    {
        std::cerr << "出错：--seconds 太短，放不下 0.5 s 处的冲激（需要大于 0.5 s）\n";
        return kExitError;
    }
    if (kind == "impulse-noise" && burstAt + burstLen > total)
    {
        std::cerr << "出错：--seconds 太短，放不下 1.0 s 到 1.2 s 的噪声突发（至少 1.2 s）\n";
        return kExitError;
    }
    std::vector<float> samples((total + shift) * ch, 0.0f);

    auto put = [&](size_t frame, size_t c, double v) {
        const size_t f = frame + shift;
        if (f < total + shift)
            samples[f * ch + c] = static_cast<float>(v);
    };

    for (size_t c = 0; c < ch; ++c)
    {
        if (kind == "impulse-train")
        {
            for (int64_t k = 0; k < count; ++k)
                put(impulseAt + static_cast<size_t>(k) * static_cast<size_t>(dSamples), c, kImpulseLevel);
            continue;
        }
        put(impulseAt, c, kImpulseLevel);
        if (kind == "impulse-noise")
        {
            // 每个声道一串独立的噪声，声道互换或串扰才看得出来
            const std::vector<double> burst =
                pinkBurst(burstLen, static_cast<uint32_t>(sr), static_cast<uint64_t>(seed) * 16u + c, kBurstPeak);
            for (size_t n = 0; n < burst.size(); ++n)
                put(burstAt + n, c, burst[n]);
        }
    }

    size_t lsbFlipAt = std::numeric_limits<size_t>::max();
    if (hasFlag(a, "lsb-flip"))
    {
        if (fmtTag != 1 || fmtBits != 32)
        {
            std::cerr << "出错：--lsb-flip 只用于 --format i32\n";
            return kExitError;
        }
        lsbFlipAt = (impulseAt + shift) * ch; // 冲激所在帧第 0 声道的整数码加 1
    }
    if (!writeWav(outPath, static_cast<uint32_t>(sr), static_cast<uint16_t>(channels), samples, fmtTag, fmtBits,
                  lsbFlipAt, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    std::cout << "已生成 " << outPath << "（" << kind << "，" << sr << " Hz，" << channels << " 声道，"
              << fixed(static_cast<double>(total + shift) / static_cast<double>(sr), 3) << " 秒，"
              << (fmtTag == 3 ? "" : "整数 PCM ") << fmtBits << " 位" << (fmtTag == 3 ? "浮点" : "") << "）\n";
    return kExitPass;
}

// ---------------------------------------------------------------- compare

struct Residual
{
    std::vector<double> peak; // 每声道残差峰值（线性）
    std::vector<double> sumSq; // 每声道残差平方和
    size_t count = 0; // 参与统计的帧数
    int64_t firstDiff = -1; // A 的帧下标：第一个残差不为 0 的位置
};

double sampleOrZero(const WavData& w, int64_t frame, size_t ch)
{
    if (frame < 0 || static_cast<size_t>(frame) >= w.frames)
        return 0.0;
    return static_cast<double>(w.at(static_cast<size_t>(frame), ch));
}

// 统计 A 的帧 [from, to) 上 A[i] - B[i + lag] 的残差；超出文件的部分按静音算
Residual measure(const WavData& a, const WavData& b, int64_t lag, int64_t from, int64_t to)
{
    Residual r;
    r.peak.assign(a.channels, 0.0);
    r.sumSq.assign(a.channels, 0.0);
    r.count = to > from ? static_cast<size_t>(to - from) : 0;
    for (int64_t i = from; i < to; ++i)
    {
        for (size_t c = 0; c < a.channels; ++c)
        {
            const double d = sampleOrZero(a, i, c) - sampleOrZero(b, i + lag, c);
            double mag = std::fabs(d);
            if (!std::isfinite(d))
                mag = kInf;
            if (mag > 0.0 && r.firstDiff < 0)
                r.firstDiff = i;
            r.peak[c] = std::max(r.peak[c], mag);
            r.sumSq[c] += mag * mag;
        }
    }
    return r;
}

double maxPeak(const Residual& r)
{
    double m = 0.0;
    for (const double p : r.peak)
        m = std::max(m, p);
    return m;
}

double rmsAll(const Residual& r)
{
    if (r.count == 0)
        return 0.0;
    double s = 0.0;
    for (const double q : r.sumSq)
        s += q;
    return std::sqrt(s / (static_cast<double>(r.count) * static_cast<double>(r.peak.size())));
}

std::string channelName(size_t c, size_t n)
{
    if (n == 2)
        return c == 0 ? "左" : "右";
    return "声道 " + std::to_string(c + 1);
}

void printResidual(const std::string& title, const Residual& r)
{
    std::cout << title << "（" << r.count << " 帧）：峰值 " << dbText(toDb(maxPeak(r))) << "，RMS "
              << dbText(toDb(rmsAll(r))) << "\n";
    if (r.peak.size() > 1)
    {
        for (size_t c = 0; c < r.peak.size(); ++c)
            std::cout << "    " << channelName(c, r.peak.size()) << "：峰值 " << dbText(toDb(r.peak[c])) << "\n";
    }
}

// 在 ±maxLag 内找 B 相对 A 的滞后：B[i + lag] 最像 A[i] 的 lag。
// 取 A 里第一个有声音的位置起一小段做互相关；并列最高时取绝对值最小的 lag，所以周期信号不会报出周期的整数倍。
int64_t findLag(const WavData& a, const WavData& b, int64_t maxLag, bool& foundSignal)
{
    foundSignal = false;
    int64_t start = -1;
    for (size_t i = 0; i < a.frames && start < 0; ++i)
        for (size_t c = 0; c < a.channels; ++c)
            if (std::fabs(a.at(i, c)) > 1e-4f)
            {
                start = static_cast<int64_t>(i);
                break;
            }
    if (start < 0)
        return 0;
    foundSignal = true;
    const int64_t window = std::min<int64_t>(32768, static_cast<int64_t>(a.frames) - start);
    int64_t bestLag = 0;
    double best = -kInf;
    for (int64_t lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (int64_t n = 0; n < window; ++n)
            for (size_t c = 0; c < a.channels; ++c)
                sum += sampleOrZero(a, start + n, c) * sampleOrZero(b, start + n + lag, c);
        const int64_t absLag = lag < 0 ? -lag : lag;
        const int64_t absBest = bestLag < 0 ? -bestLag : bestLag;
        if (sum > best || (sum == best && absLag < absBest))
        {
            best = sum;
            bestLag = lag;
        }
    }
    return bestLag;
}

bool loadWav(const std::string& path, WavData& w)
{
    std::string err;
    if (!readWav(path, w, err))
    {
        std::cerr << "出错：" << err << "\n";
        return false;
    }
    return true;
}

void describe(const char* label, const std::string& path, const WavData& w)
{
    std::cout << label << "：" << path << "（" << w.sampleRate << " Hz，" << w.channels << " 声道，" << w.frames
              << " 帧，" << fixed(static_cast<double>(w.frames) / std::max<uint32_t>(w.sampleRate, 1), 3) << " 秒）\n";
}

int runCompare(int argc, char** argv)
{
    Args a;
    std::string err;
    if (!parseArgs(argc, argv, 2, {"a", "b", "head-seconds", "threshold-db", "max-lag"}, {"allow-lag"}, a, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    std::string pathA, pathB;
    double headSeconds = 0.0, thresholdDb = 0.0;
    int64_t maxLag = 0;
    if (!getString(a, "a", pathA, err) || !getString(a, "b", pathB, err) ||
        !getNumber(a, "head-seconds", 0.0, headSeconds, err) ||
        !getNumber(a, "threshold-db", -120.0, thresholdDb, err) || !getInt(a, "max-lag", 4800, 0, 1000000, maxLag, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    if (headSeconds < 0.0)
    {
        std::cerr << "出错：--head-seconds 不能是负数\n";
        return kExitError;
    }
    WavData wa, wb;
    if (!loadWav(pathA, wa) || !loadWav(pathB, wb))
        return kExitError;

    describe("A", pathA, wa);
    describe("B", pathB, wb);

    std::vector<std::string> reasons;
    if (wa.sampleRate != wb.sampleRate)
        reasons.push_back("两份文件采样率不同（" + std::to_string(wa.sampleRate) + " 与 " +
                          std::to_string(wb.sampleRate) + "）");
    if (wa.channels != wb.channels)
        reasons.push_back("两份文件声道数不同（" + std::to_string(wa.channels) + " 与 " + std::to_string(wb.channels) +
                          "）");
    if (!reasons.empty())
    {
        std::cout << "逐位相同：否\n结果：FAIL\n";
        for (const std::string& r : reasons)
            std::cout << "原因：" << r << "\n";
        return kExitFail;
    }

    // 解码成 float 后相同，且两份格式一样时再看原始字节（32 位整数、64 位浮点解码会丢低位）
    const bool sameFormat = wa.fmtTag == wb.fmtTag && wa.bits == wb.bits;
    const bool identical = wa.frames == wb.frames &&
                           (wa.samples.empty() || std::memcmp(wa.samples.data(), wb.samples.data(),
                                                              wa.samples.size() * sizeof(float)) == 0) &&
                           (!sameFormat || wa.raw == wb.raw);
    std::cout << "逐位相同：" << (identical ? "是" : "否") << "\n";

    int64_t lag = 0;
    bool foundSignal = true;
    if (!identical)
        lag = findLag(wa, wb, maxLag, foundSignal);
    std::cout << "对齐偏移（lag）：" << lag << " 样本（正数表示 B 比 A 晚）";
    if (!foundSignal)
        std::cout << "，A 里没有找到声音，未做对齐";
    std::cout << "\n";

    const int64_t lo = std::min<int64_t>(0, -lag);
    const int64_t hi = std::max<int64_t>(static_cast<int64_t>(wa.frames), static_cast<int64_t>(wb.frames) - lag);
    const Residual whole = measure(wa, wb, lag, lo, hi);
    printResidual("整体残差", whole);
    if (wa.frames != wb.frames)
        std::cout << "提示：两份长度不同，较短的一份在结尾按静音算\n";

    const double peakDb = toDb(maxPeak(whole));
    bool headOk = true;
    if (headSeconds > 0.0)
    {
        const int64_t headFrames = std::min<int64_t>(std::llround(headSeconds * static_cast<double>(wa.sampleRate)),
                                                     static_cast<int64_t>(wa.frames));
        const Residual head = measure(wa, wb, lag, 0, headFrames);
        printResidual("开头 " + fixed(headSeconds, 2) + " 秒残差", head);
        headOk = toDb(maxPeak(head)) <= thresholdDb;
    }
    if (whole.firstDiff >= 0)
        std::cout << "第一个有差异的位置：A 的第 " << whole.firstDiff << " 帧（"
                  << fixed(static_cast<double>(whole.firstDiff) / wa.sampleRate, 4) << " 秒）\n";

    bool pass = true;
    if (!identical)
    {
        if (!(peakDb <= thresholdDb))
        {
            pass = false;
            reasons.push_back("残差峰值 " + dbText(peakDb) + " 高于门限 " + fixed(thresholdDb, 1) + " dBFS");
        }
        if (!headOk)
        {
            pass = false;
            reasons.push_back("开头一段的残差高于门限");
        }
    }
    if (lag != 0 && !hasFlag(a, "allow-lag"))
    {
        pass = false;
        reasons.push_back("两份有 " + std::to_string(lag) + " 样本的错位（如果错位是预期的，加 --allow-lag）");
    }
    std::cout << "结果：" << (pass ? "PASS" : "FAIL");
    if (pass)
        std::cout << (identical ? "（逐位相同）" : "（残差不高于 " + fixed(thresholdDb, 1) + " dBFS）");
    std::cout << "\n";
    for (const std::string& r : reasons)
        std::cout << "原因：" << r << "\n";
    return pass ? kExitPass : kExitFail;
}

// ---------------------------------------------------------------- echo

// --d 的取值：纯数字按样本，后缀 ms 按毫秒，后缀 samples 或 smp 按样本
bool parseDelay(const std::string& text, double sampleRate, int64_t& samples, std::string& err)
{
    std::string num = text;
    bool inMs = false;
    auto endsWith = [&](const std::string& suffix) {
        return num.size() > suffix.size() && num.compare(num.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (endsWith("ms"))
    {
        inMs = true;
        num.resize(num.size() - 2);
    }
    else if (endsWith("samples"))
        num.resize(num.size() - 7);
    else if (endsWith("smp"))
        num.resize(num.size() - 3);
    double v = 0.0;
    if (!parseNumber(num, v) || v <= 0.0)
    {
        err = "--d 需要一个正数，写法如 2048、2048samples 或 42.67ms，收到：" + text;
        return false;
    }
    samples = static_cast<int64_t>(std::llround(inMs ? v * sampleRate / 1000.0 : v));
    if (samples < 1)
    {
        err = "--d 换算成样本数后不足 1";
        return false;
    }
    return true;
}

int runEcho(int argc, char** argv)
{
    Args a;
    std::string err;
    if (!parseArgs(argc, argv, 2, {"in", "d", "sr", "min-peak-db", "tol-samples"}, {}, a, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    std::string path, dText;
    double sr = 0.0, minPeakDb = 0.0;
    int64_t tol = 0;
    if (!getString(a, "in", path, err) || !getString(a, "d", dText, err) || !getNumber(a, "sr", 0.0, sr, err) ||
        !getNumber(a, "min-peak-db", -20.0, minPeakDb, err) || !getInt(a, "tol-samples", 0, 0, 100000, tol, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    if (sr <= 0.0)
    {
        std::cerr << "出错：需要 --sr <采样率 Hz>，例如 --sr 48000\n";
        return kExitError;
    }
    int64_t d = 0;
    if (!parseDelay(dText, sr, d, err))
    {
        std::cerr << "出错：" << err << "\n";
        return kExitError;
    }
    WavData w;
    if (!loadWav(path, w))
        return kExitError;
    describe("录音", path, w);
    std::cout << "期望的回声间隔 D：" << d << " 样本（" << fixed(static_cast<double>(d) * 1000.0 / sr, 3) << " ms @ "
              << fixed(sr, 0) << " Hz）\n";
    if (std::fabs(static_cast<double>(w.sampleRate) - sr) > 0.5)
    {
        std::cout << "结果：FAIL\n原因：录音的采样率是 " << w.sampleRate << " Hz，和 --sr " << fixed(sr, 0)
                  << " 对不上\n";
        return kExitFail;
    }

    // 每帧取各声道绝对值的最大者，作为包络
    std::vector<double> env(w.frames, 0.0);
    double top = 0.0;
    for (size_t i = 0; i < w.frames; ++i)
    {
        for (size_t c = 0; c < w.channels; ++c)
            env[i] = std::max(env[i], static_cast<double>(std::fabs(w.at(i, c))));
        top = std::max(top, env[i]);
    }
    if (top <= 0.0)
    {
        std::cout << "结果：FAIL\n原因：录音是全静音，没有找到任何回声\n";
        return kExitFail;
    }
    const double threshold = top * std::pow(10.0, minPeakDb / 20.0);
    const size_t guard = static_cast<size_t>(std::max<int64_t>(1, d / 2 - 1));

    std::vector<size_t> peaks;
    for (size_t i = 0; i < env.size();)
    {
        if (env[i] < threshold)
        {
            ++i;
            continue;
        }
        size_t best = i;
        for (size_t j = i; j < env.size() && j <= i + guard; ++j)
            if (env[j] > env[best])
                best = j;
        peaks.push_back(best);
        i = best + guard + 1;
    }

    std::cout << "找到 " << peaks.size() << " 个回声峰（门限：最大峰值以下 " << fixed(-minPeakDb, 1) << " dB）\n";
    if (peaks.size() < 2)
    {
        std::cout << "结果：FAIL\n原因：回声峰不足 2 个，量不出间隔。检查录音里是否真的有回声，或把 --min-peak-db "
                     "调低（如 -40）\n";
        return kExitFail;
    }

    std::vector<int64_t> gaps;
    for (size_t k = 1; k < peaks.size(); ++k)
        gaps.push_back(static_cast<int64_t>(peaks[k] - peaks[k - 1]));
    std::cout << "相邻间隔（样本）：";
    for (size_t k = 0; k < gaps.size() && k < 12; ++k)
        std::cout << (k ? "，" : "") << gaps[k];
    if (gaps.size() > 12)
        std::cout << " ……";
    std::cout << "\n";

    bool pass = true;
    int64_t worst = 0;
    for (const int64_t g : gaps)
    {
        const int64_t diff = g > d ? g - d : d - g;
        if (diff > tol)
            pass = false;
        worst = std::max(worst, diff);
    }
    std::cout << "最大偏差：" << worst << " 样本（允许 " << tol << "）\n";
    std::cout << "结果：" << (pass ? "PASS" : "FAIL") << "\n";
    if (!pass)
        std::cout << "原因：有间隔和期望的 " << d << " 样本不一致\n";
    return pass ? kExitPass : kExitFail;
}

// ---------------------------------------------------------------- 帮助

void printHelp(std::ostream& os)
{
    os << "rinne_nulltest —— 对拍两份 WAV、量回声间隔的小工具\n"
          "\n"
          "用法：rinne_nulltest <子命令> [参数]\n"
          "\n"
          "make-signal   生成测试信号\n"
          "    rinne_nulltest make-signal --out test-signal.wav\n"
          "    --kind impulse-noise   冲激加 200 ms 粉噪突发（默认）；impulse 只有冲激；impulse-train 是等间隔冲激串\n"
          "    --format f32|f64|i16|i24|i32   输出样本格式，默认 f32（32 位浮点）\n"
          "    --sr 48000  --channels 2  --seconds 4  --seed 1\n"
          "    --d <样本数>  --count <个数>   冲激串的间隔与个数（默认 2048、6）\n"
          "    --lsb-flip                     只用于 --format i32：冲激的整数码加 1（自测用，造出解码成 float "
          "后看不出的差异）\n"
          "    --offset-samples <N>          在最前面加 N 个样本的静音（用来造错位）\n"
          "    同样的参数每次生成的文件逐字节相同。\n"
          "\n"
          "compare       对比两份 WAV\n"
          "    rinne_nulltest compare --a 原始.wav --b 导出.wav [--head-seconds 2]\n"
          "    报告：是否逐位相同、B 相对 A 的错位（lag，单位样本）、残差峰值与 RMS（dBFS）。\n"
          "    --head-seconds <秒>   再单独报告开头这一段的残差\n"
          "    --threshold-db <dB>   残差不高于它才算 PASS，默认 -120\n"
          "    --max-lag <样本>      找错位时的搜索范围，默认 4800\n"
          "    --allow-lag           错位是预期的时候加上，否则有错位就是 FAIL\n"
          "\n"
          "echo          量回声间隔\n"
          "    rinne_nulltest echo --in 录音.wav --d 2048 --sr 48000\n"
          "    在录音里找冲激回声，相邻间隔都等于 D 才是 PASS。录音要用只有冲激的信号（make-signal --kind impulse）。\n"
          "    --d 的写法：2048（样本）、2048samples、42.67ms\n"
          "    --min-peak-db <dB>    比最大峰值低多少以内的峰都算回声，默认 -20\n"
          "    --tol-samples <N>     允许的偏差，默认 0（必须完全相等）\n"
          "\n"
          "结果最后一行附近会写 PASS 或 FAIL。退出码：0 通过，2 未通过，1 是参数或文件有问题。\n";
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8); // 提示是中文，Windows 控制台默认不是 UTF-8
#endif
    if (argc < 2)
    {
        printHelp(std::cout);
        return kExitError;
    }
    const std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help")
    {
        printHelp(std::cout);
        return kExitPass;
    }
    for (int i = 2; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h")
        {
            printHelp(std::cout);
            return kExitPass;
        }
    }
    if (cmd == "make-signal")
        return runMakeSignal(argc, argv);
    if (cmd == "compare")
        return runCompare(argc, argv);
    if (cmd == "echo")
        return runEcho(argc, argv);
    std::cerr << "出错：不认识的子命令 " << cmd << "（可用：make-signal、compare、echo；--help 看用法）\n";
    return kExitError;
}
