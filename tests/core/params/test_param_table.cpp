// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数表（src/input/ParamTable.h）与 tests/golden/params-input.json 的对拍。参数表已冻结，规则见
// docs/contracts/PARAMETERS.md：
//   · 表自身：11 个 ID 与序号；类型、范围、默认与可自动化同契约字面量相等；ID 唯一；
//     默认值在范围内；布尔与档位的形状；delay_note 16 档顺序，以及每一档的桥取值 id（与桥文档逐个对应、时值严格升序）；
//   · 宿主参数 ID（vst3 / au 两列）：用纯 C++ 复算 JUCE 的散列，已知答案另算一份，不靠 golden 自证；
//   · golden：逐参数逐字段与表相等，行数与顺序一致，全文与表渲染出的规范排版逐字节相同。
// 重新生成：设环境变量 RINNE_PARAMS_GOLDEN_REGEN=1 跑 [golden] 用例，再不带开关跑一遍，看一眼 diff。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../../../src/input/InputBridgeApi.h"
#include "ParamsGolden.h"

namespace
{

using namespace rinne::test::params;

const Param& byId(const std::string& id)
{
    const auto at = std::find_if(table().begin(), table().end(), [&](const Param& p) { return id == p.id; });
    INFO("参数表里要有 " << id);
    REQUIRE(at != table().end());
    return *at;
}

// 契约里的 11 个参数 ID，顺序即序号
const std::vector<std::string> kContractIds = {"feedback", "loop_on",    "mix",     "delay_mode",
                                               "delay_ms", "delay_note", "low_cut", "high_cut",
                                               "polarity", "limiter",    "ceiling"};

// 契约里每个参数的类型、范围、默认与可自动化，单独写一份字面量：
// golden 是从表渲染的，表与 golden 一起改错时只有它能拦住。
// 步进、刻度、单位与显示名不在这里重复：契约文档的参数表与 golden 逐格对拍（contract-static-params）。
struct Contract
{
    const char* id;
    char kind;
    double lo;
    double hi;
    double def;
    bool automatable;
};

const std::vector<Contract> kContract = {
    {"feedback", 'F', 0.0, 150.0, 0.0, true},     {"loop_on", 'B', 0.0, 1.0, 1.0, true},
    {"mix", 'F', 0.0, 100.0, 25.0, true},         {"delay_mode", 'C', 0.0, 1.0, 0.0, false},
    {"delay_ms", 'F', 1.0, 4000.0, 100.0, false}, {"delay_note", 'C', 0.0, 15.0, 10.0, false},
    {"low_cut", 'F', 20.0, 2000.0, 20.0, true},   {"high_cut", 'F', 1000.0, 20000.0, 20000.0, true},
    {"polarity", 'B', 0.0, 1.0, 0.0, true},       {"limiter", 'B', 0.0, 1.0, 1.0, false},
    {"ceiling", 'F', -24.0, 0.0, -6.0, false}};

// delay_note 的 16 档：顺序冻结，只能在末尾追加
const std::vector<std::string> kNoteLabels = {"1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8",
                                              "1/4T", "1/8D",  "1/4",  "1/2T",  "1/4D", "1/2",  "1/2D",  "1/1"};

// 界面桥的参数表（docs/UI_BRIDGE.md 里 delay_note 一行）列出的 16 个取值 id，字面量照抄。
// 那一行列的先后不作准：宿主下标与 id 的对应以参数表为准，这里只比集合。
const std::vector<std::string> kBridgeNoteIds = {"n64", "n32t", "n32", "n16t", "n16", "n16d", "n8t", "n8",
                                                 "n8d", "n4t",  "n4",  "n4d",  "n2t", "n2",   "n2d", "n1"};

// 桥取值 id 的时值（拍数）：n<分母>，末尾 t 是三连音、d 是附点；读不懂时返回 -1
double noteBeats(const std::string& bridge)
{
    if (bridge.size() < 2 || bridge[0] != 'n')
    {
        return -1.0;
    }
    const bool triplet = bridge.back() == 't';
    const bool dotted = bridge.back() == 'd';
    const double denominator = std::atof(bridge.substr(1, bridge.size() - (triplet || dotted ? 2 : 1)).c_str());
    return denominator <= 0.0 ? -1.0 : 4.0 / denominator * (triplet ? 2.0 / 3.0 : dotted ? 1.5 : 1.0);
}

// 宿主文字 "1/16D" 对应的桥取值 id "n16d"
std::string bridgeIdOfLabel(const std::string& label)
{
    std::string id = "n" + (label.size() > 2 ? label.substr(2) : std::string());
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return id;
}

} // namespace

