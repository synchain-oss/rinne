// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针接线：两个 ProbeHost 在同一进程里经真实共享内存配对（测试命名空间注入，不碰正式前缀），时钟是假的、
// 日志不起落盘线程、定时器不起，tick 由测试按 25 Hz 的节拍调。写出的 CSV 读回来逐列核对 LOG-FORMAT 的定义。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../shell/AllocationCounter.h"
#include "../shell/ShellTestKit.h"
#include "ProbeHost.h"
#include "RinneProcessorBase.h"
#include "ipc/IpcDiag.h"
#include "ipc/LabelSeqlock.h"
#include "ipc/LoopBinding.h"
#include "ipc/Naming.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/ProcessInfo.h"
#include "ipc/SegmentLayout.h"
#include "state/StateCodec.h"

namespace
{

using rinne::probe::ProbeHost;
using rinne::probe::ProbeHostConfig;
using rinne::probe::ProbeSelection;
namespace diag = rinne::diag;
namespace engine = rinne::engine;
namespace ipc = rinne::ipc;
namespace probe = rinne::probe;
namespace fs = std::filesystem;

constexpr double kRate = 48000.0;
constexpr int kBlock = 480; // 10 ms
constexpr std::uint64_t kBlockNs = 10'000'000;
constexpr std::int64_t kDelaySamples = 4800; // 默认 D = 100 ms

struct FakeClock
{
    std::uint64_t ns = 10'000'000'000ull;
    static std::uint64_t read(void* context) noexcept { return static_cast<FakeClock*>(context)->ns; }
};

class FakePlayHead final : public juce::AudioPlayHead
{
public:
    bool playing = false;
    std::int64_t samples = 0;
    bool hostTime = true;
    std::uint64_t hostNs = 0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setTimeInSamples(samples);
        info.setBpm(120.0);
        if (hostTime)
            info.setHostTimeNs(hostNs);
        return info;
    }
};

// 最小的处理器：基类给总线与宿主约定，ProbeHost 是成员（正式构建里基类不挂探针，这里直接持有）
class TestProcessor final : public rinne::RinneProcessorBase
{
public:
    explicit TestProcessor(ProbeHostConfig config) : host(*this, std::move(config)) {}

    void prepareToPlay(double sampleRate, int maxBlock) override { host.prepare(sampleRate, maxBlock); }
    void releaseResources() override { host.release(); }
    void reset() override { host.reset(); }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (beginBlock(buffer.getNumSamples()))
            host.process(buffer, false);
    }
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (beginBlock(buffer.getNumSamples()))
            host.process(buffer, true);
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    void getStateInformation(juce::MemoryBlock& dest) override { host.getState(dest); }
    void setStateInformation(const void* data, int size) override { host.setState(data, size); }
    void updateTrackProperties(const TrackProperties& properties) override { host.trackProperties(properties); }

    ProbeHost host;
};

using Row = std::vector<std::string>;

Row split(const std::string& line)
{
    Row out(1);
    for (const char c : line)
    {
        if (c == ',')
            out.emplace_back();
        else
            out.back().push_back(c);
    }
    return out;
}

std::uint64_t num(const std::string& s)
{
    return std::stoull(s);
}

std::int64_t snum(const std::string& s)
{
    return std::stoll(s);
}

// 同一进程、同一测试作用域里的一对探针，外加假时钟、假播放头与一份左右声道的宿主缓冲
class Rig
{
public:
    Rig() : scope_(ipc::NameScope::randomForTesting())
    {
        std::random_device seed;
        dir_ = fs::temp_directory_path() / ("rinne-probe-host-" + std::to_string(seed()) + std::to_string(seed()));
        fs::create_directories(dir_);
        left_.assign(kLongestBlock, 0.0f);
        right_.assign(kLongestBlock, 0.0f);
        dry_.assign(kLongestBlock, 0.0f);
    }

    ~Rig()
    {
        input.reset();
        output.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ProbeHostConfig config(ipc::Role role, bool validation = false)
    {
        ProbeHostConfig c;
        c.role = role;
        c.validationHost = validation;
        c.scope = scope_;
        c.logDirectory = dir_.string();
        c.startLogThread = false;
        c.startTimer = false;
        c.clock = &FakeClock::read;
        c.clockContext = &clock;
        return c;
    }

    // 建两端、prepare、设同一个组，tick 到两端都持有槽
    void open(int group = 0, bool validation = false)
    {
        input = std::make_unique<TestProcessor>(config(ipc::Role::kInput, validation));
        output = std::make_unique<TestProcessor>(config(ipc::Role::kOutput, validation));
        for (TestProcessor* p : {input.get(), output.get()})
        {
            p->setPlayHead(&playHead);
            p->prepareToPlay(kRate, kBlock);
            if (group >= 0)
                p->host.probeSetGroup(group);
            p->host.tick();
        }
    }

    // 一个周期：信号源填缓冲 → Input → Output（Output 在推子前的最后一格，看到的是 Input 的输出）。
    // 时钟与时间线前进一块，每 4 块（40 ms）tick 一次两端。bypassInput 为真时 Input 走旁路回调，并逐位核对输出与输入
    void cycle(bool runInput = true, bool runOutput = true, int frames = kBlock)
    {
        REQUIRE(frames > 0);
        REQUIRE(frames <= kLongestBlock);
        const auto count = static_cast<std::size_t>(frames);
        for (std::size_t i = 0; i < count; ++i)
        {
            const float v = source ? source(position_ + i) : 0.0f;
            left_[i] = v;
            right_[i] = v;
            dry_[i] = v;
        }
        float* channels[2] = {left_.data(), right_.data()};
        juce::AudioBuffer<float> buffer(channels, 2, frames);
        juce::MidiBuffer midi;
        playHead.hostNs = clock.ns;
        if (runInput && input != nullptr)
        {
            if (bypassInput)
            {
                input->processBlockBypassed(buffer, midi);
                bypassedSamples += count;
                for (std::size_t i = 0; i < count; ++i)
                    if (std::memcmp(&left_[i], &dry_[i], sizeof(float)) != 0 ||
                        std::memcmp(&right_[i], &dry_[i], sizeof(float)) != 0)
                        ++bypassInexact;
            }
            else
            {
                input->processBlock(buffer, midi);
            }
        }
        if (record)
            recorded.insert(recorded.end(), left_.begin(), left_.begin() + frames);
        if (runOutput && output != nullptr)
            output->processBlock(buffer, midi);
        advance(static_cast<std::uint64_t>(std::llround(static_cast<double>(frames) * 1.0e9 / kRate)));
        position_ += count;
        if (playHead.playing)
            playHead.samples += frames;
    }

    void cycles(int count, bool runInput = true, bool runOutput = true)
    {
        for (int i = 0; i < count; ++i)
            cycle(runInput, runOutput);
    }

    // 不调音频回调，只让时间流过（宿主把两端挂起），照样 tick
    void advance(std::uint64_t ns)
    {
        for (std::uint64_t left = ns; left > 0;)
        {
            const std::uint64_t step = std::min<std::uint64_t>(left, kBlockNs);
            clock.ns += step;
            left -= step;
            sinceTick_ += step;
            if (sinceTick_ >= 4 * kBlockNs)
            {
                sinceTick_ = 0;
                if (ticking)
                    tick();
            }
        }
    }

    void tick()
    {
        if (input != nullptr)
            input->host.tick();
        if (output != nullptr)
            output->host.tick();
    }

    bool runUntil(const std::function<bool()>& done, int maxCycles)
    {
        for (int i = 0; i < maxCycles; ++i)
        {
            if (done())
                return true;
            cycle();
        }
        return done();
    }

    // 关掉两端（写 Z），读回某一端的日志
    std::vector<Row> closeAndRead(char role)
    {
        input.reset();
        output.reset();
        return read(role);
    }

    std::vector<Row> read(char role) const
    {
        std::vector<Row> rows;
        const std::string prefix = std::string("probe-") + role + "-";
        for (const auto& entry : fs::directory_iterator(dir_))
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) != 0)
                continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::string line;
            while (std::getline(in, line))
                rows.push_back(split(line));
        }
        return rows;
    }

    std::size_t logFiles() const
    {
        std::size_t count = 0;
        for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir_))
            ++count;
        return count;
    }

    const ipc::NameScope& scope() const noexcept { return scope_; }

    FakeClock clock;
    FakePlayHead playHead;
    std::function<float(std::uint64_t)> source;
    std::unique_ptr<TestProcessor> input;
    std::unique_ptr<TestProcessor> output;
    bool record = false;
    std::vector<float> recorded; // record 为真时 Input 处理之后的左声道
    bool ticking = true; // 假 = 时间照走、不 tick（要停在两次 tick 之间看的用例）
    bool bypassInput = false;
    std::size_t bypassedSamples = 0; // Input 走旁路回调的样本数
    std::size_t bypassInexact = 0; // 其中输出与输入不逐位相同的样本数（任一声道）

    static constexpr int kLongestBlock = 40960;

