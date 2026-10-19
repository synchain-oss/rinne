// SPDX-License-Identifier: GPL-3.0-or-later
//
// 端点槽里的标签 seqlock：FNV-1a 的口径、按码点截断的 UTF-8 前缀、载荷的打包与还原，以及读写往返。
// 序号偶数 = 稳定、奇数 = 正在写；读方遇到奇数或序号变了就放弃，失败时 out 原样不动。
// 非 ASCII 的字面量一律写成 \x 转义：源文件保持纯 ASCII，编译器与编辑器的编码设置怎么变都不影响这些字节。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "ipc/LabelSeqlock.h"
#include "ipc/SegmentLayout.h"

namespace
{

using namespace rinne::ipc;

// 一个干净的端点槽：值初始化的会合段里的第 1 组 Input 槽
std::unique_ptr<RvSegment> makeRendezvous()
{
    return std::make_unique<RvSegment>();
}

} // namespace

TEST_CASE("fnv1a32 matches the reference vectors", "[ipc][label]")
{
    CHECK(fnv1a32("") == 0x811c9dc5u);
    CHECK(fnv1a32("a") == 0xe40c292cu);
    CHECK(fnv1a32("foobar") == 0xbf9cf968u);
}

TEST_CASE("utf8PrefixLength never cuts a code point", "[ipc][label]")
{
    const std::string ascii(200, 'a');
    CHECK(utf8PrefixLength(ascii, 112) == 112);

    // U+4E2D：3 字节，38 个共 114 字节；112 落在第 38 个字符的中间，退回到 111
    std::string wide;
    for (int i = 0; i < 38; ++i)
    {
        wide += "\xe4\xb8\xad";
    }
    REQUIRE(wide.size() == 114);
    CHECK(utf8PrefixLength(wide, 112) == 111);

    // 前面先垫一个 ASCII：112 正好落在码点边界上，不用退
    CHECK(utf8PrefixLength(std::string("a") + wide, 112) == 112);

    // U+1F600：4 字节，28 个正好 112
    std::string emoji;
    for (int i = 0; i < 28; ++i)
    {
        emoji += "\xf0\x9f\x98\x80";
    }
    REQUIRE(emoji.size() == 112);
    CHECK(utf8PrefixLength(emoji, 112) == 112);
    // 垫一个 ASCII 变成 113：112 切在第 28 个字符的中间，退到 109
    CHECK(utf8PrefixLength(std::string("a") + emoji, 112) == 109);

    CHECK(utf8PrefixLength(ascii, 0) == 0);
    CHECK(utf8PrefixLength("", 112) == 0);
}

TEST_CASE("makeLabelPayload packs the prefix and hashes the full name", "[ipc][label]")
{
    const LabelPayload p = makeLabelPayload("Vox", 0xff112233u);
    CHECK(p.colourArgb == 0xff112233u);
    CHECK(p.nameLen == 3u);
    CHECK(p.nameHash == fnv1a32("Vox"));
    CHECK(p.words[0] == (static_cast<u32>('V') | (static_cast<u32>('o') << 8) | (static_cast<u32>('x') << 16)));
    for (u32 i = 1; i < kLabelNameWords; ++i)
    {
        CHECK(p.words[i] == 0u);
    }
    CHECK(labelDisplayName(p) == "Vox");

    // 长名字：长度与哈希按完整名字算，显示只有按码点截断的前缀
    std::string longName;
    for (int i = 0; i < 60; ++i)
    {
        longName += "\xe4\xb8\xad"; // 180 字节，超过 112
    }
    const LabelPayload q = makeLabelPayload(longName, 1);
    CHECK(q.nameLen == longName.size());
    CHECK(q.nameHash == fnv1a32(longName));
    CHECK(labelDisplayName(q) == longName.substr(0, 111));
}

TEST_CASE("writeLabel and readLabel round trip with an even sequence", "[ipc][label]")
{
    auto rv = makeRendezvous();
    RoleSlot& slot = rv->groups[0].input;
    CHECK(slot.label_seq.load() == 0u);

    const LabelPayload first = makeLabelPayload("Vox", 0xff00ff00u);
    REQUIRE(writeLabel(slot, first));
    CHECK(slot.label_seq.load() == 2u);
    LabelPayload out;
    REQUIRE(readLabel(slot, out));
    CHECK(out == first);

    const LabelPayload second = makeLabelPayload("Bass", 0xff0000ffu);
    REQUIRE(writeLabel(slot, second));
    CHECK(slot.label_seq.load() == 4u);
    REQUIRE(readLabel(slot, out));
    CHECK(out == second);
    CHECK(out != first);
    CHECK(labelDisplayName(out) == "Bass");
}

TEST_CASE("an odd sequence makes both sides back off without touching the payload", "[ipc][label]")
{
    auto rv = makeRendezvous();
    RoleSlot& slot = rv->groups[0].input;
    const LabelPayload payload = makeLabelPayload("Vox", 0xff112233u);
    REQUIRE(writeLabel(slot, payload));
    LabelPayload stable;
    REQUIRE(readLabel(slot, stable));
    CHECK(stable == payload);
    CHECK(slot.label_seq.load() == 2u);

    // 写方正在写（序号奇数）：读方放弃，out 原样不动
    slot.label_seq.store(3u);
    LabelPayload keep;
    keep.colourArgb = 0xdeadbeefu;
    keep.nameLen = 42;
    CHECK_FALSE(readLabel(slot, keep));
    CHECK(keep.colourArgb == 0xdeadbeefu);
    CHECK(keep.nameLen == 42u);

    // 写方也放弃：序号留在奇数上，字段一个都没动
    const LabelPayload changed = makeLabelPayload("Bass", 0u);
    CHECK_FALSE(writeLabel(slot, changed));
    CHECK(slot.label_seq.load() == 3u);
    CHECK(slot.colour_argb.load() == payload.colourArgb);
    CHECK(slot.name_len.load() == payload.nameLen);
    CHECK(slot.name_hash.load() == payload.nameHash);
    CHECK(slot.name_words[0].load() == payload.words[0]);
}
