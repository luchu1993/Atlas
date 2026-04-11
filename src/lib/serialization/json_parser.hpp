#pragma once

#include "serialization/data_section.hpp"

namespace atlas::json
{

[[nodiscard]] auto parse_file(const std::filesystem::path& path)
    -> Result<std::shared_ptr<DataSectionTree>>;
[[nodiscard]] auto parse_string(std::string_view json) -> Result<std::shared_ptr<DataSectionTree>>;

}  // namespace atlas::json
