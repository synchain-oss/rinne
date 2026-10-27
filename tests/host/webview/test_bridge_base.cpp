// SPDX-License-Identifier: GPL-3.0-or-later
//
// 桥常量、缩放模型与回执：全是纯计算。重点是三条安全边界：
//   - 开窗按显示器夹取的档位不会被当成用户选的档位写回；
//   - 缩放锁定时选档与确认都被拒，而且确认被拒时不会调用落盘；
//   - 语言码只认白名单，类型不对一律拒。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "BridgeBase.h"
#include "InputBridgeApi.h"
#include "OutputBridgeApi.h"
#include "util/DesignBox.h"

namespace
{

namespace b = rinne::bridge;
namespace d = rinne::design;

juce::Array<juce::var> args(std::initializer_list<juce::var> list)
{
    juce::Array<juce::var> out;
    for (const auto& v : list)
        out.add(v);
    return out;
}

bool isOk(const juce::var& response)
{
    return response.getProperty("ok", juce::var()).isBool() &&
           static_cast<bool>(response.getProperty("ok", juce::var()));
}

std::string reasonOf(const juce::var& response)
{
    return response.getProperty("reason", juce::var()).toString().toStdString();
}

} // namespace

TEST_CASE("bridge scale limits clamp to 0.33 .. 3.0", "[webview][bridge]")
{
    CHECK(b::clampUiScale(0.0f) == b::plugin::MinUiScale);
    CHECK(b::clampUiScale(-5.0f) == b::plugin::MinUiScale);
    CHECK(b::clampUiScale(0.33f) == 0.33f);
    CHECK(b::clampUiScale(1.0f) == 1.0f);
    CHECK(b::clampUiScale(3.0f) == 3.0f);
    CHECK(b::clampUiScale(9.0f) == b::plugin::MaxUiScale);
    CHECK(b::plugin::MinUiScale == 0.33f);
    CHECK(b::plugin::MaxUiScale == 3.0f);
}

TEST_CASE("bridge percent clamp keeps 33 .. 300 and survives 64-bit input", "[webview][bridge]")
{
    CHECK(b::clampUiScalePercent(32) == 33);
    CHECK(b::clampUiScalePercent(33) == 33);
    CHECK(b::clampUiScalePercent(100) == 100);
    CHECK(b::clampUiScalePercent(300) == 300);
    CHECK(b::clampUiScalePercent(301) == 300);
    CHECK(b::clampUiScalePercent(-1) == 33);
    // 工程文件里的缩放是无符号 32 位：4294967295 若先折成 int 就是 -1，会落在下界，那是溢出撞上的而不是范围校验
    CHECK(b::clampUiScalePercent(static_cast<std::int64_t>(4294967295LL)) == 300);
    CHECK(b::clampUiScalePercent(std::numeric_limits<std::int64_t>::max()) == 300);
    CHECK(b::clampUiScalePercent(std::numeric_limits<std::int64_t>::min()) == 33);
}

TEST_CASE("bridge language whitelist is exactly zh, en, fr", "[webview][bridge]")
{
    for (const char* ok : {"zh", "en", "fr"})
    {
        CHECK(b::isSupportedLang(ok));
        CHECK(b::normalizeLang(ok) == juce::String(ok));
    }
    for (const char* bad : {"", "de", "ZH", "zh-CN", "en ", "fr\n", "xx"})
    {
        CHECK_FALSE(b::isSupportedLang(bad));
        CHECK(b::normalizeLang(bad) == "zh");
    }
}

