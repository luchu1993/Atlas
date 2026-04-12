#include "foundation/runtime.hpp"

#include "foundation/log.hpp"
#include "foundation/log_sinks.hpp"

namespace atlas
{

std::atomic<uint32_t> Runtime::init_count_{0};
std::mutex Runtime::mutex_{};

auto Runtime::initialize(const RuntimeConfig& config) -> Result<void>
{
    std::lock_guard lock(mutex_);

    const uint32_t previous = init_count_.load(std::memory_order_acquire);
    if (previous > 0)
    {
        init_count_.store(previous + 1, std::memory_order_release);
        Logger::instance().set_level(config.log_level);
        return {};
    }

    // 1. Logging — must be first so subsequent subsystems can log.
    auto& logger = Logger::instance();
    logger.set_level(config.log_level);
    logger.add_sink(std::make_shared<ConsoleSink>());
    if (config.enable_file_logging && !config.log_file.empty())
    {
        logger.add_sink(std::make_shared<FileSink>(config.log_file));
    }

    // 2. Signal handling — placeholder for install_default_signal_handlers()
    //    Add call here when the signal_handler module is integrated.

    // 3. Platform init — placeholder for WSAStartup / platform_initialize()
    //    socket.cpp already self-initialises Winsock via ensure_winsock().

    init_count_.store(1, std::memory_order_release);
    ATLAS_LOG_INFO("Atlas Runtime initialized");
    return {};
}

void Runtime::finalize()
{
    std::lock_guard lock(mutex_);

    const uint32_t previous = init_count_.load(std::memory_order_acquire);
    if (previous == 0)
        return;

    if (previous > 1)
    {
        init_count_.store(previous - 1, std::memory_order_release);
        return;
    }

    init_count_.store(0, std::memory_order_release);

    ATLAS_LOG_INFO("Atlas Runtime finalized");
    Logger::instance().clear_sinks();
}

}  // namespace atlas
