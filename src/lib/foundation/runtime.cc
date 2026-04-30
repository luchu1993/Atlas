#include "foundation/runtime.h"

#include "foundation/log.h"
#include "foundation/log_sinks.h"

namespace atlas {

std::atomic<uint32_t> Runtime::init_count{0};
std::mutex Runtime::mutex{};

auto Runtime::Initialize(const RuntimeConfig& config) -> Result<void> {
  std::lock_guard lock(mutex);

  const uint32_t kPrevious = init_count.load(std::memory_order_acquire);
  if (kPrevious > 0) {
    init_count.store(kPrevious + 1, std::memory_order_release);
    Logger::Instance().SetLevel(config.log_level);
    return {};
  }

  auto& logger = Logger::Instance();
  logger.SetLevel(config.log_level);
  logger.AddSink(std::make_shared<ConsoleSink>());
  if (config.enable_file_logging && !config.log_file.empty()) {
    logger.AddSink(std::make_shared<FileSink>(config.log_file));
  }

  init_count.store(1, std::memory_order_release);
  ATLAS_LOG_INFO("Atlas Runtime initialized");
  return {};
}

void Runtime::Finalize() {
  std::lock_guard lock(mutex);

  const uint32_t kPrevious = init_count.load(std::memory_order_acquire);
  if (kPrevious == 0) return;

  if (kPrevious > 1) {
    init_count.store(kPrevious - 1, std::memory_order_release);
    return;
  }

  init_count.store(0, std::memory_order_release);

  ATLAS_LOG_INFO("Atlas Runtime finalized");
  Logger::Instance().ClearSinks();
}

}  // namespace atlas
