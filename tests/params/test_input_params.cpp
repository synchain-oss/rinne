// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数层（src/input/InputParams.*）：真实的参数树与参数表逐项对拍，刻度按契约公式核对，每块快照把明文值原样搬进
// EngineParams，限制器关时有效反馈不超过 100 %，快照在音频线程上不分配。
// 参数表在这里按 tests/core/params/ParamsGolden.h 的展开再读一遍（独立于被测代码），versionHint 读 golden 顶层的值。
// 环路延迟 D 的毫秒与样本互换在 test_input_params_delay.cpp。
//
// 契约（docs/contracts/PARAMETERS.md）：
//   · 明文值：state、预设、界面桥一律存明文；APVTS 的原子量读到的就是明文，不是 0…1；
//   · 线性 v = lo + p·(hi − lo)；对数 v = lo·(hi / lo)^p，p = ln(v / lo) / ln(hi / lo)；档位 p = 下标 / (档数 − 1)；
//   · 步进：宿主写来的值取整到 lo + k·step，再夹进范围。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "../../src/input/InputParams.h"
#include "params/ParamsTestProcessor.h"
#include "core/params/ParamsGolden.h"
#include "engine/EngineParams.h"
#include "engine/SafetyMachine.h"
#include "host/shell/AllocationCounter.h"

namespace
{

using namespace rinne::test::params;
using Catch::Approx;
using rinne::engine::DelayMode;
using rinne::engine::EngineParams;
using rinne::engine::SafetyChain;
using rinne::engine::SafetyControls;
using rinne::engine::TailMode;
using rinne::input::InputParamHandles;
using rinne::input::InputParams;

// 处理器加上被测的参数层，和生产里的用法一样：参数层在处理器的构造里建
struct Fixture
{
    ParamsTestProcessor processor;
    InputParams params{processor};

    juce::RangedAudioParameter& parameter(const std::string& id)
    {
        auto* found = dynamic_cast<juce::RangedAudioParameter*>(params.tree().getParameter(id));
        REQUIRE(found != nullptr);
        return *found;
    }

    // 像宿主或界面那样写参数：明文值换成归一化值，经 setValueNotifyingHost 落进参数
    void setPlain(const std::string& id, float plain)
    {
        auto& p = parameter(id);
        p.setValueNotifyingHost(p.convertTo0to1(plain));
    }

    // APVTS 里这个参数的原子量（明文值）
    std::atomic<float>& raw(const std::string& id)
    {
        auto* found = params.tree().getRawParameterValue(id);
        REQUIRE(found != nullptr);
        return *found;
    }

