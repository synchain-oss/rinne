// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresetStore.h"

#include <RinnePresetData.h> // juce_add_binary_data 生成：嵌入的 8 个工厂预设

#include <algorithm>
#include <cmath>
#include <utility>

namespace rinne::input
{

namespace
{

// ---- 8 个声音参数的范围，展开自参数表 ----

enum class Kind
{
    F, // 连续
    B, // 开关
    C, // 档位
};

struct Row
{
    const char* id;
    Kind kind;
    double lo;
    double hi;
    double def;
};

constexpr Row kRows[] = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) {#id, Kind::kind, lo, hi, def},
#include "ParamTable.h"
};

constexpr bool sameText(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

// 预设不含的三个参数：环路开关、限制器、上限（它们随工程保存，载入预设时原样不动）
constexpr bool isSoundParam(const char* id)
{
    return !sameText(id, "loop_on") && !sameText(id, "limiter") && !sameText(id, "ceiling");
}

constexpr std::size_t countSoundParams()
{
    std::size_t n = 0;
    for (const Row& row : kRows)
    {
        if (isSoundParam(row.id))
            ++n;
    }
    return n;
}

static_assert(countSoundParams() == kPresetParamCount, "预设的声音参数个数要和参数表对得上");

constexpr std::array<Row, kPresetParamCount> pickSoundRows()
{
    std::array<Row, kPresetParamCount> out{};
    std::size_t n = 0;
    for (const Row& row : kRows)
    {
        if (isSoundParam(row.id))
        {
            out[n] = row;
            ++n;
        }
    }
    return out;
}

constexpr std::array<Row, kPresetParamCount> kSound = pickSoundRows();

constexpr const char* kUserExtension = ".rinnepreset";
constexpr const char* kTempSuffix = ".tmp";
// 用户预设文件的体量上限：8 个数加一个名字，远不到这个数；更大的当坏文件，不读
constexpr juce::int64 kMaxUserFileBytes = 16 * 1024;

// ---- 名字 ----

bool isRemovedControl(juce::juce_wchar c) noexcept
{
    return c < 0x20 || (c >= 0x7f && c <= 0x9f); // C0、DEL、C1
}

// 首尾要去掉的空白。不用平台的 iswspace（Windows 与 macOS 对 NBSP、全角空格的判断不同），按 Unicode 的空白字符写死
bool isTrimmedSpace(juce::juce_wchar c) noexcept
{
    return c == 0x20 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
           c == 0x202f || c == 0x205f || c == 0x3000;
}

std::vector<juce::juce_wchar> stripAndTrim(const juce::String& raw)
{
    std::vector<juce::juce_wchar> points;
    for (auto p = raw.getCharPointer();;)
    {
        const juce::juce_wchar c = p.getAndAdvance();
        if (c == 0)
            break;
        if (!isRemovedControl(c))
            points.push_back(c);
    }
    std::size_t begin = 0;
    std::size_t end = points.size();
    while (begin < end && isTrimmedSpace(points[begin]))
        ++begin;
    while (end > begin && isTrimmedSpace(points[end - 1]))
        --end;
    return {points.begin() + static_cast<std::ptrdiff_t>(begin), points.begin() + static_cast<std::ptrdiff_t>(end)};
}

juce::String fromPoints(const std::vector<juce::juce_wchar>& points)
{
    if (points.empty())
        return {};
    return juce::String(juce::CharPointer_UTF32(points.data()), points.size());
}

// ---- JSON ----

bool isNumber(const juce::var& v)
{
    return v.isInt() || v.isInt64() || v.isDouble();
}

// 对象的键恰好是 keyAt(0) … keyAt(count - 1)，先后也一致
template<typename KeyAt>
bool keysAre(const juce::DynamicObject& object, std::size_t count, KeyAt keyAt)
{
    const juce::NamedValueSet& properties = object.getProperties();
    if (static_cast<std::size_t>(properties.size()) != count)
        return false;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (properties.getName(static_cast<int>(i)).toString() != keyAt(i))
            return false;
    }
    return true;
}

bool keysAre(const juce::DynamicObject& object, std::initializer_list<const char*> keys)
{
    const char* const* first = keys.begin();
    return keysAre(object, keys.size(), [first](std::size_t i) { return juce::String(first[i]); });
}

// f-<slug>：小写字母与数字，单个连字符分隔
bool isFactoryPresetId(const juce::String& id)
{
    if (!id.startsWith("f-") || id.length() <= 2)
        return false;
    bool previousDash = true; // 「f-」后面第一个字符不能是连字符
    for (auto p = id.getCharPointer() + 2;;)
    {
        const juce::juce_wchar c = p.getAndAdvance();
        if (c == 0)
            return !previousDash;
        if (c == '-')
        {
            if (previousDash)
                return false;
            previousDash = true;
        }
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        {
            previousDash = false;
        }
        else
        {
            return false;
        }
    }
}

bool isCleanName(const juce::String& name)
{
    const juce::String cleaned = fromPoints(stripAndTrim(name));
    return cleaned.isNotEmpty() && cleaned == name;
}

juce::String formatNumber(float value)
{
    juce::String text(static_cast<double>(value), 3);
    if (text.containsChar('.'))
        text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text == "-0" ? juce::String("0") : text;
}

bool parseRoot(const juce::String& json, juce::var& root, juce::String* problem)
{
    const juce::Result parsed = juce::JSON::parse(json, root);
    if (parsed.failed())
    {
        if (problem != nullptr)
            *problem = "not valid JSON: " + parsed.getErrorMessage();
        return false;
    }
    if (root.getDynamicObject() == nullptr)
    {
        if (problem != nullptr)
            *problem = "the top level is not an object";
        return false;
    }
    return true;
}

bool isSchemaOne(const juce::var& schema)
{
    return (schema.isInt() || schema.isInt64()) && static_cast<int>(schema) == 1;
}

} // namespace

