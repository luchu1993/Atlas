#include "serialization/data_section.hpp"

#include "serialization/json_parser.hpp"
#include "serialization/xml_parser.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace atlas
{

namespace stdfs = std::filesystem;

// ============================================================================
// Constructors
// ============================================================================

DataSection::DataSection(std::string name) : name_(std::move(name)) {}

DataSection::DataSection(std::string name, std::string value)
    : name_(std::move(name)), value_(std::move(value))
{
}

// ============================================================================
// Typed read helpers
// ============================================================================

auto DataSection::read_string(std::string_view key, std::string_view default_val) const
    -> std::string
{
    auto c = child(key);
    if (c)
    {
        return std::string(c->value());
    }
    return std::string(default_val);
}

auto DataSection::read_int(std::string_view key, int32_t default_val) const -> int32_t
{
    auto c = child(key);
    if (!c)
    {
        return default_val;
    }
    try
    {
        return std::stoi(std::string(c->value()));
    }
    catch (...)
    {
        return default_val;
    }
}

auto DataSection::read_uint(std::string_view key, uint32_t default_val) const -> uint32_t
{
    auto c = child(key);
    if (!c)
    {
        return default_val;
    }
    try
    {
        return static_cast<uint32_t>(std::stoul(std::string(c->value())));
    }
    catch (...)
    {
        return default_val;
    }
}

auto DataSection::read_float(std::string_view key, float default_val) const -> float
{
    auto c = child(key);
    if (!c)
    {
        return default_val;
    }
    try
    {
        return std::stof(std::string(c->value()));
    }
    catch (...)
    {
        return default_val;
    }
}

auto DataSection::read_bool(std::string_view key, bool default_val) const -> bool
{
    auto c = child(key);
    if (!c)
    {
        return default_val;
    }

    std::string val(c->value());
    std::transform(val.begin(), val.end(), val.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (val == "true" || val == "1" || val == "yes")
    {
        return true;
    }
    if (val == "false" || val == "0" || val == "no")
    {
        return false;
    }
    return default_val;
}

// ============================================================================
// Child access
// ============================================================================

auto DataSection::child(std::string_view name) const -> Ptr
{
    for (const auto& c : children_)
    {
        if (c->name() == name)
        {
            return c;
        }
    }
    return nullptr;
}

auto DataSection::children() const -> const std::vector<Ptr>&
{
    return children_;
}

auto DataSection::children(std::string_view name) const -> std::vector<Ptr>
{
    std::vector<Ptr> result;
    for (const auto& c : children_)
    {
        if (c->name() == name)
        {
            result.push_back(c);
        }
    }
    return result;
}

// ============================================================================
// Mutation
// ============================================================================

void DataSection::set_value(std::string_view val)
{
    value_ = std::string(val);
}

auto DataSection::add_child(std::string name) -> Ptr
{
    auto c = std::make_shared<DataSection>(std::move(name));
    children_.push_back(c);
    return c;
}

auto DataSection::add_child(std::string name, std::string value) -> Ptr
{
    auto c = std::make_shared<DataSection>(std::move(name), std::move(value));
    children_.push_back(c);
    return c;
}

// ============================================================================
// Factory methods
// ============================================================================

auto DataSection::from_xml(const stdfs::path& path) -> Result<Ptr>
{
    return xml::parse_file(path);
}

auto DataSection::from_xml_string(std::string_view xml) -> Result<Ptr>
{
    return xml::parse_string(xml);
}

auto DataSection::from_json(const stdfs::path& path) -> Result<Ptr>
{
    return json::parse_file(path);
}

auto DataSection::from_json_string(std::string_view json) -> Result<Ptr>
{
    return json::parse_string(json);
}

}  // namespace atlas
