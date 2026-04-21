#include "client_app.h"

#include "clrscript/clr_bootstrap.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/reliable_udp.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

// Reuse login/baseapp message definitions
#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <thread>

#include "baseapp/baseapp_messages.h"
#include "baseapp/delta_forwarder.h"
#include "loginapp/login_messages.h"

namespace atlas {

// ============================================================================
// Static entry point
// ============================================================================

auto ClientApp::Run(int argc, char* argv[]) -> int {
  ClientApp app;
  if (!app.Init(argc, argv)) return 1;
  auto rc = app.MainLoop();
  app.Fini();
  return rc;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

ClientApp::ClientApp() = default;
ClientApp::~ClientApp() = default;

// ============================================================================
// Init
// ============================================================================

auto ClientApp::Init(int argc, char* argv[]) -> bool {
  // Parse command-line arguments
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view(argv[++i]) : "";
    };

    if (arg == "--loginapp-host")
      config_.loginapp_host = std::string(next());
    else if (arg == "--loginapp-port")
      config_.loginapp_port = static_cast<uint16_t>(std::stoul(std::string(next())));
    else if (arg == "--username")
      config_.username = std::string(next());
    else if (arg == "--password")
      config_.password_hash = std::string(next());
    else if (arg == "--assembly")
      config_.script_assembly = std::filesystem::path(std::string(next()));
    else if (arg == "--runtime-config")
      config_.runtime_config = std::filesystem::path(std::string(next()));
    else if (arg == "--drop-inbound-ms") {
      // Two-argument flag: start_ms duration_ms.
      auto start_sv = next();
      auto duration_sv = next();
      try {
        config_.drop_inbound_start_ms = std::stoi(std::string(start_sv));
        config_.drop_inbound_duration_ms = std::stoi(std::string(duration_sv));
      } catch (...) {
        ATLAS_LOG_ERROR("Client: --drop-inbound-ms requires two integer args (got '{}', '{}')",
                        start_sv, duration_sv);
        return false;
      }
    }
  }

  // Default password to empty hash
  if (config_.password_hash.empty()) config_.password_hash = "default_hash";

  // Start RUDP server (for receiving replies)
  auto listen_result = network_.StartRudpServer(Address("127.0.0.1", 0));
  if (!listen_result) {
    ATLAS_LOG_ERROR("Client: failed to start RUDP listener: {}", listen_result.Error().Message());
    return false;
  }

  // Configure fast polling
  dispatcher_.SetMaxPollWait(std::chrono::milliseconds(1));

  ATLAS_LOG_INFO("Client: initialized");

  // Init CLR if assembly is specified. If --runtime-config wasn't supplied,
  // auto-locate `<AssemblyStem>.runtimeconfig.json` next to the assembly —
  // `dotnet build` emits it when GenerateRuntimeConfigurationFiles=true,
  // so the typical build path has one available even for library projects.
  if (!config_.script_assembly.empty()) {
    if (config_.runtime_config.empty()) {
      auto sibling = config_.script_assembly.parent_path() /
                     (config_.script_assembly.stem().string() + ".runtimeconfig.json");
      if (std::filesystem::exists(sibling)) {
        config_.runtime_config = sibling;
        ATLAS_LOG_INFO("Client: auto-located runtime config {}", sibling.string());
      }
    }
    if (!InitClr(argv[0])) return false;
  }

  return true;
}

// ============================================================================
// CLR initialization — mirrors ScriptApp pattern
// ============================================================================