    EngineParams snapshot() const
    {
        EngineParams out;
        params.snapshot(out);
        return out;
    }
};

float goldenVersionHint()
{
    const Golden golden = loadGolden();
    REQUIRE(golden.loaded);
    const auto hint = golden.top.num("versionHint");
    REQUIRE(hint.has_value());
    return static_cast<float>(*hint);
}

// 对数参数的精确值：lo·(hi / lo)^p
double logValue(const Param& p, double proportion)
{
    return p.lo * std::pow(p.hi / p.lo, proportion);
}

// 浮点比较的容差：半个步进，加上 float 表示本身的误差
double tolerance(const Param& p, double expected)
{
    return p.step * 0.5 + 4.0 * static_cast<double>(std::numeric_limits<float>::epsilon()) * std::fabs(expected);
}

// 归一化值上的容差：明文值最多被步进挪动半个步进，折算成归一化值就是这么多（对数刻度的斜率是 1 / (v·ln(hi/lo))）
double proportionTolerance(const Param& p, double plain)
{
    const bool logarithmic = std::string(p.skew) == "log";
    const double slope = logarithmic ? 1.0 / (plain * std::log(p.hi / p.lo)) : 1.0 / (p.hi - p.lo);
    return p.step * 0.5 * slope + 1.0e-5;
}

// 把 EngineParams 里属于参数的字段全部改成与默认值不同
EngineParams offDefault()
{
    EngineParams p;
    p.feedbackPercent = 77.0f;
    p.loopOn = false;
    p.mixPercent = 66.0f;
    p.delayMode = DelayMode::kSync;
    p.delayMs = 1234.0f;
    p.delayNote = 3;
    p.lowCutHz = 555.0f;
    p.highCutHz = 5555.0f;
    p.polarityInvert = true;
    p.limiterOn = false;
    p.ceilingDb = -12.0f;
    return p;
}

// 把 EngineParams 的命令序号与三个工程开关设成非默认值
void setNonParameterFields(EngineParams& p)
{
    p.presetSeq = 7;
    p.stateSeq = 8;
    p.panicSeq = 9;
    p.recoverSeq = 10;
    p.guardOn = false;
    p.resetOnPlay = false;
    p.tailMode = TailMode::kInfinite;
}

// 给安全链喂 0.6 s 的静音（淡入走完、各平滑器落定），返回最后一个样本 ⑥ 用的总增益
float settledReturnGain(const EngineParams& params)
{
    constexpr double kRate = 48000.0;
    SafetyChain chain;
    chain.prepare(kRate);
    SafetyControls controls;
    controls.paired = true;
    // 引擎做的也是这一步：百分数换成 0…1.5 的比值，之后由安全链按限制器开关给上限
    controls.feedback = params.feedbackPercent / 100.0f;
    controls.returnGain = 1.0f;
    controls.loopOn = params.loopOn;
    controls.limiterOn = params.limiterOn;
    controls.ceilingDb = params.ceilingDb;
    chain.beginBlock(controls);
    for (int i = 0; i < static_cast<int>(0.6 * kRate); ++i)
    {
        float left = 0.0f;
        float right = 0.0f;
        chain.processFront(left, right, true);
        chain.processBack(left, right, 0.0f, 0.0f);
    }
    return chain.lastGain();
}

} // namespace

TEST_CASE("the parameter tree matches the parameter table item by item", "[params][input-params][layout]")
{
    Fixture f;
    const auto& params = f.processor.getParameters();
    REQUIRE(params.size() == static_cast<int>(table().size()));
    REQUIRE(rinne::input::kParamCount == table().size());
    CHECK(f.params.tree().state.getType() == juce::Identifier("PARAMS"));
    CHECK(f.params.isBound());
    const float hint = goldenVersionHint();

    for (const Param& p : table())
    {
        INFO("参数 " << p.id);
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(params[p.index]);
        REQUIRE(hosted != nullptr);
        // 序号、ID、宿主名、可自动化与否
        CHECK(hosted->getParameterIndex() == p.index);
        CHECK(hosted->getParameterID() == juce::String(p.id));
        CHECK(hosted->getName(64) == juce::String(p.name));
        CHECK(hosted->isAutomatable() == p.automatable);
        CHECK(static_cast<float>(hosted->getVersionHint()) == hint);
        // 参数树按同一个 ID 找到的是同一个参数对象
        CHECK(f.params.tree().getParameter(p.id) == hosted);

        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(hosted);
        REQUIRE(ranged != nullptr);
        const juce::NormalisableRange<float>& range = ranged->getNormalisableRange();
        CHECK(range.start == static_cast<float>(p.lo));
        CHECK(range.end == static_cast<float>(p.hi));
        CHECK(range.interval == static_cast<float>(p.step));
        // 默认值（落在步进格上）。参数树里的原子量一开始就等于它：构造时写回过默认值，不留 float 的零头
        const double def = ranged->convertFrom0to1(ranged->getDefaultValue());
        if (p.kind == 'F')
        {
            CHECK(def == Approx(p.def).margin(tolerance(p, p.def)));
        }
        else
        {
            CHECK(def == p.def);
        }
        CHECK(static_cast<double>(f.raw(p.id).load()) == p.def);

        if (p.kind == 'F')
        {
            CHECK(dynamic_cast<juce::AudioParameterFloat*>(hosted) != nullptr);
            // 单位作为参数的标签带给宿主
            CHECK(hosted->getLabel() == juce::String(unitText(p.unit).c_str()));
        }
        else if (p.kind == 'B')
        {
            auto* flag = dynamic_cast<juce::AudioParameterBool*>(hosted);
            REQUIRE(flag != nullptr);
            CHECK(flag->get() == (p.def != 0.0));
        }
        else
        {
            auto* choice = dynamic_cast<juce::AudioParameterChoice*>(hosted);
            REQUIRE(choice != nullptr);
            // 档位文字与先后取表里的 RINNE_CHOICE 行
            const auto labels = choicesOf(p.id);
            REQUIRE(choice->choices.size() == static_cast<int>(labels.size()));
            for (std::size_t i = 0; i < labels.size(); ++i)
            {
                CHECK(choice->choices[static_cast<int>(i)] == juce::String(labels[i]));
            }
            CHECK(choice->getIndex() == static_cast<int>(p.def));
            CHECK(hosted->getNumSteps() == static_cast<int>(labels.size()));
        }
    }
}

