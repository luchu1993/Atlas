#pragma once

#include "foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atlas
{

class DataSectionTree;

class DataSection
{
public:
    using Ptr = std::shared_ptr<DataSectionTree>;

    [[nodiscard]] auto name() const -> std::string_view { return name_; }
    [[nodiscard]] auto value() const -> std::string_view { return value_; }

    [[nodiscard]] auto read_string(std::string_view key, std::string_view default_val = "") const
        -> std::string;
    [[nodiscard]] auto read_int(std::string_view key, int32_t default_val = 0) const -> int32_t;
    [[nodiscard]] auto read_uint(std::string_view key, uint32_t default_val = 0) const -> uint32_t;
    [[nodiscard]] auto read_float(std::string_view key, float default_val = 0.0f) const -> float;
    [[nodiscard]] auto read_bool(std::string_view key, bool default_val = false) const -> bool;

    [[nodiscard]] auto child(std::string_view name) const -> DataSection*;
    [[nodiscard]] auto children() const -> const std::vector<DataSection*>&;
    [[nodiscard]] auto children(std::string_view name) const -> std::vector<DataSection*>;
    [[nodiscard]] auto child_count() const -> std::size_t { return children_.size(); }

    void set_value(std::string_view val);
    auto add_child(std::string name) -> DataSection*;
    auto add_child(std::string name, std::string value) -> DataSection*;

    [[nodiscard]] static auto from_xml(const std::filesystem::path& path) -> Result<Ptr>;
    [[nodiscard]] static auto from_xml_string(std::string_view xml) -> Result<Ptr>;
    [[nodiscard]] static auto from_json(const std::filesystem::path& path) -> Result<Ptr>;
    [[nodiscard]] static auto from_json_string(std::string_view json) -> Result<Ptr>;

private:
    friend class DataSectionTree;

    DataSection() = default;
    explicit DataSection(std::string name);
    DataSection(std::string name, std::string value);

    std::string name_;
    std::string value_;
    std::vector<DataSection*> children_;
    std::unordered_map<std::string_view, std::size_t> child_index_;

    DataSectionTree* tree_{nullptr};
};

class DataSectionTree : public std::enable_shared_from_this<DataSectionTree>
{
public:
    DataSectionTree();
    explicit DataSectionTree(std::string root_name);
    ~DataSectionTree();

    DataSectionTree(const DataSectionTree&) = delete;
    auto operator=(const DataSectionTree&) -> DataSectionTree& = delete;
    DataSectionTree(DataSectionTree&& other) noexcept;
    auto operator=(DataSectionTree&& other) noexcept -> DataSectionTree&;

    [[nodiscard]] auto root() -> DataSection* { return root_; }
    [[nodiscard]] auto root() const -> const DataSection* { return root_; }

    [[nodiscard]] auto name() const -> std::string_view { return root_->name(); }
    [[nodiscard]] auto value() const -> std::string_view { return root_->value(); }
    [[nodiscard]] auto child(std::string_view n) const -> DataSection* { return root_->child(n); }
    [[nodiscard]] auto children() const -> const std::vector<DataSection*>&
    {
        return root_->children();
    }
    [[nodiscard]] auto children(std::string_view n) const -> std::vector<DataSection*>
    {
        return root_->children(n);
    }
    [[nodiscard]] auto read_string(std::string_view key, std::string_view def = "") const
        -> std::string
    {
        return root_->read_string(key, def);
    }
    [[nodiscard]] auto read_int(std::string_view key, int32_t def = 0) const -> int32_t
    {
        return root_->read_int(key, def);
    }
    [[nodiscard]] auto read_uint(std::string_view key, uint32_t def = 0) const -> uint32_t
    {
        return root_->read_uint(key, def);
    }
    [[nodiscard]] auto read_float(std::string_view key, float def = 0.0f) const -> float
    {
        return root_->read_float(key, def);
    }
    [[nodiscard]] auto read_bool(std::string_view key, bool def = false) const -> bool
    {
        return root_->read_bool(key, def);
    }
    void set_value(std::string_view val) { root_->set_value(val); }
    auto add_child(std::string n) -> DataSection* { return root_->add_child(std::move(n)); }
    auto add_child(std::string n, std::string v) -> DataSection*
    {
        return root_->add_child(std::move(n), std::move(v));
    }

    auto allocate_node() -> DataSection*;
    auto allocate_node(std::string name) -> DataSection*;
    auto allocate_node(std::string name, std::string value) -> DataSection*;

    [[nodiscard]] static auto from_xml(const std::filesystem::path& path)
        -> Result<std::shared_ptr<DataSectionTree>>;
    [[nodiscard]] static auto from_xml_string(std::string_view xml)
        -> Result<std::shared_ptr<DataSectionTree>>;
    [[nodiscard]] static auto from_json(const std::filesystem::path& path)
        -> Result<std::shared_ptr<DataSectionTree>>;
    [[nodiscard]] static auto from_json_string(std::string_view json)
        -> Result<std::shared_ptr<DataSectionTree>>;

private:
    static constexpr std::size_t kBlockSize = 4096;
    struct Block
    {
        std::unique_ptr<std::byte[]> data;
        std::size_t used{0};
        std::size_t capacity{0};
    };

    auto arena_alloc(std::size_t size, std::size_t align) -> void*;

    std::vector<Block> blocks_;
    std::vector<DataSection*> all_nodes_;
    DataSection* root_{nullptr};
};

}  // namespace atlas
