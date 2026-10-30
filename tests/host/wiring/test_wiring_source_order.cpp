// SPDX-License-Identifier: GPL-3.0-or-later
//
// 每块读序的内存序：Input 处理器的 snapshotBlock 先做参数快照，再一道 acquire 栅栏，再读序号（胶水快照里的
// stateSeq、预设库的 presetSeq）。参数快照是 relaxed 读，「读到新参数时序号必定已加过」靠的是这道栅栏与参数写（JUCE
// 的 seq_cst store）同步。内存序在单测里钉不住（x86 的硬件不对调两次读，编译器与 ARM64 的重排也只是偶发），
// 所以改成钉源码：读 InputProcessor.h 的文本，断言栅栏在 snapshotBlock 里、位于参数快照与两次读序号之间；
// 再读 InputProcessor.cpp，断言每块的处理经 snapshotBlock 取参数，不另起一份不带栅栏的读法。
// 扫描器自己带正反例，保证它不是空转。源文件路径由本文件的 __FILE__ 推出，
// 取法同 tests/host/webview/test_source_hygiene.cpp；找不到文件就判红，不静默跳过。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include <juce_core/juce_core.h>

namespace
{

juce::File repoRoot()
{
    return juce::File::getCurrentWorkingDirectory()
        .getChildFile(__FILE__)
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory();
}

std::string readSource(const juce::File& file)
{
    REQUIRE(file.existsAsFile());
    return file.loadFileAsString().toStdString();
}

// 去掉行注释与块注释（注释里提到栅栏不算数）。这几处源码里没有含 // 或 /* 的字符串字面量
std::string stripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        if (src.compare(i, 2, "//") == 0)
        {
            while (i < src.size() && src[i] != '\n')
                ++i;
            if (i < src.size())
                out += '\n';
        }
        else if (src.compare(i, 2, "/*") == 0)
        {
            const std::size_t end = src.find("*/", i + 2);
            i = end == std::string::npos ? src.size() : end + 1;
        }
        else
        {
            out += src[i];
        }
    }
    return out;
}

// 从 marker 之后的第一个 { 起，取到与它配对的 }（不含两端）。找不到返回空串
std::string bodyAfter(const std::string& code, const std::string& marker)
{
    const std::size_t at = code.find(marker);
    if (at == std::string::npos)
        return {};
    const std::size_t open = code.find('{', at);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return code.substr(open + 1, i - open - 1);
    }
    return {};
}

// 恰好出现一次时返回位置，否则返回 npos
std::size_t onlyOnce(const std::string& text, const std::string& needle)
{
    const std::size_t first = text.find(needle);
    if (first == std::string::npos || text.find(needle, first + 1) != std::string::npos)
        return std::string::npos;
    return first;
}

constexpr const char* kParams = "params.snapshot(out);";
constexpr const char* kFence = "std::atomic_thread_fence(std::memory_order_acquire);";
constexpr const char* kGlue = "glue.snapshot(out);";
constexpr const char* kPresets = "presets.presetSeq()";

// InputProcessor.h 的 snapshotBlock 读序有问题时返回问题描述，没问题返回空串
std::string snapshotOrderProblem(const std::string& header)
{
    const std::string body = bodyAfter(stripComments(header), "static void snapshotBlock(");
    if (body.empty())
        return "snapshotBlock not found";
    const std::size_t params = onlyOnce(body, kParams);
    const std::size_t fence = onlyOnce(body, kFence);
    const std::size_t glue = onlyOnce(body, kGlue);
    const std::size_t presets = onlyOnce(body, kPresets);
    if (params == std::string::npos || glue == std::string::npos || presets == std::string::npos)
        return "parameter snapshot, glue snapshot or presetSeq read missing or repeated";
    if (fence == std::string::npos)
        return "acquire fence missing or repeated";
    if (!(params < fence))
        return "acquire fence comes before the parameter snapshot";
    if (!(fence < glue && fence < presets))
        return "a sequence is read before the acquire fence";
    return {};
}

