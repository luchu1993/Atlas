#include "net_client/client_api.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "foundation/log.h"
#include "net_client/client_session.h"

namespace {

thread_local std::string g_global_last_error;
std::atomic<AtlasLogFn> g_log_handler{nullptr};

class CallbackLogSink final : public atlas::LogSink {
 public:
  void Write(atlas::LogLevel level, std::string_view /*category*/, std::string_view message,
             const std::source_location& /*location*/) override {
    AtlasLogFn fn = g_log_handler.load(std::memory_order_acquire);
    if (!fn) return;
    fn(static_cast<int32_t>(level), message.data(), static_cast<int32_t>(message.size()));
  }
  void Flush() override {}
};

void EnsureLogSinkInstalled() {
  static std::once_flag once;
  std::call_once(once,
                 [] { atlas::Logger::Instance().AddSink(std::make_shared<CallbackLogSink>()); });
}

auto AbiCompatible(uint32_t expected) -> bool {
  constexpr uint32_t kOur = ATLAS_NET_ABI_VERSION;
  const uint32_t our_major = (kOur >> 24) & 0xFFu;
  const uint32_t our_minor = (kOur >> 16) & 0xFFu;
  const uint32_t exp_major = (expected >> 24) & 0xFFu;
  const uint32_t exp_minor = (expected >> 16) & 0xFFu;
  return exp_major == our_major && exp_minor <= our_minor;
}

}  // namespace

extern "C" {

uint32_t AtlasNetGetAbiVersion(void) {
  return ATLAS_NET_ABI_VERSION;
}

const char* AtlasNetLastError(AtlasNetContext* ctx) {
  if (!ctx) return AtlasNetGlobalLastError();
  const auto& err = ctx->LastError();
  return err.empty() ? "" : err.c_str();
}

const char* AtlasNetGlobalLastError(void) {
  return g_global_last_error.empty() ? "" : g_global_last_error.c_str();
}

AtlasNetContext* AtlasNetCreate(uint32_t expected_abi) {
  if (!AbiCompatible(expected_abi)) {
    g_global_last_error = "ABI version mismatch";
    ATLAS_LOG_ERROR("AtlasNetCreate: ABI mismatch (caller=0x{:08x}, dll=0x{:08x})", expected_abi,
                    static_cast<uint32_t>(ATLAS_NET_ABI_VERSION));
    return nullptr;
  }

  AtlasNetContext* ctx = nullptr;
  try {
    ctx = new AtlasNetContext{};
  } catch (const std::bad_alloc&) {
    g_global_last_error = "ctx allocation failed";
    return nullptr;
  }

  AtlasNetCallbacks empty_table{};
  (void)ctx->SetCallbacks(empty_table);
  return ctx;
}

void AtlasNetDestroy(AtlasNetContext* ctx) {
  if (!ctx) return;
  ctx->Disconnect(ATLAS_DISCONNECT_INTERNAL);
  delete ctx;
}

int32_t AtlasNetPoll(AtlasNetContext* ctx) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->Poll();
}

AtlasNetState AtlasNetGetState(AtlasNetContext* ctx) {
  if (!ctx) return ATLAS_NET_STATE_DISCONNECTED;
  return ctx->GetState();
}

int32_t AtlasNetLogin(AtlasNetContext* ctx, const char* loginapp_host, uint16_t loginapp_port,
                      const char* username, const char* password_hash, AtlasLoginResultFn callback,
                      void* user_data) {
  if (!ctx || !loginapp_host || !username || !password_hash) {
    return ATLAS_NET_ERR_INVAL;
  }
  return ctx->StartLogin(loginapp_host, loginapp_port, username, password_hash, callback,
                         user_data);
}

int32_t AtlasNetAuthenticate(AtlasNetContext* ctx, AtlasAuthResultFn callback, void* user_data) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->StartAuthenticate(callback, user_data);
}

int32_t AtlasNetDisconnect(AtlasNetContext* ctx, AtlasDisconnectReason reason) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->Disconnect(reason);
}

int32_t AtlasNetSendBaseRpc(AtlasNetContext* ctx, uint32_t entity_id, uint32_t rpc_id,
                            const uint8_t* payload, int32_t len) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SendBaseRpc(entity_id, rpc_id, payload, len);
}

int32_t AtlasNetSendCellRpc(AtlasNetContext* ctx, uint32_t entity_id, uint32_t rpc_id,
                            const uint8_t* payload, int32_t len) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SendCellRpc(entity_id, rpc_id, payload, len);
}

int32_t AtlasNetSetCallbacks(AtlasNetContext* ctx, const AtlasNetCallbacks* callbacks) {
  if (!ctx || !callbacks) return ATLAS_NET_ERR_INVAL;
  return ctx->SetCallbacks(*callbacks);
}

void AtlasNetSetLogHandler(AtlasLogFn handler) {
  g_log_handler.store(handler, std::memory_order_release);
  if (handler) EnsureLogSinkInstalled();
}

int32_t AtlasNetGetStats(AtlasNetContext* ctx, AtlasNetStats* out_stats) {
  if (!ctx || !out_stats) return ATLAS_NET_ERR_INVAL;
  return ctx->FillStats(out_stats);
}

}  // extern "C"
