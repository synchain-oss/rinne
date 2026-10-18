// SPDX-License-Identifier: GPL-3.0-or-later
//
// golden：tests/golden/state/{input,output}-abi1.bin 是 abi 1 线格式的基线，tests/golden/state-schema.json 是同一
// 格式的机读说明。
//   · 两份 bin 由 StateTestKit.h 的两份示例 state 编码而来：编码结果与 bin 逐字节相同，解码 bin 还原那份示例。
//     以后升 abi 时它们保留下来，作迁移测试的输入。
//   · schema 由这里按代码里的键表与常量生成，与文件逐字节比较：改了键表、上限或取值就必须重新生成，diff 落在
//     冻结面上，评审看得见。
//   · docs/contracts/STATE_SCHEMA.md 是手写的，但其中的 CFGS 键表必须与这里按键表生成的那张逐字相同。
// 重新生成：设环境变量 RINNE_STATE_GOLDEN_REGEN=1 跑 [golden] 用例，再看一眼 diff。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
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

// schema 的全文。键表、上限与 abi 来自代码；枚举的名字写在这里，数值由下面的断言钉住。
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

    const std::string n = "\n";
    return "{" + n + //
           "  \"status\": \"draft\"," + n + //
           "  \"format\": \"RNST\"," + n + //
           "  \"abi\": " + std::to_string(rinne::state::kAbi) + "," + n + //
           "  \"probeAbi\": " + std::to_string(rinne::state::kProbeAbi) + "," + n +
           "  \"byteOrder\": \"little-endian\"," + n + "  \"header\": [" + n +
           "    { \"offset\": 0, \"field\": \"magic\", \"type\": \"u32\", \"value\": \"RNST\" }," + n +
           "    { \"offset\": 4, \"field\": \"abi\", \"type\": \"u32\" }," + n +
           "    { \"offset\": 8, \"field\": \"role\", \"type\": \"u32\", \"values\": { \"1\": \"input\", \"2\": "
           "\"output\" } }," +
           n + "    { \"offset\": 12, \"field\": \"chunkCount\", \"type\": \"u32\", \"max\": " +
           std::to_string(rinne::state::kMaxChunkCount) + " }" + n + "  ]," + n +
           "  \"headerFixedAcrossAbi\": [\"magic\", \"abi\", \"role\"]," + n +
           "  \"chunk\": { \"layout\": [\"u32 fourcc\", \"u32 sizeBytes\", \"payload\", \"zero padding to 4 bytes\"], "
           "\"maxSizeBytes\": " +
           std::to_string(rinne::state::kMaxChunkBytes) + ", \"bytesAfterLastChunk\": \"rejected\" }," + n +
           "  \"chunks\": { \"input\": [\"PRMS\", \"CFGS\"], \"output\": [\"CFGS\"], \"unknown\": \"kept, written back "
           "after the known chunks in their original order\" }," +
           n + "  \"load\": [" + n + "    { \"case\": \"zero bytes\", \"result\": \"empty\" }," + n +
           "    { \"case\": \"short header or wrong magic\", \"result\": \"corrupt\" }," + n +
           "    { \"case\": \"abi above this build, role of this end\", \"result\": \"rejectedNewer\", \"note\": "
           "\"original bytes written back unchanged\" }," +
           n + "    { \"case\": \"abi above this build, other role\", \"result\": \"corrupt\" }," + n +
           "    { \"case\": \"abi equal to probeAbi\", \"result\": \"ignoredProbe\", \"note\": \"only the header is "
           "read\" }," +
           n +
           "    { \"case\": \"abi below this build\", \"result\": \"migrated\", \"note\": \"one step at a time\" }," +
           n + "    { \"case\": \"any other failure\", \"result\": \"corrupt\", \"note\": \"nothing applied\" }" + n +
           "  ]," + n +
           "  \"prms\": { \"layout\": [\"u32 count\", \"count x { u8 idLen, id, f32 value }\"], \"maxCount\": " +
           std::to_string(rinne::state::kMaxParamCount) + ", \"idBytes\": [1, " +
           std::to_string(rinne::state::kMaxParamIdBytes) +
           "], \"idChars\": \"0x21-0x7e\", \"duplicateIds\": \"rejected\", \"value\": \"plain saved value\" }," + n +
           "  \"cfgs\": {" + n + "    \"layout\": [\"repeat to end of payload: u16 key, u16 len, bytes[len]\"]," + n +
           "    \"knownKeyWrongLength\": \"rejected\"," + n + "    \"knownKeyRepeated\": \"rejected\"," + n +
           "    \"missingKnownKey\": \"default\"," + n +
           "    \"unknownKeys\": \"kept, written back after the known keys in their original order\"," + n +
           "    \"keys\": [" + n + keys + "    ]," + n + "    \"values\": {" + n +
           "      \"group\": { \"0\": \"unset\", \"1-8\": \"groups A-H\" }," + n +
           "      \"group_origin\": { \"0\": \"undecided\", \"1\": \"user\", \"2\": \"smart\" }," + n +
           "      \"tail_mode\": { \"0\": \"auto\", \"1\": \"infinite\", \"2\": \"30 s\" }," + n +
           "      \"delay_unit\": { \"0\": \"ms\", \"1\": \"samples\" }," + n +
           "      \"fb_unit\": { \"0\": \"percent\", \"1\": \"dB\" }," + n + "      \"ui_scale\": \"percent\"," + n +
           "      \"ui_lang\": \"letters, digits and '-'\"," + n +
           "      \"preset_name\": \"well-formed UTF-8 without control characters\"" + n + "    }" + n + "  }," + n +
           "  \"notInProject\": [\"guide_seen\", \"rules_ack_version\", \"diag_log\"]" + n + "}" + n;
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
    const std::string path = rinne::test::goldenDir() + "../../docs/contracts/STATE_SCHEMA.md";
    Bytes bytes;
    REQUIRE(rinne::test::readFileBytes(path, bytes));
    std::string doc;
    for (const std::uint8_t byte : bytes)
    {
        if (byte != '\r') // 检出成 CRLF 的工作树也照样比
            doc.push_back(static_cast<char>(byte));
    }
    const std::string table = docKeyTable();
    INFO("文档里应当逐字出现这张表：\n" << table);
    CHECK(doc.find(table) != std::string::npos);
}