TEST_CASE("every parameter carries the version hint recorded in the golden", "[params][input-params][version-hint]")
{
    // 契约：现有 11 个参数的 versionHint 都是 golden 顶层记的那个值。读 golden，不写常量
    const float hint = goldenVersionHint();
    CHECK(hint >= 1.0f);
    Fixture f;
    int seen = 0;
    for (auto* param : f.processor.getParameters())
    {
        INFO("参数 " << param->getName(64));
        CHECK(static_cast<float>(param->getVersionHint()) == hint);
        ++seen;
    }
    CHECK(seen == static_cast<int>(table().size()));
    // 具名常量也要和 golden 对得上
    CHECK(static_cast<float>(rinne::input::kParamVersionHint) == hint);
    // 单独建出的参数也是
    for (std::size_t i = 0; i < rinne::input::kParamCount; ++i)
    {
        const auto param = rinne::input::makeInputParameter(i);
        REQUIRE(param != nullptr);
        CHECK(static_cast<float>(param->getVersionHint()) == hint);
    }
    CHECK(rinne::input::makeInputParameter(rinne::input::kParamCount) == nullptr);
}

TEST_CASE("linear parameters map to 0..1 proportionally, logarithmic ones do not", "[params][input-params][scale]")
{
    Fixture f;
    // 线性：25 % 的混合在 0.25 的位置
    CHECK(f.parameter("mix").convertTo0to1(25.0f) == 0.25f);
    CHECK(f.parameter("mix").convertFrom0to1(0.25f) == 25.0f);
    CHECK(f.parameter("feedback").convertTo0to1(75.0f) == 0.5f);
    CHECK(f.parameter("ceiling").convertTo0to1(-6.0f) == 0.75f);
    CHECK(f.parameter("ceiling").convertFrom0to1(0.5f) == -12.0f);

    for (const Param& p : table())
    {
        if (p.kind != 'F')
        {
            continue;
        }
        INFO("参数 " << p.id);
        auto& param = f.parameter(p.id);
        const bool logarithmic = std::string(p.skew) == "log";
        // 两端：精确落在端点上
        CHECK(param.convertFrom0to1(0.0f) == static_cast<float>(p.lo));
        CHECK(param.convertFrom0to1(1.0f) == static_cast<float>(p.hi));
        CHECK(param.convertTo0to1(static_cast<float>(p.lo)) == 0.0f);
        CHECK(param.convertTo0to1(static_cast<float>(p.hi)) == 1.0f);
        // 越界的明文值夹到端点
        CHECK(param.convertTo0to1(static_cast<float>(p.lo - 1.0)) == 0.0f);
        CHECK(param.convertTo0to1(static_cast<float>(p.hi + 1.0)) == 1.0f);

        for (const double proportion : {0.1, 0.25, 0.5, 0.75, 0.9})
        {
            const double expected = logarithmic ? logValue(p, proportion) : p.lo + proportion * (p.hi - p.lo);
            INFO("p = " << proportion);
            const double plain = param.convertFrom0to1(static_cast<float>(proportion));
            CHECK(plain == Approx(expected).margin(tolerance(p, expected)));
            // 反向：明文值换回归一化值。步进格的取整最多挪动半个步进，折算成归一化值的误差也很小
            const double back = param.convertTo0to1(static_cast<float>(expected));
            CHECK(back == Approx(proportion).margin(proportionTolerance(p, expected)));
        }

        // 线性的中点是算术中点，对数的中点是几何中点；两者在这些范围里差得很远
        const double arithmeticMid = 0.5 * (p.lo + p.hi);
        const double pAtArithmeticMid = param.convertTo0to1(static_cast<float>(arithmeticMid));
        if (logarithmic)
        {
            CHECK(std::fabs(pAtArithmeticMid - 0.5) > 0.2);
        }
        else
        {
            CHECK(pAtArithmeticMid == Approx(0.5).margin(1.0e-6));
        }
    }

    // 契约写明的三个对数中点：delay_ms 约 63.2 ms，low_cut 200 Hz，high_cut 约 4472 Hz
    CHECK(f.parameter("delay_ms").convertFrom0to1(0.5f) == Approx(63.2).margin(0.05));
    CHECK(f.parameter("low_cut").convertFrom0to1(0.5f) == Approx(200.0).margin(0.05));
    CHECK(f.parameter("high_cut").convertFrom0to1(0.5f) == Approx(4472.0).margin(0.5));
    // 100 ms 在 delay_ms 的位置：ln(100) / ln(4000)
    CHECK(f.parameter("delay_ms").convertTo0to1(100.0f) == Approx(std::log(100.0) / std::log(4000.0)).margin(1.0e-6));
}

