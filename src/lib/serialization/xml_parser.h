#ifndef ATLAS_LIB_SERIALIZATION_XML_PARSER_H_
#define ATLAS_LIB_SERIALIZATION_XML_PARSER_H_

#include "serialization/data_section.h"

namespace atlas::xml {

[[nodiscard]] auto ParseFile(const std::filesystem::path& path)
    -> Result<std::shared_ptr<DataSectionTree>>;
[[nodiscard]] auto ParseString(std::string_view xml) -> Result<std::shared_ptr<DataSectionTree>>;

}  // namespace atlas::xml

#endif  // ATLAS_LIB_SERIALIZATION_XML_PARSER_H_
