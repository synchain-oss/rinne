// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针日志单测的公用件：临时目录、可注入的假时钟、文件读取与按行 / 按列拆分、仓内契约文档与夹具的路径，
// 以及一套确定性的「标准场景」（夹具文件就是它写出来的）。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "diag/ProbeLog.h"
#include "ipc/ProcessInfo.h"

namespace rinne::test::probe
{

namespace fs = std::filesystem;
using namespace rinne::diag;

// ---- 路径 ----
// 仓根由本文件的位置推出（tests/core/probe/ 往上四级）：测试不依赖工作目录，也不读环境变量
inline fs::path repoRoot()
{
    return fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

inline fs::path fixtureDir()
{
    return fs::path(__FILE__).parent_path() / "fixtures";
}

inline fs::path logFormatDoc()
{
    return repoRoot() / "scripts" / "probe" / "LOG-FORMAT.md";
}

// ---- 临时目录 ----
// 系统临时目录下新建一个本进程独占的目录；用完只删里面的普通文件和这个空目录，不做递归删除
class TempDir
{
public:
    TempDir()
    {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const fs::path candidate =
                base / ("rinne-probe-test-" + std::to_string(rinne::ipc::currentProcessId()) + "-" +
                        std::to_string(counter.fetch_add(1)) + "-" + std::to_string(probeMonoNs() % 1000000007u));
            if (fs::create_directory(candidate, ec))
            {
                path_ = candidate;
                return;
            }
        }
    }
    ~TempDir()
    {
        if (path_.empty())
        {
            return;
        }
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(path_, ec), end; !ec && it != end; it.increment(ec))
        {
            files.push_back(it->path());
        }
        for (const fs::path& file : files)
        {
            fs::remove(file, ec);
        }
        fs::remove(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }
    std::string utf8() const { return path_.u8string(); }

private:
    fs::path path_;
};

// ---- 文件与文本 ----
inline std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// 按 LF 拆行，不含行尾；最后一行没有 LF 时也算一行（调用方另行检查结尾）
inline std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
        {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

inline std::vector<std::string> splitFields(const std::string& line, char sep = ',')
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t end = line.find(sep, start);
        if (end == std::string::npos)
        {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

// 某一类行（第一列等于 kind）
inline std::vector<std::vector<std::string>> rowsOfKind(const std::string& text, const std::string& kind)
{
    std::vector<std::vector<std::string>> rows;
    for (const std::string& line : splitLines(text))
    {
        std::vector<std::string> fields = splitFields(line);
        if (!fields.empty() && fields[0] == kind)
        {
            rows.push_back(std::move(fields));
        }
    }
    return rows;
}

// ---- 结构检查 ----
// 只看形状：魔数、H 在第 2 行、纯 ASCII、没有 CR、以 LF 结尾、每类行的列数。通过返回空串，否则返回问题的描述。
// 取值是否合规由 scripts/probe 的分析器把关，这里不重复实现它。
inline std::string structureProblem(const std::string& text)
{
    if (text.empty() || text.back() != '\n')
    {
        return "文件为空或不以 LF 结尾";
    }
    for (const char c : text)
    {
        if (c == '\r' || static_cast<unsigned char>(c) >= 0x80)
        {
            return "含 CR 或非 ASCII 字节";
        }
    }
    const std::vector<std::string> lines = splitLines(text);
    if (lines.size() < 2 || lines[0] != "rinne-probe-log,1")
    {
        return "第 1 行不是魔数";
    }
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        const std::vector<std::string> fields = splitFields(lines[i]);
        const std::string& kind = fields[0];
        std::size_t columns = 0;
        if (kind == "H")
            columns = 10;
        else if (kind == "C")
            columns = 7;
        else if (kind == "B")
            columns = 18;
        else if (kind == "S")
            columns = 7;
        else if (kind == "E")
            columns = 5;
        else if (kind == "Z")
            columns = 4;
        else
            return "行类型未知";
        if (fields.size() != columns)
            return "第 " + std::to_string(i + 1) + " 行列数不对";
        if ((kind == "H") != (i == 1))
            return "H 行必须且只能在第 2 行";
        if (kind == "Z" && i + 1 != lines.size())
            return "Z 行之后还有内容";
    }
    return "";
}

// ---- 假时钟 ----
// 每读一次前进 step 纳秒；hook 在每次读之前被调用（测试用它在两次读之间插入「另一个线程的动作」）
struct FakeClock
{
    std::atomic<std::uint64_t> value{1000000000};
    std::uint64_t step = 100;
    void (*hook)(FakeClock&) = nullptr;
    void* hookContext = nullptr;

    static std::uint64_t tick(void* context) noexcept
    {
        auto* self = static_cast<FakeClock*>(context);
        if (self->hook != nullptr)
        {
            self->hook(*self);
        }
        return self->value.fetch_add(self->step);
    }
};

// ---- 构造 ----
inline ProbeHeader testHeader(ProbeRole role, const std::string& tok4 = "")
{
    ProbeHeader header;
    header.role = role;
    header.format = ProbeFormat::kVst3;
    header.host = ProbeHost::kCubase;
    header.os = ProbeOs::kWin;
    header.arch = ProbeArch::kX64;
    header.pid = 4100;
    header.ppid = 900;
    header.tok4 = tok4;
    header.build = "0123abcd";
    return header;
}

// 手动 pump、假时钟、小环：确定性的测试配置
inline ProbeLogOptions manualOptions(const TempDir& dir, ProbeRole role, FakeClock& clock, const std::string& tok4,
                                     std::size_t blockSlots = 64)
{
    ProbeLogOptions options;
    options.directory = dir.utf8();
    options.header = testHeader(role, tok4);
    options.blockRingSlots = blockSlots;
    options.startThread = false;
    options.clock = &FakeClock::tick;
    options.clockContext = &clock;
    return options;
}

// 一个回调的确定性字段（稳态：m = 0，off 恒定，w 逐块前进 n）
inline BlockFields steadyBlock(ProbeRole role, std::uint32_t index, std::uint64_t monoBaseNs)
{
    BlockFields f;
    f.callSeq = std::uint64_t{index} * 2 + (role == ProbeRole::kInput ? 0u : 1u);
    f.monoNs = monoBaseNs + std::uint64_t{index} * 5333333u;
    f.hostNs = 7000000000u + std::uint64_t{index} * 5333333u;
    f.hostNsValid = true;
    f.playing = true;
    f.n = 256;
    f.nDecl = 256;
    f.thread = 0x1a2b3c4du;
    f.m = 0;
    f.w = 4800u + std::uint64_t{index} * 256u;
    f.off = 480;
    f.rs = index == 0 ? 1u : 0u;
    f.d = 9600;
    return f;
}

constexpr std::uint32_t kStandardBlocks = 48;

// 标准场景：一个实例从创建到收尾写出的完整文件（夹具就是它）。
// prepare / backend / paired 先于选 case 发生，所以会在 C 行之后以快照再出现一次；Output 一侧另收到一条带逗号、换行、
// 引号、非 ASCII 和百分号的轨道名，验证百分号编码。
inline fs::path writeStandardLog(const TempDir& dir, ProbeRole role, const std::string& tok4)
{
    FakeClock clock;
    ProbeLog log(manualOptions(dir, role, clock, tok4, 128));
    log.logEvent(ProbeEventCode::kPrepare, ProbeEventArgs{48000, 256, {}});
    log.logEvent(ProbeEventCode::kBackend, ProbeEventArgs{0, 0, {}});
    log.logEvent(ProbeEventCode::kPaired, ProbeEventArgs{4100, 0, {}});
    if (role == ProbeRole::kOutput)
    {
        log.logEvent(ProbeEventCode::kTrackProps,
                     ProbeEventArgs{0, probeColourArg(0xff8800), "Vox, Lead\n\"\xC3\xA9\xE4\xB8\xAD\"%"});
    }
    ProbeCase c;
    c.caseId = "CB-01";
    c.variant = "steady";
    c.buf = 256;
    c.guard = ProbeGuard::kLow;
    c.sampleRate = 48000;
    log.beginCase(c);
    const std::uint64_t base = clock.value.load() + 1000000;
    for (std::uint32_t i = 0; i < kStandardBlocks; ++i)
    {
        if (i == 0)
        {
            log.onAudioEvent(ProbeEventCode::kFirstBlock, 0, 0, base);
            log.onAudioEvent(ProbeEventCode::kFirstPairedBlock, 0, 0, base);
        }
        log.onBlock(steadyBlock(role, i, base));
    }
    log.pump();
    clock.value.fetch_add(1000000000);
    log.pump();
    log.endCase();
    clock.value.fetch_add(1000000000);
    log.pump();
    log.logEvent(ProbeEventCode::kDestruct);
    const fs::path path = fs::u8path(log.filePath());
    log.close();
    return path;
}

} // namespace rinne::test::probe