TEST_CASE("bridge window size follows the design box of each role", "[webview][bridge]")
{
    const auto input1 = b::designBoxWindowSize("input", 1.0f);
    CHECK(input1.width == d::kInputDesignW);
    CHECK(input1.height == d::kInputDesignH);
    const auto output1 = b::designBoxWindowSize("output", 1.0f);
    CHECK(output1.width == d::kOutputDesignW);
    CHECK(output1.height == d::kOutputDesignH);

    const auto half = b::designBoxWindowSize("output", 0.5f);
    CHECK(half.width == 230);
    CHECK(half.height == 182);
    const auto big = b::designBoxWindowSize("input", 2.0f);
    CHECK(big.width == 1600);
    CHECK(big.height == 1160);

    // 其余角色名一律回落到 Output 的设计盒
    const auto other = b::designBoxWindowSize("monitor", 1.0f);
    CHECK(other.width == d::kOutputDesignW);
    CHECK(other.height == d::kOutputDesignH);
}

TEST_CASE("open-time fit picks the largest preset that fits the display area", "[webview][bridge][fit]")
{
    // Input 的 1.0 档窗口是 800 x 580 加宿主框 16 x 64，即 816 x 644。边界上相等算放得下
    CHECK(b::fitScaleToDisplay("input", 1.0f, 816, 644) == 1.0f);
    CHECK(b::fitScaleToDisplay("input", 1.0f, 815, 644) == 0.8f); // 宽少一像素
    CHECK(b::fitScaleToDisplay("input", 1.0f, 816, 643) == 0.8f); // 高少一像素

    // 放得下就原样用，不降档也不升档
    CHECK(b::fitScaleToDisplay("input", 1.5f, 1920, 1040) == 1.5f);
    CHECK(b::fitScaleToDisplay("input", 0.5f, 1920, 1040) == 0.5f);

    // 放不下取放得下的最大档：2.0 需要 1616 x 1224，1080p 的可用区放不下，1.5 放得下
    CHECK(b::fitScaleToDisplay("input", 2.0f, 1920, 1040) == 1.5f);
    // Output 的 3.0 档窗口是 1380 x 1092 加框，1920 x 1040 放不下；2.5 档 1166 x 974 放得下
    CHECK(b::fitScaleToDisplay("output", 3.0f, 1920, 1040) == 2.5f);

    // 一档都放不下时取最小档，而不是原样返回
    CHECK(b::fitScaleToDisplay("input", 2.0f, 300, 200) == 0.5f);
    CHECK(b::fitScaleToDisplay("output", 3.0f, 100, 100) == 0.33f);

    // 可用区未知（不大于 0）时不夹取
    CHECK(b::fitScaleToDisplay("input", 2.0f, 0, 0) == 2.0f);
    CHECK(b::fitScaleToDisplay("input", 2.0f, 1920, 0) == 2.0f);
    CHECK(b::fitScaleToDisplay("output", 3.0f, -1, 1080) == 3.0f);

    // 其余角色名走 Output 的档位表
    CHECK(b::fitScaleToDisplay("monitor", 3.0f, 1920, 1040) == 2.5f);
}

TEST_CASE("stored percent maps back to a preset of the role, otherwise 100 percent", "[webview][bridge]")
{
    CHECK(b::scaleFromStoredPercent("input", 0) == 1.0f);
    CHECK(b::scaleFromStoredPercent("input", 65) == 0.65f);
    CHECK(b::scaleFromStoredPercent("input", 200) == 2.0f);
    // 33 是 Output 的一档，不是 Input 的：被另一个角色的键污染时不能当成有效档位
    CHECK(b::scaleFromStoredPercent("input", 33) == 1.0f);
    CHECK(b::scaleFromStoredPercent("output", 33) == 0.33f);
    CHECK(b::scaleFromStoredPercent("output", 175) == 1.75f);
    CHECK(b::scaleFromStoredPercent("output", 300) == 3.0f);
    CHECK(b::scaleFromStoredPercent("output", 77) == 1.0f);
    CHECK(b::scaleFromStoredPercent("output", -5) == 1.0f);
}