private:
    ipc::NameScope scope_;
    fs::path dir_;
    std::vector<float> left_;
    std::vector<float> right_;
    std::vector<float> dry_;
    std::uint64_t position_ = 0;
    std::uint64_t sinceTick_ = 0;
};

// 测试自己再映射一次本作用域的会合段，直接看槽里的字
class RvPeek
{
public:
    explicit RvPeek(const ipc::NameScope& scope)
    {
        const ipc::Naming naming(scope);
        std::string name;
        REQUIRE(naming.name(ipc::SegmentId::rendezvous(), name) == ipc::NameStatus::kOk);
        REQUIRE(backend_.openExisting(name, view_) == ipc::InitResult::kOk);
        REQUIRE(view_.size >= sizeof(ipc::RvSegment));
    }
    ~RvPeek() { backend_.unmap(view_); }
    RvPeek(const RvPeek&) = delete;
    RvPeek& operator=(const RvPeek&) = delete;

    ipc::RvSegment& rv() const { return *static_cast<ipc::RvSegment*>(view_.base); }

private:
    ipc::PlatformSegmentBackend backend_;
    ipc::SegmentView view_;
};

std::vector<Row> rowsOf(const std::vector<Row>& rows, const char* type)
{
    std::vector<Row> out;
    for (const Row& r : rows)
        if (!r.empty() && r[0] == type)
            out.push_back(r);
    return out;
}

std::vector<Row> eventsNamed(const std::vector<Row>& rows, const std::string& name)
{
    std::vector<Row> out;
    for (const Row& r : rowsOf(rows, "E"))
        if (r.size() >= 3 && r[2] == name)
            out.push_back(r);
    return out;
}

bool paired(const Rig& rig)
{
    return rig.input->host.probeStatus().paired && rig.output->host.probeStatus().paired;
}

ProbeSelection cell(std::uint32_t buf = 256, diag::ProbeGuard guard = diag::ProbeGuard::kNormal)
{
    ProbeSelection s;
    s.caseId = "CB-01";
    s.variant = "steady";
    s.buf = buf;
    s.guard = guard;
    return s;
}

int indexOf(std::size_t count, std::string (*at)(std::size_t), const std::string& wanted)
{
    for (std::size_t i = 0; i < count; ++i)
        if (at(i) == wanted)
            return static_cast<int>(i);
    return -1;
}

void setParam(ProbeHost& host, const char* id, int value)
{
    auto* parameter = host.parameters().getParameter(id);
    REQUIRE(parameter != nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

float sine(std::uint64_t i)
{
    return 0.25f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / kRate));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// 相邻样本之差的最大值，从 previous 起算
float largestStep(const std::vector<float>& samples, std::size_t from, float previous)
{
    float largest = 0.0f;
    for (std::size_t i = from; i < samples.size(); ++i)
    {
        largest = std::max(largest, std::abs(samples[i] - previous));
        previous = samples[i];
    }
    return largest;
}

// 打不开段的后端：建、开都被拒（同宿主沙盒拒绝）。error 不为 0 时先在调用线程上经 IpcDiag 报一条带它的诊断，
// 和真后端在失败点报的一样。transient 次之前的「建」只是暂时失败（下一拍重试），什么都不报
class DeniedBackend final : public ipc::ISegmentBackend
{
public:
    explicit DeniedBackend(int error, int transient = 0) : error_(error), transient_(transient) {}
    ~DeniedBackend() override { forgetRetired(); }

    ipc::InitResult createOrOpen(const std::string& name, std::size_t, ipc::SegmentView&) override
    {
        if (transient_ > 0)
        {
            --transient_;
            return ipc::InitResult::kFailed;
        }
        if (error_ != 0)
        {
            ipc::IpcDiagEvent e;
            e.op = ipc::IpcDiagOp::kOpen;
            e.code = ipc::IpcDiagCode::kShmDenied;
            e.error = error_;
            e.segment = name.c_str();
            e.detail = "denied by the test backend";
            ipc::reportIpcDiag(e);
        }
        return ipc::InitResult::kFailedDenied;
    }
    ipc::InitResult openExisting(const std::string&, ipc::SegmentView&) override { return ipc::InitResult::kFailed; }
    void unmap(ipc::SegmentView& view) noexcept override { view = ipc::SegmentView(); }

private:
    int error_;
    int transient_;
};

// 诊断出口的用例：一个只数次数的出口，和一个「再装一次」的钩子（模拟另一个线程恰在这时也在装）
std::atomic<int> gForwarded{0};

void countingSink(const ipc::IpcDiagEvent&) noexcept
{
    gForwarded.fetch_add(1);
}

void installAgain() noexcept
{
    ProbeHost::installIpcDiagSink();
}

// 用例结束后把槽还给转发出口、转发目标还原成用例开始时的那个
class SinkRestore
{
public:
    SinkRestore() : forward_(ProbeHost::ipcDiagForwardTarget()) {}
    ~SinkRestore()
    {
        ProbeHost::setIpcDiagInstallHook(nullptr);
        ipc::setIpcDiagSink(forward_);
        ProbeHost::installIpcDiagSink();
    }
    SinkRestore(const SinkRestore&) = delete;
    SinkRestore& operator=(const SinkRestore&) = delete;

private:
    ipc::IpcDiagSink forward_;
};

} // namespace

