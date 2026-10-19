// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/state.json
//
// RNST 容器层与 PRMS、CFGS 载荷层的实现。每个公开函数都是「内部实现 + noThrow 外壳」：内部实现只在全部校验
// 通过之后才给出参赋值（移动赋值不抛异常），外壳把内存不足变成失败返回。
#include "state/StateCodec.h"

#include <cstring>
#include <utility>

namespace rinne::state
{
namespace
{

// 这里唯一可能出现的异常是分配失败（std::bad_alloc）：当失败处理。出参在内部实现的最后一步才被赋值，所以抛出时
// 出参还是调用前的样子。
template<typename Fn>
bool noThrow(Fn&& fn) noexcept
{
    try
    {
        return fn();
    }
    catch (...)
    {
        return false;
    }
}

// ---- 确定性的小端读写 ----
// 写：逐字节拼小端，与主机字节序无关；不隐式补齐，4 字节对齐由调用方显式要。
class ByteWriter
{
public:
    void u8(std::uint8_t value) { buf_.push_back(value); }

    void u16(std::uint16_t value)
    {
        buf_.push_back(static_cast<std::uint8_t>(value & 0xffu));
        buf_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    }

    void u32(std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }

    void f32(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void bytes(const std::uint8_t* data, std::size_t size)
    {
        if (size != 0u)
            buf_.insert(buf_.end(), data, data + size);
    }

    void bytes(const std::string& text) { bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }

    void align4()
    {
        while (buf_.size() % 4u != 0u)
            buf_.push_back(0);
    }

    std::size_t size() const noexcept { return buf_.size(); }
    std::vector<std::uint8_t> take() noexcept { return std::move(buf_); }

private:
    std::vector<std::uint8_t> buf_;
};

// 读：越界检查只在这一处。任何一步不够读就置失败位，之后的读一律失败、返回 0，绝不越过缓冲区。
class ByteReader
{
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    bool ok() const noexcept { return ok_; }
    std::size_t remaining() const noexcept { return ok_ ? size_ - pos_ : 0u; }

    std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(read(1u)); }
    std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(read(2u)); }
    std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(read(4u)); }

    float f32() noexcept
    {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // 取下一段 count 字节的起点；不够时置失败位并返回 false。count 为 0 时不读任何字节。
    bool take(std::size_t count, const std::uint8_t*& out) noexcept
    {
        if (!ok_ || count > size_ - pos_)
        {
            ok_ = false;
            return false;
        }
        out = data_ + pos_;
        pos_ += count;
        return true;
    }

private:
    std::uint32_t read(std::size_t count) noexcept
    {
        const std::uint8_t* at = nullptr;
        if (!take(count, at))
            return 0u;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < count; ++i)
            value |= static_cast<std::uint32_t>(at[i]) << (8u * i);
        return value;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::size_t paddingFor(std::size_t size) noexcept
{
    return (4u - size % 4u) % 4u;
}

// ---- 文本内容的规则 ----
// 参数 ID 是标识符：可打印 ASCII，不含空格。
bool isParamIdBytes(const std::uint8_t* data, std::size_t size) noexcept
{
    if (size == 0u || size > kMaxParamIdBytes)
        return false;
    for (std::size_t i = 0; i < size; ++i)
    {
        if (data[i] < 0x21u || data[i] > 0x7eu)
            return false;
    }
    return true;
}

// 语言代码：只有字母、数字与连字符（zh、en、fr、zh-CN 这一类）。别的字节说明写它的一方坏了，取默认比照着用安全。
bool isLangCode(const std::uint8_t* data, std::size_t size) noexcept
{
    for (std::size_t i = 0; i < size; ++i)
    {
        const std::uint8_t c = data[i];
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!alnum && c != '-')
            return false;
    }
    return true;
}

// 预置名要上屏：必须是合法 UTF-8（过长编码、代理区码点、超出 U+10FFFF 都算坏），且不含 C0 控制字符与 DEL。
bool isCleanUtf8(const std::uint8_t* data, std::size_t size) noexcept
{
    std::size_t i = 0;
    while (i < size)
    {
        const std::uint8_t first = data[i];
        if (first < 0x80u)
        {
            if (first < 0x20u || first == 0x7fu)
                return false;
            ++i;
            continue;
        }

        std::size_t extra = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t lowest = 0;
        if (first >= 0xc2u && first <= 0xdfu)
        {
            extra = 1;
            codePoint = first & 0x1fu;
            lowest = 0x80u;
        }
        else if (first >= 0xe0u && first <= 0xefu)
        {
            extra = 2;
            codePoint = first & 0x0fu;
            lowest = 0x800u;
        }
        else if (first >= 0xf0u && first <= 0xf4u)
        {
            extra = 3;
            codePoint = first & 0x07u;
            lowest = 0x10000u;
        }
        else
        {
            return false; // 0x80..0xC1 是续字节或必然过长的首字节，0xF5.. 超出 U+10FFFF
        }

        if (extra >= size - i)
            return false;
        for (std::size_t k = 1; k <= extra; ++k)
        {
            const std::uint8_t next = data[i + k];
            if ((next & 0xc0u) != 0x80u)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3fu);
        }
        if (codePoint < lowest || (codePoint >= 0xd800u && codePoint <= 0xdfffu) || codePoint > 0x10ffffu)
            return false;
        i += extra + 1u;
    }
    return true;
}