TEST_CASE("every preset survives the percent round trip and the json conversion", "[webview][bridge]")
{
    for (const double f : d::kInputPresets)
    {
        const auto scale = static_cast<float>(f);
        CHECK(b::scaleFromStoredPercent("input", b::scaleToPercent(scale)) == scale);
        CHECK(b::scaleForJson(scale) == f);
    }
    for (const double f : d::kOutputPresets)
    {
        const auto scale = static_cast<float>(f);
        CHECK(b::scaleFromStoredPercent("output", b::scaleToPercent(scale)) == scale);
        CHECK(b::scaleForJson(scale) == f);
    }
    // 单精度的 0.65 直接转双精度是 0.6499999761...，页面按档位表做相等比较会认不出来
    CHECK(static_cast<double>(0.65f) != 0.65);
    CHECK(b::scaleForJson(0.65f) == 0.65);
}

TEST_CASE("scale model: fit at open does not leave anything to persist", "[webview][bridge][scale]")
{
    b::UiScaleModel model("input", 2.0f);
    CHECK(model.requested() == 2.0f);
    CHECK(model.scale() == 2.0f);

    const float fitted = model.fitToDisplay(1920, 1040);
    CHECK(fitted == 1.5f);
    CHECK(model.scale() == 1.5f);
    CHECK(model.requested() == 2.0f); // 已保存的那一份不动
    CHECK_FALSE(model.pending());

    // 开窗夹取不产生待确认的预览，所以确认只会被拒，没有任何值可以落盘
    const auto commit = model.commit();
    CHECK_FALSE(commit.ok());
    CHECK(std::string(commit.reason) == b::Reason::NoPending);
}

TEST_CASE("scale model: preview then commit", "[webview][bridge][scale]")
{
    b::UiScaleModel model("output", 1.0f);
    const auto applied = model.apply(1.75f);
    CHECK(applied.ok());
    CHECK(applied.scale == 1.75f);
    CHECK(model.scale() == 1.75f);
    CHECK(model.pending());

    const auto commit = model.commit();
    CHECK(commit.ok());
    CHECK(commit.scale == 1.75f);
    CHECK_FALSE(model.pending());

    // 第二次确认没有待确认的预览
    const auto again = model.commit();
    CHECK_FALSE(again.ok());
    CHECK(std::string(again.reason) == b::Reason::NoPending);
}

TEST_CASE("scale model: construction clamps to the hard limits", "[webview][bridge][scale]")
{
    CHECK(b::UiScaleModel("output", 9.0f).scale() == 3.0f);
    CHECK(b::UiScaleModel("output", 0.0f).scale() == 0.33f);
    CHECK(b::UiScaleModel("input", -2.0f).requested() == 0.33f);
}

TEST_CASE("scale model: lock pins the window to 100 percent and refuses preview and commit", "[webview][bridge][scale]")
{
    b::UiScaleModel model("input", 1.5f);
    CHECK(model.scale() == 1.5f);

    model.setLocked(true);
    CHECK(model.locked());
    CHECK(model.scale() == 1.0f);
    CHECK(model.requested() == 1.5f); // 已保存的档位不动

    const auto apply = model.apply(0.8f);
    CHECK_FALSE(apply.ok());
    CHECK(std::string(apply.reason) == b::Reason::ScaleLocked);
    CHECK(apply.scale == 1.0f);
    CHECK_FALSE(model.pending());
    CHECK(model.scale() == 1.0f);

    const auto commit = model.commit();
    CHECK_FALSE(commit.ok());
    CHECK(std::string(commit.reason) == b::Reason::ScaleLocked);

    // 解锁后回到锁定前生效的档位，被拒的选档没有悄悄改掉它
    model.setLocked(false);
    CHECK(model.scale() == 1.5f);
}

