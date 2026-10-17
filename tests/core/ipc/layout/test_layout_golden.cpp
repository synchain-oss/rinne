// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存布局与 golden 的编译后对拍。SegmentLayout.h 的字段表已经展开成 static_assert（偏移、大小不符就编不过），
// 这里再拿同一张表逐行比 tests/golden/ipc-layout.json：golden 改了一个偏移、多了或少了一项，这里就变红。
// 不编译的那一遍是 contract-static-ipc。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "GoldenReader.h"
#include "ipc/SegmentLayout.h"

namespace
{

using namespace rinne::ipc;

struct FieldRow
{
    std::string structName;
    std::string field;
    std::size_t offset;
    std::size_t size;
    std::string type;
    bool permanent;
};

#define RINNE_TEST_FIELD_ROW(S, f, off, bytes, ty, perm) FieldRow{#S, #f, offsetof(S, f), sizeof(S::f), ty, perm},
const std::vector<FieldRow> kFields = {RINNE_IPC_LAYOUT_FIELDS(RINNE_TEST_FIELD_ROW)};
#undef RINNE_TEST_FIELD_ROW

struct StructRow
{
    std::string structName;
    std::size_t size;
    std::size_t align;
};

#define RINNE_TEST_STRUCT_ROW(S, bytes, al) StructRow{#S, sizeof(S), alignof(S)},
const std::vector<StructRow> kStructs = {RINNE_IPC_LAYOUT_STRUCTS(RINNE_TEST_STRUCT_ROW)};
#undef RINNE_TEST_STRUCT_ROW

struct ConstantRow
{
    std::string name;
    long long value;
};

#define RINNE_TEST_CONSTANT_ROW(name) ConstantRow{#name, static_cast<long long>(name)},
const std::vector<ConstantRow> kConstants = {RINNE_IPC_LAYOUT_CONSTANTS(RINNE_TEST_CONSTANT_ROW)};
#undef RINNE_TEST_CONSTANT_ROW

const rinne::test::Golden& golden()
{
    static const rinne::test::Golden g = rinne::test::loadIpcGolden();
    return g;
}

} // namespace

TEST_CASE("ipc golden is readable", "[ipc][layout][golden]")
{
    INFO("golden: " << golden().path);
    REQUIRE(golden().loaded);
    CHECK(golden().top.at("abi") == kIpcAbi);
    CHECK(golden().top.at("maxNameLength") == kMaxSegmentNameLength);
    CHECK(golden().top.at("nameAbiHeadroom") >= 99);
}

TEST_CASE("every layout field matches the golden row by row", "[ipc][layout][golden]")
{
    REQUIRE(golden().loaded);
    const auto& rows = golden().sections.at("fields");
    REQUIRE(rows.size() == kFields.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& g = rows[i];
        const auto& f = kFields[i];
        INFO("row " << i << ": " << g.line);
        CHECK(g.str("struct") == f.structName);
        CHECK(g.str("field") == f.field);
        CHECK(g.num("offset") == static_cast<long long>(f.offset));
        CHECK(g.num("size") == static_cast<long long>(f.size));
        CHECK(g.str("type") == f.type);
        CHECK(g.flag("permanent") == f.permanent);
    }
}

TEST_CASE("struct sizes and alignment match the golden", "[ipc][layout][golden]")
{
    REQUIRE(golden().loaded);
    const auto& rows = golden().sections.at("structs");
    REQUIRE(rows.size() == kStructs.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        INFO(rows[i].line);
        CHECK(rows[i].str("struct") == kStructs[i].structName);
        CHECK(rows[i].num("size") == static_cast<long long>(kStructs[i].size));
        CHECK(rows[i].num("align") == static_cast<long long>(kStructs[i].align));
    }
}

TEST_CASE("constants match the golden", "[ipc][layout][golden]")
{
    REQUIRE(golden().loaded);
    const auto& rows = golden().sections.at("constants");
    REQUIRE(rows.size() == kConstants.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        INFO(rows[i].line);
        CHECK(rows[i].str("constant") == kConstants[i].name);
        CHECK(rows[i].num("value") == kConstants[i].value);
    }
}

TEST_CASE("fields of each struct are contiguous and fill it exactly", "[ipc][layout]")
{
    for (const auto& s : kStructs)
    {
        std::size_t at = 0;
        for (const auto& f : kFields)
        {
            if (f.structName != s.structName)
            {
                continue;
            }
            INFO(s.structName << "::" << f.field);
            CHECK(f.offset == at);
            at = f.offset + f.size;
            if (f.type == "u64")
            {
                CHECK(f.offset % 8 == 0);
            }
        }
        INFO(s.structName);
        CHECK(at == s.size);
    }
}

TEST_CASE("the permanently frozen fields are exactly the rv header prologue and the slot version",
          "[ipc][layout][frozen]")
{
    std::set<std::string> permanent;
    for (const auto& f : kFields)
    {
        if (f.permanent)
        {
            permanent.insert(f.structName + "::" + f.field);
        }
    }
    CHECK(permanent == std::set<std::string>{"RvHeader::magic", "RvHeader::abi", "RvHeader::generation",
                                             "RvHeader::layout_bytes", "RoleSlot::version"});
    CHECK(offsetof(RvHeader, magic) == 0);
    CHECK(offsetof(RvHeader, abi) == 4);
    CHECK(offsetof(RvHeader, generation) == 8);
    CHECK(offsetof(RvHeader, layout_bytes) == 12);
    // 端点槽 version 的绝对位置：1024·G + {0, 256} + 56
    for (u32 g = 1; g <= kGroupCount; ++g)
    {
        CHECK(offsetof(RvSegment, groups) + sizeof(GroupBlock) * (g - 1) + offsetof(GroupBlock, input) +
                  offsetof(RoleSlot, version) ==
              1024u * g + 56u);
        CHECK(roleSlotOffset(g, true) + offsetof(RoleSlot, version) == 1024u * g + 256u + 56u);
    }
    CHECK(kRvMagic == rinne::util::fourcc('R', 'N', 'R', 'V'));
    // magic 字段里的初始化标记：任何版本读到它都按「还没初始化完」处理
    CHECK(kInitMarker == rinne::util::fourcc('R', 'N', 'I', 'N'));
    CHECK(kInitMarker != kRvMagic);
    CHECK(kInitMarker != kRingMagic);
}

TEST_CASE("ctl packs generation and state into one word", "[ipc][layout]")
{
    const u64 ctl = makeCtl(0xfffffffeu, kCtlPhaseActive | kCtlProvisional);
    CHECK(ctlGen(ctl) == 0xfffffffeu);
    CHECK(ctlState(ctl) == (kCtlPhaseActive | kCtlProvisional));
    CHECK(ctlPhase(ctl) == kCtlPhaseActive);
    // 同一阶段、不同 gen 是不同的整字：同一个整字再出现要 2^32 次转移
    CHECK(makeCtl(1, kCtlPhaseFree) != makeCtl(0, kCtlPhaseFree));
    CHECK(ctlPhase(makeCtl(7, kCtlPhaseDormant)) == kCtlPhaseDormant);
    CHECK(packVersion(0, 9, 0) == 0x000900u);
}
