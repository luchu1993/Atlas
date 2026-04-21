#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "baseapp/baseapp_messages.h"
#include "foundation/clock.h"
#include "loginapp/login_messages.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "script_clients.h"

using namespace atlas;

namespace {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

struct Options {
  Address login_addr{"127.0.0.1", 0};
  std::string username_prefix{"stress_user_"};
  std::string password_hash;
  std::vector<Address> source_ips;
  std::size_t clients{100};
  std::size_t account_pool{0};
  std::size_t account_index_base{0};
  std::size_t ramp_per_sec{100};
  std::size_t worker_index{0};
  std::size_t worker_count{1};
  int duration_sec{60};
  int connect_timeout_ms{20'000};
  int retry_delay_ms{1'000};
  int hold_min_ms{30'000};
  int hold_max_ms{60'000};
  int shortline_pct{20};
  int shortline_min_ms{1'000};
  int shortline_max_ms{5'000};
  // P3: once a session is kOnline and its StressAvatar cell entity is
  // live, it fires an Echo RPC at rpc_rate_hz Hz for the remainder of
  // its hold window. Each Echo round-trip contributes one RTT sample.
  // Values <= 0 disable the periodic stream (first Echo after
  // EntityTransferred still fires, giving one RTT sample per session).
  int rpc_rate_hz{2};
  // P3.2: ReportPos stream rate — writes the StressAvatar.position
  // property via ClientCellRpc → cell method → property setter. Drives
  // the cell-side dirty/replication path at scale. 0 = disabled.
  int move_rate_hz{10};
  // P3.3: number of distinct cell-side spaces to spread avatars across.
  // Each session picks space_id = (session_id % space_count) + 1 and
  // encodes it in the SelectAvatar RPC; server-side Account.SelectAvatar
  // forwards it to CreateBaseEntity. 1 = all avatars in one space (the
  // pre-P3.3 baseline).
  int space_count{1};
  bool verbose_failures{false};
  uint32_t seed{12345};

  // Phase C2: end-to-end verification via real atlas_client.exe subprocesses
  // that load samples/client/. Zero means no script children are launched
  // and the harness retains its original raw-protocol-only behaviour.
  std::size_t script_clients{0};
  std::filesystem::path client_exe;
  std::filesystem::path client_assembly;
  std::filesystem::path client_runtime_config;
  std::string script_username_prefix{"script_user_"};
  bool script_verify{false};
};

enum class SessionState : uint8_t {
  kScheduled,
  kWaitingLogin,
  kWaitingAuth,
  kOnline,
  kCoolingDown,
};

struct Metrics {
  std::size_t login_started{0};
  std::size_t login_result_success{0};
  std::size_t login_result_fail{0};
  std::size_t auth_success{0};
  std::size_t auth_fail{0};
  std::size_t timeout_fail{0};
  std::size_t unexpected_disconnects{0};
  std::size_t planned_disconnects{0};
  // P2.3c: SelectAvatar is fire-and-forget on the wire — there is no
  // direct reply to it in the current protocol. `select_avatar_sent`
  // counts "accepted by the local send queue"; `entity_transferred`
  // counts the engine-level EntityTransferred notifications that arrive
  // after the server-side Account.SelectAvatar runs GiveClientTo (see
  // P2.3e baseapp change). The two should match 1:1 on a healthy run.
  std::size_t select_avatar_sent{0};
  std::size_t select_avatar_fail{0};
  std::size_t entity_transferred{0};
  std::size_t cell_ready{0};
  // P2.3e: Echo / EchoReply round-trip through the Cell-side CLR dispatch.
  // Each EntityTransferred triggers one Echo; a matching EchoReply from
  // CellApp → BaseApp → client updates rtt_ms.
  std::size_t echo_sent{0};
  std::size_t echo_received{0};
  // P3.2: ReportPos is fire-and-forget (no direct reply in the current
  // protocol). Counts "accepted by local send queue".
  std::size_t move_sent{0};
  std::size_t move_fail{0};
  // P4: AoI envelope counts received via BaseApp's reliable-delta relay
  // (wire msg_id 0xF003). First payload byte is CellAoIEnvelopeKind.
  std::size_t aoi_enter{0};
  std::size_t aoi_leave{0};
  std::size_t aoi_pos_update{0};
  std::size_t aoi_prop_update{0};
  std::vector<double> auth_latency_ms;
  std::vector<double> echo_rtt_ms;
  std::unordered_map<std::string, std::size_t> failure_reasons;
};

class Session {
 public:
  Session(std::size_t id, std::string username, EventDispatcher& dispatcher, const Options& opts,
          Metrics& metrics, std::mt19937& rng, std::optional<Address> source_ip)
      : id_(id),
        username_(std::move(username)),
        dispatcher_(dispatcher),
        opts_(opts),
        metrics_(metrics),
        rng_(rng),
        source_ip_(source_ip) {}

  void ScheduleInitial(TimePoint when) {
    state_ = SessionState::kScheduled;
    next_action_at_ = when;
  }

  void Update(TimePoint now) {
    if (restart_requested_) {
      TeardownNetwork();
      state_ = SessionState::kCoolingDown;
      next_action_at_ = restart_at_;
      restart_requested_ = false;
    }

    switch (state_) {
      case SessionState::kScheduled:
      case SessionState::kCoolingDown:
        if (now >= next_action_at_) {
          StartAttempt(now);
        }
        break;
      case SessionState::kWaitingLogin:
      case SessionState::kWaitingAuth:
        if (now >= deadline_at_) {
          ++metrics_.timeout_fail;
          RecordFailure("timeout");
          if (opts_.verbose_failures) {
            std::cout << std::format("[session {}] timed out in state {}\n", id_, StateName());
          }
          RestartAfter(now, opts_.retry_delay_ms);
        }
        break;
      case SessionState::kOnline:
        // Fire any due periodic Echoes. Handles the initial post-transfer
        // Echo (seeded by OnEntityTransferred with a 500 ms delay to let
        // CellEntityCreated propagate) AND the 1/rpc_rate_hz cadence that
        // follows. Loop in case we fell behind (e.g. tick jitter).
        while (echo_pending_ && now >= echo_due_at_) {
          SendEcho();
          if (opts_.rpc_rate_hz > 0) {
            echo_due_at_ += std::chrono::milliseconds(1000 / opts_.rpc_rate_hz);
          } else {
            echo_pending_ = false;  // one-shot mode
          }
        }
        // Same shape for ReportPos at move_rate_hz.
        while (move_pending_ && now >= move_due_at_) {
          SendReportPos();
          move_due_at_ += std::chrono::milliseconds(1000 / opts_.move_rate_hz);
        }
        if (now >= next_action_at_) {
          DisconnectAndRetry(now);
        }
        break;
    }
  }

  [[nodiscard]] auto IsOnline() const -> bool { return state_ == SessionState::kOnline; }
  [[nodiscard]] auto IsInflight() const -> bool {
    return state_ == SessionState::kWaitingLogin || state_ == SessionState::kWaitingAuth;
  }

 private:
  void EnsureNetwork() {
    if (network_) {
      return;
    }

    network_ = std::make_unique<NetworkInterface>(dispatcher_);
    if (source_ip_) {
      network_->SetRudpClientBindAddress(Address(source_ip_->Ip(), 0));
    }
    network_->SetDisconnectCallback([this](Channel& ch) { OnDisconnect(ch.RemoteAddress()); });

    auto& table = network_->InterfaceTable();
    (void)table.RegisterTypedHandler<login::LoginResult>(
        [this](const Address&, Channel*, const login::LoginResult& msg) { OnLoginResult(msg); });
    (void)table.RegisterTypedHandler<baseapp::AuthenticateResult>(
        [this](const Address&, Channel*, const baseapp::AuthenticateResult& msg) {
          OnAuthResult(msg);
        });
    (void)table.RegisterTypedHandler<baseapp::EntityTransferred>(
        [this](const Address&, Channel*, const baseapp::EntityTransferred& msg) {
          OnEntityTransferred(msg);
        });
    (void)table.RegisterTypedHandler<baseapp::CellReady>(
        [this](const Address&, Channel*, const baseapp::CellReady& msg) { OnCellReady(msg); });
    // EchoReply arrives as a raw packet — BaseApp forwards the cell-side
    // SelfRpcFromCell to the client via `SendMessage(static_cast<MessageID>
    // (rpc_id), payload)`, which truncates the 32-bit packed rpc_id to 16
    // bits. There's no typed message struct for it (the id is dynamic per
    // RPC), and Channel::DispatchMessages silently drops messages whose
    // id isn't in the InterfaceTable. Catch it with the pre-dispatch hook
    // instead, which runs *before* the entry check.
    table.SetPreDispatchHook([this](MessageID id, std::span<const std::byte> payload) -> bool {
      return OnRawMessage(id, payload);
    });
  }

  void StartAttempt(TimePoint now) {
    EnsureNetwork();
    const auto kResult = network_->ConnectRudp(opts_.login_addr);
    if (!kResult) {
      ++metrics_.login_result_fail;
      RecordFailure(std::format("login_connect:{}", kResult.Error().Message()));
      RestartAfter(now, opts_.retry_delay_ms);
      return;
    }

    login_channel_ = static_cast<Channel*>(*kResult);

    login::LoginRequest req;
    req.username = username_;
    req.password_hash = opts_.password_hash;

    const auto kSendResult = login_channel_->SendMessage(req);
    if (!kSendResult) {
      ++metrics_.login_result_fail;
      RecordFailure(std::format("login_send:{}", kSendResult.Error().Message()));
      RestartAfter(now, opts_.retry_delay_ms);
      return;
    }

    ++attempt_;
    ++metrics_.login_started;
    started_at_ = now;
    deadline_at_ = now + std::chrono::milliseconds(opts_.connect_timeout_ms);
    state_ = SessionState::kWaitingLogin;
    session_key_ = {};
    baseapp_addr_ = {};
    auth_channel_ = nullptr;
    intentionally_offline_ = false;
  }

  void OnLoginResult(const login::LoginResult& msg) {
    if (state_ != SessionState::kWaitingLogin) {
      return;
    }

    if (msg.status != login::LoginStatus::kSuccess) {
      ++metrics_.login_result_fail;
      RecordFailure(msg.error_message.empty()
                        ? std::format("login_status:{}", static_cast<int>(msg.status))
                        : msg.error_message);
      RequestRestart(SteadyClock::now(), opts_.retry_delay_ms);
      return;
    }

    ++metrics_.login_result_success;
    session_key_ = msg.session_key;
    baseapp_addr_ = msg.baseapp_addr;

    const auto kResult = network_->ConnectRudp(baseapp_addr_);
    if (!kResult) {
      ++metrics_.auth_fail;
      RecordFailure(std::format("baseapp_connect:{}", kResult.Error().Message()));
      RequestRestart(SteadyClock::now(), opts_.retry_delay_ms);
      return;
    }

    auth_channel_ = static_cast<Channel*>(*kResult);

    baseapp::Authenticate auth;
    auth.session_key = session_key_;
    const auto kSendResult = auth_channel_->SendMessage(auth);
    if (!kSendResult) {
      ++metrics_.auth_fail;
      RecordFailure(std::format("auth_send:{}", kSendResult.Error().Message()));
      RequestRestart(SteadyClock::now(), opts_.retry_delay_ms);
      return;
    }

    state_ = SessionState::kWaitingAuth;
    deadline_at_ = SteadyClock::now() + std::chrono::milliseconds(opts_.connect_timeout_ms);
  }

  void OnAuthResult(const baseapp::AuthenticateResult& msg) {
    if (state_ != SessionState::kWaitingAuth) {
      return;
    }

    if (!msg.success) {
      ++metrics_.auth_fail;
      RecordFailure(msg.error.empty() ? "auth_failed" : msg.error);
      RequestRestart(SteadyClock::now(), opts_.retry_delay_ms);
      return;
    }

    ++metrics_.auth_success;
    metrics_.auth_latency_ms.push_back(ElapsedMs(started_at_, SteadyClock::now()));

    state_ = SessionState::kOnline;
    entity_id_ = msg.entity_id;
    intentionally_offline_ = RandomPercent() < opts_.shortline_pct;
    next_action_at_ =
        SteadyClock::now() + std::chrono::milliseconds(
                                 intentionally_offline_
                                     ? RandomBetween(opts_.shortline_min_ms, opts_.shortline_max_ms)
                                     : RandomBetween(opts_.hold_min_ms, opts_.hold_max_ms));

    // P2.3c: fire Account.SelectAvatar(space_id) as a ClientBaseRpc. RPC ID
    // is packed as (direction<<22) | (type_index<<8) | method_index, matching
    // Atlas.Generators.Def/Emitters/RpcIdEmitter.cs. Account's type_index
    // is 1 (first alphabetically among {Account, StressAvatar}); base_methods
    // sort alphabetically to [RequestAvatarList=1, SelectAvatar=2]; direction
    // 3 is the exposed-base-RPC tag. Cf. baseapp.cc OnClientBaseRpc validation.
    // P3.3: payload is int32 space_id (1..space_count) in little-endian.
    // Server-side Account.SelectAvatar forwards this to CreateBaseEntity so
    // the StressAvatar cell entity lands in the selected space.
    constexpr uint32_t kSelectAvatarRpcId = (3u << 22) | (1u << 8) | 2u;
    const int32_t kSpaceCount = opts_.space_count > 0 ? opts_.space_count : 1;
    const int32_t kSpaceId = static_cast<int32_t>(id_ % static_cast<std::size_t>(kSpaceCount)) + 1;
    baseapp::ClientBaseRpc rpc;
    rpc.rpc_id = kSelectAvatarRpcId;
    rpc.payload.resize(sizeof(kSpaceId));
    std::memcpy(rpc.payload.data(), &kSpaceId, sizeof(kSpaceId));
    const auto kRpcSend = auth_channel_->SendMessage(rpc);
    if (kRpcSend) {
      ++metrics_.select_avatar_sent;
    } else {
      ++metrics_.select_avatar_fail;
      RecordFailure(std::format("select_avatar_send:{}", kRpcSend.Error().Message()));
    }
  }

  void OnEntityTransferred(const baseapp::EntityTransferred& msg) {
    // Engine-level handoff notification: our controlling entity has moved
    // from Account → StressAvatar (or equivalent). Update our cached id
    // so subsequent cell RPCs target the new entity. Don't start the
    // Echo / ReportPos streams yet — the cell counterpart may not be
    // bound on BaseApp's Proxy yet, so a ClientCellRpc fired here would
    // be dropped with "no cell channel for target entity". Wait for the
    // matching CellReady instead (engine guarantees it after BaseApp
    // records cell_addr on the Proxy).
    entity_id_ = msg.new_entity_id;
    ++metrics_.entity_transferred;
  }

  void OnCellReady(const baseapp::CellReady& msg) {
    // Server-confirmed: entity `msg.entity_id` now has a live cell side
    // bound to our Proxy on BaseApp. Safe to start the in-world streams.
    if (msg.entity_id != entity_id_) return;  // stale notification for a prior session
    ++metrics_.cell_ready;
    const auto kNow = SteadyClock::now();
    echo_due_at_ = kNow;
    echo_pending_ = true;
    move_due_at_ = kNow;
    move_pending_ = opts_.move_rate_hz > 0;
  }

  void SendEcho() {
    if (!auth_channel_ || entity_id_ == kInvalidEntityID) return;

    // StressAvatar.Echo is a cell_method (exposed own_client). RPC id
    // layout matches Atlas.Generators.Def/Emitters/RpcIdEmitter.cs:
    //   (direction=2 <<22) | (type_index=2 (StressAvatar, 2nd alpha) <<8)
    //   | method_index=1 (Echo sorts before ReportPos)
    //   = 0x00800201
    constexpr uint32_t kEchoRpcId = (2u << 22) | (2u << 8) | 1u;

    const uint32_t seq = next_echo_seq_++;
    const uint64_t client_ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch())
            .count());

    std::vector<std::byte> payload(sizeof(seq) + sizeof(client_ts_ns));
    std::memcpy(payload.data(), &seq, sizeof(seq));
    std::memcpy(payload.data() + sizeof(seq), &client_ts_ns, sizeof(client_ts_ns));

    baseapp::ClientCellRpc rpc;
    rpc.target_entity_id = entity_id_;
    rpc.rpc_id = kEchoRpcId;
    rpc.payload = std::move(payload);
    const auto kSend = auth_channel_->SendMessage(rpc);
    if (kSend) {
      ++metrics_.echo_sent;
    } else {
      RecordFailure(std::format("echo_send:{}", kSend.Error().Message()));
    }
  }

  void SendReportPos() {
    if (!auth_channel_ || entity_id_ == kInvalidEntityID) return;

    // StressAvatar.ReportPos is a cell_method (exposed all_clients).
    //   direction=2 (cell) <<22 | type_index=2 <<8 | method_index=2
    //   (ReportPos sorts after Echo alphabetically) = 0x00800202
    constexpr uint32_t kReportPosRpcId = (2u << 22) | (2u << 8) | 2u;

    // Tiny random-walk in a 100 m square centred at (0,0,0). Absolute
    // values stay well under CellApp's single-tick displacement cap
    // (phase10_cellapp.md §3.12) so AvatarUpdate-style rejects can't
    // confuse this with the teleport path.
    std::uniform_real_distribution<float> walk(-1.f, 1.f);
    pos_x_ = std::clamp(pos_x_ + walk(rng_), -50.f, 50.f);
    pos_z_ = std::clamp(pos_z_ + walk(rng_), -50.f, 50.f);
    const float dx = 1.f, dy = 0.f, dz = 0.f;

    // Payload: Vector3 pos (x,y,z) + Vector3 dir (x,y,z) = 6 * float, LE.
    std::vector<std::byte> payload(6 * sizeof(float));
    const float fs[6] = {pos_x_, 0.f, pos_z_, dx, dy, dz};
    std::memcpy(payload.data(), fs, sizeof(fs));

    baseapp::ClientCellRpc rpc;
    rpc.target_entity_id = entity_id_;
    rpc.rpc_id = kReportPosRpcId;
    rpc.payload = std::move(payload);
    const auto kSend = auth_channel_->SendMessage(rpc);
    if (kSend) {
      ++metrics_.move_sent;
    } else {
      ++metrics_.move_fail;
      RecordFailure(std::format("move_send:{}", kSend.Error().Message()));
    }
  }

  auto OnRawMessage(MessageID id, std::span<const std::byte> payload) -> bool {
    // EchoReply wire msg_id is the cell-direction 0-bits form of
    //   (direction=0 <<22) | (type_index=2 <<8) | method_index=1 = 0x0201
    constexpr MessageID kEchoReplyWireId = 0x0201;
    // DeltaForwarder::kClientReliableDeltaMessageId — BaseApp relays
    // CellApp's ReplicatedReliableDeltaFromCell here. The payload starts
    // with a CellAoIEnvelopeKind byte (enter=1, leave=2, pos=3, prop=4).
    constexpr MessageID kReliableDeltaWireId = 0xF003;

    if (id == kEchoReplyWireId) {
      // Payload layout matches StressAvatar.def client_methods::EchoReply:
      //   uint32 seq | uint64 serverTsNs | uint64 clientTsNs
      BinaryReader reader(payload);
      auto seq = reader.Read<uint32_t>();
      auto server_ts_ns = reader.Read<uint64_t>();
      auto client_ts_ns = reader.Read<uint64_t>();
      if (!seq || !server_ts_ns || !client_ts_ns) return true;  // malformed but ours

      const uint64_t now_ns =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    SteadyClock::now().time_since_epoch())
                                    .count());
      const double rtt_ms = static_cast<double>(now_ns - *client_ts_ns) / 1e6;
      ++metrics_.echo_received;
      metrics_.echo_rtt_ms.push_back(rtt_ms);
      (void)*server_ts_ns;  // reserved for future up-leg / down-leg split
      return true;
    }

    if (id == kReliableDeltaWireId && !payload.empty()) {
      switch (static_cast<uint8_t>(payload[0])) {
        case 1:
          ++metrics_.aoi_enter;
          break;
        case 2:
          ++metrics_.aoi_leave;
          break;
        case 3:
          ++metrics_.aoi_pos_update;
          break;
        case 4:
          ++metrics_.aoi_prop_update;
          break;
        default:
          break;
      }
      return true;
    }

    return false;
  }

  void OnDisconnect(const Address&) {
    if (suppress_disconnect_callback_) {
      return;
    }

    if (state_ == SessionState::kScheduled || state_ == SessionState::kCoolingDown) {
      return;
    }

    if (!intentionally_offline_) {
      ++metrics_.unexpected_disconnects;
      RecordFailure("disconnect");
    }

    RequestRestart(SteadyClock::now(), opts_.retry_delay_ms);
  }

  void DisconnectAndRetry(TimePoint now) {
    if (intentionally_offline_) {
      ++metrics_.planned_disconnects;
    }

    TeardownNetwork();
    RestartAfter(now, opts_.retry_delay_ms);
  }

  void RestartAfter(TimePoint now, int delay_ms) {
    TeardownNetwork();
    state_ = SessionState::kCoolingDown;
    next_action_at_ = now + std::chrono::milliseconds(delay_ms);
  }

  void RequestRestart(TimePoint now, int delay_ms) {
    restart_requested_ = true;
    restart_at_ = now + std::chrono::milliseconds(delay_ms);
  }

  void TeardownNetwork() {
    suppress_disconnect_callback_ = true;
    login_channel_ = nullptr;
    auth_channel_ = nullptr;
    network_.reset();
    entity_id_ = kInvalidEntityID;
    session_key_ = {};
    baseapp_addr_ = {};
    intentionally_offline_ = false;
    // P3 leak fix: without clearing these, a retry that transitions back
    // into kOnline before OnEntityTransferred fires would send an Echo at
    // the *old* Avatar's id, tripping BaseApp's cross-entity reject.
    echo_pending_ = false;
    next_echo_seq_ = 0;
    move_pending_ = false;
    pos_x_ = 0.f;
    pos_z_ = 0.f;
    suppress_disconnect_callback_ = false;
  }

  void RecordFailure(std::string reason) { ++metrics_.failure_reasons[std::move(reason)]; }

  [[nodiscard]] auto RandomPercent() -> int {
    std::uniform_int_distribution<int> dist(0, 99);
    return dist(rng_);
  }

  [[nodiscard]] auto RandomBetween(int min_ms, int max_ms) -> int {
    if (max_ms <= min_ms) {
      return min_ms;
    }
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return dist(rng_);
  }

  [[nodiscard]] static auto ElapsedMs(TimePoint from, TimePoint to) -> double {
    return std::chrono::duration<double, std::milli>(to - from).count();
  }

  [[nodiscard]] auto StateName() const -> std::string_view {
    switch (state_) {
      case SessionState::kScheduled:
        return "scheduled";
      case SessionState::kWaitingLogin:
        return "waiting_login";
      case SessionState::kWaitingAuth:
        return "waiting_auth";
      case SessionState::kOnline:
        return "online";
      case SessionState::kCoolingDown:
        return "cooldown";
    }
    return "unknown";
  }

  std::size_t id_;
  std::string username_;
  EventDispatcher& dispatcher_;
  const Options& opts_;
  Metrics& metrics_;
  std::mt19937& rng_;
  std::unique_ptr<NetworkInterface> network_;
  Channel* login_channel_{nullptr};
  Channel* auth_channel_{nullptr};
  SessionKey session_key_{};
  Address baseapp_addr_{};
  EntityID entity_id_{kInvalidEntityID};
  SessionState state_{SessionState::kScheduled};
  TimePoint started_at_{};
  TimePoint deadline_at_{};
  TimePoint next_action_at_{};
  TimePoint restart_at_{};
  std::size_t attempt_{0};
  bool intentionally_offline_{false};
  bool suppress_disconnect_callback_{false};
  bool restart_requested_{false};
  std::optional<Address> source_ip_;
  uint32_t next_echo_seq_{0};
  TimePoint echo_due_at_{};
  bool echo_pending_{false};
  TimePoint move_due_at_{};
  bool move_pending_{false};
  float pos_x_{0.f};
  float pos_z_{0.f};
};

void PrintUsage() {
  // P2.1: forked from login_stress; behaviour currently identical.
  // Cell-ready / RPC / AoI instrumentation lands in P2.2+.
  std::cerr
      << "Usage: world_stress --login <host:port> --password-hash <sha256hex> [options]\n"
      << "\n"
      << "Options:\n"
      << "  --clients <n>              Virtual clients to run (default: 100)\n"
      << "  --account-pool <n>         Distinct account count; smaller than clients triggers "
         "relogin\n"
      << "                             pressure (default: same as clients)\n"
      << "  --account-index-base <n>   Starting account index for this worker shard "
         "(default: 0)\n"
      << "  --username-prefix <text>   Account prefix (default: stress_user_)\n"
      << "  --ramp-per-sec <n>         New login attempts started per second (default: 100)\n"
      << "  --worker-index <n>         Worker shard index for reporting (default: 0)\n"
      << "  --worker-count <n>         Total worker shard count (default: 1)\n"
      << "  --source-ip <ipv4>         Bind outbound RUDP client sockets to this local IPv4; "
         "repeatable\n"
      << "  --duration-sec <n>         Total runtime in seconds (default: 60)\n"
      << "  --retry-delay-ms <n>       Delay before retry after failure/disconnect (default: "
         "1000)\n"
      << "  --connect-timeout-ms <n>   Timeout for login/auth stages (default: 20000)\n"
      << "  --hold-min-ms <n>          Min online hold time for normal sessions (default: "
         "30000)\n"
      << "  --hold-max-ms <n>          Max online hold time for normal sessions (default: "
         "60000)\n"
      << "  --shortline-pct <0-100>    Percent of sessions that short-disconnect after auth "
         "(default: 20)\n"
      << "  --shortline-min-ms <n>     Min time before planned short disconnect (default: 1000)\n"
      << "  --shortline-max-ms <n>     Max time before planned short disconnect (default: 5000)\n"
      << "  --rpc-rate-hz <n>          Echo RPC rate per session while in-world "
         "(default: 2, 0 = one-shot)\n"
      << "  --move-rate-hz <n>         ReportPos rate per session while in-world "
         "(default: 10, 0 = disabled)\n"
      << "  --space-count <n>          Distinct cell spaces to spread avatars across "
         "(default: 1)\n"
      << "  --seed <n>                 RNG seed (default: 12345)\n"
      << "  --verbose-failures         Print individual failures\n"
      << "  --script-clients <n>       Spawn N real atlas_client.exe subprocesses that load\n"
      << "                             --client-assembly; their Phase-B stdout is counted and\n"
      << "                             summarised alongside virtual-client metrics (default: 0)\n"
      << "  --client-exe <path>        Path to atlas_client.exe (required with --script-clients)\n"
      << "  --client-assembly <path>   Path to Atlas.ClientSample.dll (required with "
         "--script-clients)\n"
      << "  --client-runtime-config <path>  hostfxr *.runtimeconfig.json (optional)\n"
      << "  --script-username-prefix <text> Username prefix for script children (default: "
         "script_user_)\n"
      << "  --script-verify            Non-zero exit if any script child didn't observe OnInit\n"
      << "\n"
      << "Example:\n"
      << "  world_stress --login 127.0.0.1:20013 --password-hash <sha256> --clients 500 "
         "--account-pool 200 --shortline-pct 35 --duration-sec 300\n";
}

auto ParseAddress(std::string_view spec) -> std::optional<Address> {
  const auto kColon = spec.rfind(':');
  if (kColon == std::string_view::npos) {
    return std::nullopt;
  }

  auto host = std::string(spec.substr(0, kColon));
  auto port_sv = spec.substr(kColon + 1);
  if (port_sv.empty()) {
    return std::nullopt;
  }

  const auto kPort = static_cast<uint16_t>(std::strtoul(std::string(port_sv).c_str(), nullptr, 10));
  auto resolved = Address::Resolve(host, kPort);
  if (!resolved) {
    return std::nullopt;
  }
  return *resolved;
}

auto ParseIpv4Address(std::string_view spec) -> std::optional<Address> {
  auto resolved = Address::Resolve(spec, 0);
  if (!resolved) {
    return std::nullopt;
  }
  return *resolved;
}

template <typename T>
auto ParseNumeric(std::string_view text) -> std::optional<T> {
  try {
    if constexpr (std::is_same_v<T, std::size_t>) {
      return static_cast<std::size_t>(std::stoull(std::string(text)));
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      return static_cast<uint32_t>(std::stoul(std::string(text)));
    } else {
      return static_cast<T>(std::stoi(std::string(text)));
    }
  } catch (...) {
    return std::nullopt;
  }
}

auto ParseOptions(int argc, char* argv[]) -> std::optional<Options> {
  Options opts;
  bool have_login = false;
  bool have_password_hash = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view kArg(argv[i]);
    auto require_value = [&](std::string_view name) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        return std::nullopt;
      }
      return std::string_view(argv[++i]);
    };