// ---- 公开的小函数 ----

const char* presetParamId(std::size_t index) noexcept
{
    return index < kPresetParamCount ? kSound[index].id : nullptr;
}

PresetValues presetDefaults() noexcept
{
    PresetValues out{};
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
        out[i] = static_cast<float>(kSound[i].def);
    return out;
}

float clampPresetValue(std::size_t index, double value) noexcept
{
    if (index >= kPresetParamCount)
        return static_cast<float>(value);
    const Row& row = kSound[index];
    if (std::isnan(value))
        return static_cast<float>(row.def);
    double v = std::clamp(value, row.lo, row.hi);
    if (row.kind != Kind::F)
        v = std::floor(v + 0.5); // 开关与档位是下标
    return static_cast<float>(v);
}

juce::String normalisePresetName(const juce::String& raw)
{
    std::vector<juce::juce_wchar> points = stripAndTrim(raw);
    if (points.size() > static_cast<std::size_t>(kPresetNameMaxCodePoints))
    {
        points.resize(static_cast<std::size_t>(kPresetNameMaxCodePoints));
        while (!points.empty() && isTrimmedSpace(points.back()))
            points.pop_back();
    }
    return fromPoints(points);
}

const char* presetReason(PresetError error) noexcept
{
    switch (error)
    {
    case PresetError::kNone:
        break;
    case PresetError::kExists:
        return "exists";
    case PresetError::kBadName:
        return "bad-name";
    case PresetError::kWriteFailed:
        return "write-failed";
    case PresetError::kNotFound:
        return "not-found";
    case PresetError::kFactory:
        return "factory";
    case PresetError::kBadFile:
        return "bad-file";
    case PresetError::kNotReady:
        return "not-ready";
    }
    return "";
}

