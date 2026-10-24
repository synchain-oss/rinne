// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针面板（JUCE 原生，没有 WebView）：组 A-H、feedback 与 D 两个滑杆、状态文本与计数，按钮 Panic、Recover、Ping、
// 记录 60 s、复制诊断，再加 case / variant / 缓冲 / guard 四个选择器（上机清单要在面板上选好格再开始记录；开始记录时
// 这四项原样交给 C 行）。
// 面板只经 ProbeEditorHost 和处理器说话，处理器（探针构建里）去实现它，测试用假实现。
// 规则都在 ProbeSupport.h 里：feedback 上限 80 %、Ping 只在停走带或干声静音时可点、Panic 之后恢复冷却 1.5 s。
// Panic 只是界面动作，不对应任何参数。窗口尺寸固定，不用会自己改尺寸的控件（pluginval 严格度 5 会查）。
// 线程：全部在消息线程。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <string>

#include "ProbeSupport.h"

namespace rinne::probe
{

class ProbeEditorHost
{
public:
    virtual ~ProbeEditorHost() = default;

    virtual ProbeStatus probeStatus() const = 0;
    virtual void probePanic() = 0;
    virtual void probeRecover() = 0;
    virtual void probePing() = 0;
    // 开始一个测试格的记录窗口，到时由处理器停止；起不来（选择不合规则、已在记录、日志没开、日志排队满了）返回 false。
    // 返回 false 时处理器什么都没改，面板在状态文本里写明、用户可稍后再点
    virtual bool probeStartRecording(const ProbeSelection& selection, std::uint32_t seconds) = 0;
    virtual void probeSetGroup(int group) = 0; // -1 = 未设，0..7 = A..H
    virtual void probeSetFeedbackPercent(float percent) = 0; // 面板已钳到 [0, 80]
    virtual void probeSetDelayMs(float ms) = 0;
    virtual std::string probeDiagnosticsText() = 0;
};

struct ProbeEditorOptions
{
    std::function<std::uint64_t()> nowMs; // 单调毫秒，空 = MonoClock；测试注入假时钟
    std::function<void(const std::string&)> copyToClipboard; // 空 = 系统剪贴板；测试注入捕获函数
};

class ProbeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ProbeEditor(juce::AudioProcessor& processor, ProbeEditorHost& host, ProbeEditorOptions options = {});
    ~ProbeEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // 读一次处理器的状态：刷新文本、按钮可用性与滑杆。定时器每 100 ms 调一次
    void refresh();

    // 按钮的动作；按钮点击与测试都走这里，规则不满足时什么都不做
    void doPanic();
    void doRecover();
    void doPing();
    void doRecord();
    void doCopyDiagnostics();

    ProbeSelection selection() const;

    struct Controls
    {
        juce::ComboBox group, caseId, variant, buffer, guard;
        juce::Slider feedback, delay;
        juce::Label status, counters;
        juce::TextButton panic, recover, ping, record, copyDiagnostics;
    };
    Controls& controls() noexcept { return c_; }

private:
    void timerCallback() override { refresh(); }
    void fillVariants();
    void addCaption(const juce::String& text, juce::Component& target);

    ProbeEditorHost& host_;
    ProbeEditorOptions options_;
    RecoverCooldown cooldown_;
    bool recordRejected_ = false; // 上一次点记录被处理器拒了；记录开始或下一次成功后清掉
    Controls c_;
    juce::OwnedArray<juce::Label> captions_;
    std::string statusShown_;
    std::string countersShown_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProbeEditor)
};

} // namespace rinne::probe