TEST_CASE("two probe hosts pair over real shared memory and write strictly ordered block rows", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    REQUIRE(rig.input->host.rendezvousState() == ipc::RendezvousState::kActive);
    REQUIRE(rig.output->host.rendezvousState() == ipc::RendezvousState::kActive);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(50); // 配对后的淡入与对齐走完

    const std::uint64_t windowStart = rig.clock.ns;
    REQUIRE(rig.input->host.probeStartRecording(cell(), 1));
    REQUIRE(rig.output->host.probeStartRecording(cell(), 1));
    rig.cycles(60);
    const std::vector<Row> inRows = rig.closeAndRead('I');
    const std::vector<Row> outRows = rig.read('O');

    // 头两行：魔数与 H；最后一行是 Z
    for (const auto* rows : {&inRows, &outRows})
    {
        REQUIRE(rows->size() > 3);
        CHECK((*rows)[0] == Row{"rinne-probe-log", "1"});
        CHECK((*rows)[1][0] == "H");
        CHECK(rows->back()[0] == "Z");
    }
    CHECK(inRows[1][1] == "I");
    CHECK(outRows[1][1] == "O");
    CHECK(inRows[1][6] == std::to_string(ipc::currentProcessId()));

    const std::vector<Row> inBlocks = rowsOf(inRows, "B");
    const std::vector<Row> outBlocks = rowsOf(outRows, "B");
    REQUIRE(inBlocks.size() == 60);
    REQUIRE(outBlocks.size() == 60);

    // C 行的 t_ns 不晚于窗口里第一块的 mono_ns；C 的各列就是面板交来的选择
    const std::vector<Row> inCases = rowsOf(inRows, "C");
    REQUIRE(inCases.size() == 1);
    CHECK(inCases[0] == Row{"C", inCases[0][1], "CB-01", "steady", "256", "normal", "48000"});
    CHECK(num(inCases[0][1]) >= windowStart);
    CHECK(num(inCases[0][1]) <= num(inBlocks.front()[11]));

    // call_seq 在每个文件里严格递增，两端合起来恰好交错（每个周期 Input 先、Output 后）
    std::vector<std::pair<std::uint64_t, char>> order;
    for (const auto* blocks : {&inBlocks, &outBlocks})
    {
        bool first = true;
        std::uint64_t lastSeq = 0;
        std::uint64_t lastCall = 0;
        for (const Row& b : *blocks)
        {
            REQUIRE(b.size() == 18);
            CHECK((first || num(b[1]) > lastSeq));
            CHECK(num(b[2]) > lastCall);
            first = false;
            lastSeq = num(b[1]);
            lastCall = num(b[2]);
            order.emplace_back(num(b[2]), b[3][0]);
        }
    }
    std::sort(order.begin(), order.end());
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        CHECK(order[i].second == (i % 2 == 0 ? 'I' : 'O'));
        if (i > 0)
            CHECK(order[i].first == order[i - 1].first + 1);
    }

    // 各列：块长、声明的最大块长、宿主时间戳、走带、离线、旁路、单调时钟、线程散列，以及 m / w / off / rs / d
    for (const auto* blocks : {&inBlocks, &outBlocks})
    {
        std::set<std::string> offs;
        for (std::size_t i = 0; i < blocks->size(); ++i)
        {
            const Row& b = (*blocks)[i];
            CHECK(b[4] == "480");
            CHECK(b[5] == "480");
            CHECK(b[6] == b[11]); // 假播放头的宿主时间戳就是块首的假时钟
            CHECK(b[7] == "1");
            CHECK(b[8] == "1");
            CHECK(b[9] == "0");
            CHECK(b[10] == "0");
            CHECK(b[12].size() == 8);
            CHECK(b[13] == "0");
            CHECK(b[16] == "0");
            CHECK(snum(b[17]) == kDelaySamples);
            offs.insert(b[15]);
            if (i > 0)
            {
                CHECK(num(b[11]) - num((*blocks)[i - 1][11]) == kBlockNs);
                CHECK(num(b[14]) - num((*blocks)[i - 1][14]) == 480);
            }
        }
        CHECK(offs.size() == 1);
    }

    // 事件：prepare、backend（win32 或 posix，成功 0）、paired（对端 pid 与组 A）、首块、首个配对块、处理模式；
    // C 之后补写的快照里有一条空的 track_props
    for (const auto* rows : {&inRows, &outRows})
    {
        const auto prepare = eventsNamed(*rows, "prepare");
        REQUIRE(!prepare.empty());
        CHECK(prepare[0][3] == "48000");
        CHECK(prepare[0][4] == "480");
        const auto backend = eventsNamed(*rows, "backend");
        REQUIRE(!backend.empty());
        CHECK((backend[0][3] == "win32" || backend[0][3] == "posix"));
        CHECK(backend[0][4] == "0");
        // 配对只发生一次：原事件一条，C 之后的快照一条
        const auto pairedEvents = eventsNamed(*rows, "paired");
        REQUIRE(pairedEvents.size() == 2);
        CHECK(pairedEvents[0][3] == std::to_string(ipc::currentProcessId()));
        CHECK(pairedEvents[0][4] == "A");
        CHECK(eventsNamed(*rows, "first_block").size() == 1);
        CHECK(eventsNamed(*rows, "first_paired_block").size() == 1);
        const auto mode = eventsNamed(*rows, "process_mode");
        REQUIRE(mode.size() == 1);
        CHECK(mode[0][3] == "realtime");
        CHECK(eventsNamed(*rows, "offline_render").empty());
        const auto track = eventsNamed(*rows, "track_props");
        REQUIRE(track.size() == 1);
        CHECK(track[0][3].empty());
        CHECK(track[0][4].empty());
        CHECK(eventsNamed(*rows, "destruct").size() == 1);
    }
}

TEST_CASE("track properties, reset and offline changes are logged as events", "[probe_host]")
{
    Rig rig;
    rig.open(1);
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(30); // 观察者每 250 ms 扫一次：paired 事件要等它看到对端
    juce::AudioProcessor::TrackProperties props;
    props.name = juce::String("Vox, 1");
    props.colour = juce::Colour(0xff336699u);
    rig.input->updateTrackProperties(props);
    props.colour = juce::Colour(0x80336699u);
    rig.input->updateTrackProperties(props);
    rig.input->reset();
    rig.input->setNonRealtime(true);
    rig.cycles(8);
    rig.input->setNonRealtime(false);
    rig.cycles(8);

    // 名字与颜色也交给了会合层：本端槽（组 B）里的标签是最后一次送来的
    {
        const RvPeek peek(rig.scope());
        ipc::LabelPayload label;
        REQUIRE(ipc::readLabel(ipc::roleSlot(peek.rv(), 2, ipc::Role::kInput), label));
        CHECK(label.colourArgb == 0x80336699u);
        CHECK(label.nameLen == 6u);
    }
    const std::vector<Row> rows = rig.closeAndRead('I');

    const auto track = eventsNamed(rows, "track_props");
    REQUIRE(track.size() == 2);
    CHECK(track[0][3] == diag::probeEncodeText("Vox, 1"));
    CHECK(track[0][4] == "336699");
    CHECK(track[1][4] == "33669980");
    CHECK(eventsNamed(rows, "reset").size() == 1);
    const auto offline = eventsNamed(rows, "offline_render");
    REQUIRE(offline.size() == 2);
    CHECK(offline[0][3] == "1");
    CHECK(offline[1][3] == "0");
    const auto mode = eventsNamed(rows, "process_mode");
    REQUIRE(mode.size() == 3);
    CHECK(mode[1][3] == "offline");
    const auto paired = eventsNamed(rows, "paired");
    REQUIRE(!paired.empty());
    CHECK(paired[0][4] == "B");
}