const std::uint8_t* bytesOf(const std::string& text) noexcept
{
    return reinterpret_cast<const std::uint8_t*>(text.data());
}

// 合法 UTF-8 的前缀，不超过 maxBytes，且不把一个字符切成两半。
std::string utf8Prefix(const std::string& text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return text;
    std::size_t cut = maxBytes;
    while (cut > 0u && (static_cast<std::uint8_t>(text[cut]) & 0xc0u) == 0x80u)
        --cut;
    return text.substr(0, cut);
}

// ---- CFGS 的类型化字段 ----
// 数值键一律按 u16 读写（枚举与开关先转成整数），字符串键直接给出字段。两个 switch 与键表要对得上，逐键往返的
// 单测会发现漏掉的分支。
std::uint16_t numericValue(const Configs& configs, ConfigKey key) noexcept
{
    switch (key)
    {
    case ConfigKey::Group:
        return configs.group;
    case ConfigKey::GroupOrigin:
        return static_cast<std::uint16_t>(configs.groupOrigin);
    case ConfigKey::ResetOnPlay:
        return static_cast<std::uint16_t>(configs.resetOnPlay ? 1u : 0u);
    case ConfigKey::TailMode:
        return static_cast<std::uint16_t>(configs.tailMode);
    case ConfigKey::DelayUnit:
        return static_cast<std::uint16_t>(configs.delayUnit);
    case ConfigKey::FbUnit:
        return static_cast<std::uint16_t>(configs.fbUnit);
    case ConfigKey::GuardOn:
        return static_cast<std::uint16_t>(configs.guardOn ? 1u : 0u);
    case ConfigKey::UiScale:
        return configs.uiScale;
    case ConfigKey::PresetDirty:
        return static_cast<std::uint16_t>(configs.presetDirty ? 1u : 0u);
    case ConfigKey::UiLang:
    case ConfigKey::PresetName:
        break;
    }
    return 0u;
}

// value 已按键表纠正过，落在该键的合法范围里。
void setNumericValue(Configs& configs, ConfigKey key, std::uint16_t value) noexcept
{
    switch (key)
    {
    case ConfigKey::Group:
        configs.group = static_cast<std::uint8_t>(value);
        break;
    case ConfigKey::GroupOrigin:
        configs.groupOrigin = static_cast<GroupOrigin>(value);
        break;
    case ConfigKey::ResetOnPlay:
        configs.resetOnPlay = value != 0u;
        break;
    case ConfigKey::TailMode:
        configs.tailMode = static_cast<TailMode>(value);
        break;
    case ConfigKey::DelayUnit:
        configs.delayUnit = static_cast<DelayUnit>(value);
        break;
    case ConfigKey::FbUnit:
        configs.fbUnit = static_cast<FbUnit>(value);
        break;
    case ConfigKey::GuardOn:
        configs.guardOn = value != 0u;
        break;
    case ConfigKey::UiScale:
        configs.uiScale = value;
        break;
    case ConfigKey::PresetDirty:
        configs.presetDirty = value != 0u;
        break;
    case ConfigKey::UiLang:
    case ConfigKey::PresetName:
        break;
    }
}

// 字符串键只有两个；Configs 是否 const 跟着调用方走。
template<typename ConfigsT>
auto& stringField(ConfigsT& configs, ConfigKey key) noexcept
{
    return key == ConfigKey::UiLang ? configs.uiLang : configs.presetName;
}

// 越界值的纠正：合法范围内原样，否则按键表夹取或取默认。
std::uint16_t normalized(const ConfigField& field, std::uint16_t raw) noexcept
{
    if (raw >= field.minValue && raw <= field.maxValue)
        return raw;
    if (field.outOfRange == OutOfRange::Clamp)
        return raw < field.minValue ? field.minValue : field.maxValue;
    return field.defaultValue;
}

