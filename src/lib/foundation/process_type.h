#ifndef ATLAS_LIB_FOUNDATION_PROCESS_TYPE_H_
#define ATLAS_LIB_FOUNDATION_PROCESS_TYPE_H_

#include <cstdint>
#include <string_view>

#include "foundation/error.h"

namespace atlas {

enum class ProcessType : uint8_t {
  kMachined = 0,
  kLoginApp = 1,
  kBaseApp = 2,
  kBaseAppMgr = 3,
  kCellApp = 4,
  kCellAppMgr = 5,
  kDbApp = 6,
  kDbAppMgr = 7,
  kReviver = 8,
};

[[nodiscard]] auto ProcessTypeName(ProcessType type) -> std::string_view;
[[nodiscard]] auto ProcessTypeFromName(std::string_view name) -> Result<ProcessType>;

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_PROCESS_TYPE_H_
