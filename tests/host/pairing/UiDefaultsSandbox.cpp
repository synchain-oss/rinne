// SPDX-License-Identifier: GPL-3.0-or-later
//
// 正式构建的处理器在构造时读本机的诊断日志开关（UiDefaultsStore）。宿主测试里到处都有用默认配置构造的处理器，
// 不能让它们读到、更不能因此写到用户真实的设置与日志：每个用例开始之前把界面全局默认的落盘目录指向本进程独占的
// 临时目录，并清掉上一个用例留下的设置文件（开关回到默认的关：前一个用例打开过它，后面用默认配置构造的处理器就会
// 装上真的日志）。用例自己另指目录（tests/host/ui_defaults/）照常生效，下一个用例开始时再指回来，所以与执行顺序
// 无关。测试进程结束时恢复默认位置、删掉临时目录。
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <juce_core/juce_core.h>

#include "UiDefaultsStore.h"

namespace
{

class UiDefaultsSandbox : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        dir_ =
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("rinne-host-tests-ui-defaults-" + juce::String::toHexString(juce::Random().nextInt64()));
        dir_.createDirectory();
    }

    void testCaseStarting(const Catch::TestCaseInfo&) override
    {
        clearFiles();
        rinne::uidefaults::setStorageDirForTests(dir_);
    }

    void testRunEnded(const Catch::TestRunStats&) override
    {
        rinne::uidefaults::setStorageDirForTests(juce::File());
        clearFiles();
        dir_.deleteFile(); // 此时是空目录
    }

private:
    // 只删这一层的文件，不递归
    void clearFiles() const
    {
        for (const auto& f : dir_.findChildFiles(juce::File::findFiles, false))
            f.deleteFile();
    }

    juce::File dir_;
};

} // namespace

CATCH_REGISTER_LISTENER(UiDefaultsSandbox)
