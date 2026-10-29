// SPDX-License-Identifier: GPL-3.0-or-later
//
// 诊断日志的接线：开关（本机全局设置）开着、又不在验证宿主里，每个实例第一次 prepare 时装；只构造、不 prepare 不装
// （扫描插件、生成 moduleinfo 的宿主就是这样）；setDiagLogEnabled 写设置，打开时当场装；装上之后 IPC 诊断的转发出口
// 回到最上面，后端的错误照样先记到实例、再转给日志。这里的「装」是记录用的替身（PairingKit），不碰真的日志文件；
// 开关落在 UiDefaultsSandbox 指的临时目录里。
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "PairingKit.h"
#include "UiDefaultsStore.h"
#include "ipc/IpcDiag.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::InstanceHost;
using rinne::OutputProcessor;
namespace uidefaults = rinne::uidefaults;

std::vector<juce::String> expectedInstalls(std::vector<juce::String> roles)
{
    return runningAsValidationHost() ? std::vector<juce::String>{} : roles;
}

// 真的 install 会把日志的出口装进 IpcDiag 的槽；替身照做，换成一个只计数的出口
int gLogSinkCalls = 0;

void countingLogSink(const ipc::IpcDiagEvent&) noexcept
{
    ++gLogSinkCalls;
}

bool installWithSink(const juce::String& role)
{
    captured().installs.push_back(role);
    ipc::setIpcDiagSink(&countingLogSink);
    return true;
}

// 用例结束后把槽还给转发出口、转发目标还原成用例开始时的那个
class SinkRestore
{
public:
    SinkRestore() : forward_(InstanceHost::ipcDiagForwardTarget()) {}
    ~SinkRestore()
    {
        ipc::setIpcDiagSink(forward_);
        InstanceHost::installIpcDiagSink();
    }
    SinkRestore(const SinkRestore&) = delete;
    SinkRestore& operator=(const SinkRestore&) = delete;

private:
    ipc::IpcDiagSink forward_;
};

} // namespace

TEST_CASE("pairing: constructing an instance without a prepare never installs the diagnostics log",
          "[host][pairing][diag_log]")
{
    formalOnly();
    uidefaults::setDiagLogEnabled(true);
    Bench bench;
    bench.addInput(false);
    bench.addOutput(false);
    bench.idle(1000); // 定时器照常走，只是宿主从不 prepare
    CHECK(captured().installs.empty());
}

TEST_CASE("pairing: with the switch on the first prepare installs the diagnostics log outside validation hosts",
          "[host][pairing][diag_log]")
{
    formalOnly();
    uidefaults::setDiagLogEnabled(true);
    Bench bench;
    InputProcessor& input = bench.addInput(false);
    REQUIRE(captured().installs.empty());
    input.prepareToPlay(kRate, kBlock);
    CHECK(captured().installs == expectedInstalls({"input"}));

    // 之后的 prepare 不再看开关（宿主挂起再恢复是 release → prepare）
    input.releaseResources();
    input.prepareToPlay(kRate, kBlock);
    CHECK(captured().installs == expectedInstalls({"input"}));

    // Output 是另一个插件二进制：它第一次 prepare 时装它自己的
    bench.addOutput();
    CHECK(captured().installs == expectedInstalls({"input", "output"}));
}

TEST_CASE("pairing: inside a validation host neither a prepare nor the switch installs the diagnostics log",
          "[host][pairing][diag_log]")
{
    formalOnly();
    if (!runningAsValidationHost())
        SKIP("only the validation-host copy of the test binary runs as a validation host");
    uidefaults::setDiagLogEnabled(true);
    Bench bench;
    InputProcessor& input = bench.addInput();
    OutputProcessor& output = bench.addOutput();
    bench.run(500);
    CHECK(captured().installs.empty());

    input.setDiagLogEnabled(true);
    output.setDiagLogEnabled(true);
    CHECK(uidefaults::diagLogEnabled()); // 设置照写，只是不装
    CHECK(captured().installs.empty());
}

TEST_CASE("pairing: with the switch off nothing is installed", "[host][pairing][diag_log]")
{
    formalOnly();
    REQUIRE_FALSE(uidefaults::diagLogEnabled()); // 每个用例开始时设置都是空的
    Bench bench;
    bench.addInput();
    bench.addOutput();
    CHECK(captured().installs.empty());
}

TEST_CASE("pairing: turning the switch on writes the setting and installs on the spot", "[host][pairing][diag_log]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    REQUIRE(captured().installs.empty());

    output.setDiagLogEnabled(true);
    CHECK(uidefaults::diagLogEnabled()); // 验证宿主里也写设置，只是不装
    CHECK(captured().installs == expectedInstalls({"output"}));

    // 关掉只写设置；本进程的日志写到退出（没有生产可用的停写）
    output.setDiagLogEnabled(false);
    CHECK_FALSE(uidefaults::diagLogEnabled());
    CHECK(captured().installs == expectedInstalls({"output"}));

    // 设置是本机全局的：后建的实例读到的是关
    bench.addInput();
    CHECK(captured().installs == expectedInstalls({"output"}));
}

TEST_CASE("pairing: once the log is installed the IPC diagnostics still reach the instance first, then the log",
          "[host][pairing][diag_log]")
{
    formalOnly();
    if (runningAsValidationHost())
        SKIP("validation hosts never install the diagnostics log");
    const SinkRestore sinks;
    gLogSinkCalls = 0;
    uidefaults::setDiagLogEnabled(false); // 每次进 SECTION 都从关开始（设置只在用例开始时清）
    Bench bench;
    rinne::InstanceHostConfig config = bench.config();
    config.installLog = &installWithSink;

    SECTION("installed at the first prepare")
    {
        uidefaults::setDiagLogEnabled(true);
        bench.add(std::make_unique<InputProcessor>(config), true);
    }
    SECTION("installed later by the switch")
    {
        InputProcessor& input = bench.add(std::make_unique<InputProcessor>(config), true);
        REQUIRE(captured().installs.empty());
        input.setDiagLogEnabled(true);
    }
    REQUIRE(captured().installs == std::vector<juce::String>{"input"});
    CHECK(ipc::ipcDiagSink() == InstanceHost::ipcDiagRecorder());
    CHECK(InstanceHost::ipcDiagForwardTarget() == &countingLogSink);

    ipc::IpcDiagEvent event;
    event.code = ipc::IpcDiagCode::kBusy;
    ipc::reportIpcDiag(event);
    CHECK(gLogSinkCalls == 1);
}