TEST_CASE("hidden parameters open a recording window after load and close it on time", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));

    ProbeHost& host = rig.input->host;
    const int caseIndex = indexOf(probe::caseCount(), &probe::caseAt, "CB-01");
    const int variantIndex = indexOf(probe::variantCount(), &probe::variantAt, "steady");
    REQUIRE(caseIndex >= 0);
    REQUIRE(variantIndex >= 0);

    // 不合规则表的组合不开窗
    setParam(host, "auto_case", caseIndex + 1);
    setParam(host, "auto_variant", 0);
    setParam(host, "auto_record_s", 1);
    rig.cycles(10);
    CHECK_FALSE(host.log()->isRecording());

    setParam(host, "auto_variant", variantIndex + 1);
    rig.cycles(4);
    REQUIRE(host.log()->isRecording());
    CHECK(host.probeStatus().recordRemainingSeconds == 1);
    rig.cycles(110);
    CHECK_FALSE(host.log()->isRecording());
    // 同一组值不再开第二次；auto_record_s 回到 0 再给就再开一次
    rig.cycles(20);
    CHECK_FALSE(host.log()->isRecording());
    setParam(host, "auto_record_s", 0);
    rig.cycles(4);
    setParam(host, "auto_record_s", 1);
    rig.cycles(4);
    CHECK(host.log()->isRecording());
    rig.cycles(110);
    CHECK_FALSE(host.log()->isRecording());

    const std::vector<Row> rows = rig.closeAndRead('I');
    const auto cases = rowsOf(rows, "C");
    REQUIRE(cases.size() == 2);
    for (const Row& c : cases)
        CHECK(c == Row{"C", c[1], "CB-01", "steady", "-", "na", "48000"});
    // 每个窗口 1 s：落在窗口里的块在 100 块左右（tick 的节拍是 40 ms）
    const auto blocks = rowsOf(rows, "B");
    CHECK(blocks.size() >= 200);
    CHECK(blocks.size() <= 216);
}

TEST_CASE("ping measures the loop delay with no gain step and hands the return to one preset fade-in", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.input->host.probeSetFeedbackPercent(50.0f);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(300); // 3 s：回送在环里转起来

    const auto before = rig.input->host.counts();
    rig.record = true;
    rig.playHead.playing = false; // 停走带：自动 Ping
    rig.cycles(4);
    REQUIRE(rig.input->host.probeStatus().pingRunning);
    REQUIRE(rig.runUntil([&] { return !rig.input->host.probeStatus().pingRunning; }, 600));
    rig.cycles(100);
    rig.record = false;

    const probe::ProbeStatus status = rig.input->host.probeStatus();
    CHECK(status.lastPingSamples == kDelaySamples);
    CHECK(rig.input->host.inputEngine().lastBlockResult().delayEffective == kDelaySamples);
    const auto after = rig.input->host.counts();
    const auto preset = static_cast<std::size_t>(engine::FadeReason::Preset);
    CHECK(after.fadeIns[preset] - before.fadeIns[preset] == 1);
    CHECK(after.panicsExecuted == 0);

    // 逐样本差超过 0.03 的点都落在一段不超过 128 个样本的区间里：只有 MLS 本身（±0.1）。
    // 正弦干声 0.25、220 Hz 自身的逐样本差不到 0.008，回送更小；斜坡每个样本最多挪 1/480
    std::vector<std::size_t> steps;
    for (std::size_t i = 1; i < rig.recorded.size(); ++i)
        if (std::abs(rig.recorded[i] - rig.recorded[i - 1]) > 0.03f)
            steps.push_back(i);
    REQUIRE(!steps.empty());
    CHECK(steps.back() - steps.front() <= 128);

    const std::vector<Row> rows = rig.closeAndRead('I');
    const auto pings = eventsNamed(rows, "ping");
    REQUIRE(pings.size() == 1);
    CHECK(pings[0][3] == std::to_string(kDelaySamples));
}

TEST_CASE("ping is refused while playing with a live dry signal and cancelled when play resumes", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.input->host.probePing();
    rig.cycles(4);
    CHECK_FALSE(rig.input->host.probeStatus().pingRunning);

    // 停走带自动 Ping，还在静音段里就恢复播放：取消，不留结果；回送照样走一次 Preset 淡入回来
    const auto preset = static_cast<std::size_t>(engine::FadeReason::Preset);
    const auto before = rig.input->host.counts();
    rig.playHead.playing = false;
    rig.cycles(8);
    REQUIRE(rig.input->host.probeStatus().pingRunning);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return !rig.input->host.probeStatus().pingRunning; }, 20));
    rig.cycles(8);
    CHECK(rig.input->host.probeStatus().lastPingSamples == -1);
    CHECK(rig.input->host.counts().fadeIns[preset] - before.fadeIns[preset] == 1);

    // 走带中但干声静音：可以点，也不会被取消，跑完求出 D
    rig.source = nullptr;
    rig.input->host.probeSetFeedbackPercent(50.0f);
    rig.cycles(4);
    rig.input->host.probePing();
    rig.cycles(4);
    REQUIRE(rig.input->host.probeStatus().pingRunning);
    REQUIRE(rig.runUntil([&] { return !rig.input->host.probeStatus().pingRunning; }, 600));
    rig.cycles(8); // 结果由 tick 收
    CHECK(rig.input->host.probeStatus().lastPingSamples == kDelaySamples);
}

TEST_CASE("clicking ping right after one finished, before the tick, keeps the finished result", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.input->host.probeSetFeedbackPercent(50.0f);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(50);
    rig.playHead.playing = false; // 停走带：自动 Ping
    rig.cycles(4);
    REQUIRE(rig.input->host.probeStatus().pingRunning);

    // tick 不来：跑完的 Ping 结果还留在状态机里没收
    rig.ticking = false;
    REQUIRE(rig.runUntil([&] { return !rig.input->host.probeStatus().pingRunning; }, 600));
    CHECK(rig.input->host.probeStatus().lastPingSamples == -1);

    // 这时在面板上点 Ping（停着走带，可以点）：先收下上一次的结果，再开下一次
    rig.input->host.probePing();
    CHECK(rig.input->host.probeStatus().lastPingSamples == kDelaySamples);
    CHECK(rig.input->host.probeStatus().pingRunning);
    rig.ticking = true;
    rig.cycles(8);

    const std::vector<Row> rows = rig.closeAndRead('I');
    const auto pings = eventsNamed(rows, "ping");
    REQUIRE(pings.size() == 1);
    CHECK(pings[0][3] == std::to_string(kDelaySamples));
}

TEST_CASE("after a ping the dry signal is back at full level before the return is released", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    // 直流干声、反馈 0：输出里只有干声，看得清它在哪个样本回到满幅
    rig.source = [](std::uint64_t) { return 0.2f; };
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(50);
    rig.record = true;
    rig.playHead.playing = false; // 停走带：自动 Ping
    rig.cycles(4);
    REQUIRE(rig.input->host.probeStatus().pingRunning);
    REQUIRE(rig.runUntil([&] { return !rig.input->host.probeStatus().pingRunning; }, 600));

    // 录下的最后一块里 Ping 走完（回送的系数在这一块里升满）。干声的 10 ms 回升之后，回送还要再等 10 ms、再升 10 ms：
    // 输出最后一次偏离满幅的样本必须早于这一块的前一个样本
    const float full = rig.recorded.back();
    CHECK(std::abs(full) > 0.1f);
    std::size_t lastOff = 0;
    for (std::size_t i = 0; i < rig.recorded.size(); ++i)
        if (std::abs(rig.recorded[i] - full) > 1.0e-6f)
            lastOff = i;
    REQUIRE(lastOff > 0);
    const std::size_t lastBlockStart = rig.recorded.size() - static_cast<std::size_t>(kBlock);
    CHECK(lastOff + 1 < lastBlockStart);
}

