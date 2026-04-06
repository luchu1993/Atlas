#include "serialization/json_parser.hpp"

// Suppress deprecation warnings from rapidjson (uses deprecated std::iterator)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 5054)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <fstream>
#include <sstream>

namespace atlas::json
{

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{

void populate_from_value(DataSection::Ptr& section, const rapidjson::Value& val);

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
    // Object/Array handled separately
    return "";
}

void convert_value(DataSection::Ptr& parent, const std::string& key, const rapidjson::Value& val)
{
    if (val.IsObject())
    {
        auto child = parent->add_child(key);
        populate_from_value(child, val);
    }
    else if (val.IsArray())
    {
        auto child = parent->add_child(key);
        for (rapidjson::SizeType i = 0; i < val.Size(); ++i)
        {
            convert_value(child, std::to_string(i), val[i]);
        }
    }
    else
    {
        parent->add_child(key, value_to_string(val));
    }
}

void populate_from_value(DataSection::Ptr& section, const rapidjson::Value& val)
{
    if (val.IsObject())
    {
        for (auto it = val.MemberBegin(); it != val.MemberEnd(); ++it)
        {
            std::string member_name(it->name.GetString(), it->name.GetStringLength());
            convert_value(section, member_name, it->value);
        }
    }
    else if (val.IsArray())
    {
        for (rapidjson::SizeType i = 0; i < val.Size(); ++i)
        {
            convert_value(section, std::to_string(i), val[i]);
        }
    }
    else
    {
        section->set_value(value_to_string(val));
    }
}

}  // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

auto parse_file(const std::filesystem::path& path) -> Result<DataSection::Ptr>
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return Error(ErrorCode::IoError, "Failed to open JSON file: " + path.string());
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    return parse_string(oss.str());
}

auto parse_string(std::string_view json) -> Result<DataSection::Ptr>
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

    auto root = std::make_shared<DataSection>("root");
    populate_from_value(root, doc);
    return root;
}

}  // namespace atlas::json
