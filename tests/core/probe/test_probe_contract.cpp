// SPDX-License-Identifier: GPL-3.0-or-later
//
// 与日志格式契约（scripts/probe/LOG-FORMAT.md）对拍：事件名与顺序、哪些事件有参数、(case, variant) 规则表、
// 取值上界。契约文档是唯一真源，这里读它的表格，代码里的表对不上就红。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ProbeTestKit.h"
#include "diag/ProbeEvents.h"
#include "diag/ProbeLog.h"

namespace
{

using namespace rinne::diag;
using namespace rinne::test::probe;

std::string trim(std::string s)
{
    const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string stripTicks(const std::string& s)
{
    std::string out = trim(s);
    if (out.size() >= 2 && out.front() == '`' && out.back() == '`')
    {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}

// 表格行 | a | b | c | -> {"a", "b", "c"}（去掉首尾的空列）
std::vector<std::string> tableCells(const std::string& line)
{
    std::vector<std::string> cells = splitFields(line, '|');
    if (!cells.empty())
    {
        cells.erase(cells.begin());
    }
    if (!cells.empty())
    {
        cells.pop_back();
    }
    return cells;
}

struct DocEvent
{
    std::string name;
    bool aEmpty;
    bool bEmpty;
};

std::vector<DocEvent> docEvents(const std::string& doc)
{
    std::vector<DocEvent> events;
    bool inSection = false;
    for (const std::string& line : splitLines(doc))
    {
        if (line.rfind("## ", 0) == 0)
        {
            inSection = line == "## 事件";
            continue;
        }
        if (!inSection || line.rfind("| `", 0) != 0)
        {
            continue;
        }
        const std::vector<std::string> cells = tableCells(line);
        if (cells.size() >= 3)
        {
            events.push_back({stripTicks(cells[0]), trim(cells[1]).empty(), trim(cells[2]).empty()});
        }
    }
    return events;
}

std::set<std::string> docCaseKeys(const std::string& doc)
{
    std::set<std::string> keys;
    for (const std::string& line : splitLines(doc))
    {
        if (line.rfind("| `CB-", 0) != 0 && line.rfind("| `LU-", 0) != 0)
        {
            continue;
        }
        const std::vector<std::string> cells = tableCells(line);
        // 规则表：| `case` | `variant` | 类别 | 喂给 | 证据 |；case 说明表的第二列不是反引号包的 variant
        if (cells.size() >= 5 && trim(cells[1]).rfind('`', 0) == 0)
        {
            keys.insert(stripTicks(cells[0]) + ":" + stripTicks(cells[1]));
        }
    }
    return keys;
}

std::set<std::string> codeCaseKeys()
{
    std::set<std::string> keys;
    for (const std::string& token : splitFields(std::string(probeCaseKeys()), ' '))
    {
        if (!token.empty())
        {
            keys.insert(token);
        }
    }
    return keys;
}

} // namespace

TEST_CASE("probe event names and order equal the log format contract", "[probe][contract]")
{
    const std::string doc = readFile(logFormatDoc());
    REQUIRE_FALSE(doc.empty());
    const std::vector<DocEvent> expected = docEvents(doc);
    REQUIRE(expected.size() == kProbeEventCount);
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        INFO("event " << expected[i].name);
        CHECK(expected[i].name == kProbeEvents[i].name);
        // 契约里 a、b 留空的事件，代码里的参数类型必须是 kNone；有内容的必须不是
        CHECK(expected[i].aEmpty == (kProbeEvents[i].a == ProbeArgKind::kNone));
        CHECK(expected[i].bEmpty == (kProbeEvents[i].b == ProbeArgKind::kNone));
    }
}

TEST_CASE("probe event codes live in the probe segment and never collide", "[probe][contract]")
{
    // 段归属：0x0200 安全链、0x0300 环路层、0x0400 引擎、0x0500 探针
    static_assert(kProbeEventSegmentFirst == 0x0500 && kProbeEventSegmentLast == 0x05FF, "探针段");
    std::set<std::uint16_t> codes;
    std::set<std::string> names;
    for (const ProbeEventInfo& info : kProbeEvents)
    {
        const auto code = static_cast<std::uint16_t>(info.code);
        CHECK(code >= kProbeEventSegmentFirst);
        CHECK(code <= kProbeEventSegmentLast);
        CHECK(codes.insert(code).second);
        CHECK(names.insert(info.name).second);
        CHECK(probeEventInfo(code) == &info);
    }
    CHECK(probeEventInfo(std::uint16_t{0x0300}) == nullptr); // 环路层的码不属于探针
    CHECK(probeEventInfo(std::uint16_t{0x0512}) == nullptr); // 段内还没定义的码
}

TEST_CASE("the case and variant table equals the contract rule table", "[probe][contract]")
{
    const std::string doc = readFile(logFormatDoc());
    const std::set<std::string> fromDoc = docCaseKeys(doc);
    const std::set<std::string> fromCode = codeCaseKeys();
    REQUIRE_FALSE(fromDoc.empty());
    CHECK(fromCode == fromDoc);
    // 首尾各有一个空格：isValidProbeCase 靠整词查找
    const std::string_view keys = probeCaseKeys();
    REQUIRE(keys.size() > 2);
    CHECK(keys.front() == ' ');
    CHECK(keys.back() == ' ');
}

TEST_CASE("only contract combinations pass case validation", "[probe][contract]")
{
    CHECK(isValidProbeCase("CB-01", "steady"));
    CHECK(isValidProbeCase("LU-08", "-"));
    CHECK(isValidProbeCase("CB-03", "guard-out"));
    CHECK_FALSE(isValidProbeCase("CB-01", "deactivate")); // 表外组合
    CHECK_FALSE(isValidProbeCase("CB-13", "steady"));
    CHECK_FALSE(isValidProbeCase("", "steady"));
    CHECK_FALSE(isValidProbeCase("CB-01", ""));
    CHECK_FALSE(isValidProbeCase("CB-01:steady CB-02", "toggle")); // 拼出整串也不行
    CHECK_FALSE(isValidProbeCase("CB-01", "steady CB-02:toggle"));
    CHECK_FALSE(isValidProbeCase("cb-01", "steady"));
}

TEST_CASE("the documented limits are the limits the writer clamps to", "[probe][contract]")
{
    const std::string doc = readFile(logFormatDoc());
    for (const std::string& literal :
         {std::to_string(kProbeMaxBlock), std::to_string(kProbeMaxPos), std::to_string(kProbeMaxD),
          std::to_string(kProbeMaxBuf), std::to_string(kProbeMaxSampleRate), std::to_string(kProbeMaxPid),
          std::to_string(kProbeNameBytes) + " 字节", std::string("rinne-probe-log,1")})
    {
        INFO(literal);
        CHECK(doc.find(literal) != std::string::npos);
    }
}