bool isUserPresetId(const juce::String& id)
{
    if (id.length() != 14 || !id.startsWith("u-"))
        return false;
    for (auto p = id.getCharPointer() + 2;;)
    {
        const juce::juce_wchar c = p.getAndAdvance();
        if (c == 0)
            return true;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
}

// ---- 解析与序列化 ----

std::optional<FactoryPreset> parseFactoryPreset(const juce::String& json, juce::String* problem)
{
    const auto fail = [problem](const juce::String& why) -> std::optional<FactoryPreset> {
        if (problem != nullptr)
            *problem = why;
        return std::nullopt;
    };

    juce::var root;
    juce::String parseProblem;
    if (!parseRoot(json, root, &parseProblem))
        return fail(parseProblem);
    const juce::DynamicObject& object = *root.getDynamicObject();
    if (!keysAre(object, {"schema", "id", "name", "params"}))
        return fail("top-level keys must be exactly schema, id, name, params in that order");
    if (!isSchemaOne(object.getProperty("schema")))
        return fail("schema must be the integer 1");

    FactoryPreset preset;
    const juce::var id = object.getProperty("id");
    if (!id.isString() || !isFactoryPresetId(id.toString()))
        return fail("id must look like f-<slug>");
    preset.id = id.toString();

    const juce::DynamicObject* names = object.getProperty("name").getDynamicObject();
    if (names == nullptr || !keysAre(*names, {"zh", "en", "fr"}))
        return fail("name must be an object with exactly zh, en, fr");
    juce::String* const targets[] = {&preset.zh, &preset.en, &preset.fr};
    const char* const languages[] = {"zh", "en", "fr"};
    for (std::size_t i = 0; i < 3; ++i)
    {
        const juce::var value = names->getProperty(languages[i]);
        if (!value.isString() || !isCleanName(value.toString()))
            return fail(juce::String("name.") + languages[i] +
                        " must be a non-empty string without control characters");
        *targets[i] = value.toString();
    }

    const juce::DynamicObject* params = object.getProperty("params").getDynamicObject();
    if (params == nullptr ||
        !keysAre(*params, kPresetParamCount, [](std::size_t i) { return juce::String(presetParamId(i)); }))
        return fail("params must hold exactly the 8 sound parameters in parameter-table order");
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        const juce::var value = params->getProperty(presetParamId(i));
        if (!isNumber(value))
            return fail(juce::String(presetParamId(i)) + " must be a number");
        const double v = static_cast<double>(value);
        const Row& row = kSound[i];
        if (!std::isfinite(v) || v < row.lo || v > row.hi)
            return fail(juce::String(presetParamId(i)) + " is outside its range");
        if (row.kind != Kind::F && v != std::floor(v))
            return fail(juce::String(presetParamId(i)) + " must be a whole number");
        preset.values[i] = static_cast<float>(v);
    }
    return preset;
}

std::optional<UserPreset> parseUserPreset(const juce::String& json, const juce::String& expectedId)
{
    juce::var root;
    if (!parseRoot(json, root, nullptr))
        return std::nullopt;
    const juce::DynamicObject& object = *root.getDynamicObject();
    if (!isSchemaOne(object.getProperty("schema")))
        return std::nullopt;

    const juce::var id = object.getProperty("id");
    if (!id.isString() || id.toString() != expectedId || !isUserPresetId(expectedId))
        return std::nullopt;

    // 名字：读入时只去控制字符与首尾空白，超长算坏文件（保存时才截断）
    const juce::var name = object.getProperty("name");
    if (!name.isString())
        return std::nullopt;
    const std::vector<juce::juce_wchar> points = stripAndTrim(name.toString());
    if (points.empty() || points.size() > static_cast<std::size_t>(kPresetNameMaxCodePoints))
        return std::nullopt;

    UserPreset preset;
    preset.id = expectedId;
    preset.name = fromPoints(points);
    preset.values = presetDefaults();

    const juce::var params = object.getProperty("params");
    const juce::DynamicObject* paramObject = params.getDynamicObject();
    if (paramObject == nullptr)
        return std::nullopt;
    // 认识的键逐个读：缺的取默认，类型不对判坏，越界夹回范围；不认识的键忽略
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        if (!paramObject->hasProperty(presetParamId(i)))
            continue;
        const juce::var value = paramObject->getProperty(presetParamId(i));
        if (!isNumber(value))
            return std::nullopt;
        preset.values[i] = clampPresetValue(i, static_cast<double>(value));
    }
    return preset;
}

