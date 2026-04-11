#include "serialization/json_parser.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 5054)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <cassert>
#include <charconv>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "platform/filesystem.hpp"

namespace atlas::json
{

namespace
{

void populate_from_value(DataSection* section, const rapidjson::Value& val);

auto value_to_string(const rapidjson::Value& val) -> std::string
{
    if (val.IsString())
    {
        return std::string(val.GetString(), val.GetStringLength());
    }
    if (val.IsInt())
    {
        return std::to_string(val.GetInt());
    }
    if (val.IsUint())
    {
        return std::to_string(val.GetUint());
    }
    if (val.IsInt64())
    {
        return std::to_string(val.GetInt64());
    }
    if (val.IsUint64())
    {
        return std::to_string(val.GetUint64());
    }
    if (val.IsDouble())
    {
        return std::to_string(val.GetDouble());
    }
    if (val.IsBool())
    {
        return val.GetBool() ? "true" : "false";
    }
    if (val.IsNull())
    {
        return "";
    }
    return "";
}

void convert_value(DataSection* parent, std::string_view key, const rapidjson::Value& val)
{
    if (val.IsObject())
    {
        auto* child = parent->add_child(std::string(key));
        populate_from_value(child, val);
    }
    else if (val.IsArray())
    {
        auto* child = parent->add_child(std::string(key));
        for (rapidjson::SizeType i = 0; i < val.Size(); ++i)
        {
            char idx_buf[32];
            auto [ptr, ec] = std::to_chars(idx_buf, idx_buf + sizeof(idx_buf),
                                           static_cast<unsigned long long>(i));
            assert(ec == std::errc{});
            convert_value(child, std::string_view(idx_buf, static_cast<std::size_t>(ptr - idx_buf)),
                          val[i]);
        }
    }
    else
    {
        parent->add_child(std::string(key), value_to_string(val));
    }
}

void populate_from_value(DataSection* section, const rapidjson::Value& val)
{
    if (val.IsObject())
    {
        for (auto it = val.MemberBegin(); it != val.MemberEnd(); ++it)
        {
            convert_value(section,
                          std::string_view(it->name.GetString(), it->name.GetStringLength()),
                          it->value);
        }
    }
    else if (val.IsArray())
    {
        for (rapidjson::SizeType i = 0; i < val.Size(); ++i)
        {
            char idx_buf[32];
            auto [ptr, ec] = std::to_chars(idx_buf, idx_buf + sizeof(idx_buf),
                                           static_cast<unsigned long long>(i));
            assert(ec == std::errc{});
            convert_value(section,
                          std::string_view(idx_buf, static_cast<std::size_t>(ptr - idx_buf)),
                          val[i]);
        }
    }
    else
    {
        section->set_value(value_to_string(val));
    }
}

}  // anonymous namespace

auto parse_file(const std::filesystem::path& path) -> Result<std::shared_ptr<DataSectionTree>>
{
    auto text = fs::read_text_file(path);
    if (!text)
    {
        return text.error();
    }
    return parse_string(*text);
}

auto parse_string(std::string_view json) -> Result<std::shared_ptr<DataSectionTree>>
{
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());

    if (doc.HasParseError())
    {
        std::string msg = "JSON parse error: ";
        msg += rapidjson::GetParseError_En(doc.GetParseError());
        msg += " at offset ";
        msg += std::to_string(doc.GetErrorOffset());
        return Error(ErrorCode::InvalidArgument, std::move(msg));
    }

    auto tree = std::make_shared<DataSectionTree>("root");
    populate_from_value(tree->root(), doc);
    return tree;
}

}  // namespace atlas::json
