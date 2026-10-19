// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/state.json
//
// RNST state 容器 abi 1 的线格式，以及它的两种载荷 PRMS、CFGS 的编解码。宿主把 state 当一串不透明的字节存进
// 工程文件，加载时原样交还；这里规定这串字节长什么样、怎样读回来。
//
// 线格式（整数一律小端，与主机字节序无关；f32 按 IEEE 754 位型原样存）：
//   头 16 B：u32 magic 'RNST' | u32 abi | u32 role（1 Input / 2 Output）| u32 chunkCount（≤ 64）
//   块 × chunkCount：u32 fourcc | u32 sizeBytes（≤ 64 KiB）| payload | 补 0 到 4 字节对齐
//   最后一块之后不能再有字节。补齐字节写 0；读入时不看它的内容，它不承载信息。
//
// 分两层：
//   · 容器层（parseHeader / decodeContainer / encodeContainer）只认头与块的边界，不解读载荷。
//   · 载荷层：PRMS 是参数值列表，CFGS 是非参数设置的键值表。认识的项目解成类型化字段；不认识的项目连原始字节
//     一起留着，编码时原样写回。旧版插件打开新版存的工程再保存，不会抹掉新版多出来的东西。
// 把两层拼成一个角色的整份 state 在 StateDocument.h；abi 判读与迁移在 StateMigration.h。
//
// 字节来自用户的工程文件，是不可信输入：长度与计数都先校验再用于分配或索引，任何分配都不超过输入本身的长度。
// 编解码函数都不抛异常（内存不足也只是返回失败），失败时不碰出参：调用方拿到的要么是完整的结果，要么还是调用前
// 的样子，不会有填了一半的状态。只有 Container::set / remove 与 Params::set 这几个改内容的小工具会在内存不足时
// 抛 std::bad_alloc。这里的函数都会分配内存，只在消息线程上用，不能进音频回调。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rinne::state
{

// fourcc 按小端拼：写盘后前 4 个字节依次就是这四个字符。
constexpr std::uint32_t makeFourCc(char a, char b, char c, char d) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

// ---- 容器 ----
// magic 与 abi 是格式的身份：magic 不符就不是这个格式；abi 高于本构建时整份拒载、原样回写（见 StateMigration.h）。
inline constexpr std::uint32_t kMagic = makeFourCc('R', 'N', 'S', 'T');
inline constexpr std::uint32_t kAbi = 1u;
inline constexpr std::size_t kHeaderBytes = 16u;
// 两个上限让坏字节换不来大块内存：块数与单块长度在读任何块之前就卡住。
inline constexpr std::uint32_t kMaxChunkCount = 64u;
inline constexpr std::uint32_t kMaxChunkBytes = 64u * 1024u;

inline constexpr std::uint32_t kFourCcPrms = makeFourCc('P', 'R', 'M', 'S');
inline constexpr std::uint32_t kFourCcCfgs = makeFourCc('C', 'F', 'G', 'S');

// 这份 state 属于哪一端。role 写在头里：Input 的字节交给 Output（或反过来）时整份拒收，不会把对端的设置套到本端。
enum class Role : std::uint32_t
{
    Input = 1,
    Output = 2,
};

constexpr bool isKnownRole(Role role) noexcept
{
    return role == Role::Input || role == Role::Output;
}

struct Header
{
    std::uint32_t magic = 0;
    std::uint32_t abi = 0;
    std::uint32_t role = 0;
    std::uint32_t chunkCount = 0;
};

// 只读 16 字节头，不判任何字段（abi 的判读顺序有讲究，留给 loadState）。不足 16 字节时返回 false。
bool parseHeader(const std::uint8_t* data, std::size_t size, Header& out) noexcept;

struct Chunk
{
    std::uint32_t fourCc = 0;
    std::vector<std::uint8_t> payload;
};

struct Container
{
    Role role = Role::Input;
    std::vector<Chunk> chunks; // 按出现序

    // 第一个 fourcc 匹配的块；没有则 nullptr。
    const Chunk* find(std::uint32_t fourCc) const noexcept;
    // 替换第一个匹配块的载荷；没有就追加到末尾。
    void set(std::uint32_t fourCc, std::vector<std::uint8_t> payload);
    // 删掉所有 fourcc 匹配的块。
    void remove(std::uint32_t fourCc);
};

// 解出容器，不看 abi（由 loadState 先判）。失败：magic 不符、role 不是期望的那一端、块数或单块长度超上限、
// 块或补齐超出数据、最后一块之后还有字节。
bool decodeContainer(const std::uint8_t* data, std::size_t size, Role role, Container& out) noexcept;

// 头写本构建的 abi，块按 chunks 的顺序写，载荷补 0 到 4 字节对齐。块数或单块长度超上限、role 不认识时失败。
bool encodeContainer(const Container& container, std::vector<std::uint8_t>& out) noexcept;

// ---- PRMS：参数值 ----
// 载荷：u32 count | count × { u8 idLen | id（ASCII）| f32 值 }
// 存参数的明文值（不是 0..1 的归一化值），而且只存 saved 值。本层只搬运：按 id 匹配、夹到合法范围、缺失的补默认、
// 非有限值的处理都归参数层。参数层保存时在解出来的那份列表上改值（set），不认识的 id 就原样留在列表里。
inline constexpr std::uint32_t kMaxParamCount = 256u;
inline constexpr std::size_t kMaxParamIdBytes = 255u; // idLen 是 u8

struct ParamValue
{
    std::string id;
    float value = 0.0f;
};

struct Params
{
    std::vector<ParamValue> entries; // 按出现序；id 两两不同

    // 这个 id 的条目；没有则 nullptr。
    const ParamValue* find(const std::string& id) const noexcept;
    // 改这个 id 的值；没有就追加到末尾。
    void set(const std::string& id, float value);
};

// 参数 ID：1..255 字节的可打印 ASCII，不含空格。
bool isValidParamId(const std::string& id) noexcept;

// 失败：条数超上限、某个 id 不合规、id 重复。
bool encodeParams(const Params& params, std::vector<std::uint8_t>& out) noexcept;
// 失败：同上，另有载荷截断、最后一条之后还有字节。重复的 id 让「按 id 匹配」有歧义，所以整份拒收。
bool decodeParams(const std::uint8_t* data, std::size_t size, Params& out) noexcept;

// ---- CFGS：非参数设置 ----
// 载荷：若干条 { u16 key | u16 len | bytes[len] }，条与条之间不补齐，也没有总条数（由载荷长度决定）。
// 键号是线格式：只能追加，不能改号或复用。Input 认识全部 11 个键；Output 只认 1、2、4、8、9，因为配置的唯一真源
// 在 Input，Output 只存它自己决定分组与外观要用的几项。「引导已看过」「规则页确认过的版本」和诊断日志开关是本机
// 设置，存在全局的界面默认值里，不进工程。
enum class ConfigKey : std::uint16_t
{
    Group = 1,
    GroupOrigin = 2,
    ResetOnPlay = 3,
    TailMode = 4,
    DelayUnit = 5,
    FbUnit = 6,
    GuardOn = 7,
    UiScale = 8,
    UiLang = 9,
    PresetName = 10,
    PresetDirty = 11,
};

// 下面四个枚举的数值都是线格式，只能追加。TailMode 的数值与插件外壳里的同名枚举逐个相同；core 不依赖 JUCE，
// 所以这里另写一份，由外壳接线时用 static_assert 对齐两边。
enum class GroupOrigin : std::uint8_t
{
    Undecided = 0,
    User = 1,
    Smart = 2,
};

enum class TailMode : std::uint8_t
{
    Auto = 0, // VST3 报无限长，AU 报 30 秒
    Infinite = 1,
    ThirtySeconds = 2,
};

enum class DelayUnit : std::uint8_t
{
    Milliseconds = 0,
    Samples = 1,
};

enum class FbUnit : std::uint8_t
{
    Percent = 0,
    Decibel = 1,
};

inline constexpr std::uint8_t kMaxGroup = 8u; // 组号 1..8 在界面上是 A–H，0 是未设
inline constexpr std::uint16_t kMinUiScale = 33u;
inline constexpr std::uint16_t kMaxUiScale = 300u;
inline constexpr std::uint16_t kDefaultUiScale = 100u;
inline constexpr std::uint16_t kMaxUiLangBytes = 8u;
inline constexpr std::uint16_t kMaxPresetNameBytes = 96u;

enum class ConfigType : std::uint8_t
{
    U8, // 定长 1 字节
    U16, // 定长 2 字节，小端
    Ascii, // 变长，不超过上限；内容只能是字母、数字与连字符（语言代码）
    Utf8, // 变长，不超过上限；内容必须是合法 UTF-8，且不含控制字符
};

// 已知键的值不合法时怎么处理。长度不对不在此列：那一律整份失败。
enum class OutOfRange : std::uint8_t
{
    UseDefault, // 取默认值
    Clamp, // 夹到 [minValue, maxValue]
};

struct ConfigField
{
    ConfigKey key;
    const char* name; // 与 STATE_SCHEMA.md、state-schema.json 里的键名相同
    ConfigType type;
    std::uint16_t bytes; // U8 / U16 是精确长度；Ascii / Utf8 是长度上限
    std::uint16_t minValue; // 数值键的合法范围（含两端）；字符串键填 0
    std::uint16_t maxValue;
    std::uint16_t defaultValue; // 数值键的默认值；字符串键的默认值恒为空串
    OutOfRange outOfRange;
    bool input; // Input 认识这个键
    bool output; // Output 认识这个键
};

// CFGS 的唯一真源：解码规则、编码规则、默认值与 golden 里的 schema 都由这张表推出来。
inline constexpr std::array<ConfigField, 11> kConfigFields{{
    {ConfigKey::Group, "group", ConfigType::U8, 1, 0, kMaxGroup, 0, OutOfRange::UseDefault, true, true},
    {ConfigKey::GroupOrigin, "group_origin", ConfigType::U8, 1, 0, 2, 0, OutOfRange::UseDefault, true, true},
    {ConfigKey::ResetOnPlay, "reset_on_play", ConfigType::U8, 1, 0, 1, 1, OutOfRange::UseDefault, true, false},
    {ConfigKey::TailMode, "tail_mode", ConfigType::U8, 1, 0, 2, 0, OutOfRange::UseDefault, true, true},
    {ConfigKey::DelayUnit, "delay_unit", ConfigType::U8, 1, 0, 1, 0, OutOfRange::UseDefault, true, false},
    {ConfigKey::FbUnit, "fb_unit", ConfigType::U8, 1, 0, 1, 0, OutOfRange::UseDefault, true, false},
    {ConfigKey::GuardOn, "guard_on", ConfigType::U8, 1, 0, 1, 1, OutOfRange::UseDefault, true, false},
    {ConfigKey::UiScale, "ui_scale", ConfigType::U16, 2, kMinUiScale, kMaxUiScale, kDefaultUiScale, OutOfRange::Clamp,
     true, true},
    {ConfigKey::UiLang, "ui_lang", ConfigType::Ascii, kMaxUiLangBytes, 0, 0, 0, OutOfRange::UseDefault, true, true},
    {ConfigKey::PresetName, "preset_name", ConfigType::Utf8, kMaxPresetNameBytes, 0, 0, 0, OutOfRange::UseDefault, true,
     false},
    {ConfigKey::PresetDirty, "preset_dirty", ConfigType::U8, 1, 0, 1, 0, OutOfRange::UseDefault, true, false},
}};

// 这一端认识的键；不认识（含只有对端才认识的键）返回 nullptr。
const ConfigField* findConfigField(Role role, std::uint16_t key) noexcept;
// 不分角色，键号在不在表里。
const ConfigField* findConfigField(std::uint16_t key) noexcept;

struct ConfigEntry
{
    std::uint16_t key = 0;
    std::vector<std::uint8_t> bytes;
};

// 一端的全部 CFGS 设置。字段初值就是默认值，与键表的 defaultValue 一致（单测钉住）。
// Output 只用 group、groupOrigin、tailMode、uiScale、uiLang；其余字段在 Output 上不编码，恒为默认。
struct Configs
{
    std::uint8_t group = 0;
    GroupOrigin groupOrigin = GroupOrigin::Undecided;
    bool resetOnPlay = true;
    TailMode tailMode = TailMode::Auto;
    DelayUnit delayUnit = DelayUnit::Milliseconds;
    FbUnit fbUnit = FbUnit::Percent;
    bool guardOn = true;
    std::uint16_t uiScale = kDefaultUiScale; // 百分比
    std::string uiLang; // 空串 = 未设
    std::string presetName;
    bool presetDirty = false;
    std::vector<ConfigEntry> unknown; // 这一端不认识的键（含对端专有的键），按出现序原样回写
};

// 缺失的已知键取默认值。
// 整份失败（不碰 out）：条目截断；已知键长度不对或重复出现；不认识的条目多到写回时容不下这一端的全部已知键。
// 不失败、只纠正：已知键的值不合法时按键表处理（枚举、开关、组号取默认，ui_scale 夹到 33..300，字符串内容
// 不合规取空串）。
bool decodeConfigs(const std::uint8_t* data, std::size_t size, Role role, Configs& out) noexcept;

// 先按键号升序写这一端认识的全部键，再按原序写 unknown。字段值按解码的同一套规则纠正后再写；预置名超长时在
// UTF-8 字符边界上截到上限，语言代码不合规或超长时写空串。
// 失败：role 不认识、unknown 里混进这一端的已知键、某条 unknown 放不进 u16 长度、整份超过单块上限。
bool encodeConfigs(const Configs& configs, Role role, std::vector<std::uint8_t>& out) noexcept;

} // namespace rinne::state