// InputProcessor.cpp 每块的处理（process）有问题时返回问题描述：必须经 snapshotBlock 取参数，不另起一份读法
std::string processUsesSnapshotProblem(const std::string& source)
{
    const std::string body = bodyAfter(stripComments(source), "void InputProcessor::process(");
    if (body.empty())
        return "InputProcessor::process not found";
    const std::size_t call = onlyOnce(body, "snapshotBlock(");
    const std::size_t engine = onlyOnce(body, "instanceHost().process(");
    if (call == std::string::npos || engine == std::string::npos || !(call < engine))
        return "process does not read the block through snapshotBlock before the engine";
    for (const char* direct : {".snapshot(", "presetSeq()", "stateSeq()"})
        if (body.find(direct) != std::string::npos)
            return std::string("process reads ") + direct + " outside snapshotBlock";
    return {};
}

std::string sampleHeader(const std::string& statements)
{
    // 字面量只用 ASCII（中文只写在注释里）：类注释里提到栅栏，扫描时不算数
    return "class X {\n"
           "    // a comment naming " +
           std::string(kFence) +
           " does not count\n"
           "    template<typename P, typename G, typename S>\n"
           "    static void snapshotBlock(const P& params, const G& glue, const S& presets, E& out) noexcept\n"
           "    {\n" +
           statements +
           "    }\n"
           "};\n";
}

} // namespace

TEST_CASE("the read-order scanner flags a missing, misplaced or commented-out acquire fence",
          "[host][wiring][order][source]")
{
    const std::string p = std::string("        ") + kParams + "\n";
    const std::string f = std::string("        ") + kFence + "\n";
    const std::string g = std::string("        ") + kGlue + "\n";
    const std::string s = std::string("        out.presetSeq = ") + kPresets + ";\n";

    CHECK(snapshotOrderProblem(sampleHeader(p + f + g + s)).empty());
    CHECK(snapshotOrderProblem(sampleHeader(p + f + s + g)).empty()); // 两个序号谁先谁后都行，都在栅栏之后
    CHECK_FALSE(snapshotOrderProblem(sampleHeader(p + g + s)).empty()); // 没有栅栏
    CHECK_FALSE(snapshotOrderProblem(sampleHeader(f + p + g + s)).empty()); // 栅栏在参数快照之前
    CHECK_FALSE(snapshotOrderProblem(sampleHeader(p + g + f + s)).empty()); // stateSeq 在栅栏之前
    CHECK_FALSE(snapshotOrderProblem(sampleHeader(p + s + f + g)).empty()); // presetSeq 在栅栏之前
    CHECK_FALSE(snapshotOrderProblem(sampleHeader(p + "        // " + kFence + "\n" + g + s)).empty()); // 只在注释里
    CHECK_FALSE(
        snapshotOrderProblem(sampleHeader(p + "        std::atomic_thread_fence(std::memory_order_relaxed);\n" + g + s))
            .empty()); // 栅栏不是 acquire
    CHECK_FALSE(snapshotOrderProblem(sampleHeader(p + f + f + g + s)).empty()); // 重复的栅栏也不认（不猜哪一道算数）
    CHECK_FALSE(snapshotOrderProblem("class X {};").empty()); // 没有 snapshotBlock

    const std::string viaSnapshot = "void InputProcessor::process(B& b, bool x) noexcept\n{\n"
                                    "    E params;\n    snapshotBlock(a, b, c, params);\n"
                                    "    instanceHost().process(b, params, x);\n}\n";
    const std::string direct = "void InputProcessor::process(B& b, bool x) noexcept\n{\n"
                               "    E params;\n    wiring_->params.snapshot(params);\n"
                               "    params.presetSeq = wiring_->presets.presetSeq();\n"
                               "    instanceHost().process(b, params, x);\n}\n";
    CHECK(processUsesSnapshotProblem(viaSnapshot).empty());
    CHECK_FALSE(processUsesSnapshotProblem(direct).empty());
    CHECK_FALSE(processUsesSnapshotProblem("void other() {}").empty());
}

TEST_CASE("snapshotBlock reads the sequences only after an acquire fence that follows the parameter snapshot",
          "[host][wiring][order][source]")
{
    const juce::File input = repoRoot().getChildFile("src").getChildFile("input");
    REQUIRE(input.isDirectory()); // 路径推不出来就是红，不静默跳过

    // ⛔ 删掉 snapshotBlock 里的 acquire 栅栏，或把它挪到参数快照之前 → 本用例变红
    CHECK(snapshotOrderProblem(readSource(input.getChildFile("InputProcessor.h"))) == "");
    // ⛔ process 绕过 snapshotBlock 自己读参数与序号 → 本用例变红
    CHECK(processUsesSnapshotProblem(readSource(input.getChildFile("InputProcessor.cpp"))) == "");
}
