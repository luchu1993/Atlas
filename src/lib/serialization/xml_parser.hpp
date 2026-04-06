#pragma once

#include "serialization/data_section.hpp"

namespace atlas::xml
{

[[nodiscard]] auto parse_file(const std::filesystem::path& path) -> Result<DataSection::Ptr>;
[[nodiscard]] auto parse_string(std::string_view xml) -> Result<DataSection::Ptr>;

} // namespace atlas::xml
