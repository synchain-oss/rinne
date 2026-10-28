// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数表测试的公用件：把 src/input/ParamTable.h 展开成可比较的表；读 tests/golden/params-input.json（规范排版：每个
// 参数一行、键序固定，所以按行读，不需要完整的 JSON 解析器）；复算宿主参数 ID 的散列；按表渲染出 golden 的规范文本。
// 参数表已冻结，规则见 docs/contracts/PARAMETERS.md：表和 golden 谁变了，对拍用例就变红；冻结面另由
// contract-static-params 写死核对。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace rinne::test::params
{

struct Param
{
    const char* id;
    int index;
    char kind; // F 连续、B 布尔、C 档位
    double lo;
    double hi;
    double def;
    double step;
    const char* skew; // lin、log
    bool automatable;
    const char* unit; // none、pct、ms、hz、db
    const char* name;
};

struct Choice
{
    const char* id;
    int index;
    const char* bridge; // 界面桥上这一档的取值 id
    const char* label; // 宿主里看到的文字
};

// 同一张 X-macro 表展开两次：参数行与档位行各取一半，另一半展开成空
inline const std::vector<Param>& table()
{
    static const std::vector<Param> params = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) \
    {#id, index, #kind[0], lo, hi, def, step, #skew, (automatable) != 0, #unit, name},
#include "../../../src/input/ParamTable.h"
    };
    return params;
}

inline const std::vector<Choice>& choices()
{
    static const std::vector<Choice> all = {
#define RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name)
#define RINNE_CHOICE(id, index, bridge, label) {#id, index, #bridge, label},
#include "../../../src/input/ParamTable.h"
    };
    return all;
}

// 某个档位参数的一列（文字或桥 id），按表里的先后
inline std::vector<std::string> choiceColumn(const std::string& id, const char* Choice::*column)
{
    std::vector<std::string> out;
    for (const auto& c : choices())
    {
        if (id == c.id)
        {
            out.push_back(c.*column);
        }
    }
    return out;
}

inline std::vector<std::string> choicesOf(const std::string& id)
{
    return choiceColumn(id, &Choice::label);
}

inline std::vector<std::string> bridgeIdsOf(const std::string& id)
{
    return choiceColumn(id, &Choice::bridge);
}

// ---- 宿主参数 ID ----
// JUCE 8 下 VST3 与 AU 都是 String::hashCode()（32 位，乘子 31，逐字符累加）再清最高位；前提是
// JUCE_FORCE_USE_LEGACY_PARAM_IDS=0 且 JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1（tests/golden/identity.json 钉着）。
// 参数 ID 全是 ASCII，这里不链 JUCE 也能复算。
inline std::uint32_t plainHash(const std::string& text)
{
    std::uint32_t hash = 0;
    for (const unsigned char c : text)
    {
        hash = 31u * hash + c;
    }
    return hash;
}

inline std::uint32_t hostParamId(const std::string& id)
{
    return plainHash(id) & 0x7fffffffu;
}

// ---- golden 的规范文本 ----
inline std::string numberText(double v)
{
    if (v == std::floor(v) && std::fabs(v) < 1e15)
    {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[40];
    for (int digits = 1; digits <= 17; ++digits)
    {
        std::snprintf(buf, sizeof buf, "%.*g", digits, v);
        if (std::strtod(buf, nullptr) == v)
        {
            break;
        }
    }
    return buf;
}

inline std::string kindText(char kind)
{
    return kind == 'F' ? "float" : kind == 'B' ? "bool" : kind == 'C' ? "choice" : std::string("?") + kind;
}

inline std::string skewText(const std::string& token)
{
    return token == "lin" ? "linear" : token == "log" ? "log" : "?" + token;
}

inline std::string unitText(const std::string& token)
{
    return token == "none"  ? ""
           : token == "pct" ? "%"
           : token == "ms"  ? "ms"
           : token == "hz"  ? "Hz"
           : token == "db"  ? "dB"
                            : "?" + token;
}

inline std::string listText(const std::vector<std::string>& items)
{
    std::string text = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        text += (i == 0 ? "\"" : ", \"") + items[i] + "\"";
    }
    return text + "]";
}

inline std::string rowText(const Param& p)
{
    const std::string id = p.id;
    std::string row = "  \"" + id + "\": { \"index\": " + std::to_string(p.index) + ", \"name\": \"" + p.name +
                      "\", \"kind\": \"" + kindText(p.kind) + "\", \"min\": " + numberText(p.lo) +
                      ", \"max\": " + numberText(p.hi) + ", \"default\": " + numberText(p.def) +
                      ", \"step\": " + numberText(p.step) + ", \"skew\": \"" + skewText(p.skew) +
                      "\", \"automatable\": " + (p.automatable ? "true" : "false") + ", \"unit\": \"" +
                      unitText(p.unit) + "\", \"vst3\": " + std::to_string(hostParamId(id)) +
                      ", \"au\": " + std::to_string(hostParamId(id));
    if (p.kind == 'C')
    {
        row += ", \"choices\": " + listText(choicesOf(id)) + ", \"bridgeIds\": " + listText(bridgeIdsOf(id));
    }
    return row + " }";
}