TEST_CASE("steps land on lo + k*step in plain values, also on the logarithmic scales", "[params][input-params][step]")
{
    Fixture f;
    for (const Param& p : table())
    {
        if (p.kind != 'F')
        {
            continue;
        }
        INFO("参数 " << p.id);
        const auto& range = f.parameter(p.id).getNormalisableRange();
        // 格点上的值原样不动；格间的值落到最近的格点；中点以下进位前的那一侧向下
        for (const int k : {0, 1, 3, 500})
        {
            const double grid = p.lo + k * p.step;
            if (grid > p.hi)
            {
                continue;
            }
            const double tol = tolerance(p, grid) * 0.01;
            CHECK(range.snapToLegalValue(static_cast<float>(grid)) == Approx(grid).margin(tol));
            CHECK(range.snapToLegalValue(static_cast<float>(grid + 0.3 * p.step)) == Approx(grid).margin(tol));
            if (grid + p.step <= p.hi)
            {
                CHECK(range.snapToLegalValue(static_cast<float>(grid + 0.7 * p.step)) ==
                      Approx(grid + p.step).margin(tol));
            }
        }
        // 范围之外夹到端点
        CHECK(range.snapToLegalValue(static_cast<float>(p.lo - 5.0 * p.step)) == static_cast<float>(p.lo));
        CHECK(range.snapToLegalValue(static_cast<float>(p.hi + 5.0 * p.step)) == static_cast<float>(p.hi));
    }

    // 对数参数的几个具体格点：步进不随位置变化（0.001 ms、0.1 Hz、1 Hz）
    const auto& delay = f.parameter("delay_ms").getNormalisableRange();
    CHECK(delay.snapToLegalValue(100.0004f) == 100.0f);
    CHECK(delay.snapToLegalValue(100.0006f) == Approx(100.001).margin(1.0e-5));
    CHECK(delay.snapToLegalValue(3000.0004f) == Approx(3000.0).margin(0.0005));
    const auto& lowCut = f.parameter("low_cut").getNormalisableRange();
    CHECK(lowCut.snapToLegalValue(200.04f) == Approx(200.0).margin(1.0e-3));
    CHECK(lowCut.snapToLegalValue(200.06f) == Approx(200.1).margin(1.0e-3));
    const auto& highCut = f.parameter("high_cut").getNormalisableRange();
    CHECK(highCut.snapToLegalValue(4472.136f) == 4472.0f);
    CHECK(highCut.snapToLegalValue(4472.6f) == 4473.0f);

    // 宿主写进来的归一化值经参数落成格点上的明文值
    auto& highCutParam = f.parameter("high_cut");
    highCutParam.setValueNotifyingHost(0.5f);
    CHECK(f.raw("high_cut").load() == 4472.0f);
}

