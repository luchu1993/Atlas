#pragma once

#include "serialization/data_section.hpp"

namespace atlas::xml
{

[[nodiscard]] auto parse_file(const std::filesystem::path& path)
    -> Result<std::shared_ptr<DataSectionTree>>;
[[nodiscard]] auto parse_string(std::string_view xml) -> Result<std::shared_ptr<DataSectionTree>>;

}  // namespace atlas::xml