TEST_CASE("golden file is the canonical rendering of the table", "[params][golden]")
{
    const std::string expected = renderGolden();
    if (regenEnabled())
    {
        std::ofstream out(goldenPath(), std::ios::binary | std::ios::trunc);
        out << expected;
        REQUIRE(out.good());
    }
    const Golden golden = loadGolden();
    INFO("golden 文件：" << goldenPath().string() << "（设 RINNE_PARAMS_GOLDEN_REGEN=1 重跑本用例可重新生成）");
    REQUIRE(golden.loaded);
    INFO("表渲染出的全文：\n" << expected);
    CHECK(golden.text == expected);
}

TEST_CASE("table has the eleven contract ids in contract order", "[params][table]")
{
    REQUIRE(table().size() == kContractIds.size());
    for (std::size_t i = 0; i < kContractIds.size(); ++i)
    {
        INFO("序号 " << i);
        CHECK(kContractIds[i] == table()[i].id);
        CHECK(table()[i].index == static_cast<int>(i));
    }
}

TEST_CASE("table kinds, ranges, defaults and automation equal the contract", "[params][table]")
{
    REQUIRE(table().size() == kContract.size());
    for (const auto& c : kContract)
    {
        INFO("参数 " << c.id);
        const Param& p = byId(c.id);
        CHECK(p.kind == c.kind);
        CHECK(p.lo == c.lo);
        CHECK(p.hi == c.hi);
        CHECK(p.def == c.def);
        CHECK(p.automatable == c.automatable);
    }
}

TEST_CASE("table ids are unique lowercase ascii and match the bridge names", "[params][table]")
{
    std::set<std::string> seen;
    for (const auto& p : table())
    {
        const std::string id = p.id;
        INFO("参数 " << id);
        CHECK(seen.insert(id).second);
        CHECK(!id.empty());
        CHECK(std::islower(static_cast<unsigned char>(id[0])));
        for (const unsigned char c : id)
        {
            CHECK((std::islower(c) || std::isdigit(c) || c == '_'));
        }
    }
    // 桥名表是参数 ID 的另一处真源，必须逐名相等
    namespace bridge = rinne::input::bridge;
    const std::vector<std::string> bridgeIds = {bridge::kParamFeedback,  bridge::kParamLoopOn,  bridge::kParamMix,
                                                bridge::kParamDelayMode, bridge::kParamDelayMs, bridge::kParamDelayNote,
                                                bridge::kParamLowCut,    bridge::kParamHighCut, bridge::kParamPolarity,
                                                bridge::kParamLimiter,   bridge::kParamCeiling};
    REQUIRE(bridgeIds.size() == table().size());
    for (std::size_t i = 0; i < bridgeIds.size(); ++i)
    {
        CHECK(bridgeIds[i] == table()[i].id);
    }
}

TEST_CASE("table defaults lie inside their ranges and kinds have the right shape", "[params][table]")
{
    for (const auto& p : table())
    {
        INFO("参数 " << p.id);
        CHECK(p.lo < p.hi);
        CHECK(p.def >= p.lo);
        CHECK(p.def <= p.hi);
        CHECK(p.step > 0.0);
        CHECK((std::string(p.skew) == "lin" || std::string(p.skew) == "log"));
        if (std::string(p.skew) == "log")
        {
            CHECK(p.lo > 0.0); // 对数刻度的下限必须为正
        }
        CHECK((p.kind == 'F' || p.kind == 'B' || p.kind == 'C'));
        if (p.kind == 'B')
        {
            CHECK(p.lo == 0.0);
            CHECK(p.hi == 1.0);
            CHECK(p.step == 1.0);
            CHECK((p.def == 0.0 || p.def == 1.0));
        }
        if (p.kind == 'C')
        {
            const auto labels = choicesOf(p.id);
            CHECK(labels.size() >= 2);
            CHECK(p.lo == 0.0);
            CHECK(p.hi == static_cast<double>(labels.size()) - 1.0);
            CHECK(p.step == 1.0);
            CHECK(p.def == static_cast<int>(p.def));
        }
        else
        {
            CHECK(choicesOf(p.id).empty());
        }
    }
}