TEST_CASE("panic and recover each execute exactly once", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.input->host.probeSetFeedbackPercent(30.0f);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(100);

    rig.input->host.probePanic();
    rig.cycles(20);
    CHECK(rig.input->host.counts().panicsExecuted == 1);
    CHECK(rig.input->host.probeStatus().tripped);
    rig.cycles(200); // 冷却 1.5 s 过去；Panic 不重放
    CHECK(rig.input->host.counts().panicsExecuted == 1);

    rig.input->host.probeRecover();
    rig.cycles(20);
    CHECK(rig.input->host.counts().recoversExecuted == 1);
    CHECK_FALSE(rig.input->host.probeStatus().tripped);
    rig.cycles(100);
    CHECK(rig.input->host.counts().recoversExecuted == 1);
    CHECK(rig.input->host.counts().panicsExecuted == 1);
    // Panic 不算安全跳闸
    CHECK(rig.input->host.log()->counters().trips == 0);

    // Output 端这三个按钮什么都不做；它告诉面板自己没有回送，面板把它们灰掉
    CHECK(rig.input->host.probeHasReturn());
    CHECK_FALSE(rig.output->host.probeHasReturn());
    rig.output->host.probePanic();
    rig.output->host.probeRecover();
    rig.output->host.probePing();
    rig.cycles(20);
    CHECK(rig.output->host.counts().panicsExecuted == 0);
    CHECK_FALSE(rig.output->host.probeStatus().pingRunning);
}

TEST_CASE("both ends suspended for more than 250 ms clears the local line, shorter or input-only does not",
          "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(50);
    REQUIRE(rig.input->host.counts().suspendFlushes == 0);

    // 两端一起停 100 ms：不清
    rig.advance(100'000'000);
    rig.cycles(20);
    CHECK(rig.input->host.counts().suspendFlushes == 0);

    // 只停 Input 300 ms（Output 照常写环）：不清
    rig.cycles(30, false, true);
    rig.cycles(20);
    CHECK(rig.input->host.counts().suspendFlushes == 0);

    // 两端一起停 300 ms：清一次，引擎回报「因挂起清空」
    rig.advance(300'000'000);
    rig.cycles(20);
    const auto counts = rig.input->host.counts();
    CHECK(counts.suspendFlushes == 1);
    CHECK(counts.suspendClears == 1);

    // 宿主把两端一起停用（release）、300 ms 后恢复（同采样率 prepare）：同样清一次
    rig.input->releaseResources();
    rig.output->releaseResources();
    rig.advance(300'000'000);
    rig.input->prepareToPlay(kRate, kBlock);
    rig.output->prepareToPlay(kRate, kBlock);
    rig.cycles(20);
    const auto resumed = rig.input->host.counts();
    CHECK(resumed.suspendFlushes == 2);
    CHECK(resumed.suspendClears == 2);
}

TEST_CASE("a validation host opens no segment, logs nothing and passes the dry signal bit for bit", "[probe_host]")
{
    Rig rig;
    rig.open(0, true);
    CHECK(rig.input->host.rendezvousState() == ipc::RendezvousState::kValidator);
    CHECK(rig.output->host.rendezvousState() == ipc::RendezvousState::kValidator);
    CHECK(rig.input->host.log() == nullptr);
    CHECK(rig.logFiles() == 0);

    rinne::test::BlockFixture fixture;
    juce::MidiBuffer midi;
    for (int round = 0; round < 8; ++round)
    {
        for (TestProcessor* p : {rig.input.get(), rig.output.get()})
        {
            fixture.fill(2, kBlock);
            auto buffer = fixture.view(2, kBlock);
            p->processBlock(buffer, midi);
            CHECK(fixture.unchanged(2, kBlock));
        }
        rig.tick();
    }

    // 会合段与组 A 的数据段都不存在；同一作用域里起一个正常的 Input 之后就打得开（对照：查法本身有效）
    const ipc::Naming naming(rig.scope());
    std::string rvName;
    std::string ringName;
    REQUIRE(naming.name(ipc::SegmentId::rendezvous(), rvName) == ipc::NameStatus::kOk);
    REQUIRE(naming.name(ipc::SegmentId::groupData(1), ringName) == ipc::NameStatus::kOk);
    ipc::PlatformSegmentBackend backend;
    ipc::SegmentView view;
    CHECK(backend.openExisting(rvName, view) != ipc::InitResult::kOk);
    CHECK(backend.openExisting(ringName, view) != ipc::InitResult::kOk);

    TestProcessor control(rig.config(ipc::Role::kInput));
    control.prepareToPlay(kRate, kBlock);
    control.host.probeSetGroup(0);
    control.host.tick();
    REQUIRE(control.host.rendezvousState() == ipc::RendezvousState::kActive);
    REQUIRE(backend.openExisting(rvName, view) == ipc::InitResult::kOk);
    backend.unmap(view);
}

TEST_CASE("an instance that is only constructed and destroyed leaves no log file", "[probe_host]")
{
    Rig rig;
    // 插件扫描、构建时生成 VST3 模块信息的工具：构造一下就析构
    {
        TestProcessor scanned(rig.config(ipc::Role::kInput));
        CHECK(scanned.host.log() == nullptr);
        CHECK_FALSE(scanned.host.probeStatus().logOpen);
    }
    CHECK(rig.logFiles() == 0);

    // 宿主第一次真用它才开日志：存状态不算，设状态算，set_state 记在里面
    {
        TestProcessor used(rig.config(ipc::Role::kOutput));
        juce::MemoryBlock state;
        used.getStateInformation(state);
        CHECK(rig.logFiles() == 0);
        used.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        CHECK(used.host.probeStatus().logOpen);
    }
    REQUIRE(rig.logFiles() == 1);
    CHECK(eventsNamed(rig.read('O'), "set_state").size() == 1);

    // 送轨道信息也算
    {
        TestProcessor named(rig.config(ipc::Role::kInput));
        juce::AudioProcessor::TrackProperties props;
        props.name = juce::String("Bus");
        named.updateTrackProperties(props);
        CHECK(named.host.probeStatus().logOpen);
    }
    CHECK(rig.logFiles() == 2);
}

TEST_CASE("probe state round-trips and a release build's bytes are kept and written back verbatim", "[probe_host]")
{
    Rig rig;
    rig.open(-1);
    ProbeHost& host = rig.input->host;
    host.probeSetGroup(2);
    host.probeSetFeedbackPercent(30.0f);
    host.probeSetDelayMs(250.0f);
    juce::MemoryBlock saved;
    rig.input->getStateInformation(saved);

    TestProcessor restored(rig.config(ipc::Role::kInput));
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    probe::ProbeStatus status = restored.host.probeStatus();
    CHECK(status.group == 2);
    CHECK(status.feedbackPercent == 30.0f);
    CHECK(status.delayMs == 250.0f);
    CHECK(status.ceilingDb == probe::kCeilingDefaultDb);
    // 组也交给了会合层（还没 prepare：等 prepare 再开段）
    CHECK(restored.host.rendezvousState() == ipc::RendezvousState::kWaitingPrepare);

    // 对端（Output）存下的探针字节不进 Input
    TestProcessor other(rig.config(ipc::Role::kOutput));
    other.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(other.host.probeStatus().group == -1);
    CHECK(other.host.rendezvousState() == ipc::RendezvousState::kUnassigned);

    // 正式版写的字节（abi 1、角色是 Input）：一个字节都不解析，保存时原样交回
    std::vector<std::uint8_t> release;
    putU32(release, rinne::state::kMagic);
    putU32(release, 1u);
    putU32(release, static_cast<std::uint32_t>(rinne::state::Role::Input));
    putU32(release, 0u);
    for (std::uint8_t b = 0; b < 24; ++b)
        release.push_back(static_cast<std::uint8_t>(0xA0u + b));
    CHECK_FALSE(restored.host.probeStatus().stateHeld);
    restored.setStateInformation(release.data(), static_cast<int>(release.size()));
    CHECK(restored.host.probeStatus().group == 2);
    // 留着正式字节期间，面板的状态里有这一项（面板据此提示改动不会保存），改了也照样交回原字节
    CHECK(restored.host.probeStatus().stateHeld);
    restored.host.probeSetFeedbackPercent(10.0f);
    juce::MemoryBlock back;
    restored.getStateInformation(back);
    REQUIRE(back.getSize() == release.size());
    CHECK(std::memcmp(back.getData(), release.data(), release.size()) == 0);

    // 再读到探针自己的字节就放掉留住的正式字节
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK_FALSE(restored.host.probeStatus().stateHeld);
    CHECK(restored.host.probeStatus().feedbackPercent == 30.0f);

    // 设状态之后的第一块，引擎从 0 淡入（原因 SetState）；set_state 事件带着组
    restored.setPlayHead(&rig.playHead);
    restored.prepareToPlay(kRate, kBlock);
    rinne::test::BlockFixture fixture;
    fixture.fill(2, kBlock);
    auto buffer = fixture.view(2, kBlock);
    juce::MidiBuffer midi;
    restored.processBlock(buffer, midi);
    CHECK(restored.host.counts().fadeIns[static_cast<std::size_t>(engine::FadeReason::SetState)] >= 1);
}