TEST_CASE("scale model: locking drops a pending preview so it cannot be committed afterwards",
          "[webview][bridge][scale]")
{
    b::UiScaleModel model("output", 1.0f);
    REQUIRE(model.apply(2.0f).ok());
    REQUIRE(model.pending());

    model.setLocked(true);
    CHECK_FALSE(model.pending());
    CHECK(std::string(model.commit().reason) == b::Reason::ScaleLocked);

    model.setLocked(false);
    // 锁定期间丢掉的预览不会复活成可确认的状态
    CHECK(std::string(model.commit().reason) == b::Reason::NoPending);
}

TEST_CASE("setUiScale argument validation uses the role preset table", "[webview][bridge]")
{
    float out = 0.0f;
    CHECK(b::parseUiScaleArg(args({0.65}), "input", out));
    CHECK(out == 0.65f);
    CHECK(b::parseUiScaleArg(args({1}), "input", out)); // 整数形式的 1
    CHECK(out == 1.0f);

    CHECK_FALSE(b::parseUiScaleArg(args({}), "input", out));
    CHECK_FALSE(b::parseUiScaleArg(args({juce::var("1.0")}), "input", out)); // 字符串不收
    CHECK_FALSE(b::parseUiScaleArg(args({true}), "input", out));
    CHECK_FALSE(b::parseUiScaleArg(args({0.7}), "input", out)); // 不在表里
    CHECK_FALSE(b::parseUiScaleArg(args({0.33}), "input", out)); // 33 档是 Output 的，不是 Input 的
    CHECK(b::parseUiScaleArg(args({0.33}), "output", out));
    CHECK(out == 0.33f);
    CHECK_FALSE(b::parseUiScaleArg(args({3.0001}), "output", out));
    CHECK(b::parseUiScaleArg(args({3}), "output", out));
    CHECK(out == 3.0f);
}

TEST_CASE("setUiScale receipt: ok carries the scale, failures carry the contract reason", "[webview][bridge][receipt]")
{
    b::UiScaleModel model("input", 1.0f);
    bool changed = true;

    const auto bad = b::respondSetUiScale(model, "input", args({0.7}), changed);
    CHECK_FALSE(isOk(bad));
    CHECK(reasonOf(bad) == "bad-scale");
    CHECK_FALSE(changed);
    CHECK(model.scale() == 1.0f);

    const auto noArg = b::respondSetUiScale(model, "input", args({}), changed);
    CHECK(reasonOf(noArg) == "bad-scale");
    CHECK_FALSE(changed);

    const auto good = b::respondSetUiScale(model, "input", args({0.65}), changed);
    CHECK(isOk(good));
    CHECK(changed);
    CHECK(static_cast<double>(good.getProperty("scale", juce::var())) == 0.65);
    CHECK(model.scale() == 0.65f);
    CHECK(model.pending());
}

TEST_CASE("setUiScale receipt while locked: scale-locked and the window does not change", "[webview][bridge][receipt]")
{
    b::UiScaleModel model("output", 1.5f);
    model.setLocked(true);
    bool changed = true;

    const auto response = b::respondSetUiScale(model, "output", args({2.0}), changed);
    CHECK_FALSE(isOk(response));
    CHECK(reasonOf(response) == "scale-locked");
    CHECK_FALSE(changed);
    CHECK_FALSE(model.pending());
    CHECK(model.scale() == 1.0f);

    // 参数本身不合法时先报参数错，不是锁定错
    CHECK(reasonOf(b::respondSetUiScale(model, "output", args({0.7}), changed)) == "bad-scale");
}

