#ifndef ATLAS_LIB_SERIALIZATION_JSON_PARSER_H_
#define ATLAS_LIB_SERIALIZATION_JSON_PARSER_H_

#include "serialization/data_section.h"

namespace atlas::json {

[[nodiscard]] auto ParseFile(const std::filesystem::path& path)
    -> Result<std::shared_ptr<DataSectionTree>>;
[[nodiscard]] auto ParseString(std::string_view json) -> Result<std::shared_ptr<DataSectionTree>>;

}  // namespace atlas::json

#endif  // ATLAS_LIB_SERIALIZATION_JSON_PARSER_H_
