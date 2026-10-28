// SPDX-License-Identifier: GPL-3.0-or-later
//
// state 测试共用的工具：手写线格式的字节（与编码器无关的第二个真源）、文档逐字段比较、golden 文件的读写、
// 固定种子的伪随机数，以及 golden 用的两份示例 state。
//
// golden 目录由本文件的位置推出来：编译时拿到的是绝对路径，所以不依赖进程的工作目录，也不必往构建脚本里加宏。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "state/StateCodec.h"
#include "state/StateDocument.h"

namespace rinne::test
{

using Bytes = std::vector<std::uint8_t>;

inline Bytes byteList(std::initializer_list<int> values)
{
    Bytes out;
    for (const int value : values)
        out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

inline Bytes textBytes(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

inline void putU16(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

inline void putU32(Bytes& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

inline void putBytes(Bytes& out, const Bytes& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

// ---- 手写的线格式 ----
struct RawChunk
{
    std::uint32_t fourCc;
    Bytes payload;
};

// 16 字节头 + 块；每块补 0 到 4 字节对齐。chunkCount 默认按实际块数写，给定时照写（用来造声明与实际不符的字节）。
inline Bytes rawContainer(std::uint32_t abi, std::uint32_t role, const std::vector<RawChunk>& chunks,
                          long long chunkCount = -1)
{
    Bytes out;
    putU32(out, rinne::state::kMagic);
    putU32(out, abi);
    putU32(out, role);
    putU32(out, chunkCount < 0 ? static_cast<std::uint32_t>(chunks.size()) : static_cast<std::uint32_t>(chunkCount));
    for (const auto& chunk : chunks)
    {
        putU32(out, chunk.fourCc);
        putU32(out, static_cast<std::uint32_t>(chunk.payload.size()));
        putBytes(out, chunk.payload);
        while (out.size() % 4u != 0u)
            out.push_back(0);
    }
    return out;
}

// CFGS 的一条：u16 key | u16 len | bytes
inline Bytes kv(std::uint32_t key, const Bytes& value)
{
    Bytes out;
    putU16(out, key);
    putU16(out, static_cast<std::uint32_t>(value.size()));
    putBytes(out, value);
    return out;
}

inline Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const auto& part : parts)
        putBytes(out, part);
    return out;
}

// PRMS：u32 count | { u8 idLen | id | f32 }
struct RawParam
{
    std::string id;
    std::uint32_t bits;
};

inline Bytes rawParams(const std::vector<RawParam>& params, long long count = -1)
{
    Bytes out;
    putU32(out, count < 0 ? static_cast<std::uint32_t>(params.size()) : static_cast<std::uint32_t>(count));
    for (const auto& param : params)
    {
        out.push_back(static_cast<std::uint8_t>(param.id.size()));
        putBytes(out, textBytes(param.id));
        putU32(out, param.bits);
    }
    return out;
}

inline std::uint32_t floatBits(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// 255 字节的参数 id：上限用例用它把一条参数顶到最长。序号放在结尾，保证两两不同。
inline std::string longestParamId(std::size_t index)
{
    std::string id(rinne::state::kMaxParamIdBytes, 'a');
    const std::string tail = std::to_string(index);
    id.replace(id.size() - tail.size(), tail.size(), tail);
    return id;
}

// 指定长度的参数 id（1..255 字节）：序号放在结尾，保证同一长度下两两不同。拿它凑出 PRMS 载荷的精确字节数。
inline std::string paramIdOfLength(std::size_t length, std::size_t index)
{
    std::string id(length, 'a');
    const std::string tail = std::to_string(index);
    id.replace(id.size() - tail.size(), tail.size(), tail);
    return id;
}

inline float bitsFloat(std::uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- 逐字段比较（浮点按位比：NaN 载荷与 -0.0 都要分得出来） ----
inline bool sameParams(const rinne::state::Params& left, const rinne::state::Params& right)
{
    if (left.entries.size() != right.entries.size())
        return false;
    for (std::size_t i = 0; i < left.entries.size(); ++i)
    {
        if (left.entries[i].id != right.entries[i].id ||
            floatBits(left.entries[i].value) != floatBits(right.entries[i].value))
            return false;
    }
    return true;
}

inline bool sameConfigs(const rinne::state::Configs& left, const rinne::state::Configs& right)
{
    if (left.group != right.group || left.groupOrigin != right.groupOrigin || left.resetOnPlay != right.resetOnPlay ||
        left.tailMode != right.tailMode || left.delayUnit != right.delayUnit || left.fbUnit != right.fbUnit ||
        left.guardOn != right.guardOn || left.uiScale != right.uiScale || left.uiLang != right.uiLang ||
        left.presetName != right.presetName || left.presetDirty != right.presetDirty ||
        left.unknown.size() != right.unknown.size())
        return false;
    for (std::size_t i = 0; i < left.unknown.size(); ++i)
    {
        if (left.unknown[i].key != right.unknown[i].key || left.unknown[i].bytes != right.unknown[i].bytes)
            return false;
    }
    return true;
}

inline bool sameDocument(const rinne::state::StateDocument& left, const rinne::state::StateDocument& right)
{
    if (left.role != right.role || !sameParams(left.params, right.params) ||
        !sameConfigs(left.configs, right.configs) || left.unknownChunks.size() != right.unknownChunks.size())
        return false;
    for (std::size_t i = 0; i < left.unknownChunks.size(); ++i)
    {
        if (left.unknownChunks[i].fourCc != right.unknownChunks[i].fourCc ||
            left.unknownChunks[i].payload != right.unknownChunks[i].payload)
            return false;
    }
    return true;
}

// 出参的哨兵：每个字段都与默认值不同。失败与忽略的路径不许把它改成别的样子。
inline rinne::state::StateDocument sentinelDocument(rinne::state::Role role)
{
    rinne::state::StateDocument document = rinne::state::makeDefaultDocument(role);
    document.params.entries.push_back({"sentinel", 0.75f});
    document.configs.group = 6;
    document.configs.uiScale = 222;
    document.configs.presetName = "sentinel";
    document.configs.unknown.push_back({900, byteList({0x5e})});
    document.unknownChunks.push_back({rinne::state::makeFourCc('S', 'E', 'N', 'T'), byteList({0x01})});
    return document;
}

// ---- 确定性的伪随机数（splitmix64）：失败时按种子能复现同一串字节 ----
class Rng
{
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept
    {
        state_ += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    std::size_t below(std::size_t n) noexcept { return static_cast<std::size_t>(next() % n); }

private:
    std::uint64_t state_;
};

// ---- golden ----
inline std::string goldenDir()
{
    const std::string file = __FILE__; // tests/core/state/StateTestKit.h 的绝对路径
    const std::size_t slash = file.find_last_of("/\\");
    return file.substr(0, slash == std::string::npos ? 0 : slash) + "/../../golden/";
}

inline bool readFileBytes(const std::string& path, Bytes& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

inline bool writeFileBytes(const std::string& path, const Bytes& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

// 重新生成 golden 的开关：设 RINNE_STATE_GOLDEN_REGEN=1 跑 [golden] 用例。只有测试读环境变量。
inline bool goldenRegenEnabled()
{
#ifdef _MSC_VER
    // MSVC 把 getenv 标成不安全（C4996，告警即错误），用它的替代品
    char* flag = nullptr;
    std::size_t length = 0;
    const bool on = _dupenv_s(&flag, &length, "RINNE_STATE_GOLDEN_REGEN") == 0 && flag != nullptr && flag[0] == '1';
    std::free(flag);
    return on;
#else
    const char* flag = std::getenv("RINNE_STATE_GOLDEN_REGEN");
    return flag != nullptr && flag[0] == '1';
#endif
}

inline std::string hexDump(const Bytes& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i != 0u)
            out.push_back(i % 16u == 0u ? '\n' : ' ');
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0fu]);
    }
    return out;
}

// ---- golden 的两份示例 state ----
// 参数表（src/input/ParamTable.h）的 11 个参数，按表里的序号、取表里的默认值（明文值）。
inline std::vector<rinne::state::ParamValue> paramTableDefaults()
{
    return {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) {#id, static_cast<float>(def)},
#include "../../../src/input/ParamTable.h"
    };
}

// Input 的 PRMS 从参数表生成：11 个参数按序号排、取默认值，再把其中五个改成非默认值（反馈、混合、环路延迟、
// 限制器关、限制器上限），值都能精确表示；这样基线证明参数表的 11 个 ID 都在 PRMS 里，也钉住限制器与上限随工程保存。
// 每个 CFGS 键都取与默认不同的值；另带一个未知参数、一个未知 CFGS 键和一个未知块，钉住「原样保留回写」。
inline rinne::state::StateDocument sampleInputDocument()
{
    using namespace rinne::state;
    StateDocument document = makeDefaultDocument(Role::Input);
    document.params.entries = paramTableDefaults();
    // set 在 ID 不存在时追加到末尾：参数表改了 ID，基线的字节就变了，golden 用例会变红
    document.params.set("feedback", 75.0f);
    document.params.set("mix", 50.0f);
    document.params.set("delay_ms", 90.0f);
    document.params.set("limiter", 0.0f);
    document.params.set("ceiling", -12.0f);
    document.params.entries.push_back({"future_param", 0.5f});
    Configs& configs = document.configs;
    configs.group = 2;
    configs.groupOrigin = GroupOrigin::User;
    configs.resetOnPlay = false;
    configs.tailMode = TailMode::Infinite;
    configs.delayUnit = DelayUnit::Samples;
    configs.fbUnit = FbUnit::Decibel;
    configs.guardOn = false;
    configs.uiScale = 125;
    configs.uiLang = "zh";
    configs.presetName = "回旋配音 1/4D";
    configs.presetDirty = true;
    configs.unknown.push_back({200, byteList({0xaa, 0xbb, 0xcc})});
    document.unknownChunks.push_back({makeFourCc('X', 'T', 'R', 'A'), byteList({1, 2, 3})});
    return document;
}

inline rinne::state::StateDocument sampleOutputDocument()
{
    using namespace rinne::state;
    StateDocument document = makeDefaultDocument(Role::Output);
    Configs& configs = document.configs;
    configs.group = 2;
    configs.groupOrigin = GroupOrigin::Smart;
    configs.tailMode = TailMode::ThirtySeconds;
    configs.uiScale = 150;
    configs.uiLang = "en";
    configs.unknown.push_back({200, byteList({0x5a, 0xa5})});
    return document;
}

} // namespace rinne::test