TEST_CASE("choice and bool parameters use index / (steps - 1) as their proportion", "[params][input-params][scale]")
{
    Fixture f;
    for (const Param& p : table())
    {
        if (p.kind == 'F')
        {
            continue;
        }
        INFO("参数 " << p.id);
        auto& param = f.parameter(p.id);
        const int steps = static_cast<int>(p.hi) + 1;
        for (int index = 0; index < steps; ++index)
        {
            const float proportion = static_cast<float>(index) / static_cast<float>(steps - 1);
            CHECK(param.convertTo0to1(static_cast<float>(index)) == proportion);
            CHECK(param.convertFrom0to1(proportion) == static_cast<float>(index));
        }
    }
}

TEST_CASE("the snapshot carries plain values into EngineParams unchanged", "[params][input-params][snapshot]")
{
    Fixture f;
    f.setPlain("feedback", 150.0f);
    f.setPlain("mix", 25.0f);
    f.setPlain("loop_on", 0.0f);
    f.setPlain("delay_mode", 1.0f);
    f.setPlain("delay_ms", 250.0f);
    f.setPlain("delay_note", 3.0f);
    f.setPlain("low_cut", 200.0f);
    f.setPlain("high_cut", 4000.0f);
    f.setPlain("polarity", 1.0f);
    f.setPlain("limiter", 0.0f);
    f.setPlain("ceiling", -12.0f);

    const EngineParams p = f.snapshot();
    // 明文原样搬运：150 % 就是 150，不是 1.5；25 % 就是 25，不是 0.25，也不是归一化后再换算一遍
    CHECK(p.feedbackPercent == 150.0f);
    CHECK(p.mixPercent == 25.0f);
    CHECK(p.ceilingDb == -12.0f);
    CHECK(p.delayMs == Approx(250.0).margin(0.002));
    CHECK(p.lowCutHz == Approx(200.0).margin(0.1));
    CHECK(p.highCutHz == Approx(4000.0).margin(1.0));
    CHECK_FALSE(p.loopOn);
    CHECK(p.delayMode == DelayMode::kSync);
    CHECK(p.delayNote == 3);
    CHECK(p.polarityInvert);
    CHECK_FALSE(p.limiterOn);

    // 搬的就是参数树里的原子量
    CHECK(p.delayMs == f.raw("delay_ms").load());
    CHECK(p.lowCutHz == f.raw("low_cut").load());
    CHECK(p.highCutHz == f.raw("high_cut").load());
    CHECK(p.mixPercent == f.raw("mix").load());

    // 再改一遍，快照跟着变
    f.setPlain("feedback", 0.0f);
    f.setPlain("mix", 100.0f);
    f.setPlain("delay_note", 15.0f);
    f.setPlain("delay_mode", 0.0f);
    f.setPlain("loop_on", 1.0f);
    const EngineParams q = f.snapshot();
    CHECK(q.feedbackPercent == 0.0f);
    CHECK(q.mixPercent == 100.0f);
    CHECK(q.delayNote == 15);
    CHECK(q.delayMode == DelayMode::kTime);
    CHECK(q.loopOn);
}