juce::String serialiseUserPreset(const UserPreset& preset)
{
    juce::String text;
    text << "{\n";
    text << "  \"schema\": 1,\n";
    text << "  \"id\": " << juce::JSON::toString(juce::var(preset.id), true) << ",\n";
    text << "  \"name\": " << juce::JSON::toString(juce::var(preset.name), true) << ",\n";
    text << "  \"params\": {\n";
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        text << "    \"" << presetParamId(i) << "\": " << formatNumber(clampPresetValue(i, preset.values[i]))
             << (i + 1 < kPresetParamCount ? ",\n" : "\n");
    }
    text << "  }\n";
    text << "}\n";
    return text;
}

// ---- PresetStore ----

juce::File PresetStore::defaultUserDirectory()
{
    juce::File base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    // JUCE 在 macOS 上给的是 ~/Library；应用数据放在它下面的 Application Support
    base = base.getChildFile("Application Support");
#endif
    return base.getChildFile("Synchain").getChildFile("Rinne").getChildFile("Presets");
}

PresetStore::PresetStore() : PresetStore(defaultUserDirectory()) {}

PresetStore::PresetStore(const juce::File& userDirectory, Hooks hooks)
    : userDirectory_(userDirectory), hooks_(std::move(hooks))
{
    loadFactoryPresets();
}

void PresetStore::loadFactoryPresets()
{
    // 按原始文件名排序：文件名带两位序号前缀，先后就是界面上的先后，不依赖 BinaryData 里的次序
    std::vector<std::pair<juce::String, juce::String>> files;
    for (int i = 0; i < RinnePresetData::namedResourceListSize; ++i)
    {
        int size = 0;
        const char* data = RinnePresetData::getNamedResource(RinnePresetData::namedResourceList[i], size);
        const juce::String original = RinnePresetData::originalFilenames[i];
        if (data == nullptr || size <= 0)
        {
            if (factoryProblem_.isEmpty())
                factoryProblem_ = original + ": the embedded resource is empty";
            continue;
        }
        files.emplace_back(original, juce::String::fromUTF8(data, size));
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [original, text] : files)
    {
        juce::String problem;
        std::optional<FactoryPreset> preset = parseFactoryPreset(text, &problem);
        if (!preset)
        {
            if (factoryProblem_.isEmpty())
                factoryProblem_ = original + ": " + problem;
            continue;
        }
        const bool duplicate = std::any_of(factory_.begin(), factory_.end(),
                                           [&preset](const FactoryPreset& other) { return other.id == preset->id; });
        if (duplicate)
        {
            if (factoryProblem_.isEmpty())
                factoryProblem_ = original + ": duplicate id " + preset->id;
            continue;
        }
        factory_.push_back(std::move(*preset));
    }
    if (factoryProblem_.isEmpty() && factory_.size() != kFactoryPresetCount)
        factoryProblem_ = "expected " + juce::String(static_cast<int>(kFactoryPresetCount)) +
                          " factory presets, found " + juce::String(static_cast<int>(factory_.size()));
}

juce::File PresetStore::fileFor(const juce::String& id) const
{
    // 调用方已经用 isUserPresetId 验过 id，拼出来的名字不含路径分隔符
    return userDirectory_.getChildFile(id + kUserExtension);
}

bool PresetStore::removeIfExists(const juce::File& file) const
{
    return !file.exists() || file.deleteFile();
}