// 字符串的内容规则：不合规的取默认（空串）。长度上限在解码时是整份失败的理由，在这里不管。
bool isCleanText(const ConfigField& field, const std::uint8_t* data, std::size_t size) noexcept
{
    return field.type == ConfigType::Ascii ? isLangCode(data, size) : isCleanUtf8(data, size);
}

bool knownBy(const ConfigField& field, Role role) noexcept
{
    return role == Role::Input ? field.input : field.output;
}

// 这一端全部已知键按最长的值写出来要多少字节：解码时给 unknown 留的预算要扣掉它，写回才放得下。
std::size_t maxKnownConfigBytes(Role role) noexcept
{
    std::size_t total = 0;
    for (const auto& field : kConfigFields)
    {
        if (knownBy(field, role))
            total += 4u + field.bytes;
    }
    return total;
}

// 已知键的一条记录。长度不对返回 false（整份失败）；值不合法按键表纠正。
bool applyKnownConfig(const ConfigField& field, const std::uint8_t* value, std::size_t length, Configs& out)
{
    switch (field.type)
    {
    case ConfigType::U8:
        if (length != 1u)
            return false;
        setNumericValue(out, field.key, normalized(field, value[0]));
        return true;
    case ConfigType::U16:
        if (length != 2u)
            return false;
        setNumericValue(out, field.key, normalized(field, static_cast<std::uint16_t>(value[0] | (value[1] << 8))));
        return true;
    case ConfigType::Ascii:
    case ConfigType::Utf8: {
        if (length > field.bytes)
            return false;
        std::string& text = stringField(out, field.key);
        if (isCleanText(field, value, length))
            text.assign(reinterpret_cast<const char*>(value), length);
        else
            text.clear();
        return true;
    }
    }
    return false;
}

void appendKnownConfig(const ConfigField& field, const Configs& configs, ByteWriter& writer)
{
    writer.u16(static_cast<std::uint16_t>(field.key));
    switch (field.type)
    {
    case ConfigType::U8:
        writer.u16(1u);
        writer.u8(static_cast<std::uint8_t>(normalized(field, numericValue(configs, field.key))));
        return;
    case ConfigType::U16:
        writer.u16(2u);
        writer.u16(normalized(field, numericValue(configs, field.key)));
        return;
    case ConfigType::Ascii:
    case ConfigType::Utf8: {
        const std::string& source = stringField(configs, field.key);
        std::string text = isCleanText(field, bytesOf(source), source.size()) ? source : std::string{};
        // 语言代码截短了就是另一个代码，超长时整个不要；预置名截在字符边界上，名字的开头总比没有好
        if (field.type == ConfigType::Ascii && text.size() > field.bytes)
            text.clear();
        text = utf8Prefix(text, field.bytes);
        writer.u16(static_cast<std::uint16_t>(text.size()));
        writer.bytes(text);
        return;
    }
    }
}

// ---- 各层的内部实现：全部校验通过之后才动出参 ----
bool parseHeaderImpl(const std::uint8_t* data, std::size_t size, Header& out) noexcept
{
    if (data == nullptr && size != 0u)
        return false;
    ByteReader reader(data, size);
    Header header;
    header.magic = reader.u32();
    header.abi = reader.u32();
    header.role = reader.u32();
    header.chunkCount = reader.u32();
    if (!reader.ok())
        return false;
    out = header;
    return true;
}

bool decodeContainerImpl(const std::uint8_t* data, std::size_t size, Role role, Container& out)
{
    Header header;
    if (!parseHeaderImpl(data, size, header) || header.magic != kMagic)
        return false;
    if (!isKnownRole(role) || header.role != static_cast<std::uint32_t>(role))
        return false;
    if (header.chunkCount > kMaxChunkCount)
        return false;

    ByteReader reader(data + kHeaderBytes, size - kHeaderBytes);
    Container result;
    result.role = role;
    result.chunks.reserve(header.chunkCount);
    for (std::uint32_t i = 0; i < header.chunkCount; ++i)
    {
        Chunk chunk;
        chunk.fourCc = reader.u32();
        const std::uint32_t sizeBytes = reader.u32();
        if (sizeBytes > kMaxChunkBytes)
            return false;
        const std::uint8_t* payload = nullptr;
        const std::uint8_t* padding = nullptr;
        if (!reader.take(sizeBytes, payload) || !reader.take(paddingFor(sizeBytes), padding))
            return false;
        chunk.payload.assign(payload, payload + sizeBytes);
        result.chunks.push_back(std::move(chunk));
    }
    // 块数决定了格式在哪里结束：多出来的字节不是这个格式写的
    if (reader.remaining() != 0u)
        return false;

    out = std::move(result);
    return true;
}

