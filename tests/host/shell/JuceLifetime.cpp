// SPDX-License-Identifier: GPL-3.0-or-later
//
// 整个测试进程期间持有 JUCE 的初始化（消息管理器等），处理器的构造与析构都在它里面。

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace
{

class JuceLifetime : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        juce_ = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
    }

    void testRunEnded(const Catch::TestRunStats&) override { juce_.reset(); }

private:
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce_;
};

} // namespace

CATCH_REGISTER_LISTENER(JuceLifetime)
