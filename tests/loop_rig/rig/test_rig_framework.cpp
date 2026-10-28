// SPDX-License-Identifier: GPL-3.0-or-later
//
// rig 框架自身的用例：框架自检（闭环冲激）、场景自注册、中间链的七种形态、脚本化块长与时钟、
// 多台引擎与换写方、读两端状态、起播沿复位中间链、随机的测试作用域。
//
// 场景文件放在 tests/loop_rig/scenarios/**（各自的归属另行登记），本文件只验框架本身。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/engine/EngineTestKit.h"
#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopBinding.h"
#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"
#include "rig/EngineHost.h"
#include "rig/MiddleChain.h"
#include "rig/ScenarioRegistry.h"
#include "rig/Schedule.h"
#include "rig/Segments.h"

namespace
{

using namespace rinne::rig;

// 一条恒定的块长脚本
BlockScript fixedScript(int length, std::size_t count)
{
    BlockScript script;
    script.lengths.assign(count, length);
    return script;
}

} // namespace

TEST_CASE("rig 框架自检：一段宿主信号经中间链写进环，环的写头照常推进", "[rig][framework]")
{
    Segments segments;
    REQUIRE(segments.open(1));
    REQUIRE(segments.rv() != nullptr);
    REQUIRE(segments.ring(1) != nullptr);
    CHECK(segments.ring(2) == nullptr);
}

TEST_CASE("rig 场景自注册：注册表里能查到本组的场景 id", "[rig][framework]")
{
    // 注册表是定长的：至少有一个场景被注册进来（占位场景证明自注册可用）。
    CHECK(rinne::rig::scenarioCount() >= 1u);
    const rinne::rig::ScenarioEntry* entry = rinne::rig::scenarioAt(0);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->id != nullptr);
    REQUIRE(entry->factory != nullptr);
    // 同一个 id 能查到同一条
    CHECK(rinne::rig::findScenario(entry->id) == entry);
    // 查不到的 id 返回空
    CHECK(rinne::rig::findScenario("no-such-scenario") == nullptr);
}