TEST_CASE("probe defaults are conservative and the panel limits hold", "[probe_host]")
{
    Rig rig;
    rig.open(-1);
    const probe::ProbeStatus status = rig.input->host.probeStatus();
    CHECK(status.feedbackPercent == 0.0f);
    CHECK(status.ceilingDb == -18.0f);
    CHECK(status.group == -1);
    CHECK(status.logOpen);
    CHECK_FALSE(status.recording);
    CHECK(status.sampleRate == 48000);
    rig.input->host.probeSetFeedbackPercent(120.0f);
    CHECK(rig.input->host.probeStatus().feedbackPercent == probe::kFeedbackMaxPercent);
    rig.input->host.probeSetDelayMs(10000.0f);
    CHECK(rig.input->host.probeStatus().delayMs == engine::kMaxDelayMs);
    CHECK(rig.input->host.rendezvousState() == ipc::RendezvousState::kUnassigned);

    const std::string text = rig.input->host.probeDiagnosticsText(cell());
    CHECK(text.find("default") != std::string::npos);
    CHECK(text.find(fs::temp_directory_path().string()) == std::string::npos);
    CHECK(text.find("probe_case: case=CB-01 variant=steady buf=256 guard=normal\n") != std::string::npos);

    // 表外的格不开窗
    ProbeSelection bad = cell();
    bad.caseId = "ZZ-99";
    CHECK_FALSE(rig.input->host.probeStartRecording(bad, 1));
    CHECK_FALSE(rig.input->host.probeStatus().recording);

    // 还没设组、没有会合段就取不到 call_seq：窗口开着也不写 B 行
    REQUIRE(rig.input->host.probeStartRecording(cell(), 1));
    // 诊断里的格是面板交来的当前选择，不是开始记录时用的那个
    CHECK(rig.input->host.probeDiagnosticsText(cell(512, diag::ProbeGuard::kLow))
              .find("probe_case: case=CB-01 variant=steady buf=512 guard=low\n") != std::string::npos);
    rig.cycles(20);
    const std::vector<Row> rows = rig.closeAndRead('I');
    CHECK(rowsOf(rows, "C").size() == 1);
    CHECK(rowsOf(rows, "B").empty());
}

TEST_CASE("the loop switch reaches the engine and stays out of the probe state", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.input->host.probeSetFeedbackPercent(50.0f);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    // 正弦送 1 s，停掉干声，量停掉之后 100 到 400 ms 里 Input 输出的峰值：干声已是 0，剩下的只有回送
    const auto echoPeak = [&rig] {
        rig.source = &sine;
        rig.cycles(100);
        rig.source = nullptr;
        rig.cycles(10);
        rig.recorded.clear();
        rig.record = true;
        rig.cycles(30);
        rig.record = false;
        float peak = 0.0f;
        for (const float v : rig.recorded)
            peak = std::max(peak, std::abs(v));
        return peak;
    };
    CHECK(rig.input->host.probeStatus().loopOn); // 默认开
    CHECK(echoPeak() > 0.01f);

    juce::MemoryBlock withLoop;
    rig.input->getStateInformation(withLoop);
    rig.input->host.probeSetLoopOn(false);
    CHECK_FALSE(rig.input->host.probeStatus().loopOn);
    CHECK(echoPeak() < 1.0e-6f);

    // 环路开关不进探针 state：存下的字节不变，载入也不改它
    juce::MemoryBlock withoutLoop;
    rig.input->getStateInformation(withoutLoop);
    CHECK(withoutLoop == withLoop);
    rig.input->setStateInformation(withLoop.getData(), static_cast<int>(withLoop.getSize()));
    CHECK_FALSE(rig.input->host.probeStatus().loopOn);

    rig.input->host.probeSetLoopOn(true);
    CHECK(echoPeak() > 0.01f);
}

TEST_CASE("a block whose slot owner word was changed by someone else hands the engine no segment", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(10);
    REQUIRE(rig.input->host.inputEngine().lastBlockResult().paired);

    // 块首租约照样成立，但本端槽的属主字变了：这一块不把段交给引擎，引擎看到的是没有对端
    const RvPeek peek(rig.scope());
    std::atomic<std::uint64_t>& ctl = ipc::roleSlot(peek.rv(), 1, ipc::Role::kInput).ctl;
    const std::uint64_t mine = ctl.load();
    ctl.store(mine ^ 1u);
    rinne::test::BlockFixture fixture;
    fixture.fill(2, kBlock);
    auto buffer = fixture.view(2, kBlock);
    juce::MidiBuffer midi;
    rig.input->processBlock(buffer, midi);
    CHECK_FALSE(rig.input->host.inputEngine().lastBlockResult().paired);

    // 属主字回来之后照常
    ctl.store(mine);
    rig.cycles(4);
    CHECK(rig.input->host.inputEngine().lastBlockResult().paired);
}

TEST_CASE("the probe audio callback neither allocates nor frees while recording and pinging", "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.input->host.probeSetFeedbackPercent(40.0f);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(10); // tick 至少看到一次「在走带」，停走带才会自动 Ping
    REQUIRE(rig.input->host.probeStartRecording(cell(), 5));
    REQUIRE(rig.output->host.probeStartRecording(cell(), 5));
    rig.playHead.playing = false;
    rig.cycles(8);
    REQUIRE(rig.input->host.probeStatus().pingRunning);

    std::vector<float> left(kBlock, 0.1f);
    std::vector<float> right(kBlock, -0.1f);
    float* channels[2] = {left.data(), right.data()};
    juce::MidiBuffer midi;
    // 计数在作用域里读、断言在作用域外：断言本身（报告器记结果）会分配
    long allocations = 0;
    long frees = 0;
    for (int i = 0; i < 50; ++i)
    {
        juce::AudioBuffer<float> buffer(channels, 2, kBlock);
        const rinne::test::AllocationScope scope;
        rig.input->processBlock(buffer, midi);
        rig.output->processBlock(buffer, midi);
        allocations += scope.allocations();
        frees += scope.frees();
    }
    CHECK(allocations == 0);
    CHECK(frees == 0);
    CHECK(rig.input->host.probeStatus().pingRunning);
}