std::uint64_t PresetStore::nextRandom() const
{
    if (hooks_.randomBits)
        return hooks_.randomBits();
    return static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64());
}

bool PresetStore::canWrite() const
{
    if (!userDirectory_.createDirectory().wasOk())
        return false;
    // 探测文件的名字不带 .rinnepreset，列表看不到它；名字用系统随机数，不动注入的随机源
    const juce::File probe = userDirectory_.getChildFile(
        "write-probe-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()) + kTempSuffix);
    bool written = false;
    {
        juce::FileOutputStream out(probe);
        if (!out.failedToOpen())
        {
            const char byte = 0;
            written = out.write(&byte, 1) && out.getStatus().wasOk();
            out.flush();
            written = written && out.getStatus().wasOk();
        }
    }
    const bool removed = removeIfExists(probe);
    return written && removed;
}

UserPresetList PresetStore::listUser() const
{
    UserPresetList list;
    if (!userDirectory_.isDirectory())
        return list;
    for (const juce::DirectoryEntry& entry : juce::RangedDirectoryIterator(
             userDirectory_, false, juce::String("*") + kUserExtension, juce::File::findFiles))
    {
        const juce::File file = entry.getFile();
        const juce::String id = file.getFileNameWithoutExtension();
        if (!isUserPresetId(id))
            continue; // 不是我们写的文件名：不是预设，不计数
        if (file.getSize() > kMaxUserFileBytes)
        {
            ++list.skipped;
            continue;
        }
        std::optional<UserPreset> preset = parseUserPreset(file.loadFileAsString(), id);
        if (!preset)
        {
            ++list.skipped;
            continue;
        }
        list.presets.push_back(std::move(*preset));
    }
    std::sort(list.presets.begin(), list.presets.end(), [](const UserPreset& a, const UserPreset& b) {
        const int byName = a.name.compareIgnoreCase(b.name);
        return byName != 0 ? byName < 0 : a.id < b.id;
    });
    return list;
}

PresetResult PresetStore::save(const juce::String& name, bool overwrite, const PresetValues& values)
{
    PresetResult result;
    const juce::String clean = normalisePresetName(name);
    if (clean.isEmpty())
    {
        result.error = PresetError::kBadName;
        return result;
    }

    // 重名按去完空白的显示名、不分大小写比较；同名有几个时取列表里最前的那个
    juce::String id;
    const UserPresetList known = listUser();
    for (const UserPreset& existing : known.presets)
    {
        if (existing.name.equalsIgnoreCase(clean))
        {
            id = existing.id;
            break;
        }
    }
    if (id.isNotEmpty() && !overwrite)
    {
        result.error = PresetError::kExists;
        return result;
    }

    if (id.isEmpty())
    {
        // 新 id：与目录里已有的文件名（含解析不过的坏文件）都不重名，几次都撞上就放弃
        for (int attempt = 0; attempt < 16 && id.isEmpty(); ++attempt)
        {
            const juce::String candidate =
                "u-" + juce::String::toHexString(static_cast<juce::int64>(nextRandom() & 0xffffffffffffull))
                           .paddedLeft('0', 12);
            if (!fileFor(candidate).exists())
                id = candidate;
        }
        if (id.isEmpty())
        {
            result.error = PresetError::kWriteFailed;
            return result;
        }
    }

    UserPreset preset;
    preset.id = id;
    preset.name = clean;
    preset.values = values;
    const juce::String text = serialiseUserPreset(preset);

    if (!userDirectory_.createDirectory().wasOk())
    {
        result.error = PresetError::kWriteFailed;
        return result;
    }

    // 先写同目录的临时文件并落盘，再整体替换目标：目标要么是旧的整份，要么是新的整份
    const juce::File target = fileFor(id);
    const juce::File temp = userDirectory_.getChildFile(id + kUserExtension + kTempSuffix);
    bool ok = removeIfExists(temp); // FileOutputStream 接在已有文件后面写，上次崩溃留下的临时文件要先清掉
    if (ok)
    {
        juce::FileOutputStream out(temp);
        ok = !out.failedToOpen() && out.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
        if (ok)
        {
            out.flush();
            ok = out.getStatus().wasOk();
        }
    }
    if (ok)
        ok = hooks_.replaceFile ? hooks_.replaceFile(temp, target) : temp.replaceFileIn(target);
    if (!ok)
    {
        removeIfExists(temp);
        result.error = PresetError::kWriteFailed;
        return result;
    }

    result.id = id;
    result.name = clean;
    return result;
}

PresetResult PresetStore::save(const juce::String& name, bool overwrite, const juce::AudioProcessorValueTreeState& tree)
{
    return save(name, overwrite, readValues(tree));
}

PresetResult PresetStore::remove(const juce::String& id)
{
    PresetResult result;
    if (std::any_of(factory_.begin(), factory_.end(), [&id](const FactoryPreset& f) { return f.id == id; }))
    {
        result.error = PresetError::kFactory;
        return result;
    }
    if (!isUserPresetId(id) || !fileFor(id).existsAsFile())
    {
        result.error = PresetError::kNotFound;
        return result;
    }
    const juce::File file = fileFor(id);
    const std::optional<UserPreset> known = parseUserPreset(file.loadFileAsString(), id);
    if (!file.deleteFile())
    {
        result.error = PresetError::kWriteFailed;
        return result;
    }
    result.id = id;
    if (known)
        result.name = known->name;
    return result;
}

PresetResult PresetStore::load(const juce::String& id, juce::AudioProcessorValueTreeState& tree)
{
    PresetResult result;
    PresetValues values{};
    juce::String name;

    const auto factory =
        std::find_if(factory_.begin(), factory_.end(), [&id](const FactoryPreset& f) { return f.id == id; });
    if (factory != factory_.end())
    {
        values = factory->values;
        name = factory->en;
    }
    else if (isUserPresetId(id) && fileFor(id).existsAsFile())
    {
        const juce::File file = fileFor(id);
        std::optional<UserPreset> user;
        if (file.getSize() <= kMaxUserFileBytes)
            user = parseUserPreset(file.loadFileAsString(), id);
        if (!user)
        {
            result.error = PresetError::kBadFile;
            return result;
        }
        values = user->values;
        name = user->name;
    }
    else
    {
        result.error = PresetError::kNotFound;
        return result;
    }

    // 8 个参数先全部找到，缺一个就什么都不改（接线错误，不能改一半）
    std::array<juce::RangedAudioParameter*, kPresetParamCount> parameters{};
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        parameters[i] = tree.getParameter(presetParamId(i));
        if (parameters[i] == nullptr)
        {
            result.error = PresetError::kNotReady;
            return result;
        }
    }

    // 每个参数一对手势，宿主据此把一次载入记成可撤销的改动。值夹回范围，convertTo0to1 再对齐到步进格
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        juce::RangedAudioParameter& parameter = *parameters[i];
        const float plain = clampPresetValue(i, static_cast<double>(values[i]));
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(parameter.convertTo0to1(plain));
        parameter.endChangeGesture();
    }

    // 参数写完之后才加序号：序号是给引擎的通知（走一遍淡出再淡入），载入失败的路径上面都已经返回，序号不动
    presetSeq_.fetch_add(1, std::memory_order_release);

    result.id = id;
    result.name = name;
    return result;
}

PresetValues PresetStore::readValues(const juce::AudioProcessorValueTreeState& tree)
{
    PresetValues values = presetDefaults();
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        // APVTS 的原子量存的就是明文值，不是 0…1
        if (const std::atomic<float>* raw = tree.getRawParameterValue(presetParamId(i)))
            values[i] = clampPresetValue(i, static_cast<double>(raw->load(std::memory_order_relaxed)));
    }
    return values;
}

} // namespace rinne::input
