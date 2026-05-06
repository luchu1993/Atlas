#include "client_app.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <thread>

#include "baseapp/baseapp_messages.h"
#include "clrscript/clr_bootstrap.h"
#include "clrscript/clr_invoke.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "network/channel.h"
#include "network/reliable_udp.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

namespace atlas {

namespace {

auto ParseUint32Arg(std::string_view sv, uint32_t& out) -> bool {
  try {
    out = static_cast<uint32_t>(std::stoul(std::string(sv)));
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

auto ClientApp::Run(int argc, char* argv[]) -> int {
  ClientApp app;
  if (!app.Init(argc, argv)) return 1;
  auto rc = app.MainLoop();
  app.Fini();
  return rc;
}

ClientApp::ClientApp() = default;
ClientApp::~ClientApp() = default;

auto ClientApp::Init(int argc, char* argv[]) -> bool {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view(argv[++i]) : "";
    };

    if (arg == "--loginapp-host") {
      config_.loginapp_host = std::string(next());
    } else if (arg == "--loginapp-port") {
      config_.loginapp_port = static_cast<uint16_t>(std::stoul(std::string(next())));
    } else if (arg == "--username") {
      config_.username = std::string(next());
    } else if (arg == "--password") {
      config_.password_hash = std::string(next());
    } else if (arg == "--assembly") {
      config_.script_assembly = std::filesystem::path(std::string(next()));
    } else if (arg == "--runtime-config") {
      config_.runtime_config = std::filesystem::path(std::string(next()));
    } else if (arg == "--drop-inbound-ms") {
      auto start_sv = next();
      auto duration_sv = next();
      if (!ParseUint32Arg(start_sv, config_.drop_inbound_start_ms) ||
          !ParseUint32Arg(duration_sv, config_.drop_inbound_duration_ms)) {
        ATLAS_LOG_ERROR("Client: --drop-inbound-ms requires two unsigned integers (got '{}', '{}')",
                        start_sv, duration_sv);
        return false;
      }
    } else if (arg == "--drop-transport-ms") {
      auto start_sv = next();
      auto duration_sv = next();
      if (!ParseUint32Arg(start_sv, config_.drop_transport_start_ms) ||
          !ParseUint32Arg(duration_sv, config_.drop_transport_duration_ms)) {
        ATLAS_LOG_ERROR(
            "Client: --drop-transport-ms requires two unsigned integers (got '{}', '{}')", start_sv,
            duration_sv);
        return false;
      }
    }
  }

  if (config_.password_hash.empty()) {
    ATLAS_LOG_ERROR("Client: --password is required");
    return false;
  }

  // Internet profile matches the small MTU (470) that LoginApp / BaseApp
  // advertise via their accept_profile.
  auto listen_result =
      network_.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::InternetRudpProfile());
  if (!listen_result) {
    ATLAS_LOG_ERROR("Client: failed to start RUDP listener: {}", listen_result.Error().Message());
    return false;
  }

  dispatcher_.SetMaxPollWait(std::chrono::milliseconds(1));

  ATLAS_LOG_INFO("Client: initialized");