    if (kArg == "--login") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto addr = ParseAddress(*value);
      if (!addr) {
        std::cerr << "Invalid --login address, expected host:port\n";
        return std::nullopt;
      }
      opts.login_addr = *addr;
      have_login = true;
    } else if (kArg == "--password-hash") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      opts.password_hash = std::string(*value);
      have_password_hash = true;
    } else if (kArg == "--clients") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed) return std::nullopt;
      opts.clients = *parsed;
    } else if (kArg == "--account-pool") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed) return std::nullopt;
      opts.account_pool = *parsed;
    } else if (kArg == "--account-index-base") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed) return std::nullopt;
      opts.account_index_base = *parsed;
    } else if (kArg == "--username-prefix") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      opts.username_prefix = std::string(*value);
    } else if (kArg == "--source-ip") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto addr = ParseIpv4Address(*value);
      if (!addr) {
        std::cerr << "Invalid --source-ip IPv4 address\n";
        return std::nullopt;
      }
      opts.source_ips.push_back(*addr);
    } else if (kArg == "--ramp-per-sec") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed) return std::nullopt;
      opts.ramp_per_sec = std::max<std::size_t>(1, *parsed);
    } else if (kArg == "--duration-sec") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.duration_sec = *parsed;
    } else if (kArg == "--connect-timeout-ms") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.connect_timeout_ms = *parsed;
    } else if (kArg == "--retry-delay-ms") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.retry_delay_ms = *parsed;
    } else if (kArg == "--hold-min-ms") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.hold_min_ms = *parsed;
    } else if (kArg == "--hold-max-ms") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.hold_max_ms = *parsed;
    } else if (kArg == "--shortline-pct") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.shortline_pct = std::clamp(*parsed, 0, 100);
    } else if (kArg == "--shortline-min-ms") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.shortline_min_ms = *parsed;
    } else if (kArg == "--shortline-max-ms") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.shortline_max_ms = *parsed;
    } else if (kArg == "--rpc-rate-hz") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.rpc_rate_hz = *parsed;
    } else if (kArg == "--move-rate-hz") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.move_rate_hz = *parsed;
    } else if (kArg == "--space-count") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<int>(*value);
      if (!parsed) return std::nullopt;
      opts.space_count = *parsed;
    } else if (kArg == "--seed") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<uint32_t>(*value);
      if (!parsed) return std::nullopt;
      opts.seed = *parsed;
    } else if (kArg == "--worker-index") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed) return std::nullopt;
      opts.worker_index = *parsed;
    } else if (kArg == "--worker-count") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed || *parsed == 0) return std::nullopt;
      opts.worker_count = *parsed;
    } else if (kArg == "--verbose-failures") {
      opts.verbose_failures = true;
    } else if (kArg == "--script-clients") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto parsed = ParseNumeric<std::size_t>(*value);
      if (!parsed) return std::nullopt;
      opts.script_clients = *parsed;
    } else if (kArg == "--client-exe") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      opts.client_exe = std::filesystem::path(std::string(*value));
    } else if (kArg == "--client-assembly") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      opts.client_assembly = std::filesystem::path(std::string(*value));
    } else if (kArg == "--client-runtime-config") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      opts.client_runtime_config = std::filesystem::path(std::string(*value));
    } else if (kArg == "--script-username-prefix") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      opts.script_username_prefix = std::string(*value);
    } else if (kArg == "--script-verify") {
      opts.script_verify = true;
    } else if (kArg == "--help" || kArg == "-h") {
      PrintUsage();
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << kArg << "\n";
      return std::nullopt;
    }
  }

  if (!have_login || !have_password_hash) {
    return std::nullopt;
  }

  if (opts.account_pool == 0) {
    opts.account_pool = opts.clients;
  }
  if (opts.worker_index >= opts.worker_count) {
    std::cerr << "--worker-index must be smaller than --worker-count\n";
    return std::nullopt;
  }

  if (opts.hold_max_ms < opts.hold_min_ms) {
    std::swap(opts.hold_max_ms, opts.hold_min_ms);
  }
  if (opts.shortline_max_ms < opts.shortline_min_ms) {
    std::swap(opts.shortline_max_ms, opts.shortline_min_ms);
  }
  return opts;
}

