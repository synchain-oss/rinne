// SPDX-License-Identifier: GPL-3.0-or-later
//
// 不可信字节：全部截断前缀、穷举单个比特翻转、十万次随机比特翻转，以及直接对两种载荷的随机变异。判据只有两条：
//   · 不崩（越界读写由 sanitizer 构建抓）；
//   · 不留半份状态：失败与忽略时出参与调用前逐字段相同；解得开时，解出来的是一份完整、自洽的文档 —— 编得回去，
//     再读一次得到同一份，再编一次字节不变。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace
{

using rinne::state::Role;
using rinne::state::StateDocument;
using rinne::state::StateLoadResult;
using rinne::state::StateLoadStatus;
using rinne::test::Bytes;
using rinne::test::Rng;
using rinne::test::sameDocument;
using rinne::test::sentinelDocument;

constexpr int kInconsistent = -1;

// 加载一串字节并核对上面两条判据。返回 StateLoadStatus 的数值，判据不成立时返回 kInconsistent。
// 十万次循环里逐条 CHECK 会把输出淹掉，所以这里只回答「这条过不过」。
int loadAndVerify(const Bytes& bytes, Role role)
{
    StateDocument out = sentinelDocument(role);
    const StateLoadResult result = rinne::state::loadState(bytes.data(), bytes.size(), role, out);
    const int status = static_cast<int>(result.status);

    if (result.status != StateLoadStatus::Ok && result.status != StateLoadStatus::Migrated)
    {
        const bool preservedOk = result.status == StateLoadStatus::RejectedNewer ? result.preservedOriginal == bytes
                                                                                 : result.preservedOriginal.empty();
        return preservedOk && sameDocument(out, sentinelDocument(role)) ? status : kInconsistent;
    }

    Bytes first;
    if (out.role != role || !rinne::state::encodeDocument(out, first))
        return kInconsistent;
    StateDocument second = sentinelDocument(role);
    if (rinne::state::loadState(first.data(), first.size(), role, second).status != StateLoadStatus::Ok ||
        !sameDocument(second, out))
        return kInconsistent;
    Bytes again;
    if (!rinne::state::encodeDocument(second, again) || again != first)
        return kInconsistent;
    return status;
}

struct Base
{
    Role role;
    Bytes bytes;
};

std::vector<Base> bases()
{
    std::vector<Base> out;
    for (const StateDocument& document :
         {rinne::test::sampleInputDocument(), rinne::test::sampleOutputDocument(),
          rinne::state::makeDefaultDocument(Role::Input), rinne::state::makeDefaultDocument(Role::Output)})
    {
        Bytes bytes;
        REQUIRE(rinne::state::encodeDocument(document, bytes));
        out.push_back(Base{document.role, bytes});
    }
    return out;
}

void flipBit(Bytes& bytes, std::size_t bit)
{
    bytes[bit / 8u] = static_cast<std::uint8_t>(bytes[bit / 8u] ^ (1u << (bit % 8u)));
}

} // namespace

TEST_CASE("untrusted: 全部截断前缀都拒收且不碰出参", "[state][untrusted]")
{
    for (const Base& base : bases())
    {
        for (std::size_t size = 0; size < base.bytes.size(); ++size)
        {
            const Bytes prefix(base.bytes.begin(), base.bytes.begin() + static_cast<std::ptrdiff_t>(size));
            const auto expected = size == 0u ? StateLoadStatus::Empty : StateLoadStatus::Corrupt;
            INFO("前缀长度 " << size << " / " << base.bytes.size());
            CHECK(loadAndVerify(prefix, base.role) == static_cast<int>(expected));
        }
        CHECK(loadAndVerify(base.bytes, base.role) == static_cast<int>(StateLoadStatus::Ok));
    }
}

TEST_CASE("untrusted: 穷举单个比特翻转", "[state][untrusted]")
{
    std::array<std::size_t, 6> outcomes{};
    std::size_t inconsistent = 0;
    for (const Base& base : bases())
    {
        for (std::size_t bit = 0; bit < base.bytes.size() * 8u; ++bit)
        {
            Bytes mutated = base.bytes;
            flipBit(mutated, bit);
            const int status = loadAndVerify(mutated, base.role);
            if (status == kInconsistent)
            {
                UNSCOPED_INFO("不自洽：基准 role " << static_cast<int>(base.role) << "，比特 " << bit);
                ++inconsistent;
            }
            else
            {
                ++outcomes[static_cast<std::size_t>(status)];
            }
        }
    }
    CHECK(inconsistent == 0u);
    // 翻 abi 的低位分别得到探针与更高的 abi；翻值字节仍能解开；翻结构字节拒收
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::Ok)] > 0u);
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::IgnoredProbe)] > 0u);
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::RejectedNewer)] > 0u);
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::Corrupt)] > 0u);
}

