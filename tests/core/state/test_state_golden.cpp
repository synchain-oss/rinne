// SPDX-License-Identifier: GPL-3.0-or-later
//
// golden：tests/golden/state/{input,output}-abi1.bin 是 abi 1 线格式的基线，tests/golden/state-schema.json 是同一
// 格式的机读说明。
//   · 两份 bin 由 StateTestKit.h 的两份示例 state 编码而来：编码结果与 bin 逐字节相同，解码 bin 还原那份示例。
//     以后升 abi 时它们保留下来，作迁移测试的输入。
//   · schema 由这里按代码里的键表与常量生成，与文件逐字节比较：改了键表、上限或取值就必须重新生成，diff 落在
//     冻结面上，评审看得见。
//   · docs/contracts/STATE_SCHEMA.md 是手写的，但其中的 CFGS 键表、两份基线的逐字节注解、加载表与固定条款都由
//     这里逐字比对：注解的每一行偏移与字节要和 bin 相同，加载表与 schema 的 load 数组同源，固定条款与 schema 的
//     机读字段由同一组常量生成。删掉或改错其中任何一条，用例就变红。
// 重新生成：设环境变量 RINNE_STATE_GOLDEN_REGEN=1 跑 [golden] 用例，再看一眼 diff。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace
{

using rinne::state::ConfigType;
using rinne::state::StateDocument;
using rinne::test::Bytes;

std::string boolText(bool value)
{
    return value ? "true" : "false";
}

std::string typeText(ConfigType type)
{
    switch (type)
    {
    case ConfigType::U8:
        return "u8";
    case ConfigType::U16:
        return "u16";
    case ConfigType::Ascii:
        return "ascii";
    case ConfigType::Utf8:
        return "utf8";
    }
    return "?";
}

std::string keyLine(const rinne::state::ConfigField& field)
{
    std::string line = "      { \"key\": " + std::to_string(static_cast<int>(field.key)) + ", \"name\": \"" +
                       field.name + "\", \"type\": \"" + typeText(field.type) + "\", ";
    if (field.type == ConfigType::U8 || field.type == ConfigType::U16)
    {
        line += "\"bytes\": " + std::to_string(field.bytes) + ", \"min\": " + std::to_string(field.minValue) +
                ", \"max\": " + std::to_string(field.maxValue) +
                ", \"default\": " + std::to_string(field.defaultValue) + ", \"outOfRange\": \"" +
                (field.outOfRange == rinne::state::OutOfRange::Clamp ? "clamp" : "default") + "\", ";
    }
    else
    {
        line += "\"maxBytes\": " + std::to_string(field.bytes) + ", \"default\": \"\", \"invalid\": \"default\", ";
    }
    return line + "\"input\": " + boolText(field.input) + ", \"output\": " + boolText(field.output) + " }";
}

// schema 是机读文件，只放英文与常量；文档里的中文措辞留在测试里逐字比对，不写进 schema。拼进 JSON 的字符串都过
// 这个转义，将来措辞里出现引号或反斜杠也不会写出非法的 JSON。
std::string jsonText(const std::string& text)
{
    std::string escaped;
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

// 加载表的唯一来源：schema 的 load 数组（case、result、note）与文档里的表（doc）都由这张表生成，逐行对照。
struct LoadRow
{
    const char* caseName;
    const char* result;
    const char* note; // 空串表示不写 note
    const char* doc;
};

const std::array<LoadRow, 8> kLoadRows{{
    {"zero bytes", "empty", "", "| 零字节 | `Empty` | 保持当前（第一次 setState 时即默认），不提示 | 否 |"},
    {"short header or wrong magic", "corrupt", "",
     "| 不足 16 字节，或 magic 不符 | `Corrupt` | 保持当前，提示 state 未能恢复 | 否 |"},
    {"abi above this build, role of this end", "rejectedNewer",
     "original bytes kept by any setState and written back unchanged until a successfully loaded state replaces them",
     "| abi 高于本构建，role 是这一端 | `RejectedNewer` | 保持当前，提示工程由更新的版本保存 | "
     "是，整份；任何一次 setState 都保留，保存时原样写回，直到被成功读入的 state 取代 |"},
    {"abi above this build, other role", "corrupt", "",
     "| abi 高于本构建，role 不是这一端 | `Corrupt` | 保持当前，提示 state 未能恢复 | 否 |"},
    {"abi equal to probeAbi", "ignoredProbe", "only the header is read",
     "| abi 0（探针） | `IgnoredProbe` | 保持当前，不提示 | 否 |"},
    {"abi below this build", "migrated", "one step at a time",
     "| abi 低于本构建（≥ 1） | 逐级迁移后解码，`Migrated` | 迁移后的 state（第一次）；之后的 setState 只取参数 | "
     "否 |"},
    {"abi equal to this build", "ok", "decoded as is",
     "| abi 等于本构建 | 解码，`Ok` | 读入的 state（第一次）；之后的 setState 只取参数 | 否 |"},
    {"any other failure", "corrupt", "nothing applied",
     "| 以上任何一步之后失败 | `Corrupt` | 保持当前，提示 state 未能恢复 | 否 |"},
}};

// 文档里必须逐字出现的固定条款（doc，中文）。数值从代码常量来；schema 的 invariants 数组放 id 与英文 rule，
// 中文措辞只在这里与文档对拍，不进 schema。
struct Invariant
{
    std::string id;
    std::string doc;
    std::string rule;
};

std::vector<Invariant> schemaInvariants()
{
    const std::string headerBytes = std::to_string(rinne::state::kHeaderBytes);
    const std::string maxCount = std::to_string(rinne::state::kMaxParamCount);
    const std::string maxBytes = std::to_string(rinne::state::kMaxParamBytes);
    return {
        {"minimum-bytes", "整份 state 至少 " + headerBytes + " 字节", "a state is at least " + headerBytes + " bytes"},
        {"header-fixed", "前 " + headerBytes + " 字节（magic、abi、role、块数）的位置与含义永不改变",
         "the first " + headerBytes + " bytes (magic, abi, role, chunk count) never move or change meaning"},
        {"role-whole-value", "role 按整字（u32）比较：Input 恒写 1，Output 恒写 2",
         "role is compared as a whole u32: input writes 1, output writes 2"},
        {"framing-fixed", "容器框架跨 abi 不变", "the container framing is fixed across abi"},
        {"migrations-payload-only", "迁移只改载荷", "migrations only change payloads"},
        {"prms-limits", "读入与写出用同一组上限：条数不超过 " + maxCount + "、载荷不超过 " + maxBytes + " 字节",
         "PRMS reads and writes share one limit: at most " + maxCount + " entries and " + maxBytes + " bytes"},
        {"prms-trim", "编码之前调用 `trimToDecodable`，只丢 PRMS 的未知 id，从末尾往前丢，认识的参数一个不丢",
         "trimToDecodable before encoding drops only unknown PRMS ids, from the end, never a known parameter"},
        {"write-back-scope", "由 `decodeDocument` 解出来、没再加东西的文档一定编得出来",
         "a document from decodeDocument with nothing added always encodes"},
        {"first-setstate", "实例一生的第一次 setState 全量应用",
         "the first setState of an instance applies everything"},
        {"later-setstate", "之后的 setState（宿主预设、A/B、撤销）只改参数",
         "later setState calls (host presets, A/B, undo) only change parameters"},
        {"rejected-newer-any-setstate", "任何一次 `setState` 收到 `RejectedNewer`（role 是这一端）都拒载并保留原字节",
         "any setState that gets RejectedNewer for this role keeps the original bytes, regardless of call order"},
        {"write-back-replaced", "被后来一份成功读入的 state（`Ok`、`Migrated`）取代",
         "kept bytes are replaced only by a successfully loaded state (Ok or Migrated)"},
        {"write-back-edits-lost", "保留期间的参数改动不进 state：保存交回的仍是原字节",
         "parameter edits made while bytes are kept do not enter the state and are lost on save"},
    };
}

std::string loadJsonLines()
{
    std::string lines;
    for (std::size_t i = 0; i < kLoadRows.size(); ++i)
    {
        const std::string note = kLoadRows[i].note;
        lines += "    { \"case\": \"" + jsonText(kLoadRows[i].caseName) + "\", \"result\": \"" +
                 jsonText(kLoadRows[i].result) + "\"" +
                 (note.empty() ? std::string() : ", \"note\": \"" + jsonText(note) + "\"") + " }" +
                 (i + 1u < kLoadRows.size() ? ",\n" : "\n");
    }
    return lines;
}

std::string docLoadTable()
{
    std::string table = "| 情形 | 结果 | 实例的 state | 交回原字节 |\n|---|---|---|---|\n";
    for (const auto& row : kLoadRows)
        table += std::string(row.doc) + "\n";
    return table;
}

// schema 的全文。键表、上限与 abi 来自代码；加载表与固定条款由上面两张表生成。
std::string schemaText()
{
    using rinne::state::DelayUnit;
    using rinne::state::FbUnit;
    using rinne::state::GroupOrigin;
    using rinne::state::TailMode;
    static_assert(static_cast<int>(GroupOrigin::Smart) == 2 && static_cast<int>(TailMode::ThirtySeconds) == 2 &&
                      static_cast<int>(DelayUnit::Samples) == 1 && static_cast<int>(FbUnit::Decibel) == 1,
                  "枚举的数值是线格式");

    std::string keys;
    for (std::size_t i = 0; i < rinne::state::kConfigFields.size(); ++i)
        keys += keyLine(rinne::state::kConfigFields[i]) + (i + 1u < rinne::state::kConfigFields.size() ? ",\n" : "\n");

    std::string invariants;
    const auto rules = schemaInvariants();
    for (std::size_t i = 0; i < rules.size(); ++i)
    {
        invariants += "    { \"id\": \"" + jsonText(rules[i].id) + "\", \"rule\": \"" + jsonText(rules[i].rule) +
                      "\" }" + (i + 1u < rules.size() ? ",\n" : "\n");
    }

    const std::string n = "\n";
    std::string text;
    text += "{" + n;
    text += "  \"status\": \"draft\"," + n;
    text += "  \"format\": \"RNST\"," + n;
    text += "  \"abi\": " + std::to_string(rinne::state::kAbi) + "," + n;
    text += "  \"probeAbi\": " + std::to_string(rinne::state::kProbeAbi) + "," + n;
    text += "  \"byteOrder\": \"little-endian\"," + n;
    text += "  \"minBytes\": " + std::to_string(rinne::state::kHeaderBytes) + "," + n;
    text += "  \"header\": [" + n;
    text += "    { \"offset\": 0, \"field\": \"magic\", \"type\": \"u32\", \"value\": \"RNST\" }," + n;
    text += "    { \"offset\": 4, \"field\": \"abi\", \"type\": \"u32\" }," + n;
    text += "    { \"offset\": 8, \"field\": \"role\", \"type\": \"u32\", \"values\": { \"1\": \"input\", \"2\": "
            "\"output\" } }," +
            n;
    text += "    { \"offset\": 12, \"field\": \"chunkCount\", \"type\": \"u32\", \"max\": " +
            std::to_string(rinne::state::kMaxChunkCount) + " }" + n;
    text += "  ]," + n;
    text += "  \"headerFixedBytes\": " + std::to_string(rinne::state::kHeaderBytes) + "," + n;
    text += "  \"headerFixedAcrossAbi\": [\"magic\", \"abi\", \"role\", \"chunkCount\"]," + n;
    text += "  \"roleCompare\": \"whole u32: input writes 1, output writes 2; new values only for new ends; no flag "
            "bits\"," +
            n;
    text += "  \"containerFramingAcrossAbi\": \"fixed: header length, chunk header, padding, chunk count meaning; "
            "migrations change payloads only\"," +
            n;
    text += "  \"chunk\": { \"layout\": [\"u32 fourcc\", \"u32 sizeBytes\", \"payload\", \"zero padding to 4 "
            "bytes\"], \"maxSizeBytes\": " +
            std::to_string(rinne::state::kMaxChunkBytes) + ", \"bytesAfterLastChunk\": \"rejected\" }," + n;
    text += "  \"chunks\": { \"input\": [\"PRMS\", \"CFGS\"], \"output\": [\"CFGS\"], \"unknown\": \"kept, written "
            "back after the known chunks in their original order\" }," +
            n;
    text += "  \"load\": [" + n + loadJsonLines() + "  ]," + n;
    text += "  \"prms\": { \"layout\": [\"u32 count\", \"count x { u8 idLen, id, f32 value }\"], \"maxCount\": " +
            std::to_string(rinne::state::kMaxParamCount) +
            ", \"maxBytes\": " + std::to_string(rinne::state::kMaxParamBytes) +
            ", \"trimDropsOnly\": \"unknown PRMS ids, from the end\"" + ", \"idBytes\": [1, " +
            std::to_string(rinne::state::kMaxParamIdBytes) +
            "], \"idChars\": \"0x21-0x7e\", \"duplicateIds\": \"rejected\", \"value\": \"plain saved value\" }," + n;
    text += "  \"cfgs\": {" + n;
    text += "    \"layout\": [\"repeat to end of payload: u16 key, u16 len, bytes[len]\"]," + n;
    text += "    \"keyTableMax\": " + std::to_string(rinne::state::kMaxConfigFields) + "," + n;
    text += "    \"knownKeyWrongLength\": \"rejected\"," + n;
    text += "    \"knownKeyRepeated\": \"rejected\"," + n;
    text += "    \"missingKnownKey\": \"default\"," + n;
    text += "    \"unknownKeys\": \"kept, written back after the known keys in their original order\"," + n;
    text += "    \"keys\": [" + n + keys + "    ]," + n;
    text += "    \"values\": {" + n;
    text += "      \"group\": { \"0\": \"unset\", \"1-8\": \"groups A-H\" }," + n;
    text += "      \"group_origin\": { \"0\": \"undecided\", \"1\": \"user\", \"2\": \"smart\" }," + n;
    text += "      \"tail_mode\": { \"0\": \"auto\", \"1\": \"infinite\", \"2\": \"30 s\" }," + n;
    text += "      \"delay_unit\": { \"0\": \"ms\", \"1\": \"samples\" }," + n;
    text += "      \"fb_unit\": { \"0\": \"percent\", \"1\": \"dB\" }," + n;
    text += "      \"ui_scale\": \"percent\"," + n;
    text += "      \"ui_lang\": \"letters, digits and '-'\"," + n;
    text += "      \"preset_name\": \"well-formed UTF-8 without control characters\"" + n;
    text += "    }" + n;
    text += "  }," + n;
    text += "  \"invariants\": [" + n + invariants + "  ]," + n;
    text += "  \"notInProject\": [\"guide_seen\", \"rules_ack_version\", \"diag_log\"]" + n;
    text += "}" + n;
    return text;
}

void compareWithFile(const std::string& path, const Bytes& expected)
{
    if (rinne::test::goldenRegenEnabled())
        REQUIRE(rinne::test::writeFileBytes(path, expected));
    Bytes actual;
    INFO("golden 文件：" << path << "（设 RINNE_STATE_GOLDEN_REGEN=1 重跑本用例可重新生成）");
    REQUIRE(rinne::test::readFileBytes(path, actual));
    INFO("代码生成的字节：\n" << rinne::test::hexDump(expected));
    INFO("文件里的字节：\n" << rinne::test::hexDump(actual));
    CHECK(actual == expected);
}

// STATE_SCHEMA.md 里的 CFGS 键表。文档不生成，但这张表必须逐字出现在文档里：键表改了，文档跟着改。
std::string docKeyTable()
{
    std::string table = "| 键 | 名字 | 类型 | 长度（字节） | 范围 | 默认 | 不合法时 | Input | Output |\n"
                        "| --: | --- | --- | --- | --- | --- | --- | :-: | :-: |\n";
    for (const auto& field : rinne::state::kConfigFields)
    {
        const bool numeric = field.type == ConfigType::U8 || field.type == ConfigType::U16;
        const std::string range =
            numeric ? std::to_string(field.minValue) + ".." + std::to_string(field.maxValue) : std::string("—");
        const std::string fallback = field.outOfRange == rinne::state::OutOfRange::Clamp ? "夹到范围内" : "取默认";
        table += "| " + std::to_string(static_cast<int>(field.key)) + " | `" + field.name + "` | " +
                 typeText(field.type) + " | " +
                 (numeric ? std::to_string(field.bytes) : "≤ " + std::to_string(field.bytes)) + " | " + range + " | " +
                 (numeric ? std::to_string(field.defaultValue) : std::string("空串")) + " | " + fallback + " | " +
                 (field.input ? "是" : "—") + " | " + (field.output ? "是" : "—") + " |\n";
    }
    return table;
}

std::string contractDoc()
{
    Bytes bytes;
    REQUIRE(rinne::test::readFileBytes(rinne::test::goldenDir() + "../../docs/contracts/STATE_SCHEMA.md", bytes));
    std::string doc;
    for (const std::uint8_t byte : bytes)
    {
        if (byte != '\r') // 检出成 CRLF 的工作树也照样比
            doc.push_back(static_cast<char>(byte));
    }
    return doc;
}

// 文档里某个标题之后的第一段 ```text 块。
std::string textBlockAfter(const std::string& doc, const std::string& marker)
{
    const std::size_t markerAt = doc.find(marker);
    if (markerAt == std::string::npos)
        return {};
    const std::size_t fenceAt = doc.find("```text", markerAt);
    if (fenceAt == std::string::npos)
        return {};
    const std::size_t bodyAt = doc.find('\n', fenceAt);
    if (bodyAt == std::string::npos)
        return {};
    const std::size_t endAt = doc.find("```", bodyAt + 1u);
    if (endAt == std::string::npos)
        return {};
    return doc.substr(bodyAt + 1u, endAt - bodyAt - 1u);
}

bool isHexDigit(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

struct AnnotationLine
{
    std::size_t offset = 0;
    Bytes bytes;
};

// 逐字节注解：行首 4 位十六进制偏移，两个空格之后是若干两位十六进制的字节，两个空格之后是含义。
// 字节列只认「两位十六进制 + 一个空格」；遇到连续两个空格（含义列）或行尾就停。
std::vector<AnnotationLine> parseAnnotations(const std::string& block)
{
    std::vector<AnnotationLine> lines;
    std::size_t pos = 0;
    while (pos < block.size())
    {
        std::size_t end = block.find('\n', pos);
        if (end == std::string::npos)
            end = block.size();
        const std::string line = block.substr(pos, end - pos);
        pos = end + 1u;

        if (line.size() < 6u)
            continue;
        std::size_t at = 0;
        std::size_t offset = 0;
        while (at < 4u && at < line.size() && isHexDigit(line[at]))
        {
            offset = offset * 16u + static_cast<std::size_t>(hexValue(line[at]));
            ++at;
        }
        if (at != 4u || at + 1u >= line.size() || line[at] != ' ' || line[at + 1u] != ' ')
            continue;
        at += 2u;

        Bytes bytes;
        while (at < line.size())
        {
            if (at + 1u >= line.size() || !isHexDigit(line[at]) || !isHexDigit(line[at + 1u]))
                break;
            bytes.push_back(static_cast<std::uint8_t>(hexValue(line[at]) * 16 + hexValue(line[at + 1u])));
            at += 2u;
            if (at >= line.size())
                break;
            if (line[at] != ' ')
                break;
            if (at + 1u < line.size() && line[at + 1u] == ' ')
                break; // 两个空格：含义列开始
            ++at;
        }
        if (!bytes.empty())
            lines.push_back(AnnotationLine{offset, bytes});
    }
    return lines;
}

void checkBaselineAnnotations(const std::string& doc, const std::string& marker, const std::string& file, bool complete)
{
    INFO("基线：" << file);
    const std::string block = textBlockAfter(doc, marker);
    REQUIRE_FALSE(block.empty());

    Bytes bin;
    REQUIRE(rinne::test::readFileBytes(rinne::test::goldenDir() + "state/" + file, bin));
    CHECK(doc.find("`" + file + "`（" + std::to_string(bin.size()) + " 字节）") != std::string::npos);

    const std::vector<AnnotationLine> lines = parseAnnotations(block);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().offset == 0u);

    std::size_t expected = 0;
    for (const auto& line : lines)
    {
        INFO("偏移 " << line.offset);
        REQUIRE(line.offset + line.bytes.size() <= bin.size());
        CHECK(line.offset >= expected);
        if (complete)
            CHECK(line.offset == expected);
        CHECK(Bytes(bin.begin() + static_cast<std::ptrdiff_t>(line.offset),
                    bin.begin() + static_cast<std::ptrdiff_t>(line.offset + line.bytes.size())) == line.bytes);
        expected = line.offset + line.bytes.size();
    }
    CHECK(expected == bin.size());
}

// 文档里的固定条款与 schema 的机读字段对拍：条款丢了、schema 字段没了都变红。
void checkInvariant(const std::string& doc, const std::string& schema, const std::string& id)
{
    for (const auto& rule : schemaInvariants())
    {
        if (rule.id != id)
            continue;
        INFO("条款：" << id);
        CHECK(doc.find(rule.doc) != std::string::npos);
        CHECK(schema.find("\"id\": \"" + id + "\"") != std::string::npos);
        return;
    }
    FAIL("schema 里没有这条固定条款：" << id);
}

} // namespace

TEST_CASE("golden: 两份 abi 1 基线逐字节锁定", "[state][golden]")
{
    struct Baseline
    {
        const char* file;
        StateDocument document;
        std::uint8_t chunkCount;
    };
    const std::vector<Baseline> baselines = {
        {"input-abi1.bin", rinne::test::sampleInputDocument(), 3},
        {"output-abi1.bin", rinne::test::sampleOutputDocument(), 1},
    };

    for (const auto& baseline : baselines)
    {
        const std::string path = rinne::test::goldenDir() + "state/" + baseline.file;
        Bytes encoded;
        REQUIRE(rinne::state::encodeDocument(baseline.document, encoded));
        compareWithFile(path, encoded);

        Bytes golden;
        REQUIRE(rinne::test::readFileBytes(path, golden));
        // 头单独再钉一次：改坏 magic、abi、role、块数时，报错比「整份不等」清楚
        REQUIRE(golden.size() >= rinne::state::kHeaderBytes);
        CHECK(Bytes(golden.begin(), golden.begin() + 16) ==
              rinne::test::byteList({'R', 'N', 'S', 'T', 1, 0, 0, 0, static_cast<int>(baseline.document.role), 0, 0, 0,
                                     baseline.chunkCount, 0, 0, 0}));

        StateDocument decoded = rinne::test::sentinelDocument(baseline.document.role);
        const auto result = rinne::state::loadState(golden.data(), golden.size(), baseline.document.role, decoded);
        REQUIRE(result.status == rinne::state::StateLoadStatus::Ok);
        CHECK(rinne::test::sameDocument(decoded, baseline.document));
    }
}

TEST_CASE("golden: state-schema.json 由代码生成", "[state][golden]")
{
    const std::string text = schemaText();
    compareWithFile(rinne::test::goldenDir() + "state-schema.json", Bytes(text.begin(), text.end()));
}

TEST_CASE("golden: STATE_SCHEMA.md 的键表与代码一致", "[state][golden]")
{
    const std::string doc = contractDoc();
    const std::string table = docKeyTable();
    INFO("文档里应当逐字出现这张表：\n" << table);
    CHECK(doc.find(table) != std::string::npos);
}

TEST_CASE("golden: STATE_SCHEMA.md 的逐字节注解与基线一致", "[state][golden]")
{
    const std::string doc = contractDoc();
    checkBaselineAnnotations(doc, "`output-abi1.bin`（", "output-abi1.bin", true);
    checkBaselineAnnotations(doc, "`input-abi1.bin`（", "input-abi1.bin", false);
}

TEST_CASE("golden: STATE_SCHEMA.md 的加载表与 schema 的 load 数组一致", "[state][golden]")
{
    const std::string doc = contractDoc();
    const std::string table = docLoadTable();
    INFO("文档里应当逐字出现这张表：\n" << table);
    CHECK(doc.find(table) != std::string::npos);

    // schema 与文档同源：每一行都按 case 与 result 出现在 schema 的 load 数组里；中文措辞不进 schema。
    const std::string schema = schemaText();
    CHECK(schema.find("\"doc\":") == std::string::npos);
    for (const auto& row : kLoadRows)
    {
        const std::string entry =
            "{ \"case\": \"" + std::string(row.caseName) + "\", \"result\": \"" + row.result + "\"";
        INFO("load 行：" << row.caseName);
        CHECK(schema.find(entry) != std::string::npos);
    }
}

TEST_CASE("golden: 契约文档写清 PRMS 读写共用的上限与裁剪规则", "[state][golden]")
{
    const std::string doc = contractDoc();
    const std::string schema = schemaText();
    checkInvariant(doc, schema, "prms-limits");
    checkInvariant(doc, schema, "prms-trim");
    checkInvariant(doc, schema, "write-back-scope");
    CHECK(schema.find("\"maxCount\": " + std::to_string(rinne::state::kMaxParamCount)) != std::string::npos);
    CHECK(schema.find("\"maxBytes\": " + std::to_string(rinne::state::kMaxParamBytes)) != std::string::npos);
}

TEST_CASE("golden: 契约文档写死容器框架跨 abi 不变", "[state][golden]")
{
    const std::string doc = contractDoc();
    const std::string schema = schemaText();
    checkInvariant(doc, schema, "framing-fixed");
    checkInvariant(doc, schema, "migrations-payload-only");
    CHECK(schema.find("\"containerFramingAcrossAbi\":") != std::string::npos);
    CHECK(doc.find("容器框架跨 abi 不变") != std::string::npos);
}

TEST_CASE("golden: 契约文档写死 16 字节头与 role 整字比较", "[state][golden]")
{
    const std::string doc = contractDoc();
    const std::string schema = schemaText();
    checkInvariant(doc, schema, "minimum-bytes");
    checkInvariant(doc, schema, "header-fixed");
    checkInvariant(doc, schema, "role-whole-value");
    CHECK(schema.find("\"minBytes\": " + std::to_string(rinne::state::kHeaderBytes)) != std::string::npos);
    CHECK(schema.find("\"headerFixedBytes\": " + std::to_string(rinne::state::kHeaderBytes)) != std::string::npos);
    CHECK(schema.find("\"headerFixedAcrossAbi\": [\"magic\", \"abi\", \"role\", \"chunkCount\"]") != std::string::npos);
    CHECK(schema.find("\"roleCompare\":") != std::string::npos);
}

TEST_CASE("golden: 契约文档区分应用方式与写回保护", "[state][golden]")
{
    const std::string doc = contractDoc();
    const std::string schema = schemaText();
    checkInvariant(doc, schema, "first-setstate");
    checkInvariant(doc, schema, "later-setstate");
    checkInvariant(doc, schema, "rejected-newer-any-setstate");
    checkInvariant(doc, schema, "write-back-replaced");
    checkInvariant(doc, schema, "write-back-edits-lost");
    CHECK(doc.find("界面必须常驻提示「这份工程由更新版本保存，本版本不会改写它」") != std::string::npos);
    CHECK(doc.find("宿主可能先套默认预设、再给工程") != std::string::npos);
}
