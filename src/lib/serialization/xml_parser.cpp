#include "serialization/xml_parser.hpp"

#include <pugixml.hpp>

namespace atlas::xml
{

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{

// Store XML attributes as child nodes with the "@name" naming convention,
// mirroring the BigWorld .def file format expectations.
void add_attributes(DataSection::Ptr& section, const pugi::xml_node& node)
{
    for (auto attr = node.first_attribute(); attr; attr = attr.next_attribute())
    {
        std::string key = "@";
        key += attr.name();
        section->add_child(std::move(key), std::string(attr.value()));
    }
}

void populate_section(DataSection::Ptr& section, const pugi::xml_node& node)
{
    for (auto child = node.first_child(); child; child = child.next_sibling())
    {
        if (child.type() != pugi::node_element)
        {
            continue;
        }

        // Check if this child has element sub-children
        bool child_has_elements = false;
        for (auto gc = child.first_child(); gc; gc = gc.next_sibling())
        {
            if (gc.type() == pugi::node_element)
            {
                child_has_elements = true;
                break;
            }
        }

        DataSection::Ptr child_section;
        if (!child_has_elements)
        {
            child_section = section->add_child(std::string(child.name()),
                                               std::string(child.text().as_string()));
        }
        else
        {
            child_section = section->add_child(std::string(child.name()));
            populate_section(child_section, child);
        }

        add_attributes(child_section, child);
    }
}

// Shared post-parse logic: build a DataSection tree from a loaded document.
auto build_section_from_doc(pugi::xml_document& doc) -> Result<DataSection::Ptr>
{
    auto root_node = doc.first_child();
    while (root_node && root_node.type() != pugi::node_element)
    {
        root_node = root_node.next_sibling();
    }

    if (!root_node)
    {
        return Error(ErrorCode::InvalidArgument, "XML document has no root element");
    }

    auto root = std::make_shared<DataSection>(std::string(root_node.name()));

    bool has_element_children = false;
    for (auto child = root_node.first_child(); child; child = child.next_sibling())
    {
        if (child.type() == pugi::node_element)
        {
            has_element_children = true;
            break;
        }
    }

    if (!has_element_children)
    {
        root->set_value(root_node.text().as_string());
    }
    else
    {
        populate_section(root, root_node);
    }

    add_attributes(root, root_node);

    return root;
}

}  // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

auto parse_file(const std::filesystem::path& path) -> Result<DataSection::Ptr>
{
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());

    if (!result)
    {
        std::string msg = "XML parse error: ";
        msg += result.description();
        msg += " at offset ";
        msg += std::to_string(result.offset);
        return Error(ErrorCode::IoError, std::move(msg));
    }

    return build_section_from_doc(doc);
}

auto parse_string(std::string_view xml) -> Result<DataSection::Ptr>
{
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());

    if (!result)
    {
        std::string msg = "XML parse error: ";
        msg += result.description();
        msg += " at offset ";
        msg += std::to_string(result.offset);
        return Error(ErrorCode::InvalidArgument, std::move(msg));
    }

    return build_section_from_doc(doc);
}

}  // namespace atlas::xml
