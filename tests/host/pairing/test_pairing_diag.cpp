// SPDX-License-Identifier: GPL-3.0-or-later
//
// 诊断文本的接线：各项来源有没有接对、Output 只出子集、state 一段的四项、复制诊断交出同一份文本。格式本身由
// tests/core/diag 的快照覆盖。隐私：宿主名带路径时整串不要；文本里不出现本机名、登录名、用户目录与测试程序自己的
// 文件名（这几样都在运行时取，不写进源码）。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "PairingKit.h"
#include "diag/DiagReport.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// 某一行（以 prefix 开头）的整行，没有就返回空
std::string lineOf(const std::string& text, const std::string& prefix)
{
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const std::size_t end = text.find('\n', pos);
        const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (line.rfind(prefix, 0) == 0)
            return line;
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return {};
}

// 这台机器、这个账号、这个程序自己的名字：都不许出现在诊断里。太短的名字（两个字符以内）会误撞普通字段，不查
std::vector<std::string> machineNames()
{
    std::vector<std::string> names;
    const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    for (const juce::String& name :
         {juce::SystemStats::getComputerName(), juce::SystemStats::getLogonName(), juce::SystemStats::getFullUserName(),
          juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName(),
          exe.getFileNameWithoutExtension(), exe.getParentDirectory().getFullPathName()})
    {
        if (name.trim().length() > 2)
            names.push_back(name.trim().toStdString());
    }
    return names;
}

void checkNoMachineNames(const std::string& text)
{
    for (const std::string& name : machineNames())
    {
        INFO("a machine-specific name leaked into the diagnostics");
        CHECK_FALSE(has(text, name));
    }
}

} // namespace

TEST_CASE("pairing: the diagnostics text carries the pairing facts of both ends", "[host][pairing][diag]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& input = bench.addInput();
    bench.run(400);
    REQUIRE(holds(input, 1));
    OutputProcessor& output = bench.addOutput();
    bench.run(400);
    REQUIRE(holds(output, 1));

    const std::string inputText = input.diagnosticsText();
    const std::string outputText = output.diagnosticsText();
    for (const std::string& text : {inputText, outputText})
    {
        CHECK(text.rfind("rinne-diag v1\n", 0) == 0);
        CHECK(has(text, " host=Test Host 1.0 "));
        CHECK(has(text, " group=A origin=smart pid=" + std::to_string(ipc::currentProcessId()) +
                            " pair=connectedUnverified\n"));
        CHECK(has(text, " state=active same_pid=1 "));
        CHECK(has(text, "prepare: count=1 sr=48000 max=" + std::to_string(kBlock) + " "));
        CHECK(has(text, "\nevents("));
    }
    CHECK(has(inputText, "peer: role=output "));
    CHECK(has(outputText, "peer: role=input "));

    // 手选的组记为 manual
    REQUIRE(input.selectGroup(4));
    bench.run(100);
    CHECK(has(input.diagnosticsText(), " group=D origin=manual "));
}

TEST_CASE("pairing: the Output diagnostics are the documented subset", "[host][pairing][diag]")
{
    formalOnly();
    Bench bench;
    InputProcessor& input = bench.addInput();
    OutputProcessor& output = bench.addOutput();
    bench.run(100);
    const std::string inputText = input.diagnosticsText();
    const std::string outputText = output.diagnosticsText();
    for (const char* section : {"\nloop: ", "\nsync: ", "\norder: ", "\nsafety: "})
    {
        CHECK(has(inputText, section));
        CHECK_FALSE(has(outputText, section));
    }
    CHECK(has(lineOf(inputText, "state: "), " dropped_unknown="));
    CHECK_FALSE(lineOf(outputText, "state: ").empty());
    CHECK_FALSE(has(lineOf(outputText, "state: "), "dropped_unknown"));
}

TEST_CASE("pairing: the state section reports the last load, drops, encode failures and a held newer state",
          "[host][pairing][diag][state_section]")
{
    formalOnly();
    Bench bench;

    SECTION("Input")
    {
        InputProcessor& input = bench.addInput();
        CHECK(lineOf(input.diagnosticsText(), "state: ") ==
              "state: last_load=none dropped_unknown=0 encode_failures=0 holding_newer=0");
        const std::vector<std::uint8_t> good = savedState(input);

        restore(input, std::vector<std::uint8_t>{1, 2, 3});
        CHECK(has(lineOf(input.diagnosticsText(), "state: "), "last_load=corrupt "));

        restore(input, good);
        CHECK(lineOf(input.diagnosticsText(), "state: ") ==
              "state: last_load=ok dropped_unknown=0 encode_failures=0 holding_newer=0");

        // 更高 abi、同一角色的 state：拒载并原样留着回写
        std::vector<std::uint8_t> newer = good;
        REQUIRE(newer.size() > 8);
        ++newer[4];
        InputProcessor& fresh = bench.addInput();
        restore(fresh, newer);
        REQUIRE(fresh.stateGlue()->holdingNewerState());
        CHECK(lineOf(fresh.diagnosticsText(), "state: ") ==
              "state: last_load=rejected_newer dropped_unknown=0 encode_failures=0 holding_newer=1");
    }
    SECTION("Output")
    {
        OutputProcessor& output = bench.addOutput();
        CHECK(lineOf(output.diagnosticsText(), "state: ") == "state: last_load=none encode_failures=0 holding_newer=0");
        std::vector<std::uint8_t> newer = savedState(output);
        REQUIRE(newer.size() > 8);
        ++newer[4];
        OutputProcessor& fresh = bench.addOutput();
        restore(fresh, newer);
        CHECK(lineOf(fresh.diagnosticsText(), "state: ") ==
              "state: last_load=rejected_newer encode_failures=0 holding_newer=1");
    }
}

TEST_CASE("pairing: copying the diagnostics hands the same text to the clipboard", "[host][pairing][diag]")
{
    formalOnly();
    Bench bench;
    InputProcessor& input = bench.addInput();
    OutputProcessor& output = bench.addOutput();
    bench.run(100);
    input.copyDiagnosticsToClipboard();
    output.copyDiagnosticsToClipboard();
    REQUIRE(captured().clipboard.size() == 2);
    CHECK(captured().clipboard[0].toStdString() == input.diagnosticsText());
    CHECK(captured().clipboard[1].toStdString() == output.diagnosticsText());
}

TEST_CASE("pairing: the diagnostics carry no path, user or machine name", "[host][pairing][diag][privacy]")
{
    formalOnly();
    Bench bench;

    SECTION("a host description that carries a path is dropped whole")
    {
        const std::string pathy =
            "Host 1.0 " +
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString();
        bench.hostDescription = pathy.c_str();
        InputProcessor& input = bench.addInput();
        OutputProcessor& output = bench.addOutput();
        bench.run(100);
        for (const std::string& text : {input.diagnosticsText(), output.diagnosticsText()})
        {
            CHECK(has(text, " host=none "));
            checkNoMachineNames(text);
        }
    }
    SECTION("the real host description of this process")
    {
        bench.hostDescription = nullptr;
        InputProcessor& input = bench.addInput();
        bench.run(100);
        const std::string text = input.diagnosticsText();
        const std::string header = lineOf(text, "product=");
        CHECK(has(header, " host="));
        checkNoMachineNames(text);
    }
}