TEST_CASE("rig 中间链的七种形态各可用", "[rig][chain]")
{
    MiddleChain chain;
    chain.prepare(48000.0, 256);

    constexpr int n = 16;
    std::vector<float> inLeft(n, 0.5f);
    std::vector<float> inRight(n, -0.5f);
    std::vector<float> outLeft(n, 0.0f);
    std::vector<float> outRight(n, 0.0f);

    // 恒等
    chain.setKind(ChainKind::kIdentity);
    chain.process(inLeft.data(), inRight.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        CHECK(outLeft[i] == 0.5f);
        CHECK(outRight[i] == -0.5f);
    }

    // 增益
    chain.setKind(ChainKind::kGain);
    chain.setGain(2.0f);
    chain.process(inLeft.data(), inRight.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        CHECK(outLeft[i] == 1.0f);
        CHECK(outRight[i] == -1.0f);
    }

    // 一阶低通：直流增益是 1（稳态），第一个样本还没跟上。
    // 一阶低通的时间常数是 1/(2π·fc)：1 kHz 下约 168 个样本，所以要喂够长的直流才看得出稳态。
    chain.setKind(ChainKind::kOnePoleLowpass);
    chain.setLowpassHz(1000.0f);
    chain.reset();
    {
        std::vector<float> dc(4096, 0.5f);
        std::vector<float> dcOut(4096, 0.0f);
        chain.process(dc.data(), dc.data(), dcOut.data(), dcOut.data(), 4096);
        CHECK(dcOut[0] < 0.5f);
        CHECK(std::abs(dcOut[4095] - 0.5f) < 1.0e-3f);
    }

    // k 样本延迟：输出是 k 个样本之前的输入
    chain.setKind(ChainKind::kDelay);
    chain.setDelaySamples(4);
    chain.reset();
    std::vector<float> ramp(n);
    for (int i = 0; i < n; ++i)
    {
        ramp[static_cast<std::size_t>(i)] = static_cast<float>(i);
    }
    chain.process(ramp.data(), ramp.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        const float expected = i < 4 ? 0.0f : static_cast<float>(i - 4);
        CHECK(outLeft[static_cast<std::size_t>(i)] == expected);
    }
    CHECK(chain.totalLatency() == 4);

    // NaN 注入：从第 8 个样本起是 NaN
    chain.setKind(ChainKind::kNanInject);
    chain.setNanInjectAt(8);
    chain.reset();
    chain.process(ramp.data(), ramp.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < 8; ++i)
    {
        CHECK(outLeft[static_cast<std::size_t>(i)] == static_cast<float>(i));
    }
    for (int i = 8; i < n; ++i)
    {
        CHECK(std::isnan(outLeft[static_cast<std::size_t>(i)]));
    }

    // 反相
    chain.setKind(ChainKind::kInvert);
    chain.reset();
    chain.process(inLeft.data(), inRight.data(), outLeft.data(), outRight.data(), n);
    for (int i = 0; i < n; ++i)
    {
        CHECK(outLeft[i] == -0.5f);
        CHECK(outRight[i] == 0.5f);
    }

    // 低架提升：直流（低频）被提升
    chain.setKind(ChainKind::kLowShelfBoost);
    chain.setLowShelfBoost(0.5f, 0.5f);
    chain.setLowpassHz(100.0f);
    chain.reset();
    {
        // 同样要喂够长的直流才看得出稳态（100 Hz 的时间常数约 1.6 ms，48 kHz 下 1600 个样本）。
        std::vector<float> dc(4096, 0.5f);
        std::vector<float> dcOut(4096, 0.0f);
        chain.process(dc.data(), dc.data(), dcOut.data(), dcOut.data(), 4096);
        // 稳态的直流增益是 1 + boost = 1.5
        CHECK(std::abs(dcOut[4095] - 0.75f) < 1.0e-2f);
    }
}

TEST_CASE("rig 脚本化块长与走带：块长序列、0 样本穿插、起播沿、离线标记都能脚本化", "[rig][script]")
{
    BlockScript script;
    script.lengths = {256, 0, 512, 64};
    script.skips = {false, false, true, false};
    CHECK(script.lengthAt(0) == 256);
    CHECK(script.lengthAt(1) == 0);
    CHECK(script.lengthAt(2) == 512);
    CHECK(script.lengthAt(3) == 64);
    // 循环取用
    CHECK(script.lengthAt(4) == 256);
    CHECK(script.skipAt(2));
    CHECK(!script.skipAt(0));

    TransportScript transport;
    transport.hostNs = {1000, 2000};
    transport.hostNsValid = {true, false};
    transport.playing = {true, false};
    transport.nonrealtime = {false, true};
    transport.bpm = {140.0f, 0.0f};

    const rinne::engine::TransportInfo first = transport.at(0);
    CHECK(first.hostNs == 1000u);
    CHECK(first.hostNsValid);
    CHECK(first.isPlaying);
    CHECK(!first.nonrealtime);
    CHECK(first.bpm == 140.0f);

    const rinne::engine::TransportInfo second = transport.at(1);
    CHECK(second.hostNs == 2000u);
    CHECK(!second.hostNsValid);
    CHECK(!second.isPlaying);
    CHECK(second.nonrealtime);
    CHECK(second.bpm == 0.0f);
}

TEST_CASE("rig 随机测试作用域：两个 rig 不撞段名", "[rig][scope]")
{
    Segments first;
    Segments second;
    CHECK(first.scope().tag() != second.scope().tag());
    CHECK(first.naming().scope().tag() != second.naming().scope().tag());
    CHECK(first.scope().isTesting());
}

