// SPDX-License-Identifier: GPL-3.0-or-later
//
// 占位场景：证明自注册宏与注册表可用。
//
// 场景文件由各自的卡拥有（tests/loop_rig/scenarios/**）。本模块不建那个目录，
// 占位场景放在 rig/ 里（属本模块路径），不占用任何一张卡要写的位置。

#include "rig/ScenarioRegistry.h"

namespace
{

using namespace rinne::rig;

class PlaceholderScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s00_placeholder"; }

    bool run(ScenarioContext& context) override
    {
        // 占位场景不跑音频：它只证明「注册表能查到、能造出来、能跑完」。
        (void)context;
        return true;
    }
};

} // namespace

RINNE_RIG_SCENARIO("s00_placeholder")
{
    return new PlaceholderScenario();
}