// 表渲染成的 golden 全文（LF 换行）。status 与契约文档的 status 行一致（contract-static-params 核对）；
// versionHint 是 ParameterID 的版本号，目前所有参数都是 1
inline std::string renderGolden()
{
    std::string text =
        "{\n  \"status\": \"frozen\",\n  \"versionHint\": 1,\n  \"count\": " + std::to_string(table().size()) + ",\n";
    for (std::size_t i = 0; i < table().size(); ++i)
    {
        text += rowText(table()[i]) + (i + 1 < table().size() ? ",\n" : "\n");
    }
    return text + "}\n";
}

// ---- 读 golden ----
// 仓根由本文件的位置推出（tests/core/params/ 往上四级）：测试不依赖工作目录
inline std::filesystem::path goldenPath()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "tests" /
           "golden" / "params-input.json";
}

// 重新生成 golden 的开关：设 RINNE_PARAMS_GOLDEN_REGEN=1 跑 [golden] 用例。只有测试读环境变量。
inline bool regenEnabled()
{
#ifdef _MSC_VER
    char* flag = nullptr;
    std::size_t length = 0;
    const bool on = _dupenv_s(&flag, &length, "RINNE_PARAMS_GOLDEN_REGEN") == 0 && flag != nullptr && flag[0] == '1';
    std::free(flag);
    return on;
#else
    const char* flag = std::getenv("RINNE_PARAMS_GOLDEN_REGEN");
    return flag != nullptr && flag[0] == '1';
#endif
}

// golden 里的一行：参数行或顶层的标量行。按「"键": 值」取字段
struct Line
{
    std::string text;

    std::optional<std::size_t> find(const std::string& key) const
    {
        const std::string needle = "\"" + key + "\": ";
        const auto at = text.find(needle);
        return at == std::string::npos ? std::nullopt : std::optional<std::size_t>(at + needle.size());
    }

    std::optional<std::string> str(const std::string& key) const
    {
        const auto at = find(key);
        if (!at || *at >= text.size() || text[*at] != '"')
        {
            return std::nullopt;
        }
        const auto end = text.find('"', *at + 1);
        return end == std::string::npos ? std::nullopt
                                        : std::optional<std::string>(text.substr(*at + 1, end - *at - 1));
    }

    std::optional<double> num(const std::string& key) const
    {
        const auto at = find(key);
        if (!at)
        {
            return std::nullopt;
        }
        char* end = nullptr;
        const double v = std::strtod(text.c_str() + *at, &end);
        return end == text.c_str() + *at ? std::nullopt : std::optional<double>(v);
    }

    std::optional<bool> flag(const std::string& key) const
    {
        const auto at = find(key);
        if (!at)
        {
            return std::nullopt;
        }
        return text.compare(*at, 4, "true") == 0    ? std::optional<bool>(true)
               : text.compare(*at, 5, "false") == 0 ? std::optional<bool>(false)
                                                    : std::nullopt;
    }

    // "键": ["a", "b"]，没有这个键时返回空
    std::optional<std::vector<std::string>> list(const std::string& key) const
    {
        auto at = find(key);
        if (!at || *at >= text.size() || text[*at] != '[')
        {
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (std::size_t i = *at + 1; i < text.size() && text[i] != ']';)
        {
            if (text[i] != '"')
            {
                ++i;
                continue;
            }
            const auto end = text.find('"', i + 1);
            if (end == std::string::npos)
            {
                return std::nullopt;
            }
            out.push_back(text.substr(i + 1, end - i - 1));
            i = end + 1;
        }
        return out;
    }
};

struct Golden
{
    bool loaded = false;
    std::string text; // 去掉 CR 的全文
    std::vector<std::string> ids; // 参数行的 id，按文件里的顺序
    std::vector<Line> rows; // 与 ids 同序
    Line top; // 顶层标量行（status、versionHint、count）拼在一起

    const Line* row(const std::string& id) const
    {
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == id)
            {
                return &rows[i];
            }
        }
        return nullptr;
    }
};

inline Golden loadGolden()
{
    Golden g;
    std::ifstream in(goldenPath(), std::ios::binary);
    if (!in)
    {
        return g;
    }
    g.loaded = true;
    for (const char c : std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()))
    {
        if (c != '\r')
        {
            g.text.push_back(c);
        }
    }
    std::size_t begin = 0;
    while (begin < g.text.size())
    {
        const auto end = g.text.find('\n', begin);
        const std::string line = g.text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = end == std::string::npos ? g.text.size() : end + 1;
        if (line.rfind("  \"", 0) != 0)
        {
            continue;
        }
        const auto quote = line.find("\": ", 3);
        if (quote == std::string::npos)
        {
            continue;
        }
        if (line.compare(quote + 3, 1, "{") == 0)
        {
            g.ids.push_back(line.substr(3, quote - 3));
            g.rows.push_back(Line{line});
        }
        else
        {
            g.top.text += line;
        }
    }
    return g;
}

} // namespace rinne::test::params