TEST_CASE("untrusted: 十万次随机比特翻转", "[state][untrusted]")
{
    const std::vector<Base> all = bases();
    Rng rng(0x524e5354ull); // 固定种子：每次跑出同一串字节
    std::array<std::size_t, 6> outcomes{};
    std::size_t inconsistent = 0;
    std::size_t firstInconsistent = 0;

    const std::size_t iterations = 100000u;
    for (std::size_t i = 0; i < iterations; ++i)
    {
        const Base& base = all[i % all.size()];
        Bytes mutated = base.bytes;
        // 一半只翻一位（最容易落在「还能解开」的地方），一半翻 2..8 位
        const std::size_t flips = (i % 2u == 0u) ? 1u : 2u + rng.below(7u);
        for (std::size_t f = 0; f < flips; ++f)
            flipBit(mutated, rng.below(mutated.size() * 8u));

        const int status = loadAndVerify(mutated, base.role);
        if (status == kInconsistent)
        {
            if (inconsistent == 0u)
                firstInconsistent = i;
            ++inconsistent;
            continue;
        }
        ++outcomes[static_cast<std::size_t>(status)];
    }

    INFO("第一条不自洽的样本序号：" << firstInconsistent);
    CHECK(inconsistent == 0u);
    // 四种结局都要出现：只走到失败路径的随机测试证明不了成功路径还活着
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::Ok)] > 0u);
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::IgnoredProbe)] > 0u);
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::RejectedNewer)] > 0u);
    CHECK(outcomes[static_cast<std::size_t>(StateLoadStatus::Corrupt)] > 0u);
}

TEST_CASE("untrusted: 合法的头加随机的块区", "[state][untrusted]")
{
    Rng rng(0x7e57ull);
    std::size_t inconsistent = 0;
    for (std::size_t i = 0; i < 20000u; ++i)
    {
        const Role role = (i % 2u == 0u) ? Role::Input : Role::Output;
        Bytes bytes = rinne::test::rawContainer(1, static_cast<std::uint32_t>(role), {});
        bytes[12] = static_cast<std::uint8_t>(rng.below(4u)); // 0..3 块
        const std::size_t tail = rng.below(64u);
        for (std::size_t k = 0; k < tail; ++k)
            bytes.push_back(static_cast<std::uint8_t>(rng.next()));
        if (loadAndVerify(bytes, role) == kInconsistent)
            ++inconsistent;
    }
    CHECK(inconsistent == 0u);
}

TEST_CASE("untrusted: 载荷层的随机变异", "[state][untrusted]")
{
    const StateDocument input = rinne::test::sampleInputDocument();
    Bytes params;
    REQUIRE(rinne::state::encodeParams(input.params, params));
    Bytes configs;
    REQUIRE(rinne::state::encodeConfigs(input.configs, Role::Input, configs));

    Rng rng(0xc0ffeeull);
    std::size_t paramsOk = 0;
    std::size_t configsOk = 0;
    std::size_t inconsistent = 0;
    for (std::size_t i = 0; i < 20000u; ++i)
    {
        Bytes p = params;
        Bytes c = configs;
        const std::size_t flips = 1u + rng.below(3u);
        for (std::size_t f = 0; f < flips; ++f)
        {
            flipBit(p, rng.below(p.size() * 8u));
            flipBit(c, rng.below(c.size() * 8u));
        }

        rinne::state::Params decodedParams = rinne::test::sentinelDocument(Role::Input).params;
        if (rinne::state::decodeParams(p.data(), p.size(), decodedParams))
        {
            ++paramsOk;
            Bytes again;
            rinne::state::Params reread;
            if (!rinne::state::encodeParams(decodedParams, again) || again != p ||
                !rinne::state::decodeParams(again.data(), again.size(), reread) ||
                !rinne::test::sameParams(reread, decodedParams))
                ++inconsistent;
        }
        else if (!rinne::test::sameParams(decodedParams, rinne::test::sentinelDocument(Role::Input).params))
        {
            ++inconsistent;
        }

        const Role role = (i % 2u == 0u) ? Role::Input : Role::Output;
        rinne::state::Configs decodedConfigs = rinne::test::sentinelDocument(role).configs;
        if (rinne::state::decodeConfigs(c.data(), c.size(), role, decodedConfigs))
        {
            ++configsOk;
            Bytes again;
            rinne::state::Configs reread;
            if (!rinne::state::encodeConfigs(decodedConfigs, role, again) ||
                !rinne::state::decodeConfigs(again.data(), again.size(), role, reread) ||
                !rinne::test::sameConfigs(reread, decodedConfigs))
                ++inconsistent;
        }
        else if (!rinne::test::sameConfigs(decodedConfigs, rinne::test::sentinelDocument(role).configs))
        {
            ++inconsistent;
        }
    }
    CHECK(inconsistent == 0u);
    CHECK(paramsOk > 0u);
    CHECK(configsOk > 0u);
}