  if (!config_.script_assembly.empty()) {
    // Auto-locate the sibling runtimeconfig.json `dotnet build` emits when
    // GenerateRuntimeConfigurationFiles=true, so callers can skip the flag.
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

  auto clr = std::make_unique<ClrScriptEngine>();
  ClrScriptEngine::Config clr_config;
  clr_config.runtime_config_path = config_.runtime_config;
  clr_config.runtime_assembly_path = config_.script_assembly;
  clr_config.bootstrap_args = bootstrap_args;
  // Lifecycle lives in Atlas.Client.Desktop, which is a transitive reference of
  // the script assembly — fully-qualify so the bind crosses assemblies cleanly.
  clr_config.lifecycle_type = "Atlas.Client.DesktopLifecycle, Atlas.Client.Desktop";
  clr_config.hotreload_type.clear();

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

  // Bootstrap fills ClientHost slots before the user assembly's
  // [ModuleInitializer] fires; otherwise TypeRegistryEmitter hits a NRE.
  if (auto r = InvokeDesktopBootstrap(); !r) {
    ATLAS_LOG_ERROR("Client: DesktopBootstrap.Initialize failed: {}", r.Error().Message());
    return false;
  }

  if (auto r = LoadUserAssembly(config_.script_assembly); !r) {
    ATLAS_LOG_ERROR("Client: LoadUserAssembly({}) failed: {}", config_.script_assembly.string(),
                    r.Error().Message());
    return false;
  }

  script_engine_->OnInit(false);

  ATLAS_LOG_INFO("Client: CLR initialized, assembly loaded");
  return true;
}

auto ClientApp::InvokeDesktopBootstrap() -> Result<void> {
  ClrFallibleMethod<> init_method;
  auto bind_result =
      init_method.Bind(script_engine_->Host(), config_.script_assembly,
                       "Atlas.Client.DesktopBootstrap, Atlas.Client.Desktop", "Initialize");
  if (!bind_result) return bind_result.Error();
  auto r = init_method.Invoke();
  if (!r) return r.Error();
  return {};
}

auto ClientApp::LoadUserAssembly(const std::filesystem::path& path) -> Result<void> {
  ClrFallibleMethod<const uint8_t*, int32_t> load_method;
  auto bind_result =
      load_method.Bind(script_engine_->Host(), path,
                       "Atlas.Client.DesktopBootstrap, Atlas.Client.Desktop", "LoadUserAssembly");
  if (!bind_result) return bind_result.Error();

  const auto path_str = path.u8string();
  auto r = load_method.Invoke(reinterpret_cast<const uint8_t*>(path_str.data()),
                              static_cast<int32_t>(path_str.size()));
  if (!r) return r.Error();
  ATLAS_LOG_INFO("Client: loaded user assembly {}", path.string());
  return {};
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

void ClientApp::Fini() {
  FiniClr();
  ATLAS_LOG_INFO("Client: finalized");
}

auto ClientApp::Login() -> bool {
  ATLAS_LOG_INFO("Client: connecting to LoginApp at {}:{}", config_.loginapp_host,
                 config_.loginapp_port);

  auto login_addr = Address(config_.loginapp_host, config_.loginapp_port);
  auto ch_result = network_.ConnectRudp(login_addr, NetworkInterface::InternetRudpProfile());
  if (!ch_result) {
    ATLAS_LOG_ERROR("Client: failed to connect to LoginApp: {}", ch_result.Error().Message());
    return false;
  }
  auto* login_ch = *ch_result;

  // Digest is pushed into the native registry by the user assembly's module
  // initializer; BaseApp rejects login on def_mismatch otherwise.
  login::LoginRequest req;
  req.username = config_.username;
  req.password_hash = config_.password_hash;
  const auto digest = EntityDefRegistry::Instance().Digest();
  if (digest.size() == req.entity_def_digest.size()) {
    std::memcpy(req.entity_def_digest.data(), digest.data(), req.entity_def_digest.size());
  } else {
    ATLAS_LOG_WARNING("Client: entity-def digest size mismatch ({} vs {}); login will likely fail",
                      digest.size(), req.entity_def_digest.size());
  }
  (void)login_ch->SendMessage(req);

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

  return Authenticate(login_result->baseapp_addr, login_result->session_key);
}

auto ClientApp::Authenticate(const Address& baseapp_addr, const SessionKey& session_key) -> bool {
  auto ch_result = network_.ConnectRudp(baseapp_addr, NetworkInterface::InternetRudpProfile());
  if (!ch_result) {
    ATLAS_LOG_ERROR("Client: failed to connect to BaseApp: {}", ch_result.Error().Message());
    return false;
  }
  baseapp_channel_ = *ch_result;

  baseapp::Authenticate auth_msg;
  auth_msg.session_key = session_key;
  (void)baseapp_channel_->SendMessage(auth_msg);

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

  if (native_provider_ && native_provider_->CreateEntityFn()) {
    native_provider_->CreateEntityFn()(player_entity_id_, player_type_id_);
  }

  return true;
}

void ClientApp::OnRpcMessage(uint32_t rpc_id, uint64_t trace_id, const std::byte* payload,
                             int32_t len) {
  if (!native_provider_ || !native_provider_->DispatchRpcFn()) {
    ATLAS_LOG_WARNING("Client: received RPC but no dispatcher registered");
    return;
  }
  native_provider_->DispatchRpcFn()(player_entity_id_, rpc_id,
                                    reinterpret_cast<const uint8_t*>(payload), len, trace_id);
}

void ClientApp::RegisterMessageHandlers() {
  // Typed entries are required for fixed-length BaseApp→Client messages —
  // untyped dispatch misreads them as packed-int length prefixes.
  network_.InterfaceTable().RegisterTypedHandler<baseapp::EntityTransferred>(
      [this](const Address&, Channel*, const baseapp::EntityTransferred& msg) {
        player_entity_id_ = msg.new_entity_id;
        player_type_id_ = msg.new_type_id;
        // Spawn the matching client-side entity so owner-scope deltas land on
        // a script instance — Witness never ships kEntityEnter for self.
        if (native_provider_ && native_provider_->CreateEntityFn()) {
          native_provider_->CreateEntityFn()(msg.new_entity_id, msg.new_type_id);
        }
      });
  network_.InterfaceTable().RegisterTypedHandler<baseapp::CellReady>(
      [](const Address&, Channel*, const baseapp::CellReady&) {});

  // 0xF001/2/3 — opaque state-replication forward; 0xF004 — RPC envelope
  // [u32 rpc_id][u64 trace_id][args]; anything else logs and drops.
  network_.InterfaceTable().SetDefaultHandler(
      [this](const Address&, Channel*, MessageID msg_id, BinaryReader& reader) {
        const bool is_state_channel = msg_id == baseapp::kClientDeltaMessageId ||
                                      msg_id == baseapp::kClientBaselineMessageId ||
                                      msg_id == baseapp::kClientReliableDeltaMessageId;

        if (is_state_channel) {
          if (config_.drop_inbound_duration_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - loop_start_)
                                     .count();
            if (elapsed >= config_.drop_inbound_start_ms &&
                elapsed < config_.drop_inbound_start_ms + config_.drop_inbound_duration_ms) {
              return;
            }
          }
          const auto rem = reader.Remaining();
          auto bytes = reader.ReadBytes(rem);
          if (!bytes) return;
          if (native_provider_ && native_provider_->DeliverFromServerFn()) {
            native_provider_->DeliverFromServerFn()(msg_id,
                                                    reinterpret_cast<const uint8_t*>(bytes->data()),
                                                    static_cast<int32_t>(bytes->size()));
          } else {
            ATLAS_LOG_WARNING(
                "Client: state-channel message 0x{:04X} arrived but no deliver_from_server "
                "callback registered",
                static_cast<unsigned>(msg_id));
          }
          return;
        }

        if (msg_id == baseapp::kClientRpcMessageId) {
          auto rpc_id = reader.Read<uint32_t>();
          auto trace_id = reader.Read<uint64_t>();
          if (!rpc_id || !trace_id) {
            ATLAS_LOG_WARNING("Client: RPC envelope header truncated");
            return;
          }
          const auto rem = reader.Remaining();
          auto args = reader.ReadBytes(rem);
          if (!args) return;
          OnRpcMessage(*rpc_id, *trace_id, args->data(), static_cast<int32_t>(args->size()));
          return;
        }

        ATLAS_LOG_WARNING("Client: unknown MessageID 0x{:04X} ({} bytes payload)",
                          static_cast<unsigned>(msg_id), reader.Remaining());
      });
}

auto ClientApp::MainLoop() -> int {
  if (!Login()) return 1;

  RegisterMessageHandlers();

  ATLAS_LOG_INFO("Client: entering main loop (press Ctrl+C to exit)");

  loop_start_ = std::chrono::steady_clock::now();
  if (config_.drop_inbound_duration_ms > 0) {
    ATLAS_LOG_WARNING(
        "Client: --drop-inbound-ms active: state-channel messages received in "
        "[{} ms, {} ms) after MainLoop entry will be dropped (test mode)",
        config_.drop_inbound_start_ms,
        config_.drop_inbound_start_ms + config_.drop_inbound_duration_ms);
  }
  if (config_.drop_transport_duration_ms > 0) {
    ATLAS_LOG_WARNING(
        "Client: --drop-transport-ms active: every inbound datagram in "
        "[{} ms, {} ms) after MainLoop entry is dropped at the RUDP layer",
        config_.drop_transport_start_ms,
        config_.drop_transport_start_ms + config_.drop_transport_duration_ms);
    if (auto* rudp = dynamic_cast<ReliableUdpChannel*>(baseapp_channel_)) {
      rudp->SetInboundDropWindow(loop_start_, config_.drop_transport_start_ms,
                                 config_.drop_transport_duration_ms);
    } else {
      ATLAS_LOG_ERROR(
          "Client: --drop-transport-ms set but baseapp_channel_ is not a "
          "ReliableUdpChannel — drop window not installed");
    }
  }

  constexpr auto kTickPeriod = std::chrono::milliseconds(16);
  auto last_tick = std::chrono::steady_clock::now();

  while (!shutdown_requested_) {
    dispatcher_.ProcessOnce();

    if (script_engine_) {
      const auto now = std::chrono::steady_clock::now();
      const auto dt = std::chrono::duration<float>(now - last_tick).count();
      last_tick = now;
      script_engine_->OnTick(dt);
    }

    std::this_thread::sleep_for(kTickPeriod);
  }

  return 0;
}

}  // namespace atlas
