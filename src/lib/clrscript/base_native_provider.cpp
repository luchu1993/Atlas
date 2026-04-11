#include "clrscript/base_native_provider.hpp"

#include "entitydef/entity_def_registry.hpp"
#include "foundation/log.hpp"

#include <cstddef>
#include <string_view>

namespace atlas
{

void BaseNativeProvider::log_message(int32_t level, const char* msg, int32_t len)
{
    // Guard against bad inputs from across the C#/C++ boundary.
    if (msg == nullptr || len <= 0)
        return;

    std::string_view message(msg, static_cast<std::size_t>(len));
    auto log_level = static_cast<LogLevel>(level);
    // Call Logger directly rather than expanding 6 ATLAS_LOG_* macros.
    // Each macro captures source_location and does compile-time level checks
    // that are meaningless for C#-originated messages.  Direct Logger::log()
    // is one call with a single runtime level check, and adds a "clr" category
    // to distinguish managed-side log output in filtered views.
    auto& logger = Logger::instance();
    if (static_cast<uint8_t>(log_level) >= static_cast<uint8_t>(logger.level()))
        logger.log(log_level, "clr", message);
}

double BaseNativeProvider::server_time()
{
    return 0.0;
}

float BaseNativeProvider::delta_time()
{
    return 0.0f;
}

uint8_t BaseNativeProvider::get_process_prefix()
{
    ATLAS_LOG_ERROR("get_process_prefix() not implemented for this process type");
    return 0;
}

void BaseNativeProvider::send_client_rpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                         uint8_t /*target*/, const std::byte* /*payload*/,
                                         int32_t /*len*/)
{
    ATLAS_LOG_ERROR(
        "send_client_rpc() not supported on this process type "
        "(entity_id={})",
        entity_id);
}

void BaseNativeProvider::send_cell_rpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                       const std::byte* /*payload*/, int32_t /*len*/)
{
    ATLAS_LOG_ERROR(
        "send_cell_rpc() not supported on this process type "
        "(entity_id={})",
        entity_id);
}

void BaseNativeProvider::send_base_rpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                       const std::byte* /*payload*/, int32_t /*len*/)
{
    ATLAS_LOG_ERROR(
        "send_base_rpc() not supported on this process type "
        "(entity_id={})",
        entity_id);
}

void BaseNativeProvider::register_entity_type(const std::byte* data, int32_t len)
{
    EntityDefRegistry::instance().register_type(data, len);
}

void BaseNativeProvider::unregister_all_entity_types()
{
    EntityDefRegistry::instance().clear();
}

}  // namespace atlas
