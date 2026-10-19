// SPDX-License-Identifier: GPL-3.0-or-later
//
// 读 tests/golden/ipc-layout.json 的最小解析器。golden 是规范排版（contract-static-ipc 检查）：每条记录一行、
// 键序固定，所以这里按行读，用「"键": 值」取字段，不需要完整的 JSON 解析器。记录按所在的数组分节。
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rinne::test
{

struct GoldenRecord
{
    std::string line;

    std::optional<std::string> str(const std::string& key) const
    {
        const auto at = find(key);
        if (!at || *at >= line.size() || line[*at] != '"')
        {
            return std::nullopt;
        }
        std::string out;
        for (std::size_t i = *at + 1; i < line.size(); ++i)
        {
            if (line[i] == '\\' && i + 1 < line.size())
            {
                out.push_back(line[++i]);
            }
            else if (line[i] == '"')
            {
                return out;
            }
            else
            {
                out.push_back(line[i]);
            }
        }
        return std::nullopt;
    }

    std::optional<long long> num(const std::string& key) const
    {
        const auto at = find(key);
        if (!at)
        {
            return std::nullopt;
        }
        char* end = nullptr;
        const long long v = std::strtoll(line.c_str() + *at, &end, 10);
        if (end == line.c_str() + *at)
        {
            return std::nullopt;
        }
        return v;
    }

    std::optional<bool> flag(const std::string& key) const
    {
        const auto at = find(key);
        if (!at)
        {
            return std::nullopt;
        }
        if (line.compare(*at, 4, "true") == 0)
        {
            return true;
        }
        if (line.compare(*at, 5, "false") == 0)
        {
            return false;
        }
        return std::nullopt;
    }

private:
    // 值开始的位置
    std::optional<std::size_t> find(const std::string& key) const
    {
        const std::string needle = "\"" + key + "\": ";
        const auto at = line.find(needle);
        if (at == std::string::npos)
        {
            return std::nullopt;
        }
        return at + needle.size();
    }
};

struct Golden
{
    bool loaded = false;
    std::string path;
    std::map<std::string, std::vector<GoldenRecord>> sections;
    std::map<std::string, long long> top;
};

// 仓根由本文件的位置推出（tests/core/ipc/layout/ 往上四级）：测试不依赖工作目录，也不读环境变量
inline std::filesystem::path repoRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
}

inline Golden loadIpcGolden()
{
    Golden g;
    const auto file = repoRoot() / "tests" / "golden" / "ipc-layout.json";
    g.path = file.string();
    std::ifstream in(file);
    if (!in)
    {
        return g;
    }
    std::string section;
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const auto colon = line.find("\": ");
        if (line.rfind("  \"", 0) == 0 && line.size() > 3 && line[3] != ' ' && colon != std::string::npos)
        {
            const std::string key = line.substr(3, colon - 3);
            const std::string rest = line.substr(colon + 3);
            if (rest == "[")
            {
                section = key;
            }
            else
            {
                g.top[key] = std::strtoll(rest.c_str(), nullptr, 10);
            }
            continue;
        }
        if (line.find("{ \"") != std::string::npos && !section.empty())
        {
            g.sections[section].push_back(GoldenRecord{line});
        }
    }
    g.loaded = true;
    return g;
}

} // namespace rinne::test
