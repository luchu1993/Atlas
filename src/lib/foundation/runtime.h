#ifndef ATLAS_LIB_FOUNDATION_RUNTIME_H_
#define ATLAS_LIB_FOUNDATION_RUNTIME_H_

#include <atomic>
#include <filesystem>
#include <mutex>

#include "foundation/error.h"
#include "foundation/log.h"

namespace atlas {

struct RuntimeConfig {
  LogLevel log_level = LogLevel::kInfo;
  bool enable_file_logging = false;
  std::filesystem::path log_file;
};

class Runtime {
 public:
  [[nodiscard]] static auto Initialize(const RuntimeConfig& config = {}) -> Result<void>;

  static void Finalize();

  [[nodiscard]] static auto IsInitialized() -> bool {
    return init_count.load(std::memory_order_acquire) > 0;
  }

 private:
  static std::atomic<uint32_t> init_count;
  static std::mutex mutex;
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_RUNTIME_H_