TEST_CASE("table defaults are the safe ones the contract names", "[params][table]")
{
    CHECK(byId("feedback").def == 0.0); // 默认安全：反馈为 0
    CHECK(byId("loop_on").def == 1.0);
    CHECK(byId("mix").def == 25.0);
    CHECK(byId("limiter").def == 1.0);
    CHECK(byId("ceiling").def == -6.0);
    CHECK(byId("ceiling").step == 0.1);
    CHECK(byId("low_cut").def == byId("low_cut").lo); // 下限 = 关
    CHECK(byId("high_cut").def == byId("high_cut").hi); // 上限 = 关
    CHECK(byId("delay_ms").def == 100.0);
    CHECK(byId("polarity").def == 0.0);
    CHECK(byId("delay_mode").def == 0.0);
}

TEST_CASE("delay_note keeps its sixteen steps in the frozen order", "[params][table]")
{
    const auto labels = choicesOf("delay_note");
    REQUIRE(labels.size() == kNoteLabels.size());
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        INFO("档 " << i);
        CHECK(labels[i] == kNoteLabels[i]);
    }
    // 默认档先 REQUIRE 挡住越界再取下标：默认值被改出范围时判红，不会读到数组外面
    const double noteDefault = byId("delay_note").def;
    REQUIRE(noteDefault >= 0.0);
    REQUIRE(noteDefault < static_cast<double>(labels.size()));
    CHECK(labels.at(static_cast<std::size_t>(noteDefault)) == "1/4"); // 默认 1/4
    // 档位行的序号逐档递增，不跳不重
    int expected = 0;
    for (const auto& c : choices())
    {
        if (std::string(c.id) == "delay_note")
        {
            CHECK(c.index == expected++);
        }
    }
    const std::vector<std::string> modes = {"Time", "Sync"};
    CHECK(choicesOf("delay_mode") == modes);
}

TEST_CASE("delay_note bridge ids map the host steps one to one in rising note value", "[params][table]")
{
    const auto ids = bridgeIdsOf("delay_note");
    const auto labels = choicesOf("delay_note");
    REQUIRE(ids.size() == kNoteLabels.size());
    REQUIRE(labels.size() == ids.size());

    // 16 个 id 互不相同，且恰好是桥文档列出的那 16 个
    const std::set<std::string> idSet(ids.begin(), ids.end());
    CHECK(idSet.size() == ids.size());
    CHECK(idSet == std::set<std::string>(kBridgeNoteIds.begin(), kBridgeNoteIds.end()));

    // 按本表的先后，时值严格递增；每个 id 与同一行的宿主文字互相对得上
    double previous = 0.0;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        INFO("档 " << i << "：" << labels[i] << " ↔ " << ids[i]);
        const double beats = noteBeats(ids[i]);
        CHECK(beats > previous);
        previous = beats;
        CHECK(ids[i] == bridgeIdOfLabel(labels[i]));
    }
    // 时值解析本身的几个已知值
    CHECK(noteBeats("n64") == 0.0625);
    CHECK(noteBeats("n4") == 1.0);
    CHECK(noteBeats("n8d") == 0.75);
    CHECK(noteBeats("n1") == 4.0);

    // 默认档的桥 id 是 n4；delay_mode 的两个 id 是 time / sync
    const double noteDefault = byId("delay_note").def;
    REQUIRE(noteDefault >= 0.0);
    REQUIRE(noteDefault < static_cast<double>(ids.size()));
    CHECK(ids.at(static_cast<std::size_t>(noteDefault)) == "n4");
    const std::vector<std::string> modeIds = {"time", "sync"};
    CHECK(bridgeIdsOf("delay_mode") == modeIds);

    // 只有档位参数带桥 id
    for (const auto& p : table())
    {
        INFO("参数 " << p.id);
        CHECK(bridgeIdsOf(p.id).empty() == (p.kind != 'C'));
    }
}

