// SPDX-License-Identifier: GPL-3.0-or-later
//
// rinne_nulltest 自测用的 WAV 夹具生成器（只给测试用，不随包发布）。
// CMake 脚本写不了任意字节，所以读入路径要覆盖的"真实 DAW 导出的头形状"由它手工拼出来：
//   make_fixtures <名字> <输出文件>
// 同一组 8 帧 24 位采样（含负数、满幅与最低位）以不同的容器写出，和 ref-f32 逐值相同。

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using Bytes = std::vector<uint8_t>;

void u16(Bytes& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void u32(Bytes& b, uint32_t v)
{
    u16(b, v & 0xFFFF);
    u16(b, v >> 16);
}

void tag(Bytes& b, const char* t)
{
    b.insert(b.end(), t, t + 4);
}

void append(Bytes& b, const Bytes& more)
{
    b.insert(b.end(), more.begin(), more.end());
}

constexpr int kFrames = 8;
constexpr int32_t kVals[kFrames] = {4194304, -4194304, 1, -1, 8388607, -8388608, 0, 100000};

// 左声道取 kVals[i]，右声道取倒序，两个声道不同
int32_t sampleAt(int frame, int ch)
{
    return ch == 0 ? kVals[frame] : kVals[kFrames - 1 - frame];
}

Bytes pcm24Data()
{
    Bytes d;
    for (int i = 0; i < kFrames; ++i)
        for (int c = 0; c < 2; ++c)
        {
            const uint32_t u = static_cast<uint32_t>(sampleAt(i, c)) & 0xFFFFFFu;
            d.push_back(static_cast<uint8_t>(u & 0xFF));
            d.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
            d.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
        }
    return d;
}

Bytes fmtPlain24()
{
    Bytes f;
    u16(f, 1);
    u16(f, 2);
    u32(f, 48000);
    u32(f, 48000 * 6);
    u16(f, 6);
    u16(f, 24);
    return f;
}

// WAVE_FORMAT_EXTENSIBLE：40 字节，真正的格式码在子格式 GUID 的前两个字节
Bytes fmtExtensible24()
{
    Bytes f = fmtPlain24();
    f[0] = 0xFE;
    f[1] = 0xFF;
    u16(f, 22); // cbSize
    u16(f, 24); // 有效位数
    u32(f, 3); // 声道掩码
    u16(f, 1); // 子格式：PCM
    const uint8_t guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    f.insert(f.end(), guidTail, guidTail + 14);
    return f;
}

void chunk(Bytes& out, const char* t, const Bytes& body, bool pad = true)
{
    tag(out, t);
    u32(out, static_cast<uint32_t>(body.size()));
    append(out, body);
    if (pad && (body.size() & 1u) != 0)
        out.push_back(0);
}

Bytes riff(const Bytes& chunks, const char* form = "RIFF")
{
    Bytes out;
    tag(out, form);
    u32(out, static_cast<uint32_t>(4 + chunks.size()));
    tag(out, "WAVE");
    append(out, chunks);
    return out;
}

Bytes f32File(const std::vector<float>& samples, uint32_t channels)
{
    Bytes fmt;
    u16(fmt, 3);
    u16(fmt, channels);
    u32(fmt, 48000);
    u32(fmt, 48000 * channels * 4);
    u16(fmt, channels * 4);
    u16(fmt, 32);
    Bytes data;
    for (const float s : samples)
    {
        uint32_t u = 0;
        std::memcpy(&u, &s, sizeof(u));
        u32(data, u);
    }
    Bytes c;
    chunk(c, "fmt ", fmt);
    chunk(c, "data", data);
    return riff(c);
}

std::vector<float> noise(size_t frames, float amp)
{
    std::vector<float> v(frames * 2);
    uint32_t x = 12345u;
    for (float& s : v)
    {
        x = x * 1664525u + 1013904223u;
        s = amp * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
    }
    return v;
}

bool build(const std::string& name, Bytes& out)
{
    if (name == "ref-f32")
    {
        std::vector<float> v;
        for (int i = 0; i < kFrames; ++i)
            for (int c = 0; c < 2; ++c)
                v.push_back(static_cast<float>(sampleAt(i, c)) / 8388608.0f);
        out = f32File(v, 2);
    }
    else if (name == "plain24")
    {
        Bytes c;
        chunk(c, "fmt ", fmtPlain24());
        chunk(c, "data", pcm24Data());
        out = riff(c);
    }
    else if (name == "ext24")
    {
        // EXTENSIBLE 的 fmt，后面跟一个 fact 块、一个奇数长度（5 字节，带补字节）的 LIST 块，data 之后还有一个 LIST
        Bytes c;
        chunk(c, "fmt ", fmtExtensible24());
        Bytes fact;
        u32(fact, kFrames);
        chunk(c, "fact", fact);
        chunk(c, "LIST", Bytes{'a', 'b', 'c', 'd', 'e'});
        chunk(c, "data", pcm24Data());
        chunk(c, "LIST", Bytes{'t', 'a', 'i', 'l'});
        out = riff(c);
    }
    else if (name == "data-ffff")
    {
        // 录音软件中途停止：data 长度声明为 0xFFFFFFFF，实际只有 8 帧
        Bytes c;
        chunk(c, "fmt ", fmtPlain24());
        tag(c, "data");
        u32(c, 0xFFFFFFFFu);
        append(c, pcm24Data());
        out = riff(c);
    }
    else if (name == "rf64")
    {
        Bytes c;
        chunk(c, "fmt ", fmtPlain24());
        chunk(c, "data", pcm24Data());
        out = riff(c, "RF64");
    }
    else if (name == "trunc")
    {
        // fmt 块声明 16 字节，文件里只剩 4 字节
        Bytes c;
        tag(c, "fmt ");
        u32(c, 16);
        u32(c, 0);
        out = riff(c);
    }
    else if (name == "empty")
        out = f32File({}, 2);
    else if (name == "silence")
        out = f32File(std::vector<float>(2000, 0.0f), 2);
    else if (name == "weak")
        out = f32File(std::vector<float>(2000, 1e-7f), 2);
    else if (name == "nan")
        out = f32File(std::vector<float>(2000, std::nanf("")), 2);
    else if (name == "noise-a")
        out = f32File(noise(4096, 0.5f), 2);
    else if (name.rfind("noise-b", 0) == 0)
    {
        // noise-b<dB>：在 noise-a 的每个采样上加同一个常数，残差峰值恰好是 10^(dB/20)；dB 写成 m118 表示 -118
        std::string num = name.substr(7);
        if (!num.empty() && num[0] == 'm')
            num[0] = '-';
        const double offset = std::pow(10.0, std::stod(num) / 20.0);
        std::vector<float> v = noise(4096, 0.5f);
        for (float& s : v)
            s = static_cast<float>(static_cast<double>(s) + offset);
        out = f32File(v, 2);
    }
    else
        return false;
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "用法：make_fixtures <名字> <输出文件>" << std::endl;
        return 1;
    }
    Bytes out;
    if (!build(argv[1], out))
    {
        std::cerr << "不认识的夹具：" << argv[1] << std::endl;
        return 1;
    }
    std::ofstream f(argv[2], std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    f.flush();
    if (!f)
    {
        std::cerr << "写入失败：" << argv[2] << std::endl;
        return 1;
    }
    return 0;
}
