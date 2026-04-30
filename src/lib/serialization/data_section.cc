#include "serialization/data_section.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <new>

#include "serialization/json_parser.h"
#include "serialization/xml_parser.h"

namespace atlas {

namespace stdfs = std::filesystem;

namespace {

auto Iequals(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

DataSection::DataSection(std::string name) : name_(std::move(name)) {}

DataSection::DataSection(std::string name, std::string value)
    : name_(std::move(name)), value_(std::move(value)) {}

auto DataSection::ReadString(std::string_view key, std::string_view default_val) const
    -> std::string {
  auto* c = Child(key);
  if (c) {
    return std::string(c->Value());
  }
  return std::string(default_val);
}

auto DataSection::ReadInt(std::string_view key, int32_t default_val) const -> int32_t {
  auto* c = Child(key);
  if (!c) {
    return default_val;
  }
  auto sv = c->Value();
  int32_t result{};
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
  if (ec != std::errc{}) {
    return default_val;
  }
  return result;
}

auto DataSection::ReadUint(std::string_view key, uint32_t default_val) const -> uint32_t {
  auto* c = Child(key);
  if (!c) {
    return default_val;
  }
  auto sv = c->Value();
  uint32_t result{};
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
  if (ec != std::errc{}) {
    return default_val;
  }
  return result;
}

auto DataSection::ReadFloat(std::string_view key, float default_val) const -> float {
  auto* c = Child(key);
  if (!c) {
    return default_val;
  }
  auto sv = c->Value();
  float result{};
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
  if (ec != std::errc{}) {
    return default_val;
  }
  return result;
}

auto DataSection::ReadBool(std::string_view key, bool default_val) const -> bool {
  auto* c = Child(key);
  if (!c) {
    return default_val;
  }

  auto sv = c->Value();
  if (Iequals(sv, "true") || sv == "1" || Iequals(sv, "yes")) {
    return true;
  }
  if (Iequals(sv, "false") || sv == "0" || Iequals(sv, "no")) {
    return false;
  }
  return default_val;
}

auto DataSection::Child(std::string_view name) const -> DataSection* {
  auto it = child_index_.find(name);
  if (it != child_index_.end()) {
    return children_[it->second];
  }
  return nullptr;
}

auto DataSection::Children() const -> const std::vector<DataSection*>& {
  return children_;
}

auto DataSection::Children(std::string_view name) const -> std::vector<DataSection*> {
  std::vector<DataSection*> result;
  for (auto* c : children_) {
    if (c->Name() == name) {
      result.push_back(c);
    }
  }
  return result;
}

void DataSection::SetValue(std::string_view val) {
  value_ = std::string(val);
}

auto DataSection::AddChild(std::string name) -> DataSection* {
  auto* node = tree_->AllocateNode(std::move(name));
  auto idx = children_.size();
  children_.push_back(node);
  child_index_.try_emplace(std::string_view(children_.back()->name_), idx);
  return node;
}

auto DataSection::AddChild(std::string name, std::string value) -> DataSection* {
  auto* node = tree_->AllocateNode(std::move(name), std::move(value));
  auto idx = children_.size();
  children_.push_back(node);
  child_index_.try_emplace(std::string_view(children_.back()->name_), idx);
  return node;
}

DataSectionTree::DataSectionTree() {
  root_ = AllocateNode();
}

DataSectionTree::DataSectionTree(std::string root_name) {
  root_ = AllocateNode(std::move(root_name));
}

DataSectionTree::~DataSectionTree() {
  for (auto* node : all_nodes_) {
    node->~DataSection();
  }
}

DataSectionTree::DataSectionTree(DataSectionTree&& other) noexcept
    : blocks_(std::move(other.blocks_)),
      all_nodes_(std::move(other.all_nodes_)),
      root_(other.root_) {
  other.root_ = nullptr;
  for (auto* node : all_nodes_) {
    node->tree_ = this;
  }
}

auto DataSectionTree::operator=(DataSectionTree&& other) noexcept -> DataSectionTree& {
  if (this != &other) {
    for (auto* node : all_nodes_) {
      node->~DataSection();
    }

    blocks_ = std::move(other.blocks_);
    all_nodes_ = std::move(other.all_nodes_);
    root_ = other.root_;
    other.root_ = nullptr;

    for (auto* node : all_nodes_) {
      node->tree_ = this;
    }
  }
  return *this;
}

auto DataSectionTree::AllocateNode() -> DataSection* {
  void* mem = ArenaAlloc(sizeof(DataSection), alignof(DataSection));
  auto* node = new (mem) DataSection();
  node->tree_ = this;
  all_nodes_.push_back(node);
  return node;
}

auto DataSectionTree::AllocateNode(std::string name) -> DataSection* {
  void* mem = ArenaAlloc(sizeof(DataSection), alignof(DataSection));
  auto* node = new (mem) DataSection(std::move(name));
  node->tree_ = this;
  all_nodes_.push_back(node);
  return node;
}

auto DataSectionTree::AllocateNode(std::string name, std::string value) -> DataSection* {
  void* mem = ArenaAlloc(sizeof(DataSection), alignof(DataSection));
  auto* node = new (mem) DataSection(std::move(name), std::move(value));
  node->tree_ = this;
  all_nodes_.push_back(node);
  return node;
}

auto DataSectionTree::ArenaAlloc(std::size_t size, std::size_t align) -> void* {
  if (!blocks_.empty()) {
    auto& back = blocks_.back();
    auto addr = reinterpret_cast<std::uintptr_t>(back.data.get()) + back.used;
    auto aligned = (addr + align - 1) & ~(align - 1);
    auto offset = aligned - reinterpret_cast<std::uintptr_t>(back.data.get());
    if (offset + size <= back.capacity) {
      back.used = offset + size;
      return reinterpret_cast<void*>(aligned);
    }
  }

  auto cap = std::max(kBlockSize, size + align);
  Block block;
  block.data = std::make_unique<std::byte[]>(cap);
  block.capacity = cap;

  auto addr = reinterpret_cast<std::uintptr_t>(block.data.get());
  auto aligned = (addr + align - 1) & ~(align - 1);
  auto offset = aligned - addr;
  block.used = offset + size;

  auto* ptr = reinterpret_cast<void*>(aligned);
  blocks_.push_back(std::move(block));
  return ptr;
}

auto DataSectionTree::FromXml(const stdfs::path& path) -> Result<std::shared_ptr<DataSectionTree>> {
  return xml::ParseFile(path);
}

auto DataSectionTree::FromXmlString(std::string_view xml)
    -> Result<std::shared_ptr<DataSectionTree>> {
  return xml::ParseString(xml);
}

auto DataSectionTree::FromJson(const stdfs::path& path)
    -> Result<std::shared_ptr<DataSectionTree>> {
  return json::ParseFile(path);
}

auto DataSectionTree::FromJsonString(std::string_view json)
    -> Result<std::shared_ptr<DataSectionTree>> {
  return json::ParseString(json);
}

auto DataSection::FromXml(const stdfs::path& path) -> Result<Ptr> {
  return DataSectionTree::FromXml(path);
}

auto DataSection::FromXmlString(std::string_view xml) -> Result<Ptr> {
  return DataSectionTree::FromXmlString(xml);
}

auto DataSection::FromJson(const stdfs::path& path) -> Result<Ptr> {
  return DataSectionTree::FromJson(path);
}

auto DataSection::FromJsonString(std::string_view json) -> Result<Ptr> {
  return DataSectionTree::FromJsonString(json);
}

}  // namespace atlas
