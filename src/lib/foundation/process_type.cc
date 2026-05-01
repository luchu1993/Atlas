#include "foundation/process_type.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

namespace atlas {

auto ProcessTypeName(ProcessType type) -> std::string_view {
  switch (type) {
    case ProcessType::kMachined:
      return "machined";
    case ProcessType::kLoginApp:
      return "loginapp";
    case ProcessType::kBaseApp:
      return "baseapp";
    case ProcessType::kBaseAppMgr:
      return "baseappmgr";
    case ProcessType::kCellApp:
      return "cellapp";
    case ProcessType::kCellAppMgr:
      return "cellappmgr";
    case ProcessType::kDbApp:
      return "dbapp";
    case ProcessType::kDbAppMgr:
      return "dbappmgr";
    case ProcessType::kReviver:
      return "reviver";
  }
  return "unknown";
}

auto ProcessTypeFromName(std::string_view name) -> Result<ProcessType> {
  auto lower = std::string(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "machined") return ProcessType::kMachined;
  if (lower == "loginapp") return ProcessType::kLoginApp;
  if (lower == "baseapp") return ProcessType::kBaseApp;
  if (lower == "baseappmgr") return ProcessType::kBaseAppMgr;
  if (lower == "cellapp") return ProcessType::kCellApp;
  if (lower == "cellappmgr") return ProcessType::kCellAppMgr;
  if (lower == "dbapp") return ProcessType::kDbApp;
  if (lower == "dbappmgr") return ProcessType::kDbAppMgr;
  if (lower == "reviver") return ProcessType::kReviver;

  return Error{ErrorCode::kInvalidArgument, std::format("unknown process type: '{}'", name)};
}

}  // namespace atlas