TEST_CASE("commitUiScale receipt persists only a real commit", "[webview][bridge][receipt]")
{
    std::vector<float> persisted;
    const auto persist = [&persisted](float scale) { persisted.push_back(scale); };

    b::UiScaleModel model("input", 1.0f);

    // 没有待确认的预览：不落盘
    auto response = b::respondCommitUiScale(model, persist);
    CHECK(reasonOf(response) == "no-pending");
    CHECK(persisted.empty());

    // 预览之后确认：落盘一次，落的是预览的档位
    bool changed = false;
    REQUIRE(isOk(b::respondSetUiScale(model, "input", args({1.25}), changed)));
    response = b::respondCommitUiScale(model, persist);
    CHECK(isOk(response));
    CHECK(static_cast<double>(response.getProperty("scale", juce::var())) == 1.25);
    REQUIRE(persisted.size() == 1);
    CHECK(persisted[0] == 1.25f);

    // 已经确认过，再确认被拒，不重复落盘
    response = b::respondCommitUiScale(model, persist);
    CHECK(reasonOf(response) == "no-pending");
    CHECK(persisted.size() == 1);
}

TEST_CASE("commitUiScale while locked never reaches the store", "[webview][bridge][receipt]")
{
    int persistCalls = 0;
    const auto persist = [&persistCalls](float) { ++persistCalls; };

    b::UiScaleModel model("output", 1.0f);
    bool changed = false;
    REQUIRE(isOk(b::respondSetUiScale(model, "output", args({2.0}), changed)));
    model.setLocked(true); // 预览已生效时被锁定

    CHECK(reasonOf(b::respondCommitUiScale(model, persist)) == "scale-locked");
    CHECK(persistCalls == 0);
}

TEST_CASE("page-enforced 100 percent (old WebKit): the window follows and the saved scale is untouched",
          "[webview][bridge][receipt]")
{
    // 旧 WebKit 上页面量到旧语义后自己请求回 100%（只发 setUiScale，不发 commitUiScale）。
    // 宿主这一半：窗口跟到 100%，已保存的档位原样留着，等换到新 WebKit 后照旧生效
    struct Row
    {
        const char* role;
        float saved;
    };
    for (const Row row : {Row{"input", 1.5f}, Row{"output", 2.0f}})
    {
        INFO(row.role);
        b::UiScaleModel model(row.role, row.saved);
        REQUIRE(model.scale() == row.saved); // 宿主按存档开窗

        bool changed = false;
        const auto response = b::respondSetUiScale(model, row.role, args({1.0}), changed);
        CHECK(isOk(response));
        CHECK(changed);
        CHECK(model.scale() == 1.0f);
        CHECK(model.requested() == row.saved);
        CHECK(model.pending()); // 只是预览：页面不确认，就没有任何东西会落盘
    }
}

TEST_CASE("open-time fit is not written back by a later commit", "[webview][bridge][receipt]")
{
    int persistCalls = 0;
    const auto persist = [&persistCalls](float) { ++persistCalls; };

    b::UiScaleModel model("input", 2.0f);
    model.fitToDisplay(1920, 1040);
    REQUIRE(model.scale() == 1.5f);

    // 页面没有选档就确认：被拒，夹取出来的 1.5 不会被当成用户选的档位写进全局默认
    CHECK(reasonOf(b::respondCommitUiScale(model, persist)) == "no-pending");
    CHECK(persistCalls == 0);
}

TEST_CASE("setLang receipt accepts only whitelisted strings", "[webview][bridge][receipt]")
{
    std::vector<std::string> accepted;
    const auto onAccepted = [&accepted](const juce::String& code) { accepted.push_back(code.toStdString()); };

    const auto fr = b::respondSetLang(args({juce::var("fr")}), onAccepted);
    CHECK(isOk(fr));
    CHECK(fr.getProperty("lang", juce::var()).toString() == "fr");
    REQUIRE(accepted.size() == 1);
    CHECK(accepted[0] == "fr");

    for (const auto& bad :
         {juce::var("de"), juce::var(""), juce::var("ZH"), juce::var(1), juce::var(true), juce::var(), juce::var(0.5)})
    {
        const auto response = b::respondSetLang(args({bad}), onAccepted);
        CHECK_FALSE(isOk(response));
        CHECK(reasonOf(response) == "bad-lang");
    }
    CHECK(reasonOf(b::respondSetLang(args({}), onAccepted)) == "bad-lang");
    CHECK(accepted.size() == 1); // 被拒的一律不触发落盘
}