auto PercentileMs(std::vector<double> values, double pct) -> double {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto kIndex = static_cast<std::size_t>(
      std::clamp<double>((pct / 100.0) * static_cast<double>(values.size() - 1), 0.0,
                         static_cast<double>(values.size() - 1)));
  return values[kIndex];
}

void PrintProgress(const Options& opts, const Metrics& metrics,
                   const std::vector<Session>& sessions, TimePoint started_at, TimePoint now) {
  const auto kElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count();
  const auto kOnline = static_cast<std::size_t>(std::count_if(
      sessions.begin(), sessions.end(), [](const Session& s) { return s.IsOnline(); }));
  const auto kInflight = static_cast<std::size_t>(std::count_if(
      sessions.begin(), sessions.end(), [](const Session& s) { return s.IsInflight(); }));

  std::cout << std::format(
      "[{:>4}s] started={} login_ok={} auth_ok={} login_fail={} auth_fail={} timeouts={} "
      "online={} inflight={} planned_disc={} unexpected_disc={}\n",
      kElapsed, metrics.login_started, metrics.login_result_success, metrics.auth_success,
      metrics.login_result_fail, metrics.auth_fail, metrics.timeout_fail, kOnline, kInflight,
      metrics.planned_disconnects, metrics.unexpected_disconnects);
  (void)opts;
}