auto ClientApp::InitClr(const char* exe_path) -> bool {
  native_provider_ = std::make_unique<ClientNativeProvider>(*this);
  SetNativeApiProvider(native_provider_.get());

  using SetNativeApiProviderFn = void (*)(void*);
  using GetClrBridgeFn = void* (*)();

#if ATLAS_PLATFORM_WINDOWS
  constexpr auto kModuleName = "atlas_engine.dll";
#else
  constexpr auto kModuleName = "libatlas_engine.so";
#endif

  auto module_path = std::filesystem::absolute(exe_path).parent_path() / kModuleName;
  auto lib_result = DynamicLibrary::Load(module_path);
  if (!lib_result) {
    ATLAS_LOG_ERROR("Client: failed to load {}: {}", module_path.string(),
                    lib_result.Error().Message());
    return false;
  }
  native_api_library_ = std::move(*lib_result);

  auto set_provider =
      native_api_library_->GetSymbol<SetNativeApiProviderFn>("AtlasSetNativeApiProvider");
  if (!set_provider) {
    ATLAS_LOG_ERROR("Client: failed to resolve AtlasSetNativeApiProvider");
    return false;
  }
  (*set_provider)(native_provider_.get());

  // Resolve CLR error bridge
  auto error_set = native_api_library_->GetSymbol<GetClrBridgeFn>("AtlasGetClrErrorSetFn");
  auto error_clear = native_api_library_->GetSymbol<GetClrBridgeFn>("AtlasGetClrErrorClearFn");
  auto error_code = native_api_library_->GetSymbol<GetClrBridgeFn>("AtlasGetClrErrorGetCodeFn");
  if (!error_set || !error_clear || !error_code) {
    ATLAS_LOG_ERROR("Client: failed to resolve CLR error bridge exports");
    return false;
  }

  ClrBootstrapArgs bootstrap_args;
  bootstrap_args.error_set = reinterpret_cast<decltype(bootstrap_args.error_set)>((*error_set)());
  bootstrap_args.error_clear =
      reinterpret_cast<decltype(bootstrap_args.error_clear)>((*error_clear)());
  bootstrap_args.error_get_code =
      reinterpret_cast<decltype(bootstrap_args.error_get_code)>((*error_code)());

  // Create CLR engine
  auto clr = std::make_unique<ClrScriptEngine>();
  ClrScriptEngine::Config clr_config;
  clr_config.runtime_config_path = config_.runtime_config;
  clr_config.runtime_assembly_path = config_.script_assembly;
  clr_config.bootstrap_args = bootstrap_args;

  auto cfg_result = clr->Configure(clr_config);
  if (!cfg_result) {
    ATLAS_LOG_ERROR("Client: CLR configure failed: {}", cfg_result.Error().Message());
    return false;
  }
  script_engine_ = std::move(clr);

  auto init_result = script_engine_->Initialize();
  if (!init_result) {
    ATLAS_LOG_ERROR("Client: CLR initialize failed: {}", init_result.Error().Message());
    return false;
  }

  auto load_result = script_engine_->LoadModule(config_.script_assembly);
  if (!load_result) {
    ATLAS_LOG_ERROR("Client: load_module({}) failed: {}", config_.script_assembly.string(),
                    load_result.Error().Message());
    return false;
  }

  script_engine_->OnInit(false);

  ATLAS_LOG_INFO("Client: CLR initialized, assembly loaded");
  return true;
}

void ClientApp::FiniClr() {
  using SetNativeApiProviderFn = void (*)(void*);

  if (script_engine_) {
    script_engine_->OnShutdown();
    script_engine_->Finalize();
    script_engine_.reset();
  }

  if (native_api_library_) {
    auto set_provider =
        native_api_library_->GetSymbol<SetNativeApiProviderFn>("AtlasSetNativeApiProvider");
    if (set_provider) (*set_provider)(nullptr);
    native_api_library_.reset();
  }

  SetNativeApiProvider(nullptr);
  native_provider_.reset();
}

// ============================================================================
// Fini
// ============================================================================

void ClientApp::Fini() {
  FiniClr();
  ATLAS_LOG_INFO("Client: finalized");
}

// ============================================================================
// Login flow
// ============================================================================

auto ClientApp::Login() -> bool {
  ATLAS_LOG_INFO("Client: connecting to LoginApp at {}:{}", config_.loginapp_host,
                 config_.loginapp_port);

  auto login_addr = Address(config_.loginapp_host, config_.loginapp_port);
  auto ch_result = network_.ConnectRudp(login_addr);
  if (!ch_result) {
    ATLAS_LOG_ERROR("Client: failed to connect to LoginApp: {}", ch_result.Error().Message());
    return false;
  }
  auto* login_ch = *ch_result;

  // Send LoginRequest
  login::LoginRequest req;
  req.username = config_.username;
  req.password_hash = config_.password_hash;
  (void)login_ch->SendMessage(req);

  // Wait for LoginResult
  std::optional<login::LoginResult> login_result;
  network_.InterfaceTable().RegisterTypedHandler<login::LoginResult>(
      [&](const Address&, Channel*, const login::LoginResult& msg) { login_result = msg; });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!login_result && std::chrono::steady_clock::now() < deadline) {
    dispatcher_.ProcessOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  if (!login_result || login_result->status != login::LoginStatus::kSuccess) {
    ATLAS_LOG_ERROR("Client: login failed: {}",
                    login_result ? login_result->error_message : "timeout");
    return false;
  }

  ATLAS_LOG_INFO("Client: login succeeded, BaseApp at {}", login_result->baseapp_addr.ToString());

  // Authenticate on BaseApp
  return Authenticate(login_result->baseapp_addr, login_result->session_key);
}

auto ClientApp::Authenticate(const Address& baseapp_addr, const SessionKey& session_key) -> bool {
  auto ch_result = network_.ConnectRudp(baseapp_addr);
  if (!ch_result) {
    ATLAS_LOG_ERROR("Client: failed to connect to BaseApp: {}", ch_result.Error().Message());
    return false;
  }
  baseapp_channel_ = *ch_result;

  // Send Authenticate
  baseapp::Authenticate auth_msg;
  auth_msg.session_key = session_key;
  (void)baseapp_channel_->SendMessage(auth_msg);

  // Wait for AuthenticateResult
  std::optional<std::tuple<bool, EntityID, uint16_t>> auth_result;
  network_.InterfaceTable().RegisterTypedHandler<baseapp::AuthenticateResult>(
      [&](const Address&, Channel*, const baseapp::AuthenticateResult& msg) {
        auth_result = std::make_tuple(msg.success, msg.entity_id, msg.type_id);
      });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!auth_result && std::chrono::steady_clock::now() < deadline) {
    dispatcher_.ProcessOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  if (!auth_result || !std::get<0>(*auth_result)) {
    ATLAS_LOG_ERROR("Client: authentication failed");
    return false;
  }

  player_entity_id_ = std::get<1>(*auth_result);
  player_type_id_ = std::get<2>(*auth_result);
  authenticated_ = true;

  ATLAS_LOG_INFO("Client: authenticated as entity={} type={}", player_entity_id_, player_type_id_);

  // Create the client-side entity via C# callback
  if (native_provider_ && native_provider_->CreateEntityFn()) {
    native_provider_->CreateEntityFn()(player_entity_id_, player_type_id_);
  }

  return true;
}