TEST_CASE("snapshot and initial data come from the same pairs", "[webview][bridge][snapshot]")
{
    b::UiSeed seed;
    seed.role = "input";
    seed.version = "1.2.3";
    seed.lang = "fr";
    seed.uiScale = 0.65f;

    const auto pairs = b::buildUiSeedPairs(seed);
    REQUIRE(pairs.size() == 4);
    CHECK(pairs[0].first == b::Init::Version);
    CHECK(pairs[1].first == b::Init::Role);
    CHECK(pairs[2].first == b::Init::Lang);
    CHECK(pairs[3].first == b::Init::UiScale);

    b::UiFlags flags;
    flags.langChosen = true;
    flags.rulesAck = 4;
    flags.guideSeen = true;
    flags.diagLog = true;
    const auto snapshot = b::buildUiSnapshot(seed, flags);

    for (const auto& pair : pairs)
        CHECK(snapshot.getProperty(juce::Identifier(pair.first), juce::var()) == pair.second);

    CHECK(snapshot.getProperty("role", juce::var()).toString() == "input");
    CHECK(snapshot.getProperty("version", juce::var()).toString() == "1.2.3");
    CHECK(snapshot.getProperty("lang", juce::var()).toString() == "fr");
    CHECK(static_cast<double>(snapshot.getProperty("uiScale", juce::var())) == 0.65);
    CHECK(static_cast<bool>(snapshot.getProperty("langChosen", juce::var())));
    CHECK(static_cast<int>(snapshot.getProperty("rulesAck", juce::var())) == 4);
    CHECK(static_cast<bool>(snapshot.getProperty("guideSeen", juce::var())));
    CHECK(static_cast<bool>(snapshot.getProperty("diagLog", juce::var())));

    // 默认的标志位全是「没做过」
    const auto plain = b::buildUiSnapshot(seed, b::UiFlags{});
    CHECK_FALSE(static_cast<bool>(plain.getProperty("langChosen", juce::var())));
    CHECK(static_cast<int>(plain.getProperty("rulesAck", juce::var())) == 0);
    CHECK_FALSE(static_cast<bool>(plain.getProperty("guideSeen", juce::var())));
    CHECK_FALSE(static_cast<bool>(plain.getProperty("diagLog", juce::var())));
}

TEST_CASE("first-frame payload note validates before use", "[webview][bridge][paint]")
{
    const auto make = [](const juce::String& key, const juce::var& value) {
        auto* obj = new juce::DynamicObject();
        obj->setProperty(juce::Identifier(key), value);
        return juce::var(obj);
    };

    CHECK(b::paintDeltaNote(make("paintDeltaMs", 12), "paintDeltaMs").toStdString() == " (signal-firstPaint +12 ms)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", -3), "paintDeltaMs").toStdString() == " (signal-firstPaint -3 ms)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", 0), "paintDeltaMs").toStdString() == " (signal-firstPaint +0 ms)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", 16.7), "paintDeltaMs").toStdString() == " (signal-firstPaint +16 ms)");

    // 字段名对不上、缺失、类型不对：一律是「没有绘制记录」，与合法的回落路同形，所以字段名由常量统一
    CHECK(b::paintDeltaNote(make("paintDelta", 12), "paintDeltaMs").toStdString() == " (no paint record)");
    CHECK(b::paintDeltaNote(juce::var(), "paintDeltaMs").toStdString() == " (no paint record)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", "12"), "paintDeltaMs").toStdString() == " (no paint record)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", juce::var()), "paintDeltaMs").toStdString() == " (no paint record)");

    // 载荷跨了页面到原生的边界：无穷与非数不能直接转整数
    CHECK(b::paintDeltaNote(make("paintDeltaMs", std::numeric_limits<double>::infinity()), "paintDeltaMs")
              .toStdString() == " (no paint record)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", -std::numeric_limits<double>::infinity()), "paintDeltaMs")
              .toStdString() == " (no paint record)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", std::numeric_limits<double>::quiet_NaN()), "paintDeltaMs")
              .toStdString() == " (no paint record)");

    // 有限但很大的值夹到正负 60 秒
    CHECK(b::paintDeltaNote(make("paintDeltaMs", 1.0e12), "paintDeltaMs").toStdString() ==
          " (signal-firstPaint +60000 ms)");
    CHECK(b::paintDeltaNote(make("paintDeltaMs", -1.0e12), "paintDeltaMs").toStdString() ==
          " (signal-firstPaint -60000 ms)");
}

