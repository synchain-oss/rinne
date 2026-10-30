// SPDX-License-Identifier: GPL-3.0-or-later
//
// 轨道标签：宿主给的名字与颜色写进本端的标签，对端读得到；两端名字相同 / 不同 / 有一端未知分别落在 connected /
// crossTrack / connectedUnverified。AU 只给名字时颜色按未知（0）；没给名字或给了空串都是「未知」，散列为 0，诊断写
// hash=none，绝不当作一个空名字。原始轨道名不进诊断文本，也不进诊断日志。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include "PairingKit.h"
#include "diag/DiagReport.h"
#include "ipc/SegmentLayout.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;

constexpr std::uint32_t kVoxArgb = 0xff336699u;

// 一对已经配上、都在跑实时块的实例（Input 先放，Output 跟上，同在组 A）
struct Pair
{
    explicit Pair(Bench& bench) : input(bench.addInput()), output(bench.addOutput())
    {
        REQUIRE(input.selectGroup(1));
        REQUIRE(output.selectGroup(1));
        bench.run(200);
        REQUIRE(holds(input, 1));
        REQUIRE(holds(output, 1));
    }

    InputProcessor& input;
    OutputProcessor& output;
};

bool anyLogContains(const char* text)
{
    for (const juce::String& line : captured().logLines)
        if (line.contains(text))
            return true;
    return false;
}

} // namespace

TEST_CASE("pairing: a track name and colour written by one end are read by the other", "[host][pairing][labels]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    Pair pair(bench);
    setTrack(pair.input, juce::String("Lead Vox"), juce::Colour(kVoxArgb));
    setTrack(pair.output, juce::String("Lead Vox"), juce::Colour(0xff112233u));
    bench.run(100);

    const auto fromOutput = pair.output.pairState();
    CHECK(fromOutput.peerName == "Lead Vox");
    CHECK(fromOutput.peerColourArgb == kVoxArgb);
    CHECK(fromOutput.nameCheck == rinne::pairing::NameCheck::kSame);
    const auto fromInput = pair.input.pairState();
    CHECK(fromInput.peerName == "Lead Vox");
    CHECK(fromInput.peerColourArgb == 0xff112233u);
    CHECK(pair.input.instanceHost().trackNameHash() == ipc::fnv1a32("Lead Vox"));

    SECTION("a renamed track is picked up on the next tick")
    {
        setTrack(pair.input, juce::String("Backing Vox"), juce::Colour(kVoxArgb));
        bench.run(100);
        CHECK(pair.output.pairState().peerName == "Backing Vox");
        CHECK(pair.output.pairState().nameCheck == rinne::pairing::NameCheck::kDifferent);
    }
}

TEST_CASE("pairing: comparing the two track names decides between connected, cross-track and unverified",
          "[host][pairing][labels]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    Pair pair(bench);

    SECTION("same name")
    {
        setTrack(pair.input, juce::String("Lead Vox"));
        setTrack(pair.output, juce::String("Lead Vox"));
        bench.run(100);
        CHECK(pairStateOf(pair.input) == PairState::kConnected);
        CHECK(pairStateOf(pair.output) == PairState::kConnected);
    }
    SECTION("different names")
    {
        setTrack(pair.input, juce::String("Lead Vox"));
        setTrack(pair.output, juce::String("Rhythm Gtr"));
        bench.run(100);
        CHECK(pairStateOf(pair.input) == PairState::kCrossTrack);
        CHECK(pairStateOf(pair.output) == PairState::kCrossTrack);
        CHECK(pair.output.pairState().peerName == "Lead Vox");
    }
    SECTION("one end unnamed")
    {
        setTrack(pair.input, juce::String("Lead Vox"));
        bench.run(100);
        CHECK(pairStateOf(pair.input) == PairState::kConnectedUnverified);
        CHECK(pairStateOf(pair.output) == PairState::kConnectedUnverified);
        CHECK(pair.input.pairState().nameCheck == rinne::pairing::NameCheck::kUnknown);
    }
    SECTION("neither end named")
    {
        bench.run(100);
        CHECK(pairStateOf(pair.input) == PairState::kConnectedUnverified);
        CHECK(pairStateOf(pair.output) == PairState::kConnectedUnverified);
    }
}

TEST_CASE("pairing: a name without a colour (AU) publishes the colour as unknown", "[host][pairing][labels]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    Pair pair(bench);
    setTrack(pair.input, juce::String("Lead Vox"));
    bench.run(100);
    CHECK(pair.output.pairState().peerName == "Lead Vox");
    CHECK(pair.output.pairState().peerColourArgb == 0u);
    CHECK(anyLogContains("input: track properties: name known, colour unknown"));
}

TEST_CASE("pairing: no name or an empty name is unknown, never an empty name", "[host][pairing][labels]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    Pair pair(bench);

    SECTION("no name given")
    {
        setTrack(pair.input, std::nullopt, juce::Colour(kVoxArgb));
        CHECK(anyLogContains("input: track properties: name unknown, colour known"));
    }
    SECTION("an empty name given")
    {
        setTrack(pair.input, juce::String());
        CHECK(anyLogContains("input: track properties: name unknown, colour unknown"));
    }
    // 对端同样没有名字：两个「空名字」若当成已知会判成 connected，这里必须是 unverified
    setTrack(pair.output, juce::String());
    bench.run(100);
    CHECK(pair.input.instanceHost().trackNameHash() == 0u);
    CHECK(pair.output.instanceHost().trackNameHash() == 0u);
    CHECK(pair.output.pairState().peerName.empty());
    CHECK(pair.output.pairState().peerColourArgb == 0u); // 没写标签：颜色也没发布
    CHECK(pairStateOf(pair.input) == PairState::kConnectedUnverified);
    CHECK(pairStateOf(pair.output) == PairState::kConnectedUnverified);
    const std::string text = pair.input.diagnosticsText();
    CHECK(text.find("track: relation=unknown hash=none\n") != std::string::npos);
    CHECK(text.find(" name=unknown ") != std::string::npos);
}

TEST_CASE("pairing: the raw track name stays out of the diagnostics text and the diagnostics log",
          "[host][pairing][labels][privacy]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    Pair pair(bench);
    setTrack(pair.input, juce::String("Zanzibar Choir"), juce::Colour(kVoxArgb));
    setTrack(pair.output, juce::String("Zanzibar Choir"));
    bench.run(100);
    REQUIRE(pair.output.pairState().peerName == "Zanzibar Choir"); // 名字确实在会合里流通

    const std::string inputText = pair.input.diagnosticsText();
    const std::string outputText = pair.output.diagnosticsText();
    const std::string hash = rinne::diag::formatHex8(ipc::fnv1a32("Zanzibar Choir"));
    CHECK(inputText.find("track: relation=same hash=" + hash + "\n") != std::string::npos);
    CHECK(outputText.find("track: relation=same hash=" + hash + "\n") != std::string::npos);
    CHECK(inputText.find(" name=same ") != std::string::npos);
    for (const std::string& text : {inputText, outputText})
    {
        CHECK(text.find("Zanzibar") == std::string::npos);
        CHECK(text.find("Choir") == std::string::npos);
    }
    REQUIRE_FALSE(captured().logLines.empty());
    for (const juce::String& line : captured().logLines)
    {
        CHECK_FALSE(line.contains("Zanzibar"));
        CHECK_FALSE(line.contains("Choir"));
    }
}