void PrintSummary(const Options& opts, const Metrics& metrics,
                  const std::vector<Session>& sessions) {
  const auto kOnline = static_cast<std::size_t>(std::count_if(
      sessions.begin(), sessions.end(), [](const Session& s) { return s.IsOnline(); }));

  std::cout << "\nSummary\n";
  std::cout << std::format("  clients:            {}\n", opts.clients);
  std::cout << std::format("  account_pool:       {}\n", opts.account_pool);
  std::cout << std::format("  account_index_base: {}\n", opts.account_index_base);
  std::cout << std::format("  worker:             {}/{}\n", opts.worker_index, opts.worker_count);
  std::cout << std::format("  source_ip_count:    {}\n", opts.source_ips.size());
  std::cout << std::format("  login_started:      {}\n", metrics.login_started);
  std::cout << std::format("  login_success:      {}\n", metrics.login_result_success);
  std::cout << std::format("  auth_success:       {}\n", metrics.auth_success);
  std::cout << std::format("  select_avatar_sent: {}\n", metrics.select_avatar_sent);
  std::cout << std::format("  select_avatar_fail: {}\n", metrics.select_avatar_fail);
  std::cout << std::format("  entity_transferred: {}\n", metrics.entity_transferred);
  std::cout << std::format("  cell_ready:         {}\n", metrics.cell_ready);
  std::cout << std::format("  rpc_rate_hz:        {}\n", opts.rpc_rate_hz);
  std::cout << std::format("  echo_sent:          {}\n", metrics.echo_sent);
  std::cout << std::format("  echo_received:      {}\n", metrics.echo_received);
  std::cout << std::format(
      "  echo_loss:          {}\n",
      metrics.echo_sent > metrics.echo_received ? metrics.echo_sent - metrics.echo_received : 0);
  std::cout << std::format("  move_rate_hz:       {}\n", opts.move_rate_hz);
  std::cout << std::format("  space_count:        {}\n", opts.space_count);
  std::cout << std::format("  move_sent:          {}\n", metrics.move_sent);
  std::cout << std::format("  move_fail:          {}\n", metrics.move_fail);
  std::cout << std::format("  aoi_enter:          {}\n", metrics.aoi_enter);
  std::cout << std::format("  aoi_leave:          {}\n", metrics.aoi_leave);
  std::cout << std::format("  aoi_pos_update:     {}\n", metrics.aoi_pos_update);
  std::cout << std::format("  aoi_prop_update:    {}\n", metrics.aoi_prop_update);
  if (!metrics.echo_rtt_ms.empty()) {
    std::cout << std::format("  echo_rtt_p50:       {:.2f} ms\n",
                             PercentileMs(metrics.echo_rtt_ms, 50.0));
    std::cout << std::format("  echo_rtt_p95:       {:.2f} ms\n",
                             PercentileMs(metrics.echo_rtt_ms, 95.0));
    std::cout << std::format("  echo_rtt_p99:       {:.2f} ms\n",
                             PercentileMs(metrics.echo_rtt_ms, 99.0));
  }
  std::cout << std::format("  login_fail:         {}\n", metrics.login_result_fail);
  std::cout << std::format("  auth_fail:          {}\n", metrics.auth_fail);
  std::cout << std::format("  timeout_fail:       {}\n", metrics.timeout_fail);
  std::cout << std::format("  planned_disconnect: {}\n", metrics.planned_disconnects);
  std::cout << std::format("  unexpected_disc:    {}\n", metrics.unexpected_disconnects);
  std::cout << std::format("  online_at_end:      {}\n", kOnline);

  if (!metrics.auth_latency_ms.empty()) {
    std::cout << std::format("  auth_latency_p50:   {:.2f} ms\n",
                             PercentileMs(metrics.auth_latency_ms, 50.0));
    std::cout << std::format("  auth_latency_p95:   {:.2f} ms\n",
                             PercentileMs(metrics.auth_latency_ms, 95.0));
    std::cout << std::format("  auth_latency_p99:   {:.2f} ms\n",
                             PercentileMs(metrics.auth_latency_ms, 99.0));
  }

  if (!metrics.failure_reasons.empty()) {
    std::vector<std::pair<std::string, std::size_t>> failures(metrics.failure_reasons.begin(),
                                                              metrics.failure_reasons.end());
    std::sort(failures.begin(), failures.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::cout << "  top_failures:\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(10, failures.size()); ++i) {
      std::cout << std::format("    {}: {}\n", failures[i].first, failures[i].second);
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto kOpts = ParseOptions(argc, argv);
  if (!kOpts) {
    PrintUsage();
    return 1;
  }

  EventDispatcher dispatcher("world_stress");
  dispatcher.SetMaxPollWait(Milliseconds(1));

  Metrics metrics;
  std::mt19937 rng(kOpts->seed);
  std::vector<Session> sessions;
  sessions.reserve(kOpts->clients);

  const auto kStartedAt = SteadyClock::now();
  const auto kRampIntervalMs = std::max<std::size_t>(
      1, static_cast<std::size_t>(1000 / std::max<std::size_t>(1, kOpts->ramp_per_sec)));

  for (std::size_t i = 0; i < kOpts->clients; ++i) {
    const auto kAccountIndex = kOpts->account_index_base + (i % kOpts->account_pool);
    auto username = std::format("{}{}", kOpts->username_prefix, kAccountIndex);
    std::optional<Address> source_ip;
    if (!kOpts->source_ips.empty()) {
      source_ip = kOpts->source_ips[i % kOpts->source_ips.size()];
    }
    sessions.emplace_back(i, std::move(username), dispatcher, *kOpts, metrics, rng, source_ip);
    sessions.back().ScheduleInitial(kStartedAt + std::chrono::milliseconds(i * kRampIntervalMs));
  }

  std::cout << std::format(
      "Starting world_stress: login={} clients={} account_pool={} account_base={} worker={}/{} "
      "sources={} ramp/s={} duration={}s shortline={}%% seed={}\n",
      kOpts->login_addr.ToString(), kOpts->clients, kOpts->account_pool, kOpts->account_index_base,
      kOpts->worker_index, kOpts->worker_count, kOpts->source_ips.size(), kOpts->ramp_per_sec,
      kOpts->duration_sec, kOpts->shortline_pct, kOpts->seed);

  // Phase C2: optional real atlas_client.exe subprocess harness. Launched
  // BEFORE the virtual-client ramp so script children observe the same
  // server state the raw-protocol clients do. Pumped once per main-loop
  // iteration so stdout lines don't back up.
  world_stress::ScriptClientOptions sco;
  sco.exe = kOpts->client_exe;
  sco.assembly = kOpts->client_assembly;
  sco.runtime_config = kOpts->client_runtime_config;
  // Address has no standalone Host() accessor; derive it from the dotted
  // form in ToString() by trimming the ":port" suffix.
  {
    auto s = kOpts->login_addr.ToString();
    auto colon = s.rfind(':');
    sco.login_host = colon != std::string::npos ? s.substr(0, colon) : s;
  }
  sco.login_port = kOpts->login_addr.Port();
  sco.password_hash = kOpts->password_hash;
  sco.username_prefix = kOpts->script_username_prefix;
  // Offset script usernames past the virtual-client pool so both fleets can
  // share an --account-pool without colliding on the same credentials.
  sco.username_index_base = kOpts->account_index_base + kOpts->account_pool;
  sco.count = kOpts->script_clients;
  sco.verify = kOpts->script_verify;
  world_stress::ScriptClientHarness script_harness(std::move(sco));
  if (auto r = script_harness.Start(); !r.HasValue()) {
    std::cerr << "[script-clients] " << r.Error().Message() << "\n";
    return 2;
  }

  auto next_progress = kStartedAt + std::chrono::seconds(1);
  const auto kDeadline = kStartedAt + std::chrono::seconds(kOpts->duration_sec);

  while (SteadyClock::now() < kDeadline) {
    dispatcher.ProcessOnce();
    script_harness.Pump();

    const auto kNow = SteadyClock::now();
    for (auto& session : sessions) {
      session.Update(kNow);
    }

    if (kNow >= next_progress) {
      PrintProgress(*kOpts, metrics, sessions, kStartedAt, kNow);
      next_progress += std::chrono::seconds(1);
    }
  }

  script_harness.ShutdownAndWait(std::chrono::seconds(3));
  PrintSummary(*kOpts, metrics, sessions);
  const bool script_ok = script_harness.PrintSummary();
  return script_ok ? 0 : 1;
}
