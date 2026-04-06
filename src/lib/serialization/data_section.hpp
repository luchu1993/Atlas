#pragma once

#include "foundation/error.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

class DataSection
{
public:
    using Ptr = std::shared_ptr<DataSection>;

    DataSection() = default;
    explicit DataSection(std::string name);
    DataSection(std::string name, std::string value);

    [[nodiscard]] auto name() const -> std::string_view { return name_; }
    [[nodiscard]] auto value() const -> std::string_view { return value_; }

    // Typed read: search children by key, return child's value converted to type
    [[nodiscard]] auto read_string(std::string_view key, std::string_view default_val = "") const
        -> std::string;
    [[nodiscard]] auto read_int(std::string_view key, int32_t default_val = 0) const -> int32_t;
    [[nodiscard]] auto read_uint(std::string_view key, uint32_t default_val = 0) const -> uint32_t;
    [[nodiscard]] auto read_float(std::string_view key, float default_val = 0.0f) const -> float;
    [[nodiscard]] auto read_bool(std::string_view key, bool default_val = false) const -> bool;

    // Child access
    [[nodiscard]] auto child(std::string_view name) const -> Ptr;
    [[nodiscard]] auto children() const -> const std::vector<Ptr>&;
    [[nodiscard]] auto children(std::string_view name) const -> std::vector<Ptr>;

    // Mutation
    void set_value(std::string_view val);
    auto add_child(std::string name) -> Ptr;
    auto add_child(std::string name, std::string value) -> Ptr;

    // Factory from files/strings
    [[nodiscard]] static auto from_xml(const std::filesystem::path& path) -> Result<Ptr>;
    [[nodiscard]] static auto from_xml_string(std::string_view xml) -> Result<Ptr>;
    [[nodiscard]] static auto from_json(const std::filesystem::path& path) -> Result<Ptr>;
    [[nodiscard]] static auto from_json_string(std::string_view json) -> Result<Ptr>;

private:
    std::string name_;
    std::string value_;
    std::vector<Ptr> children_;
};

}  // namespace atlas
