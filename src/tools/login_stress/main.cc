#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

using namespace atlas;

namespace {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

struct Options {
  Address login_addr{"127.0.0.1", 0};
  std::string username_prefix{"stress_user_"};
  std::string password_hash;
  // SHA-256 of the server's entity-def surface; BaseApp rejects mismatched
  // builds. All-zero default keeps ungated test harnesses (fake_cluster) working.
  std::array<uint8_t, 32> entity_def_digest{};
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
  bool verbose_failures{false};
  uint32_t seed{12345};
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
  std::vector<double> auth_latency_ms;
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
  }

  void StartAttempt(TimePoint now) {
    EnsureNetwork();
    const auto kResult =
        network_->ConnectRudp(opts_.login_addr, NetworkInterface::InternetRudpProfile());
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
    req.entity_def_digest = opts_.entity_def_digest;

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

    const auto kResult =
        network_->ConnectRudp(baseapp_addr_, NetworkInterface::InternetRudpProfile());
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
};

void PrintUsage() {
  std::cerr
      << "Usage: login_stress --login <host:port> --password-hash <sha256hex> [options]\n"
      << "\n"
      << "Options:\n"
      << "  --entity-def-digest <hex>  64-char SHA-256 (Atlas.Rpc.EntityDefDigest.Sha256Hex)\n"
      << "                             stamped onto LoginRequest; required when BaseApp gates\n"
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
      << "  --seed <n>                 RNG seed (default: 12345)\n"
      << "  --verbose-failures         Print individual failures\n"
      << "\n"
      << "Example:\n"
      << "  login_stress --login 127.0.0.1:20013 --password-hash <sha256> --clients 500 "
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

auto ParseHexDigest(std::string_view hex) -> std::optional<std::array<uint8_t, 32>> {
  if (hex.size() != 64) return std::nullopt;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return out;
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
    } else if (kArg == "--entity-def-digest") {
      auto value = require_value(kArg);
      if (!value) return std::nullopt;
      auto digest = ParseHexDigest(*value);
      if (!digest) {
        std::cerr << "Invalid --entity-def-digest, expected 64-char hex (SHA-256)\n";
        return std::nullopt;
      }
      opts.entity_def_digest = *digest;
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

  EventDispatcher dispatcher("login_stress");
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
      "Starting login_stress: login={} clients={} account_pool={} account_base={} worker={}/{} "
      "sources={} ramp/s={} duration={}s shortline={}%% seed={}\n",
      kOpts->login_addr.ToString(), kOpts->clients, kOpts->account_pool, kOpts->account_index_base,
      kOpts->worker_index, kOpts->worker_count, kOpts->source_ips.size(), kOpts->ramp_per_sec,
      kOpts->duration_sec, kOpts->shortline_pct, kOpts->seed);

  auto next_progress = kStartedAt + std::chrono::seconds(1);
  const auto kDeadline = kStartedAt + std::chrono::seconds(kOpts->duration_sec);

  while (SteadyClock::now() < kDeadline) {
    dispatcher.ProcessOnce();

    const auto kNow = SteadyClock::now();
    for (auto& session : sessions) {
      session.Update(kNow);
    }

    if (kNow >= next_progress) {
      PrintProgress(*kOpts, metrics, sessions, kStartedAt, kNow);
      next_progress += std::chrono::seconds(1);
    }
  }

  PrintSummary(*kOpts, metrics, sessions);
  return 0;
}
