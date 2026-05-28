#ifndef ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_
#define ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "foundation/error.h"

namespace atlas {

struct ProcessLaunchOptions {
  std::filesystem::path exe;
  std::vector<std::string> args;
  std::filesystem::path working_directory;
  std::filesystem::path output_path;
};

[[nodiscard]] auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<uint32_t>;
[[nodiscard]] auto IsProcessAlive(uint32_t pid) -> bool;
[[nodiscard]] auto TerminateProcessByPid(uint32_t pid) -> bool;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_