TEST_CASE("strict bool accepts only bool and integers", "[webview][bridge]")
{
    bool out = false;
    CHECK(b::strictBool(juce::var(true), out));
    CHECK(out);
    CHECK(b::strictBool(juce::var(false), out));
    CHECK_FALSE(out);
    CHECK(b::strictBool(juce::var(1), out));
    CHECK(out);
    CHECK(b::strictBool(juce::var(0), out));
    CHECK_FALSE(out);
    // 字符串 "false" 经 juce::var 的布尔转换会是真，所以字符串一律不收
    CHECK_FALSE(b::strictBool(juce::var("false"), out));
    CHECK_FALSE(b::strictBool(juce::var(), out));
    CHECK_FALSE(b::strictBool(juce::var(0.5), out));
}

TEST_CASE("generic receipts have the contract shape", "[webview][bridge][receipt]")
{
    const auto ok = b::okResponse();
    CHECK(isOk(ok));
    CHECK_FALSE(ok.hasProperty("reason"));

    const auto with = b::okResponseWith("value", 3);
    CHECK(isOk(with));
    CHECK(static_cast<int>(with.getProperty("value", juce::var())) == 3);

    const auto fail = b::failResponse("some-reason");
    CHECK_FALSE(isOk(fail));
    CHECK(reasonOf(fail) == "some-reason");
}

TEST_CASE("common bridge names match the per-role api headers", "[webview][bridge][drift]")
{
    CHECK(std::string(b::Fn::RequestInitialState) == rinne::input::bridge::kFnRequestInitialState);
    CHECK(std::string(b::Fn::SetLang) == rinne::input::bridge::kFnSetLang);
    CHECK(std::string(b::Fn::SetUiScale) == rinne::input::bridge::kFnSetUiScale);
    CHECK(std::string(b::Fn::CommitUiScale) == rinne::input::bridge::kFnCommitUiScale);
    CHECK(std::string(b::Event::State) == rinne::input::bridge::kEvState);

    CHECK(std::string(b::Fn::RequestInitialState) == rinne::output::bridge::kFnRequestInitialState);
    CHECK(std::string(b::Fn::SetLang) == rinne::output::bridge::kFnSetLang);
    CHECK(std::string(b::Fn::SetUiScale) == rinne::output::bridge::kFnSetUiScale);
    CHECK(std::string(b::Fn::CommitUiScale) == rinne::output::bridge::kFnCommitUiScale);
    CHECK(std::string(b::Event::State) == rinne::output::bridge::kEvState);
}

TEST_CASE("snapshot role keys use the init key names the page reads", "[webview][bridge][drift]")
{
    CHECK(std::string(b::Init::Version) == "version");
    CHECK(std::string(b::Init::Lang) == "lang");
    CHECK(std::string(b::Init::UiScale) == "uiScale");
    CHECK(std::string(b::Init::Role) == "role");
    CHECK(std::string(b::Reason::BadScale) == "bad-scale");
    CHECK(std::string(b::Reason::BadLang) == "bad-lang");
    CHECK(std::string(b::Reason::NoPending) == "no-pending");
    CHECK(std::string(b::Reason::ScaleLocked) == "scale-locked");
}
