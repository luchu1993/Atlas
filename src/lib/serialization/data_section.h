#ifndef ATLAS_LIB_SERIALIZATION_DATA_SECTION_H_
#define ATLAS_LIB_SERIALIZATION_DATA_SECTION_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "foundation/error.h"

namespace atlas {

class DataSectionTree;

class DataSection {
 public:
  using Ptr = std::shared_ptr<DataSectionTree>;

  [[nodiscard]] auto Name() const -> std::string_view { return name_; }
  [[nodiscard]] auto Value() const -> std::string_view { return value_; }

  [[nodiscard]] auto ReadString(std::string_view key, std::string_view default_val = "") const
      -> std::string;
  [[nodiscard]] auto ReadInt(std::string_view key, int32_t default_val = 0) const -> int32_t;
  [[nodiscard]] auto ReadUint(std::string_view key, uint32_t default_val = 0) const -> uint32_t;
  [[nodiscard]] auto ReadFloat(std::string_view key, float default_val = 0.0f) const -> float;
  [[nodiscard]] auto ReadBool(std::string_view key, bool default_val = false) const -> bool;

  [[nodiscard]] auto Child(std::string_view name) const -> DataSection*;
  [[nodiscard]] auto Children() const -> const std::vector<DataSection*>&;
  [[nodiscard]] auto Children(std::string_view name) const -> std::vector<DataSection*>;
  [[nodiscard]] auto ChildCount() const -> std::size_t { return children_.size(); }

  void SetValue(std::string_view val);
  auto AddChild(std::string name) -> DataSection*;
  auto AddChild(std::string name, std::string value) -> DataSection*;

  [[nodiscard]] static auto FromXml(const std::filesystem::path& path) -> Result<Ptr>;
  [[nodiscard]] static auto FromXmlString(std::string_view xml) -> Result<Ptr>;
  [[nodiscard]] static auto FromJson(const std::filesystem::path& path) -> Result<Ptr>;
  [[nodiscard]] static auto FromJsonString(std::string_view json) -> Result<Ptr>;

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

class DataSectionTree : public std::enable_shared_from_this<DataSectionTree> {
 public:
  DataSectionTree();
  explicit DataSectionTree(std::string root_name);
  ~DataSectionTree();

  DataSectionTree(const DataSectionTree&) = delete;
  auto operator=(const DataSectionTree&) -> DataSectionTree& = delete;
  DataSectionTree(DataSectionTree&& other) noexcept;
  auto operator=(DataSectionTree&& other) noexcept -> DataSectionTree&;

  [[nodiscard]] auto Root() -> DataSection* { return root_; }
  [[nodiscard]] auto Root() const -> const DataSection* { return root_; }

  [[nodiscard]] auto Name() const -> std::string_view { return root_->Name(); }
  [[nodiscard]] auto Value() const -> std::string_view { return root_->Value(); }
  [[nodiscard]] auto Child(std::string_view n) const -> DataSection* { return root_->Child(n); }
  [[nodiscard]] auto Children() const -> const std::vector<DataSection*>& {
    return root_->Children();
  }
  [[nodiscard]] auto Children(std::string_view n) const -> std::vector<DataSection*> {
    return root_->Children(n);
  }
  [[nodiscard]] auto ReadString(std::string_view key, std::string_view def = "") const
      -> std::string {
    return root_->ReadString(key, def);
  }
  [[nodiscard]] auto ReadInt(std::string_view key, int32_t def = 0) const -> int32_t {
    return root_->ReadInt(key, def);
  }
  [[nodiscard]] auto ReadUint(std::string_view key, uint32_t def = 0) const -> uint32_t {
    return root_->ReadUint(key, def);
  }
  [[nodiscard]] auto ReadFloat(std::string_view key, float def = 0.0f) const -> float {
    return root_->ReadFloat(key, def);
  }
  [[nodiscard]] auto ReadBool(std::string_view key, bool def = false) const -> bool {
    return root_->ReadBool(key, def);
  }
  void SetValue(std::string_view val) { root_->SetValue(val); }
  auto AddChild(std::string n) -> DataSection* { return root_->AddChild(std::move(n)); }
  auto AddChild(std::string n, std::string v) -> DataSection* {
    return root_->AddChild(std::move(n), std::move(v));
  }

  auto AllocateNode() -> DataSection*;
  auto AllocateNode(std::string name) -> DataSection*;
  auto AllocateNode(std::string name, std::string value) -> DataSection*;

  [[nodiscard]] static auto FromXml(const std::filesystem::path& path)
      -> Result<std::shared_ptr<DataSectionTree>>;
  [[nodiscard]] static auto FromXmlString(std::string_view xml)
      -> Result<std::shared_ptr<DataSectionTree>>;
  [[nodiscard]] static auto FromJson(const std::filesystem::path& path)
      -> Result<std::shared_ptr<DataSectionTree>>;
  [[nodiscard]] static auto FromJsonString(std::string_view json)
      -> Result<std::shared_ptr<DataSectionTree>>;

 private:
  static constexpr std::size_t kBlockSize = 4096;
  struct Block {
    std::unique_ptr<std::byte[]> data;
    std::size_t used{0};
    std::size_t capacity{0};
  };

  auto ArenaAlloc(std::size_t size, std::size_t align) -> void*;

  std::vector<Block> blocks_;
  std::vector<DataSection*> all_nodes_;
  DataSection* root_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERIALIZATION_DATA_SECTION_H_