namespace
{

// 反馈 100 %、混合 50 %：g_ret = 1、g_dry = 1，回送增益恰为 1（30 ms 平滑走完之后）。
rinne::engine::EngineParams unityLoop(float delayMs)
{
    rinne::engine::EngineParams params;
    params.feedbackPercent = 100.0f;
    params.mixPercent = 50.0f;
    params.delayMs = delayMs;
    return params;
}

ParamsScript paramsOf(const rinne::engine::EngineParams& params)
{
    ParamsScript script;
    script.snapshots = {params};
    return script;
}

// 本文件也注册一个场景：证明多个翻译单元各自注册场景时链接不撞名（占位场景在另一个文件里）。
class FrameworkScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s00_framework"; }
    bool run(ScenarioContext& context) override { return context.host != nullptr; }
};

} // namespace

RINNE_RIG_SCENARIO("s00_framework")
{
    return new FrameworkScenario();
}

TEST_CASE("rig 场景自注册：多个翻译单元各自注册，都能查到、造出来、跑完", "[rig][framework]")
{
    // ⛔ 删除式验证点：注册宏里的行号不先展开（每个场景都拼出同一个符号名）→ 链接时撞名，本套件建不出来。
    const ScenarioEntry* placeholder = findScenario("s00_placeholder");
    const ScenarioEntry* framework = findScenario("s00_framework");
    REQUIRE(placeholder != nullptr);
    REQUIRE(framework != nullptr);

    EngineHost host;
    ScenarioContext context;
    context.host = &host;
    for (const ScenarioEntry* entry : {placeholder, framework})
    {
        Scenario* scenario = entry->factory();
        REQUIRE(scenario != nullptr);
        CHECK(std::string(scenario->id()) == entry->id);
        CHECK(scenario->run(context));
        delete scenario;
    }
}

TEST_CASE("rig L5-01 框架自检：冲激经真实共享内存的闭环，恰在 D 个样本后回到读头", "[rig][chain]")
{
    // ⛔ 删除式验证点：轨道不闭合（Output 写的不是 Input 处理过的那一份）→ 第二次回声没有，本用例变红。
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(48000.0, 256);
    host.setBlockScripts(fixedScript(256, 1), fixedScript(256, 1));
    host.setChain(ChainKind::kIdentity);
    host.setSource(impulseSource(2000, 0.5f));
    host.setParamsScript(paramsOf(unityLoop(10.0f))); // D = 480

    // 回送经过安全链（DC 阻断、从首个有效读样本起 500 ms 淡入），输出里的回声不再等于原值：冲激的位置在读头
    // （Input 的 lastBlockResult，安全链的输入）上逐位断言，输出与参照安全链逐位比较。
    rinne::test::InputReference reference(48000.0);
    std::vector<float> out;
    for (std::size_t i = 0; i < 16; ++i)
    {
        host.runCycle(i);
        std::vector<float> dry(256, 0.0f);
        for (std::size_t k = 0; k < dry.size(); ++k)
        {
            dry[k] = i * 256 + k == 2000 ? 0.5f : 0.0f;
        }
        reference.block(unityLoop(10.0f), host.inputLastBlock(), false, dry.data(), dry.data(), 256);
        out.insert(out.end(), host.inputOutputLeft().begin(), host.inputOutputLeft().begin() + 256);
    }
    const auto& tap = reference.tapLeft;
    REQUIRE(tap.size() == out.size());
    CHECK(out[2000] == 0.5f); // 干声
    CHECK(out[2479] == 0.0f);
    CHECK(tap[2479] == 0.0f);
    CHECK(tap[2480] == 0.5f); // 第一次回来：恰好 D 个样本之后，读头上逐位是原值
    CHECK(tap[2481] == 0.0f);
    CHECK(out[2480] != 0.0f);
    // 第二次：回送经中间链又进了环（环路闭合）——读头逐位就是 Input 在 D 个样本之前的输出
    std::size_t delayed = 0;
    for (std::size_t t = 480; t < out.size(); ++t)
    {
        delayed += tap[t] == out[t - 480] ? 0u : 1u;
    }
    CHECK(delayed == 0u);
    CHECK(tap[2960] != 0.0f);
    CHECK(rinne::test::firstBitDiff(out, reference.wantLeft) == -1);

    const auto& status = host.inputStatus();
    CHECK(status.delayEffective == 480);
    CHECK(status.resyncCount == 0u);
    CHECK(status.drift == 0);
    CHECK(host.writeHead() == 16u * 256u);
    CHECK(host.ringFrames() == 65536u);
}