TEST_CASE("a bypassed input passes the dry signal bit for bit and starts no ping when the transport stops",
          "[probe_host]")
{
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.input->host.probeSetFeedbackPercent(50.0f);
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(300); // 3 s：回送在环里转起来

    // 旁路开始的 10 ms 里回送照引擎的规矩淡到 0（不归探针管）；之后每一块都必须逐位直通
    rig.bypassInput = true;
    rig.cycles(5);
    rig.bypassedSamples = 0;
    rig.bypassInexact = 0;
    const auto preset = static_cast<std::size_t>(engine::FadeReason::Preset);
    const auto before = rig.input->host.counts();

    // 面板：旁路着不许点，点了也不开
    const probe::ProbeStatus status = rig.input->host.probeStatus();
    CHECK(status.bypassed);
    CHECK_FALSE(probe::pingAllowed(status));
    rig.input->host.probePing();
    CHECK_FALSE(rig.input->host.probeStatus().pingRunning);

    // 停走带：自动 Ping 的条件成立，tick 把旁路当作不许 Ping。手动 tick，停在两次音频回调之间看
    rig.ticking = false;
    rig.playHead.playing = false;
    rig.cycle();
    rig.input->host.tick();
    CHECK_FALSE(rig.input->host.probeStatus().pingRunning);
    rig.ticking = true;
    rig.cycles(300); // 3 s

    CHECK(rig.bypassedSamples == 301u * kBlock);
    CHECK(rig.bypassInexact == 0);
    CHECK(rig.input->host.counts().fadeIns[preset] == before.fadeIns[preset]);

    // 旁路结束：又可以点了
    rig.bypassInput = false;
    rig.cycles(4);
    CHECK_FALSE(rig.input->host.probeStatus().bypassed);
    CHECK(probe::pingAllowed(rig.input->host.probeStatus()));

    const std::vector<Row> rows = rig.closeAndRead('I');
    CHECK(eventsNamed(rows, "ping").empty());
}

TEST_CASE("a ping under way ends when the input is bypassed: bypassed blocks are bit exact and nothing steps after",
          "[probe_host]")
{
    // 静音段中途、注入中途（序列已放完，正在攒回读）进入旁路。反馈 0 时引擎的回送恒为 0，旁路的第一块就必须逐位直通；
    // 反馈 50 % 时引擎自己的回送要 10 ms 淡到 0（不归探针管），从旁路的第二块起逐位直通
    for (const float feedback : {0.0f, 50.0f})
    {
        for (const int intoPing : {8, 25})
        {
            INFO("feedback " << feedback << ", blocks into the ping " << intoPing);
            Rig rig;
            rig.open(0);
            rig.source = [](std::uint64_t) { return 0.2f; };
            rig.input->host.probeSetFeedbackPercent(feedback);
            rig.playHead.playing = true;
            REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
            rig.cycles(100);

            rig.ticking = false;
            rig.playHead.playing = false;
            rig.cycle();
            rig.input->host.tick(); // 停走带：自动 Ping
            rig.cycles(intoPing);
            REQUIRE(rig.input->host.probeStatus().pingRunning);
            rig.ticking = true;

            const auto preset = static_cast<std::size_t>(engine::FadeReason::Preset);
            const auto before = rig.input->host.counts();
            rig.bypassInput = true;
            if (feedback > 0.0f)
            {
                rig.cycle();
                rig.bypassedSamples = 0;
                rig.bypassInexact = 0;
            }
            rig.cycles(50);
            CHECK(rig.bypassedSamples == 50u * kBlock);
            CHECK(rig.bypassInexact == 0);
            CHECK_FALSE(rig.input->host.probeStatus().pingRunning);

            // 旁路结束：干声本来就在满幅，回送照安全链淡入回来，都不跳（斜坡每个样本最多挪 1/480）
            rig.bypassInput = false;
            rig.record = true;
            rig.cycles(100);
            rig.record = false;
            CHECK(largestStep(rig.recorded, 0, 0.2f) <= 0.002f);
            CHECK(rig.input->host.counts().fadeIns[preset] - before.fadeIns[preset] == 1);
            CHECK(rig.input->host.probeStatus().lastPingSamples == -1);
        }
    }
}

TEST_CASE("a block longer than the scratch in the settle or in the injection cancels the ping without a gain step",
          "[probe_host]")
{
    // 直流干声、反馈 0：输出只有干声乘系数加注入，相邻样本差看得清系数有没有跳。40000 帧的块超过暂存（32768）
    for (const int intoPing : {8, 25}) // 静音段中途、注入中途（序列已放完，正在攒回读）
    {
        INFO("blocks into the ping " << intoPing);
        Rig rig;
        rig.open(0);
        rig.source = [](std::uint64_t) { return 0.2f; };
        rig.playHead.playing = true;
        REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
        rig.cycles(50);

        rig.record = true;
        rig.ticking = false;
        rig.playHead.playing = false;
        rig.cycle();
        rig.input->host.tick(); // 停走带：自动 Ping
        rig.cycles(intoPing);
        REQUIRE(rig.input->host.probeStatus().pingRunning);
        rig.ticking = true;

        const auto preset = static_cast<std::size_t>(engine::FadeReason::Preset);
        const auto before = rig.input->host.counts();
        const std::size_t longStart = rig.recorded.size();
        rig.cycle(true, true, 40000);
        rig.cycles(100);
        rig.record = false;

        CHECK_FALSE(rig.input->host.probeStatus().pingRunning);
        CHECK(rig.input->host.probeStatus().lastPingSamples == -1);
        CHECK(rig.input->host.counts().fadeIns[preset] - before.fadeIns[preset] == 1);

        // 超过斜坡（每个样本 0.2 / 480）的相邻差只能是注入的序列本身（±0.1，127 个样本），而且在超长块之前
        const float bound = 0.2f / 480.0f * 1.01f + 1.0e-6f;
        std::vector<std::size_t> steps;
        for (std::size_t i = 1; i < rig.recorded.size(); ++i)
            if (std::abs(rig.recorded[i] - rig.recorded[i - 1]) > bound)
                steps.push_back(i);
        if (intoPing == 8)
            CHECK(steps.empty());
        if (!steps.empty())
        {
            CHECK(steps.back() < longStart);
            CHECK(steps.back() - steps.front() <= 128);
        }
        // 超长块里干声在开头一段就回到满幅，块尾与之后都是原样的干声
        CHECK(rig.recorded[longStart] < 0.01f);
        CHECK(rig.recorded[longStart + 1000] == 0.2f);
        CHECK(rig.recorded[longStart + 39999] == 0.2f);
        CHECK(rig.recorded.back() == 0.2f);
    }
}