TEST_CASE("a fresh parameter tree snapshots to the engine defaults", "[params][input-params][snapshot][defaults]")
{
    Fixture f;
    const EngineParams p = f.snapshot();
    const EngineParams d; // 引擎侧的默认值：反馈 0、限制器开、上限 -6、混合 25 ……
    CHECK(p.feedbackPercent == d.feedbackPercent);
    CHECK(p.loopOn == d.loopOn);
    CHECK(p.mixPercent == d.mixPercent);
    CHECK(p.delayMode == d.delayMode);
    CHECK(p.delayMs == d.delayMs); // 对数刻度上的 100 ms 也是精确的 100，不是 99.99999
    CHECK(p.delayNote == d.delayNote);
    CHECK(p.lowCutHz == d.lowCutHz);
    CHECK(p.highCutHz == d.highCutHz);
    CHECK(p.polarityInvert == d.polarityInvert);
    CHECK(p.limiterOn == d.limiterOn);
    CHECK(p.ceilingDb == d.ceilingDb);
    // 默认安全：反馈为 0，限制器开着
    CHECK(p.feedbackPercent == 0.0f);
    CHECK(p.limiterOn);
}

TEST_CASE("the snapshot rounds switches and steps at their boundaries", "[params][input-params][snapshot][rounding]")
{
    Fixture f;
    EngineParams p;
    // 开关：0.5 起为真
    f.raw("polarity").store(std::nextafter(0.5f, 0.0f));
    f.params.snapshot(p);
    CHECK_FALSE(p.polarityInvert);
    f.raw("polarity").store(0.5f);
    f.params.snapshot(p);
    CHECK(p.polarityInvert);
    f.raw("loop_on").store(0.0f);
    f.raw("limiter").store(1.0f);
    f.params.snapshot(p);
    CHECK_FALSE(p.loopOn);
    CHECK(p.limiterOn);

    // 档位：四舍五入到下标，0.5 进位
    f.raw("delay_note").store(std::nextafter(9.5f, 0.0f));
    f.params.snapshot(p);
    CHECK(p.delayNote == 9);
    f.raw("delay_note").store(9.5f);
    f.params.snapshot(p);
    CHECK(p.delayNote == 10);
    f.raw("delay_note").store(std::nextafter(0.5f, 0.0f)); // 0.49999997 + 0.5 在 float 里会进位成 1，不能那样取整
    f.params.snapshot(p);
    CHECK(p.delayNote == 0);
    f.raw("delay_mode").store(std::nextafter(0.5f, 1.0f));
    f.params.snapshot(p);
    CHECK(p.delayMode == DelayMode::kSync);
    f.raw("delay_mode").store(0.4f);
    f.params.snapshot(p);
    CHECK(p.delayMode == DelayMode::kTime);

    // 越界与非数：夹进 0…档数−1，NaN 取 0 / 假
    f.raw("delay_note").store(15.4f);
    f.params.snapshot(p);
    CHECK(p.delayNote == 15);
    f.raw("delay_note").store(99.0f);
    f.params.snapshot(p);
    CHECK(p.delayNote == 15);
    f.raw("delay_note").store(-3.0f);
    f.params.snapshot(p);
    CHECK(p.delayNote == 0);
    f.raw("delay_note").store(std::numeric_limits<float>::quiet_NaN());
    f.raw("delay_mode").store(std::numeric_limits<float>::quiet_NaN());
    f.raw("polarity").store(std::numeric_limits<float>::quiet_NaN());
    f.params.snapshot(p);
    CHECK(p.delayNote == 0);
    CHECK(p.delayMode == DelayMode::kTime);
    CHECK_FALSE(p.polarityInvert);
}