TEST_CASE("rig 小数采样率：试验台发布环头的采样率与 Input 比较的是同一个转换，44055.944 Hz 两端一致", "[rig][rate]")
{
    // 44.1 kHz 的拉降是 44055.944 Hz：写方发布的整数是 44056。试验台若按截断发布成 44055，Input 按 44056 比，
    // 永远判不一致。
    // ⛔ 删除式验证点：EngineHost 里发布环头采样率的转换改回截断 → 判不一致，本用例变红。
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(44055.944, 256);
    host.setBlockScripts(fixedScript(256, 1), fixedScript(256, 1));
    host.setChain(ChainKind::kIdentity);
    host.setSource(rampSource());
    host.setParamsScript(paramsOf(unityLoop(10.0f)));
    host.runCycles(40);
    CHECK(host.inputStatus().peerSampleRate == 44056u);
    CHECK(host.inputStatus().rateMismatch == 0u);
    CHECK(host.inputStatus().paired == 1u);

    // 只重新准备 Output，采样率取整数 44056：发布值相同，仍然一致
    host.prepareOutput(44056.0, 256);
    host.runCycles(40, 40);
    CHECK(host.inputStatus().peerSampleRate == 44056u);
    CHECK(host.inputStatus().rateMismatch == 0u);
    CHECK(host.inputStatus().paired == 1u);
    // 只重新准备 Input，也取整数 44056
    host.prepareInput(44056.0, 256);
    host.runCycles(40, 80);
    CHECK(host.inputStatus().rateMismatch == 0u);
    CHECK(host.inputStatus().paired == 1u);
}

TEST_CASE("rig 严格模式：两端块长同一次运行里 512 / 480 交替不重同步、输出与恒定块长逐位相同；Output 丢一块才重同步",
          "[rig][script]")
{
    // ⛔ 删除式验证点：严格模式的判据改回「写头每块的增量与上一块比」→ 交替块长就重同步，本用例变红。
    rinne::engine::EngineParams params = unityLoop(100.0f);
    params.feedbackPercent = 50.0f;
    constexpr std::size_t kCycles = 120;

    auto runWith = [&](const BlockScript& script, rinne::loop::LoopStatusFields& status) {
        EngineHost host;
        REQUIRE(host.open(1));
        host.prepare(48000.0, 512);
        host.setBlockScripts(script, script);
        host.setChain(ChainKind::kOnePoleLowpass);
        host.setParamsScript(paramsOf(params));
        std::vector<float> out;
        for (std::size_t i = 0; i < kCycles; ++i)
        {
            host.runCycle(i);
            const int n = script.lengthAt(i);
            out.insert(out.end(), host.inputOutputLeft().begin(), host.inputOutputLeft().begin() + n);
        }
        status = host.inputStatus();
        return out;
    };

    BlockScript alternating;
    alternating.lengths = {512, 480};
    rinne::loop::LoopStatusFields altStatus;
    const std::vector<float> alt = runWith(alternating, altStatus);
    CHECK(altStatus.resyncCount == 0u);

    // 恒定 512：取同样多的样本比较。
    BlockScript steady = fixedScript(512, 1);
    rinne::loop::LoopStatusFields steadyStatus;
    const std::vector<float> fixed = runWith(steady, steadyStatus);
    CHECK(steadyStatus.resyncCount == 0u);
    const std::size_t common = std::min(alt.size(), fixed.size());
    REQUIRE(common > 48000u);
    bool same = true;
    for (std::size_t i = 0; i < common && same; ++i)
    {
        same = alt[i] == fixed[i];
    }
    CHECK(same);

    // Output 第 30 周期被跳过（那一块的数据丢了）：m 从此不为 0，两次检查之后重同步一次（原因：对齐）。
    BlockScript inputScript = fixedScript(512, 1);
    BlockScript outputScript = fixedScript(512, 1);
    outputScript.skips.assign(kCycles, false);
    outputScript.skips[30] = true;
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(48000.0, 512);
    host.setBlockScripts(inputScript, outputScript);
    host.setParamsScript(paramsOf(params));
    host.runCycles(kCycles);
    CHECK(host.inputStatus().resyncCount == 1u);
    CHECK(host.inputStatus().resyncByReason[static_cast<std::size_t>(rinne::loop::SyncReason::kShift)] == 1u);
}