TEST_CASE("host parameter ids reproduce the juce hash", "[params][hash]")
{
    // 已知答案：按 JUCE 的算法（乘子 31，32 位回绕）另行算出，不是从 golden 抄的
    CHECK(plainHash("a") == 97u);
    CHECK(plainHash("ab") == 3105u);
    CHECK(plainHash("mix") == 108124u);
    CHECK(plainHash("feedback") == 4103465861u); // 最高位为 1
    CHECK(hostParamId("feedback") == 1955982213u); // 清最高位之后
    CHECK(hostParamId("mix") == 108124u); // 最高位本来就是 0
    CHECK(hostParamId("high_cut") == 462548517u);
}

TEST_CASE("host parameter ids are distinct and fit in 31 bits", "[params][hash]")
{
    std::set<std::uint32_t> seen;
    for (const auto& p : table())
    {
        const std::uint32_t id = hostParamId(p.id);
        INFO("参数 " << p.id << " → " << id);
        CHECK(id < 0x80000000u);
        CHECK(seen.insert(id).second);
    }
}

TEST_CASE("golden rows match the table field by field", "[params][golden]")
{
    const Golden golden = loadGolden();
    REQUIRE(golden.loaded);
    CHECK(golden.top.str("status") == std::optional<std::string>("frozen"));
    CHECK(golden.top.num("versionHint") == std::optional<double>(1.0));
    CHECK(golden.top.num("count") == std::optional<double>(static_cast<double>(table().size())));

    // 行数与顺序：少一行、多一行、调换顺序都判红
    REQUIRE(golden.ids.size() == table().size());
    for (std::size_t i = 0; i < table().size(); ++i)
    {
        INFO("第 " << i << " 行");
        CHECK(golden.ids[i] == table()[i].id);
    }

    for (const auto& p : table())
    {
        INFO("参数 " << p.id);
        const Line* row = golden.row(p.id);
        REQUIRE(row != nullptr);
        CHECK(row->num("index") == std::optional<double>(p.index));
        CHECK(row->str("name") == std::optional<std::string>(p.name));
        CHECK(row->str("kind") == std::optional<std::string>(kindText(p.kind)));
        CHECK(row->num("min") == std::optional<double>(p.lo));
        CHECK(row->num("max") == std::optional<double>(p.hi));
        CHECK(row->num("default") == std::optional<double>(p.def));
        CHECK(row->num("step") == std::optional<double>(p.step));
        CHECK(row->str("skew") == std::optional<std::string>(skewText(p.skew)));
        CHECK(row->flag("automatable") == std::optional<bool>(p.automatable));
        CHECK(row->str("unit") == std::optional<std::string>(unitText(p.unit)));
        if (p.kind == 'C')
        {
            CHECK(row->list("choices") == std::optional<std::vector<std::string>>(choicesOf(p.id)));
            CHECK(row->list("bridgeIds") == std::optional<std::vector<std::string>>(bridgeIdsOf(p.id)));
        }
        else
        {
            CHECK(!row->list("choices").has_value());
            CHECK(!row->list("bridgeIds").has_value());
        }
    }
}

TEST_CASE("golden host id columns equal the recomputed hash", "[params][golden][hash]")
{
    const Golden golden = loadGolden();
    REQUIRE(golden.loaded);
    for (const auto& p : table())
    {
        INFO("参数 " << p.id);
        const Line* row = golden.row(p.id);
        REQUIRE(row != nullptr);
        const auto expected = std::optional<double>(static_cast<double>(hostParamId(p.id)));
        CHECK(row->num("vst3") == expected);
        CHECK(row->num("au") == expected);
    }
}