TEST_CASE("a long gap during an offline render or while unpaired is not taken for a suspend", "[probe_host]")
{
    SECTION("offline: a render slower than real time looks like a suspend at every block, so the gap is not judged")
    {
        Rig rig;
        rig.open(0);
        rig.source = &sine;
        rig.playHead.playing = true;
        REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
        rig.cycles(50);

        rig.input->setNonRealtime(true);
        rig.output->setNonRealtime(true);
        rig.cycles(4);
        for (int i = 0; i < 5; ++i)
        {
            rig.advance(300'000'000);
            rig.cycle();
        }
        rig.cycles(20);
        CHECK(rig.input->host.counts().suspendFlushes == 0);
        CHECK(rig.input->host.counts().suspendClears == 0);

        // 对照：回到实时，同样的间隔就是两端一起挂起
        rig.input->setNonRealtime(false);
        rig.output->setNonRealtime(false);
        rig.cycles(20);
        rig.advance(300'000'000);
        rig.cycles(20);
        CHECK(rig.input->host.counts().suspendFlushes == 1);
    }

    SECTION("unpaired: no ring and no write head to look at, so nothing is judged or counted")
    {
        Rig rig;
        rig.input = std::make_unique<TestProcessor>(rig.config(ipc::Role::kInput));
        rig.input->setPlayHead(&rig.playHead);
        rig.input->prepareToPlay(kRate, kBlock);
        rig.input->host.tick();
        rig.source = &sine;
        rig.playHead.playing = true;
        rig.cycles(50);
        REQUIRE_FALSE(rig.input->host.probeStatus().paired);
        rig.advance(300'000'000);
        rig.cycles(20);
        rig.advance(1'000'000'000);
        rig.cycles(20);
        CHECK(rig.input->host.counts().suspendFlushes == 0);
    }

    SECTION("paired during the gap: the first block with a ring has no earlier write head to compare with")
    {
        // 宿主插入 Output 时把音频停了 1 s：Input 之前的块都没有环，停的期间两端配上对，之后第一块就拿到环
        Rig rig;
        rig.open(-1);
        rig.source = &sine;
        rig.playHead.playing = true;
        rig.cycles(50, true, false);
        REQUIRE_FALSE(rig.input->host.probeStatus().paired);
        for (TestProcessor* p : {rig.input.get(), rig.output.get()})
            p->host.probeSetGroup(0);
        rig.advance(1'000'000'000);
        rig.cycle();
        REQUIRE(rig.input->host.inputEngine().lastBlockResult().paired); // 停之后的第一块就拿到了环
        rig.cycles(20);
        CHECK(rig.input->host.counts().suspendFlushes == 0);
    }
}

TEST_CASE("a ring change during the gap is not compared across rings, and the new ring is judged again", "[probe_host]")
{
    // 只停 Input 300 ms，这期间两端换到组 2：旧环退休、新环另开，Output 照常往新环里写，不是两端一起挂起。
    // 新环的写头从 0 数起，比旧环上一块的写头小，拿来相减就会当成「写头没走」
    Rig rig;
    rig.open(0);
    rig.source = &sine;
    rig.playHead.playing = true;
    REQUIRE(rig.runUntil([&] { return paired(rig); }, 200));
    rig.cycles(200);
    for (TestProcessor* p : {rig.input.get(), rig.output.get()})
        p->host.probeSetGroup(2);
    rig.cycles(30, false, true);
    rig.cycle();
    REQUIRE(rig.input->host.inputEngine().lastBlockResult().paired); // 停之后的第一块已在新环上
    rig.cycles(20);
    CHECK(rig.input->host.counts().suspendFlushes == 0);

    // 对照：之后在新环上两端一起停 300 ms，照样清一次
    rig.advance(300'000'000);
    rig.cycles(20);
    CHECK(rig.input->host.counts().suspendFlushes == 1);
}

TEST_CASE("the backend error sink is swapped in one step and never forwards to itself", "[probe_host]")
{
    const SinkRestore restore;
    ipc::IpcDiagEvent event;
    event.error = 5;
    event.detail = "test report";

    SECTION("installed twice in a row: it forwards to the sink that was there, one report arrives once")
    {
        ipc::setIpcDiagSink(&countingSink);
        ProbeHost::installIpcDiagSink();
        ProbeHost::installIpcDiagSink();
        CHECK(ipc::ipcDiagSink() == ProbeHost::ipcDiagRecorder());
        REQUIRE(ProbeHost::ipcDiagForwardTarget() == &countingSink);
        gForwarded = 0;
        ipc::reportIpcDiag(event);
        CHECK(gForwarded == 1);
    }

    SECTION("two hosts constructed one after the other: the same")
    {
        ipc::setIpcDiagSink(&countingSink);
        Rig rig;
        TestProcessor a(rig.config(ipc::Role::kInput));
        TestProcessor b(rig.config(ipc::Role::kOutput));
        CHECK(ipc::ipcDiagSink() == ProbeHost::ipcDiagRecorder());
        REQUIRE(ProbeHost::ipcDiagForwardTarget() == &countingSink);
        gForwarded = 0;
        ipc::reportIpcDiag(event);
        CHECK(gForwarded == 1);
    }

    SECTION("another install landing between this one's look and its swap still leaves the target alone")
    {
        ipc::setIpcDiagSink(&countingSink);
        ProbeHost::setIpcDiagInstallHook(&installAgain);
        ProbeHost::installIpcDiagSink();
        CHECK(ipc::ipcDiagSink() == ProbeHost::ipcDiagRecorder());
        REQUIRE(ProbeHost::ipcDiagForwardTarget() == &countingSink);
        gForwarded = 0;
        ipc::reportIpcDiag(event);
        CHECK(gForwarded == 1);
    }

    SECTION("two threads installing at the same moment, many times over")
    {
        int bad = 0;
        for (int round = 0; round < 2000; ++round)
        {
            ipc::setIpcDiagSink(&countingSink);
            std::atomic<int> ready{0};
            const auto worker = [&ready] {
                ready.fetch_add(1);
                while (ready.load() < 2)
                {
                }
                ProbeHost::installIpcDiagSink();
            };
            std::thread first(worker);
            std::thread second(worker);
            first.join();
            second.join();
            if (ipc::ipcDiagSink() != ProbeHost::ipcDiagRecorder() ||
                ProbeHost::ipcDiagForwardTarget() != &countingSink)
                ++bad;
        }
        REQUIRE(bad == 0);
        gForwarded = 0;
        ipc::reportIpcDiag(event);
        CHECK(gForwarded == 1);
    }
}

TEST_CASE("the backend event carries this instance's own error code, not another instance's", "[probe_host]")
{
    // 同一进程里两个实例的段都被拒：先 tick 的那个的后端报了错误码 13，后 tick 的那个的后端什么都没报
    Rig rig;
    DeniedBackend reporting(13);
    DeniedBackend silent(0);
    ProbeHostConfig first = rig.config(ipc::Role::kInput);
    first.backend = &reporting;
    ProbeHostConfig second = rig.config(ipc::Role::kOutput);
    second.backend = &silent;
    {
        TestProcessor a(first);
        TestProcessor b(second);
        for (TestProcessor* p : {&a, &b})
        {
            p->prepareToPlay(kRate, kBlock);
            p->host.probeSetGroup(0);
        }
        a.host.tick();
        b.host.tick();
        CHECK(a.host.rendezvousState() == ipc::RendezvousState::kIpcUnavailable);
        CHECK(b.host.rendezvousState() == ipc::RendezvousState::kIpcUnavailable);
    }
    const std::vector<Row> in = rig.read('I');
    const std::vector<Row> out = rig.read('O');
    const auto inBackend = eventsNamed(in, "backend");
    const auto outBackend = eventsNamed(out, "backend");
    REQUIRE(inBackend.size() == 1);
    REQUIRE(outBackend.size() == 1);
    CHECK(inBackend[0][4] == "13");
    CHECK(outBackend[0][4] == "0");
    CHECK(eventsNamed(in, "shm_denied").size() == 1);
    CHECK(eventsNamed(out, "shm_denied").size() == 1);
}

TEST_CASE("an error code reported between two ticks belongs to no instance", "[probe_host]")
{
    // 第一拍后端暂时失败、什么都没报，第二拍被拒、也没报；两拍之间同一线程上报的错误码不是这个实例的会合报的
    Rig rig;
    DeniedBackend late(0, 1);
    ProbeHostConfig config = rig.config(ipc::Role::kInput);
    config.backend = &late;
    {
        TestProcessor a(config);
        a.prepareToPlay(kRate, kBlock);
        a.host.probeSetGroup(0);
        a.host.tick();
        REQUIRE(a.host.rendezvousState() == ipc::RendezvousState::kOpening);
        ipc::IpcDiagEvent stray;
        stray.error = 7;
        stray.detail = "reported outside any tick";
        ipc::reportIpcDiag(stray);
        a.host.tick();
        REQUIRE(a.host.rendezvousState() == ipc::RendezvousState::kIpcUnavailable);
    }
    const auto backend = eventsNamed(rig.read('I'), "backend");
    REQUIRE(backend.size() == 1);
    CHECK(backend[0][4] == "0");
}
