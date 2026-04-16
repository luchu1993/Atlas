#ifndef ATLAS_CLIENT_CLIENT_APP_H_
#define ATLAS_CLIENT_CLIENT_APP_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "client_native_provider.h"
#include "clrscript/clr_script_engine.h"
#include "foundation/clock.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "platform/dynamic_library.h"
#include "server/entity_types.h"

namespace atlas {

// ============================================================================
// ClientApp — standalone client console application
//
// Does NOT inherit from ServerApp (no machined registration needed).
// Embeds CoreCLR to load Atlas.Client C# assembly, connects to LoginApp/BaseApp
// via RUDP, and dispatches RPCs between C++ network and C# entity layer.
// ============================================================================

class ClientApp {
 public:
  struct Config {
    std::string loginapp_host{"127.0.0.1"};
    uint16_t loginapp_port{20018};
    std::string username{"testuser"};
    std::string password_hash;
    std::filesystem::path script_assembly;
    std::filesystem::path runtime_config;
  };

  static auto Run(int argc, char* argv[]) -> int;

  ClientApp();
  ~ClientApp();

  // Accessors for ClientNativeProvider
  [[nodiscard]] auto baseapp_channel() -> Channel* { return baseapp_channel_; }

 private:
  auto Init(int argc, char* argv[]) -> bool;
  void Fini();
  auto MainLoop() -> int;

  // CLR embedding
  auto InitClr(const char* exe_path) -> bool;
  void FiniClr();

  // Login flow
  auto Login() -> bool;
  auto Authenticate(const Address& baseapp_addr, const SessionKey& session_key) -> bool;

  // RPC handling — dispatch incoming messages from BaseApp
  void OnRpcMessage(uint32_t rpc_id, const std::byte* payload, int32_t len);

  Config config_;
  EventDispatcher dispatcher_{"client"};
  NetworkInterface network_{dispatcher_};

  // CLR
  std::optional<DynamicLibrary> native_api_library_;
  std::unique_ptr<ClrScriptEngine> script_engine_;
  std::unique_ptr<ClientNativeProvider> native_provider_;

  // Connection state
  Channel* baseapp_channel_{nullptr};
  EntityID player_entity_id_{kInvalidEntityID};
  uint16_t player_type_id_{0};
  bool authenticated_{false};
  bool shutdown_requested_{false};
};

}  // namespace atlas

#endif  // ATLAS_CLIENT_CLIENT_APP_H_