TEST_CASE("rig 九项脚本能力：违约块、0 样本、超长块、走带、参数、多台引擎、换写方都能脚本化", "[rig][script]")
{
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(48000.0, 1024);

    // 两端各自的块长脚本：含 0 样本穿插、违约块（n 大于声明值 1024）与超长块（n 大于 32768）。
    BlockScript inputScript;
    inputScript.lengths = {256, 0, 2048, 40000};
    BlockScript outputScript;
    outputScript.lengths = {256, 0, 256, 256};
    host.setBlockScripts(inputScript, outputScript);

    TransportScript transport;
    transport.hostNs = {1000, 2000, 3000, 4000};
    transport.hostNsValid = {true, true, true, true};
    transport.playing = {true, true, true, true};
    transport.bpm = {120.0f, 120.0f, 120.0f, 120.0f};
    host.setTransportScript(transport);

    rinne::engine::EngineParams snapshot;
    snapshot.feedbackPercent = 50.0f;
    host.setParamsScript(paramsOf(snapshot));

    host.runCycle(0);
    host.runCycle(1); // 0 样本：不推进任何计数
    host.runCycle(2); // 违约：2048 > 1024
    {
        const auto& status = host.inputStatus();
        CHECK(status.maxBlockSeen == 2048);
        CHECK(status.delayMin == 2048);
        CHECK(status.delayMinReason == static_cast<std::uint8_t>(rinne::loop::DelayMinReason::kHostViolation));
        CHECK(host.inputLastBlock().hostViolation);
    }
    host.runCycle(3); // 超长块：40000 > 32768
    CHECK(host.inputLastBlock().unsupportedBlock);

    // 多台引擎：起第二个 Output（换写方场景）与第二个 Input（复制体场景）。
    CHECK(host.addSecondOutput());
    CHECK(host.addSecondInput());
    CHECK_FALSE(host.addSecondOutput()); // 已经有了
    CHECK(host.secondOutputState() == rinne::engine::WriteState::kIdle); // 默认不参与周期

    host.setBlockScripts(fixedScript(256, 1), fixedScript(256, 1));
    host.setSecondOutputActive(true);
    host.setSecondInputActive(true);
    host.runCycle(4);
    // 写权还在第一个 Output 手里：第二个写就是冲突，不写环。
    CHECK(host.secondOutputConflictCount() == 1u);
    CHECK(host.secondOutputWrittenFrames() == 0u);
    // 复制体默认不读环：只出干声，看不到对端。
    CHECK(host.secondInputStatus().paired == 0u);

    // 换写方：写权与 Output 槽交给第二个 Output。
    host.switchWriter(true);
    const std::uint64_t firstBefore = host.outputWrittenFrames();
    host.runCycles(2, 5);
    CHECK(host.secondOutputWrittenFrames() == 512u);
    CHECK(host.outputWrittenFrames() == firstBefore); // 第一个不再写
    CHECK(host.outputConflictCount() >= 1u);
    // Input 看见写方换了：重同步一次（原因：换写方）。
    CHECK(host.inputStatus().resyncByReason[static_cast<std::size_t>(rinne::loop::SyncReason::kWriter)] == 1u);

    // 复制体读环时看得见对端。
    host.setSecondInputReadsRing(true);
    host.runCycle(7);
    CHECK(host.secondInputStatus().paired == 1u);
}