bool encodeContainerImpl(const Container& container, std::vector<std::uint8_t>& out)
{
    if (!isKnownRole(container.role) || container.chunks.size() > kMaxChunkCount)
        return false;

    ByteWriter writer;
    writer.u32(kMagic);
    writer.u32(kAbi);
    writer.u32(static_cast<std::uint32_t>(container.role));
    writer.u32(static_cast<std::uint32_t>(container.chunks.size()));
    for (const auto& chunk : container.chunks)
    {
        if (chunk.payload.size() > kMaxChunkBytes)
            return false;
        writer.u32(chunk.fourCc);
        writer.u32(static_cast<std::uint32_t>(chunk.payload.size()));
        writer.bytes(chunk.payload.data(), chunk.payload.size());
        writer.align4();
    }
    out = writer.take();
    return true;
}

bool encodeParamsImpl(const Params& params, std::vector<std::uint8_t>& out)
{
    if (params.entries.size() > kMaxParamCount)
        return false;

    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(params.entries.size()));
    for (std::size_t i = 0; i < params.entries.size(); ++i)
    {
        const ParamValue& entry = params.entries[i];
        if (!isValidParamId(entry.id))
            return false;
        for (std::size_t j = 0; j < i; ++j)
        {
            if (params.entries[j].id == entry.id)
                return false;
        }
        writer.u8(static_cast<std::uint8_t>(entry.id.size()));
        writer.bytes(entry.id);
        writer.f32(entry.value);
    }
    if (writer.size() > kMaxParamBytes)
        return false;
    out = writer.take();
    return true;
}

bool decodeParamsImpl(const std::uint8_t* data, std::size_t size, Params& out)
{
    if (data == nullptr && size != 0u)
        return false;
    if (size > kMaxParamBytes)
        return false;
    ByteReader reader(data, size);
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > kMaxParamCount)
        return false;

    Params result;
    result.entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t idBytes = reader.u8();
        const std::uint8_t* id = nullptr;
        if (!reader.take(idBytes, id) || !isParamIdBytes(id, idBytes))
            return false;
        ParamValue entry;
        entry.id.assign(reinterpret_cast<const char*>(id), idBytes);
        entry.value = reader.f32();
        if (!reader.ok() || result.find(entry.id) != nullptr)
            return false;
        result.entries.push_back(std::move(entry));
    }
    // 条数已经声明完：多出来的字节不是这个格式写的
    if (reader.remaining() != 0u)
        return false;

    out = std::move(result);
    return true;
}

bool decodeConfigsImpl(const std::uint8_t* data, std::size_t size, Role role, Configs& out)
{
    if (!isKnownRole(role) || (data == nullptr && size != 0u))
        return false;

    Configs result; // 字段初值就是默认值：缺失的已知键取默认
    std::uint32_t seen = 0; // 已出现的已知键，按键表下标记位
    std::size_t unknownBytes = 0;
    ByteReader reader(data, size);
    while (reader.remaining() != 0u)
    {
        const std::uint16_t key = reader.u16();
        const std::uint16_t length = reader.u16();
        const std::uint8_t* value = nullptr;
        if (!reader.take(length, value))
            return false;

        const ConfigField* field = findConfigField(role, key);
        if (field == nullptr)
        {
            unknownBytes += 4u + length;
            result.unknown.push_back(ConfigEntry{key, std::vector<std::uint8_t>(value, value + length)});
            continue;
        }
        // 同一个已知键出现两次，以哪一条为准都说不通
        const std::uint32_t bit = 1u << static_cast<std::uint32_t>(field - kConfigFields.data());
        if ((seen & bit) != 0u)
            return false;
        seen |= bit;
        if (!applyKnownConfig(*field, value, length, result))
            return false;
    }
    // 解得开的就要写得回去：写回时已知键一个不少，unknown 原样，合起来不能超过单块上限
    if (unknownBytes + maxKnownConfigBytes(role) > kMaxChunkBytes)
        return false;

    out = std::move(result);
    return true;
}