// ============================================================================
// RPC message handling
// ============================================================================

void ClientApp::OnRpcMessage(uint32_t rpc_id, const std::byte* payload, int32_t len) {
  if (!native_provider_ || !native_provider_->DispatchRpcFn()) {
    ATLAS_LOG_WARNING("Client: received RPC but no dispatcher registered");
    return;
  }
  native_provider_->DispatchRpcFn()(player_entity_id_, rpc_id,
                                    reinterpret_cast<const uint8_t*>(payload), len);
}

// ============================================================================
// Main loop
// ============================================================================

auto ClientApp::MainLoop() -> int {
  // Login first
  if (!Login()) return 1;

  ATLAS_LOG_INFO("Client: entering main loop (press Ctrl+C to exit)");

  loop_start_ = std::chrono::steady_clock::now();
  if (config_.drop_inbound_duration_ms > 0) {
    ATLAS_LOG_WARNING(
        "Client: --drop-inbound-ms active: state-channel messages received in "
        "[{} ms, {} ms) after MainLoop entry will be dropped (test mode)",
        config_.drop_inbound_start_ms,
        config_.drop_inbound_start_ms + config_.drop_inbound_duration_ms);
  }

  // Register a catch-all handler for messages from BaseApp.
  //
  // Two transport-level concerns are multiplexed over this default handler:
  //
  //   (a) The three reserved state-replication channels
  //       (DeltaForwarder::kClientDeltaMessageId          0xF001 — unreliable),
  //       (DeltaForwarder::kClientBaselineMessageId       0xF002 — reliable),
  //       (DeltaForwarder::kClientReliableDeltaMessageId  0xF003 — reliable)
  //       are routed opaquely to the managed script host via
  //       deliver_from_server_fn_. The native layer performs zero envelope
  //       decoding so an alternative script host (Lua, TS, …) can bind the
  //       same transport hook without touching this file.
  //
  //   (b) Any other MessageID is treated as a ClientRpc — BaseApp sends
  //       ClientRpcs using the rpc_id directly as the MessageID.
  network_.InterfaceTable().SetDefaultHandler(
      [this](const Address&, Channel*, MessageID msg_id, BinaryReader& reader) {
        const bool is_state_channel = msg_id == DeltaForwarder::kClientDeltaMessageId ||
                                      msg_id == DeltaForwarder::kClientBaselineMessageId ||
                                      msg_id == DeltaForwarder::kClientReliableDeltaMessageId;

        const uint8_t* payload_ptr = nullptr;
        int32_t payload_len = 0;
        const auto rem = reader.Remaining();
        if (rem > 0) {
          auto read = reader.ReadBytes(rem);
          if (read) {
            payload_ptr = reinterpret_cast<const uint8_t*>(read->data());
            payload_len = static_cast<int32_t>(read->size());
          }
        }

        if (is_state_channel) {
          // Phase C3 test hook: silently drop state-channel traffic inside
          // the [start, start+duration) window to simulate packet loss on
          // the reliable / volatile / baseline channels. RPCs and other
          // traffic flow normally so login, auth and script method calls
          // still work.
          if (config_.drop_inbound_duration_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - loop_start_)
                                     .count();
            if (elapsed >= config_.drop_inbound_start_ms &&
                elapsed < config_.drop_inbound_start_ms + config_.drop_inbound_duration_ms) {
              return;  // dropped
            }
          }
          // MessageID is already uint16_t (see src/lib/network/message.h); the
          // deliver_from_server callback takes uint16_t by value, so no cast is
          // needed.
          if (native_provider_ && native_provider_->DeliverFromServerFn()) {
            native_provider_->DeliverFromServerFn()(msg_id, payload_ptr, payload_len);
          } else {
            ATLAS_LOG_WARNING(
                "Client: state-channel message 0x{:04X} arrived but no deliver_from_server "
                "callback registered",
                static_cast<unsigned>(msg_id));
          }
          return;
        }

        OnRpcMessage(static_cast<uint32_t>(msg_id), reinterpret_cast<const std::byte*>(payload_ptr),
                     payload_len);
      });

  while (!shutdown_requested_) {
    dispatcher_.ProcessOnce();

    // Drive C# tick if CLR is loaded
    if (script_engine_) {
      script_engine_->OnTick(0.016f);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  return 0;
}

}  // namespace atlas
