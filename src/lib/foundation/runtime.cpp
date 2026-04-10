#include "foundation/runtime.hpp"

#include "foundation/log.hpp"
#include "foundation/log_sinks.hpp"

namespace atlas
{

std::atomic<bool> Runtime::initialized_{false};

auto Runtime::initialize(const RuntimeConfig& config) -> Result<void>
{
    if (initialized_.exchange(true, std::memory_order_acq_rel))
    {
        return Error{ErrorCode::AlreadyExists, "Runtime already initialized"};
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

    ATLAS_LOG_INFO("Atlas Runtime initialized");
    return {};
}

void Runtime::finalize()
{
    if (!initialized_.exchange(false, std::memory_order_acq_rel))
        return;

    ATLAS_LOG_INFO("Atlas Runtime finalized");
    Logger::instance().clear_sinks();
}

}  // namespace atlas
