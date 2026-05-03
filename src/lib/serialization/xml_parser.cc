#include "serialization/xml_parser.h"

#include <format>

#include <pugixml.hpp>

namespace atlas::xml {

namespace {

auto HasElementChildren(const pugi::xml_node& node) -> bool {
  for (auto c = node.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_element) {
      return true;
    }
  }
  return false;
}

void AddAttributes(DataSection* section, const pugi::xml_node& node) {
  for (auto attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
    section->AddChild(std::format("@{}", attr.name()), std::string(attr.value()));
  }
}

void PopulateSection(DataSection* section, const pugi::xml_node& node) {
  for (auto child = node.first_child(); child; child = child.next_sibling()) {
    if (child.type() != pugi::node_element) {
      continue;
    }

    DataSection* child_section;
    if (!HasElementChildren(child)) {
      child_section =
          section->AddChild(std::string(child.name()), std::string(child.text().as_string()));
    } else {
      child_section = section->AddChild(std::string(child.name()));
      PopulateSection(child_section, child);
    }

    AddAttributes(child_section, child);
  }
}

auto BuildTreeFromDoc(pugi::xml_document& doc) -> Result<std::shared_ptr<DataSectionTree>> {
  auto root_node = doc.first_child();
  while (root_node && root_node.type() != pugi::node_element) {
    root_node = root_node.next_sibling();
  }

  if (!root_node) {
    return Error(ErrorCode::kInvalidArgument, "XML document has no root element");
  }

  auto tree = std::make_shared<DataSectionTree>(std::string(root_node.name()));
  auto* root = tree->Root();

  if (!HasElementChildren(root_node)) {
    root->SetValue(root_node.text().as_string());
  } else {
    PopulateSection(root, root_node);
  }

  AddAttributes(root, root_node);

  return tree;
}

}  // anonymous namespace

auto ParseFile(const std::filesystem::path& path) -> Result<std::shared_ptr<DataSectionTree>> {
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_file(path.c_str());

  if (!result) {
    return Error(ErrorCode::kIoError, std::format("XML parse error: {} at offset {}",
                                                  result.description(), result.offset));
  }

  return BuildTreeFromDoc(doc);
}

auto ParseString(std::string_view xml) -> Result<std::shared_ptr<DataSectionTree>> {
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());

  if (!result) {
    return Error(ErrorCode::kInvalidArgument, std::format("XML parse error: {} at offset {}",
                                                          result.description(), result.offset));
  }

  return BuildTreeFromDoc(doc);
}

}  // namespace atlas::xml
