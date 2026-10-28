// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的预设库：8 个工厂预设（经 BinaryData 嵌进二进制）与用户预设（用户目录里一份预设一个文件）。
//
// 预设只含 8 个声音参数：feedback、mix、delay_mode、delay_ms、delay_note、low_cut、high_cut、polarity，
// 按参数表序号排列。
// 不含 loop_on、limiter、ceiling（它们是工程设置，随工程保存、载入预设时原样不动），也不含组号。
// 值一律是明文值（% / ms / Hz，开关 0 或 1，档位写下标），与参数契约、工程 state 同一口径；
// 档位不写文字，文字是界面的事。
//
// 文件格式（工厂预设与用户预设同形，键序固定，每个参数一行）：
//   {"schema":1, "id":"f-<slug>" 或 "u-<12 位小写十六进制>", "name":…, "params":{8 个键}}
//   工厂预设的 name 是 {"zh","en","fr"} 三语对象；用户预设的 name 是一个字符串。
//   工厂预设必须恰好带 8 个键且都在范围内，多一个键（含 loop_on、limiter、ceiling）、少一个键、越界、类型不对都判坏——
//   坏了是构建错误：parseFactoryPreset 把问题说出来，用例要求 8 个全部通过。
//   用户预设读入时宽松：缺的键取默认，多出来的键忽略，越界值夹回范围。
//
// 线程：本类只在消息线程用，不在音频线程做任何事。音频线程只读 presetSeq()（一个无锁原子量）。
//
// 载入（工厂或用户预设）：
//   · 只改 8 个声音参数；每个参数各做一遍 beginChangeGesture → setValueNotifyingHost(convertTo0to1(值)) →
//     endChangeGesture，宿主据此把一次载入记成可撤销的手势；
//   · 值夹回范围（NaN 取默认），预设里不认识的键忽略；
//   · 8 个参数都写完之后 presetSeq 加一。接线的一方（处理器）每块把 presetSeq() 填进 EngineParams，引擎看到序号变了
//     就走「10 ms 降到 0、再从首个有效读样本起 500 ms 淡入」，原因 Preset；
//   · 载入失败（找不到、文件坏、参数树缺参数）什么都不改，序号不变；
//   · 载入不碰安全状态：锁存（TRIPPED）时载入预设不会解除它，这一点由引擎层负责并有场景验证，本类不接触安全状态机。
//
// 接线要求（本类只管预设本身，下面这些由接线的一方做）：
//   · 当前预设的 id 与显示名：load / save 成功后在返回值里交回调用方，由调用方写进工程状态里的 preset_name 等键，
//     并在参数被手动改动后置脏标记；本类不记「当前预设」，也不碰工程状态；
//   · 工厂预设的显示名在返回值里是 en（zh 与 en 相同）；界面要按语言显示时从 factoryPresets() 取三语名；
//   · 桥函数 loadPreset、savePreset、deletePreset 的返回码由 PresetError 对应（presetReason 给出字符串）。
//
// 用户预设：
//   · 目录：<用户应用数据目录>/Synchain/Rinne/Presets（macOS 在 Application Support 下）；
//     文件名 u-<12 位十六进制>.rinnepreset，显示名存在文件里，文件名与显示名无关。
//     目录可由构造参数注入（测试用临时目录）；发布版不读环境变量；
//   · 名字：最多 24 个 Unicode 码点（汉字也算 1 个）；去 C0、DEL、C1 控制字符，去首尾空白，超长截断；
//     去完是空串返回 bad-name；
//     其余可见字符都允许（包括 / : * ?，因为文件名不用显示名）；
//   · 重名：按去完空白的显示名不分大小写比较；重名且没允许覆盖返回 exists，允许覆盖时沿用原 id；
//   · 写：先写同目录的临时文件，写完落盘，再整体替换目标；任何一步失败都返回 write-failed，清掉临时文件，原文件不动；
//   · 读列表：解析失败的文件跳过并计数，不删除；与工厂预设同名的用户预设允许存在，靠 id 区分；
//   · 删除：只删用户预设；工厂预设返回 factory，id 不存在返回 not-found。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace rinne::input
{

// 预设里的声音参数个数与顺序（参数表里除 loop_on、limiter、ceiling 之外的 8 个，按表里的先后）
inline constexpr std::size_t kPresetParamCount = 8;
using PresetValues = std::array<float, kPresetParamCount>;

// 工厂预设的个数（presets/factory 下的文件数）
inline constexpr std::size_t kFactoryPresetCount = 8;

// 用户预设显示名的长度上限（Unicode 码点）。state 里 preset_name 上限 96 字节，24 个码点最坏 4 × 24 = 96 字节
inline constexpr int kPresetNameMaxCodePoints = 24;

// 第 index 个声音参数的 ID；index 越界返回空指针
const char* presetParamId(std::size_t index) noexcept;

// 8 个声音参数在参数表里的默认值
PresetValues presetDefaults() noexcept;

// 把一个值夹进第 index 个声音参数的范围；开关与档位再取整到下标；NaN 取默认值。index 越界原样返回
float clampPresetValue(std::size_t index, double value) noexcept;

// 用户预设显示名的规范化：去 C0、DEL、C1 控制字符，去首尾空白（含 NBSP 与全角空格），截到 24 个码点，截后再去尾部空白。
// 返回空串表示这个名字不合格（bad-name）。
juce::String normalisePresetName(const juce::String& raw);

enum class PresetError
{
    kNone,
    kExists, // 保存：重名且没允许覆盖
    kBadName, // 保存：名字去完控制字符与空白后是空串
    kWriteFailed, // 保存或删除：目录建不出来、文件写不进去或换不进去、文件删不掉
    kNotFound, // 载入或删除：没有这个 id 的预设
    kFactory, // 删除：工厂预设不能删
    kBadFile, // 载入：预设文件在，但解析不过
    kNotReady, // 载入：参数树里缺声音参数（接线错误）
};

// 桥上 reason 字符串：exists、bad-name、write-failed、not-found、factory、bad-file、not-ready；kNone 返回空串
const char* presetReason(PresetError error) noexcept;

struct PresetResult
{
    PresetError error = PresetError::kNone;
    juce::String id; // 成功时：被载入、保存的预设 id
    juce::String name; // 成功时：显示名（工厂预设取 en）

    bool ok() const noexcept { return error == PresetError::kNone; }
};

struct FactoryPreset
{
    juce::String id; // f-<slug>
    juce::String zh;
    juce::String en;
    juce::String fr;
    PresetValues values{};
};

struct UserPreset
{
    juce::String id; // u-<12 位小写十六进制>
    juce::String name;
    PresetValues values{};
};

struct UserPresetList
{
    std::vector<UserPreset> presets; // 按显示名（不分大小写）再按 id 排序
    int skipped = 0; // 名字合规但解析失败而跳过的文件个数
};

// ---- 解析与序列化（纯函数，测试直接调用）----

// 工厂预设：严格校验，通过返回预设；不通过返回空，problem 非空时写入第一个问题
std::optional<FactoryPreset> parseFactoryPreset(const juce::String& json, juce::String* problem = nullptr);

// 用户预设：宽松读入（见文件头）。expectedId 是文件名里的 id，文件里的 id 必须与它相同
std::optional<UserPreset> parseUserPreset(const juce::String& json, const juce::String& expectedId);

// 用户预设的文件文本（键序固定、每个参数一行、末尾换行）。值先夹进范围
juce::String serialiseUserPreset(const UserPreset& preset);

// 用户预设 id 的形状：u- 加 12 位小写十六进制
bool isUserPresetId(const juce::String& id);

class PresetStore
{
public:
    // 测试注入：发布版两个都留空。构造参数注入，不读环境变量
    struct Hooks
    {
        // 生成用户预设 id 的 48 位随机数（取低 48 位）；空 = 系统随机
        std::function<std::uint64_t()> randomBits;
        // 把写好的临时文件换进目标；空 = from.replaceFileIn(to)。返回 false 表示失败
        std::function<bool(const juce::File& from, const juce::File& to)> replaceFile;
    };

    // 用户目录取 defaultUserDirectory()
    PresetStore();
    // 用户目录由调用方给（测试用）；目录不必已存在，第一次保存时建
    explicit PresetStore(const juce::File& userDirectory, Hooks hooks = {});

    // <用户应用数据目录>/Synchain/Rinne/Presets；macOS 在 Application Support 下
    static juce::File defaultUserDirectory();

    const juce::File& userDirectory() const noexcept { return userDirectory_; }

    // 8 个工厂预设，按文件名先后（即界面上的先后）。解析或校验没过的不在其中
    const std::vector<FactoryPreset>& factoryPresets() const noexcept { return factory_; }
    // 8 个都解析并校验通过。坏了是构建错误（用例据此变红）；生产里不会为此崩溃，只是少几个预设
    bool factoryPresetsValid() const noexcept { return factoryProblem_.isEmpty(); }
    // 第一个问题；没有问题是空串
    const juce::String& factoryProblem() const noexcept { return factoryProblem_; }

    // 用户目录现在能写吗：目录不在就建，再建一个探测文件、写一个字节、删掉。任何一步失败返回 false。
    // 会建目录（保存时本来也要建），不会留下探测文件
    bool canWrite() const;

    UserPresetList listUser() const;

    // 保存。values 是要存的 8 个值（取当前参数用 readValues）。成功时 id 是新建的或沿用的
    PresetResult save(const juce::String& name, bool overwrite, const PresetValues& values);
    PresetResult save(const juce::String& name, bool overwrite, const juce::AudioProcessorValueTreeState& tree);

    // 删除用户预设
    PresetResult remove(const juce::String& id);

    // 载入。只改 8 个声音参数，见文件头
    PresetResult load(const juce::String& id, juce::AudioProcessorValueTreeState& tree);

    // 参数树里 8 个声音参数的当前明文值（取不到的参数取默认值）
    static PresetValues readValues(const juce::AudioProcessorValueTreeState& tree);

    // 载入成功的次数（回绕）。音频线程可读：无锁原子量，每块填进 EngineParams::presetSeq
    std::uint32_t presetSeq() const noexcept { return presetSeq_.load(std::memory_order_acquire); }

private:
    juce::File fileFor(const juce::String& id) const;
    bool removeIfExists(const juce::File& file) const;
    std::uint64_t nextRandom() const;
    void loadFactoryPresets();

    juce::File userDirectory_;
    Hooks hooks_;
    std::vector<FactoryPreset> factory_;
    juce::String factoryProblem_;
    std::atomic<std::uint32_t> presetSeq_{0};

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "音频线程读 presetSeq 要求无锁原子量");

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetStore)
};

} // namespace rinne::input