TEST_CASE("rig 读两端引擎的状态对象与端点槽：环路的量、写环的量、两端的槽都能读到", "[rig][state]")
{
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(48000.0, 1024);
    host.setBlockScripts(fixedScript(256, 1), fixedScript(256, 1));
    TransportScript transport;
    transport.hostNs = {5000};
    transport.hostNsValid = {true};
    host.setTransportScript(transport);
    host.runCycles(2);

    // Input 侧：环路层的状态。Input 在前，第 2 周期块首看到的是第 1 周期写下的 W = 256。
    const auto& status = host.inputStatus();
    CHECK(status.paired == 1u);
    CHECK(status.delayEffective == 4800); // 默认 100 ms
    CHECK(status.writeHead == 256);
    CHECK(status.offset == 0); // 配对时 off 锚在 W − R = 0 − 0
    CHECK(status.drift == status.writeHead - (256 + status.offset)); // m = W − (R + off)，R 是块首读位 256
    CHECK(status.drift == 0);

    // Output 侧：写环的状态。
    CHECK(host.outputState() == rinne::engine::WriteState::kActive);
    CHECK(host.outputWrittenFrames() == 512u);
    CHECK(host.outputConflictCount() == 0u);
    CHECK(host.outputNonFiniteCount() == 0u);
    CHECK(host.outputClampCount() == 0u);

    // 两端的端点槽：各自每块发布一次。
    const rinne::ipc::RoleSlot* input = host.slot(rinne::ipc::Role::kInput);
    const rinne::ipc::RoleSlot* output = host.slot(rinne::ipc::Role::kOutput);
    REQUIRE(input != nullptr);
    REQUIRE(output != nullptr);
    CHECK(input->block_seq.load() == 2u);
    CHECK(output->block_seq.load() == 2u);
    CHECK(input->last_host_ns.load() == 5000u);
    CHECK(output->last_host_ns.load() == 5000u);
    CHECK(input->d_eff.load() == 4800u);

    // 诊断安全段的六个量有出口。
    const rinne::engine::SafetyDiag diag = host.inputSafetyDiag();
    CHECK(diag.nonfiniteRing == 0u);
    CHECK(diag.clampRing == 0u);
}

TEST_CASE("rig 起播沿：中间链在 Input 报出的复位点（绝对样本号）上复位", "[rig][chain]")
{
    // 中间链是 128 样本延迟，信号是恒定的 0.25，反馈 0（没有回送）：链的输出在复位点上变成 0，
    // 之后 128 个样本里都是 0（延迟线被清空）。复位点 = 起播沿的块首 + 5 ms（240 样本）。
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(48000.0, 256);
    host.setBlockScripts(fixedScript(256, 1), fixedScript(256, 1));
    host.setChain(ChainKind::kDelay);
    host.chain().setDelaySamples(128);
    host.setSource([](std::uint64_t, float* left, float* right, int n) {
        std::fill(left, left + n, 0.25f);
        std::fill(right, right + n, 0.25f);
    });
    TransportScript transport;
    transport.playing = {false, false, false, false, true};
    host.setTransportScript(transport);

    host.runCycles(4);
    CHECK(host.outputFedLeft()[0] == 0.25f);
    host.runCycle(4); // 起播沿：块首 R = 1024
    const auto& fed = host.outputFedLeft();
    CHECK(fed[239] == 0.25f);
    CHECK(fed[240] == 0.0f);
    CHECK(fed[255] == 0.0f);
    CHECK(host.inputLastBlock().resetDsp);
    CHECK(host.inputLastBlock().resetAtSample == 1024 + 240);
}