TEST_CASE("the snapshot writes only the eleven parameter fields", "[params][input-params][snapshot]")
{
    Fixture f;
    f.setPlain("feedback", 40.0f);
    f.setPlain("mix", 80.0f);
    EngineParams p = offDefault();
    setNonParameterFields(p);
    f.params.snapshot(p);
    // 参数字段被覆盖
    CHECK(p.feedbackPercent == 40.0f);
    CHECK(p.mixPercent == 80.0f);
    CHECK(p.loopOn);
    CHECK(p.limiterOn);
    CHECK(p.ceilingDb == -6.0f);
    CHECK(p.lowCutHz == 20.0f);
    // 命令序号与三个工程开关不归参数层管：原样留着
    CHECK(p.presetSeq == 7);
    CHECK(p.stateSeq == 8);
    CHECK(p.panicSeq == 9);
    CHECK(p.recoverSeq == 10);
    CHECK_FALSE(p.guardOn);
    CHECK_FALSE(p.resetOnPlay);
    CHECK(p.tailMode == TailMode::kInfinite);
}

TEST_CASE("binding fails loudly when the tree lacks a parameter", "[params][input-params][bind]")
{
    ParamsTestProcessor processor;
    // 少了最后一个参数（ceiling）的参数树
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (std::size_t i = 0; i + 1 < rinne::input::kParamCount; ++i)
    {
        layout.add(rinne::input::makeInputParameter(i));
    }
    juce::AudioProcessorValueTreeState partial{processor, nullptr, "PARAMS", std::move(layout)};

    InputParamHandles handles;
    CHECK_FALSE(handles.isBound());
    CHECK_FALSE(handles.bind(partial));
    CHECK_FALSE(handles.isBound());
    // 没绑定就不写：调用方给的值原样留着
    EngineParams p = offDefault();
    handles.snapshot(p);
    const EngineParams expected = offDefault();
    CHECK(p.feedbackPercent == expected.feedbackPercent);
    CHECK(p.mixPercent == expected.mixPercent);
    CHECK(p.delayMs == expected.delayMs);
    CHECK(p.loopOn == expected.loopOn);
    CHECK(p.ceilingDb == expected.ceilingDb);
    // 默认构造的 EngineParams 经未绑定的快照仍是安全默认
    EngineParams safe;
    handles.snapshot(safe);
    CHECK(safe.feedbackPercent == 0.0f);
    CHECK(safe.limiterOn);

    // 换成完整的参数树就能绑上，之前失败留下的状态不影响
    Fixture full;
    CHECK(handles.bind(full.params.tree()));
    CHECK(handles.isBound());
}

TEST_CASE("with the limiter off the effective feedback stays at or below 100 percent", "[params][input-params][safety]")
{
    Fixture f;
    f.setPlain("feedback", 150.0f);
    f.setPlain("limiter", 0.0f);
    const EngineParams off = f.snapshot();
    // 映射层不夹：设定值照样是 150 %
    CHECK(off.feedbackPercent == 150.0f);
    CHECK_FALSE(off.limiterOn);
    // 有效值由引擎里的安全链夹：限制器关时不超过 1.0
    const float effectiveOff = settledReturnGain(off);
    CHECK(effectiveOff <= 1.0f);
    CHECK(effectiveOff == 1.0f);

    // 对照：限制器开着时同样的 150 % 可以到 1.5，说明上面的 1.0 来自限制器开关，不是别处夹的
    f.setPlain("limiter", 1.0f);
    const EngineParams on = f.snapshot();
    CHECK(on.feedbackPercent == 150.0f);
    CHECK(on.limiterOn);
    CHECK(settledReturnGain(on) == 1.5f);
}

TEST_CASE("taking the snapshot does not allocate", "[params][input-params][realtime]")
{
    Fixture f;
    f.setPlain("feedback", 33.0f);
    EngineParams p;
    f.params.snapshot(p); // 预热
    long allocations = -1;
    long frees = -1;
    float checksum = 0.0f;
    {
        const rinne::test::AllocationScope scope;
        for (int i = 0; i < 1000; ++i)
        {
            f.params.snapshot(p);
            checksum += p.feedbackPercent;
        }
        allocations = scope.allocations();
        frees = scope.frees();
    }
    CHECK(allocations == 0);
    CHECK(frees == 0);
    CHECK(checksum == Approx(33000.0).margin(1.0));
}