bool encodeConfigsImpl(const Configs& configs, Role role, std::vector<std::uint8_t>& out)
{
    if (!isKnownRole(role))
        return false;

    ByteWriter writer;
    for (const auto& field : kConfigFields)
    {
        if (knownBy(field, role))
            appendKnownConfig(field, configs, writer);
    }
    // 一条放不进 u16 长度（≥ 65536 字节）的 unknown 加上它自己的 4 字节条目头就超过单块上限，由最后的总长检查
    // 拦住：截断了的长度字段只进了内部缓冲，不会交给调用方。
    static_assert(kMaxChunkBytes < 0x10000u + 4u, "单块上限必须让总长检查兜住单条长度的 u16 上限");
    for (const auto& entry : configs.unknown)
    {
        // 已知键只能由类型化字段写：再从 unknown 写一份，读回来就是重复键
        if (findConfigField(role, entry.key) != nullptr)
            return false;
        writer.u16(entry.key);
        writer.u16(static_cast<std::uint16_t>(entry.bytes.size()));
        writer.bytes(entry.bytes.data(), entry.bytes.size());
    }
    if (writer.size() > kMaxChunkBytes)
        return false;

    out = writer.take();
    return true;
}

} // namespace

// ---- 小工具 ----
const Chunk* Container::find(std::uint32_t fourCc) const noexcept
{
    for (const auto& chunk : chunks)
    {
        if (chunk.fourCc == fourCc)
            return &chunk;
    }
    return nullptr;
}

void Container::set(std::uint32_t fourCc, std::vector<std::uint8_t> payload)
{
    for (auto& chunk : chunks)
    {
        if (chunk.fourCc == fourCc)
        {
            chunk.payload = std::move(payload);
            return;
        }
    }
    chunks.push_back(Chunk{fourCc, std::move(payload)});
}

void Container::remove(std::uint32_t fourCc)
{
    for (auto it = chunks.begin(); it != chunks.end();)
        it = it->fourCc == fourCc ? chunks.erase(it) : it + 1;
}

const ParamValue* Params::find(const std::string& id) const noexcept
{
    for (const auto& entry : entries)
    {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

void Params::set(const std::string& id, float value)
{
    for (auto& entry : entries)
    {
        if (entry.id == id)
        {
            entry.value = value;
            return;
        }
    }
    entries.push_back(ParamValue{id, value});
}

bool isValidParamId(const std::string& id) noexcept
{
    return isParamIdBytes(bytesOf(id), id.size());
}

bool trimToDecodable(Params& params, const std::vector<std::string>& knownIds, std::size_t& dropped) noexcept
{
    dropped = 0;
    std::size_t bytes = 4u; // u32 count
    for (const auto& entry : params.entries)
        bytes += 1u + entry.id.size() + 4u;
    const auto isKnown = [&](const std::string& id) {
        for (const auto& known : knownIds)
        {
            if (known == id)
                return true;
        }
        return false;
    };
    for (std::size_t i = params.entries.size(); i > 0u;)
    {
        if (params.entries.size() <= kMaxParamCount && bytes <= kMaxParamBytes)
            break;
        --i;
        if (isKnown(params.entries[i].id))
            continue;
        bytes -= 1u + params.entries[i].id.size() + 4u;
        params.entries.erase(params.entries.begin() + static_cast<std::ptrdiff_t>(i));
        ++dropped;
    }
    return params.entries.size() <= kMaxParamCount && bytes <= kMaxParamBytes;
}

const ConfigField* findConfigField(Role role, std::uint16_t key) noexcept
{
    const ConfigField* field = findConfigField(key);
    return field != nullptr && isKnownRole(role) && knownBy(*field, role) ? field : nullptr;
}

const ConfigField* findConfigField(std::uint16_t key) noexcept
{
    for (const auto& field : kConfigFields)
    {
        if (static_cast<std::uint16_t>(field.key) == key)
            return &field;
    }
    return nullptr;
}

// ---- 公开的编解码 ----
bool parseHeader(const std::uint8_t* data, std::size_t size, Header& out) noexcept
{
    return parseHeaderImpl(data, size, out);
}

bool decodeContainer(const std::uint8_t* data, std::size_t size, Role role, Container& out) noexcept
{
    return noThrow([&] { return decodeContainerImpl(data, size, role, out); });
}

bool encodeContainer(const Container& container, std::vector<std::uint8_t>& out) noexcept
{
    return noThrow([&] { return encodeContainerImpl(container, out); });
}

bool encodeParams(const Params& params, std::vector<std::uint8_t>& out) noexcept
{
    return noThrow([&] { return encodeParamsImpl(params, out); });
}

bool decodeParams(const std::uint8_t* data, std::size_t size, Params& out) noexcept
{
    return noThrow([&] { return decodeParamsImpl(data, size, out); });
}

bool decodeConfigs(const std::uint8_t* data, std::size_t size, Role role, Configs& out) noexcept
{
    return noThrow([&] { return decodeConfigsImpl(data, size, role, out); });
}

bool encodeConfigs(const Configs& configs, Role role, std::vector<std::uint8_t>& out) noexcept
{
    return noThrow([&] { return encodeConfigsImpl(configs, role, out); });
}

} // namespace rinne::state
