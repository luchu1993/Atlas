#include "string_utils.hpp"

#include <algorithm>
#include <cctype>

namespace atlas::string_utils
{

[[nodiscard]] auto split(std::string_view str, char delimiter) -> std::vector<std::string_view>
{
    std::vector<std::string_view> result;
    std::size_t start = 0;

    while (true)
    {
        auto pos = str.find(delimiter, start);
        if (pos == std::string_view::npos)
        {
            result.push_back(str.substr(start));
            break;
        }
        result.push_back(str.substr(start, pos - start));
        start = pos + 1;
    }

    return result;
}

[[nodiscard]] auto split(std::string_view str, std::string_view delimiter) -> std::vector<std::string_view>
{
    std::vector<std::string_view> result;

    if (delimiter.empty())
    {
        result.push_back(str);
        return result;
    }

    std::size_t start = 0;

    while (true)
    {
        auto pos = str.find(delimiter, start);
        if (pos == std::string_view::npos)
        {
            result.push_back(str.substr(start));
            break;
        }
        result.push_back(str.substr(start, pos - start));
        start = pos + delimiter.size();
    }

    return result;
}

[[nodiscard]] auto trim_left(std::string_view str) -> std::string_view
{
    std::size_t i = 0;
    while (i < str.size() && std::isspace(static_cast<unsigned char>(str[i])))
    {
        ++i;
    }
    return str.substr(i);
}

[[nodiscard]] auto trim_right(std::string_view str) -> std::string_view
{
    auto end = str.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1])))
    {
        --end;
    }
    return str.substr(0, end);
}

[[nodiscard]] auto trim(std::string_view str) -> std::string_view
{
    return trim_left(trim_right(str));
}

[[nodiscard]] auto to_lower(std::string_view str) -> std::string
{
    std::string result{str};
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] auto to_upper(std::string_view str) -> std::string
{
    std::string result{str};
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

[[nodiscard]] auto iequals(std::string_view a, std::string_view b) -> bool
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] auto join(const std::vector<std::string_view>& parts, std::string_view separator) -> std::string
{
    std::string result;

    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            result.append(separator);
        }
        result.append(parts[i]);
    }

    return result;
}

[[nodiscard]] auto replace_all(std::string_view str, std::string_view from, std::string_view to) -> std::string
{
    if (from.empty())
    {
        return std::string{str};
    }

    std::string result;
    std::size_t start = 0;

    while (true)
    {
        auto pos = str.find(from, start);
        if (pos == std::string_view::npos)
        {
            result.append(str.substr(start));
            break;
        }
        result.append(str.substr(start, pos - start));
        result.append(to);
        start = pos + from.size();
    }

    return result;
}

} // namespace atlas::string_utils
