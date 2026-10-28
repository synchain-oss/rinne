// SPDX-License-Identifier: GPL-3.0-or-later
#include "RinneEditorFactory.h"

#include <utility>

#include "util/RinneVersion.h"

#if JUCE_WEB_BROWSER
#include "WebViewHost.h"
#endif

namespace rinne::editor
{

Identity currentIdentity() noexcept
{
    return build::kProbeBuild ? Identity::probe : Identity::formal;
}

namespace
{
bool isKnownRole(const juce::String& role)
{
    return role == "input" || role == "output";
}

juce::AudioProcessorEditor* makeWebViewEditor(juce::AudioProcessor& processor, const Options& options)
{
    if (options.webViewEditorMaker)
        return options.webViewEditorMaker(processor);

#if JUCE_WEB_BROWSER
    webview::WebViewHost::Config config;
    config.role = options.role;
    config.resourceSource = options.resources;
    config.controls = options.controls;
    return new webview::WebViewHost(processor, std::move(config));
#else
    return nullptr;
#endif
}
} // namespace

juce::AudioProcessorEditor* createEditor(juce::AudioProcessor& processor, const Options& options, Identity identity)
{
    if (!isKnownRole(options.role))
        return nullptr;

    if (identity == Identity::probe)
        return options.probeEditorFactory ? options.probeEditorFactory() : nullptr;

    return makeWebViewEditor(processor, options);
}

juce::AudioProcessorEditor* createEditorOrGeneric(juce::AudioProcessor& processor, const Options& options,
                                                  Identity identity)
{
    if (auto* editor = createEditor(processor, options, identity))
        return editor;
    return new juce::GenericAudioProcessorEditor(processor);
}

} // namespace rinne::editor
