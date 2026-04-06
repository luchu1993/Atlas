#pragma once

#include "serialization/data_section.hpp"

namespace atlas::json
{

[[nodiscard]] auto parse_file(const std::filesystem::path& path) -> Result<DataSection::Ptr>;
[[nodiscard]] auto parse_string(std::string_view json) -> Result<DataSection::Ptr>;

} // namespace atlas::json
