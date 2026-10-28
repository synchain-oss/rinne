// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两种构建与验证宿主：正式构建里处理器直接持有 InstanceHost、带参数层与胶水；探针构建里持有的是建在它之上的
// ProbeHost，不建参数层与胶水。验证宿主（suite.cmake 里那套换名副本）下接线照样在，但只出干声、一个段都不开。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "ProbeHost.h"
#include "WiringTestKit.h"
#include "ipc/Naming.h"
#include "ipc/PlatformSegmentBackend.h"

namespace
{

using namespace rinne::wiring_test;

} // namespace

TEST_CASE("each processor holds one instance host of its own role, in both builds", "[host][wiring][hosts]")
{
    Rig rig;
    auto input = rig.makeInput();
    auto output = rig.makeOutput();
    CHECK(input->instanceHost().isInput());
    CHECK_FALSE(output->instanceHost().isInput());

    const bool probeHosted = dynamic_cast<const rinne::probe::ProbeHost*>(&input->instanceHost()) != nullptr &&
                             dynamic_cast<const rinne::probe::ProbeHost*>(&output->instanceHost()) != nullptr;
    CHECK(probeHosted == rinne::build::kProbeBuild);
    // 正式构建才有参数层、胶水与预设库（探针构建里参数与 state 在 ProbeHost 里，两套参数不能同时挂在一个处理器上）
    CHECK((input->parameterLayer() != nullptr) == !rinne::build::kProbeBuild);
    CHECK((input->stateGlue() != nullptr) == !rinne::build::kProbeBuild);
    CHECK((input->presetStore() != nullptr) == !rinne::build::kProbeBuild);
    CHECK((output->stateGlue() != nullptr) == !rinne::build::kProbeBuild);
    if constexpr (!rinne::build::kProbeBuild)
        CHECK(input->getParameters().size() == 11);
}

TEST_CASE("under a validation host the wired processors pass the dry signal and open no segment",
          "[host][wiring][hosts]")
{
    formalOnly();
    Rig rig;
    rig.source = &sine;
    rig.open(1);
    hostSetParam(*rig.input, "feedback", 100.0f);
    rig.record = true;
    rig.cycles(200);

    const ipc::Naming naming(rig.scope());
    std::string rvName;
    REQUIRE(naming.name(ipc::SegmentId::rendezvous(), rvName) == ipc::NameStatus::kOk);
    ipc::PlatformSegmentBackend backend;
    ipc::SegmentView view;
    const bool rvExists = backend.openExisting(rvName, view) == ipc::InitResult::kOk;
    if (rvExists)
        backend.unmap(view);

    // ⛔ 验证宿主下也开 IPC（InstanceHost 构造时 ipcEnabled 恒为真）→ 换名副本里变红
    if (runningAsValidationHost())
    {
        CHECK(rig.input->isValidationHost());
        CHECK(rig.input->instanceHost().rendezvousState() == ipc::RendezvousState::kValidator);
        CHECK(rig.output->instanceHost().rendezvousState() == ipc::RendezvousState::kValidator);
        CHECK_FALSE(rvExists);
        CHECK(rig.outLeft == rig.dryLeft); // 没有 NaN 的信号源，按值比就是逐位比
        CHECK(rig.outputTouched == 0);
    }
    else
    {
        // 对照：普通宿主下同样的操作开了段、配上了对，回送在响（查法本身有效）
        CHECK(rvExists);
        CHECK(rig.paired());
        CHECK(rig.outLeft != rig.dryLeft);
    }
}
