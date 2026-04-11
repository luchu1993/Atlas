#include "baseapp/baseapp_messages.hpp"
#include "foundation/time.hpp"
#include "loginapp/login_messages.hpp"
#include "network/channel.hpp"
#include "network/event_dispatcher.hpp"
#include "network/network_interface.hpp"
#include "network/reliable_udp.hpp"

#include <algorithm>
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

using namespace atlas;

namespace
{

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

struct Options
{
    Address login_addr{"127.0.0.1", 0};
    std::string username_prefix{"stress_user_"};
    std::string password_hash;
    std::size_t clients{100};
    std::size_t account_pool{0};
    std::size_t ramp_per_sec{100};
    int duration_sec{60};
    int connect_timeout_ms{10'000};
    int retry_delay_ms{1'000};
    int hold_min_ms{30'000};
    int hold_max_ms{60'000};
    int shortline_pct{20};
    int shortline_min_ms{1'000};
    int shortline_max_ms{5'000};
    bool verbose_failures{false};
    uint32_t seed{12345};
};

enum class SessionState : uint8_t
{
    Scheduled,
    WaitingLogin,
    WaitingAuth,
    Online,
    CoolingDown,
};

struct Metrics
{
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

class Session
{
public:
    Session(std::size_t id, std::string username, EventDispatcher& dispatcher, const Options& opts,
            Metrics& metrics, std::mt19937& rng)
        : id_(id),
          username_(std::move(username)),
          dispatcher_(dispatcher),
          opts_(opts),
          metrics_(metrics),
          rng_(rng)
    {
    }

    void schedule_initial(TimePoint when)
    {
        state_ = SessionState::Scheduled;
        next_action_at_ = when;
    }

    void update(TimePoint now)
    {
        if (restart_requested_)
        {
            teardown_network();
            state_ = SessionState::CoolingDown;
            next_action_at_ = restart_at_;
            restart_requested_ = false;
        }

        switch (state_)
        {
            case SessionState::Scheduled:
            case SessionState::CoolingDown:
                if (now >= next_action_at_)
                {
                    start_attempt(now);
                }
                break;
            case SessionState::WaitingLogin:
            case SessionState::WaitingAuth:
                if (now >= deadline_at_)
                {
                    ++metrics_.timeout_fail;
                    record_failure("timeout");
                    if (opts_.verbose_failures)
                    {
                        std::cout << std::format("[session {}] timed out in state {}\n", id_,
                                                 state_name());
                    }
                    restart_after(now, opts_.retry_delay_ms);
                }
                break;
            case SessionState::Online:
                if (now >= next_action_at_)
                {
                    disconnect_and_retry(now);
                }
                break;
        }
    }

    [[nodiscard]] auto is_online() const -> bool { return state_ == SessionState::Online; }
    [[nodiscard]] auto is_inflight() const -> bool
    {
        return state_ == SessionState::WaitingLogin || state_ == SessionState::WaitingAuth;
    }

private:
    void ensure_network()
    {
        if (network_)
        {
            return;
        }

        network_ = std::make_unique<NetworkInterface>(dispatcher_);
        network_->set_disconnect_callback([this](Channel& ch)
                                          { on_disconnect(ch.remote_address()); });

        auto& table = network_->interface_table();
        (void)table.register_typed_handler<login::LoginResult>(
            [this](const Address&, Channel*, const login::LoginResult& msg)
            { on_login_result(msg); });
        (void)table.register_typed_handler<baseapp::AuthenticateResult>(
            [this](const Address&, Channel*, const baseapp::AuthenticateResult& msg)
            { on_auth_result(msg); });
    }

    void start_attempt(TimePoint now)
    {
        ensure_network();
        const auto result = network_->connect_rudp(opts_.login_addr);
        if (!result)
        {
            ++metrics_.login_result_fail;
            record_failure(std::format("login_connect:{}", result.error().message()));
            restart_after(now, opts_.retry_delay_ms);
            return;
        }

        login_channel_ = static_cast<Channel*>(*result);

        login::LoginRequest req;
        req.username = username_;
        req.password_hash = opts_.password_hash;

        const auto send_result = login_channel_->send_message(req);
        if (!send_result)
        {
            ++metrics_.login_result_fail;
            record_failure(std::format("login_send:{}", send_result.error().message()));
            restart_after(now, opts_.retry_delay_ms);
            return;
        }

        ++attempt_;
        ++metrics_.login_started;
        started_at_ = now;
        deadline_at_ = now + std::chrono::milliseconds(opts_.connect_timeout_ms);
        state_ = SessionState::WaitingLogin;
        session_key_ = {};
        baseapp_addr_ = {};
        auth_channel_ = nullptr;
        intentionally_offline_ = false;
    }

    void on_login_result(const login::LoginResult& msg)
    {
        if (state_ != SessionState::WaitingLogin)
        {
            return;
        }

        if (msg.status != login::LoginStatus::Success)
        {
            ++metrics_.login_result_fail;
            record_failure(msg.error_message.empty()
                               ? std::format("login_status:{}", static_cast<int>(msg.status))
                               : msg.error_message);
            request_restart(SteadyClock::now(), opts_.retry_delay_ms);
            return;
        }

        ++metrics_.login_result_success;
        session_key_ = msg.session_key;
        baseapp_addr_ = msg.baseapp_addr;

        const auto result = network_->connect_rudp(baseapp_addr_);
        if (!result)
        {
            ++metrics_.auth_fail;
            record_failure(std::format("baseapp_connect:{}", result.error().message()));
            request_restart(SteadyClock::now(), opts_.retry_delay_ms);
            return;
        }

        auth_channel_ = static_cast<Channel*>(*result);

        baseapp::Authenticate auth;
        auth.session_key = session_key_;
        const auto send_result = auth_channel_->send_message(auth);
        if (!send_result)
        {
            ++metrics_.auth_fail;
            record_failure(std::format("auth_send:{}", send_result.error().message()));
            request_restart(SteadyClock::now(), opts_.retry_delay_ms);
            return;
        }

        state_ = SessionState::WaitingAuth;
        deadline_at_ = SteadyClock::now() + std::chrono::milliseconds(opts_.connect_timeout_ms);
    }

    void on_auth_result(const baseapp::AuthenticateResult& msg)
    {
        if (state_ != SessionState::WaitingAuth)
        {
            return;
        }

        if (!msg.success)
        {
            ++metrics_.auth_fail;
            record_failure(msg.error.empty() ? "auth_failed" : msg.error);
            request_restart(SteadyClock::now(), opts_.retry_delay_ms);
            return;
        }

        ++metrics_.auth_success;
        metrics_.auth_latency_ms.push_back(elapsed_ms(started_at_, SteadyClock::now()));

        state_ = SessionState::Online;
        entity_id_ = msg.entity_id;
        intentionally_offline_ = random_percent() < opts_.shortline_pct;
        next_action_at_ = SteadyClock::now() +
                          std::chrono::milliseconds(
                              intentionally_offline_
                                  ? random_between(opts_.shortline_min_ms, opts_.shortline_max_ms)
                                  : random_between(opts_.hold_min_ms, opts_.hold_max_ms));
    }

    void on_disconnect(const Address&)
    {
        if (suppress_disconnect_callback_)
        {
            return;
        }

        if (state_ == SessionState::Scheduled || state_ == SessionState::CoolingDown)
        {
            return;
        }

        if (!intentionally_offline_)
        {
            ++metrics_.unexpected_disconnects;
            record_failure("disconnect");
        }

        request_restart(SteadyClock::now(), opts_.retry_delay_ms);
    }

    void disconnect_and_retry(TimePoint now)
    {
        if (intentionally_offline_)
        {
            ++metrics_.planned_disconnects;
        }

        teardown_network();
        restart_after(now, opts_.retry_delay_ms);
    }

    void restart_after(TimePoint now, int delay_ms)
    {
        teardown_network();
        state_ = SessionState::CoolingDown;
        next_action_at_ = now + std::chrono::milliseconds(delay_ms);
    }

    void request_restart(TimePoint now, int delay_ms)
    {
        restart_requested_ = true;
        restart_at_ = now + std::chrono::milliseconds(delay_ms);
    }

    void teardown_network()
    {
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

    void record_failure(std::string reason) { ++metrics_.failure_reasons[std::move(reason)]; }

    [[nodiscard]] auto random_percent() -> int
    {
        std::uniform_int_distribution<int> dist(0, 99);
        return dist(rng_);
    }

    [[nodiscard]] auto random_between(int min_ms, int max_ms) -> int
    {
        if (max_ms <= min_ms)
        {
            return min_ms;
        }
        std::uniform_int_distribution<int> dist(min_ms, max_ms);
        return dist(rng_);
    }

    [[nodiscard]] static auto elapsed_ms(TimePoint from, TimePoint to) -> double
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    [[nodiscard]] auto state_name() const -> std::string_view
    {
        switch (state_)
        {
            case SessionState::Scheduled:
                return "scheduled";
            case SessionState::WaitingLogin:
                return "waiting_login";
            case SessionState::WaitingAuth:
                return "waiting_auth";
            case SessionState::Online:
                return "online";
            case SessionState::CoolingDown:
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
    SessionState state_{SessionState::Scheduled};
    TimePoint started_at_{};
    TimePoint deadline_at_{};
    TimePoint next_action_at_{};
    TimePoint restart_at_{};
    std::size_t attempt_{0};
    bool intentionally_offline_{false};
    bool suppress_disconnect_callback_{false};
    bool restart_requested_{false};
};

void print_usage()
{
    std::cerr
        << "Usage: login_stress --login <host:port> --password-hash <sha256hex> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --clients <n>              Virtual clients to run (default: 100)\n"
        << "  --account-pool <n>         Distinct account count; smaller than clients triggers "
           "relogin\n"
        << "                             pressure (default: same as clients)\n"
        << "  --username-prefix <text>   Account prefix (default: stress_user_)\n"
        << "  --ramp-per-sec <n>         New login attempts started per second (default: 100)\n"
        << "  --duration-sec <n>         Total runtime in seconds (default: 60)\n"
        << "  --retry-delay-ms <n>       Delay before retry after failure/disconnect (default: "
           "1000)\n"
        << "  --connect-timeout-ms <n>   Timeout for login/auth stages (default: 10000)\n"
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

auto parse_address(std::string_view spec) -> std::optional<Address>
{
    const auto colon = spec.rfind(':');
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }

    auto host = std::string(spec.substr(0, colon));
    auto port_sv = spec.substr(colon + 1);
    if (port_sv.empty())
    {
        return std::nullopt;
    }

    const auto port =
        static_cast<uint16_t>(std::strtoul(std::string(port_sv).c_str(), nullptr, 10));
    return Address(host, port);
}

template <typename T>
auto parse_numeric(std::string_view text) -> std::optional<T>
{
    try
    {
        if constexpr (std::is_same_v<T, std::size_t>)
        {
            return static_cast<std::size_t>(std::stoull(std::string(text)));
        }
        else if constexpr (std::is_same_v<T, uint32_t>)
        {
            return static_cast<uint32_t>(std::stoul(std::string(text)));
        }
        else
        {
            return static_cast<T>(std::stoi(std::string(text)));
        }
    }
    catch (...)
    {
        return std::nullopt;
    }
}

auto parse_options(int argc, char* argv[]) -> std::optional<Options>
{
    Options opts;
    bool have_login = false;
    bool have_password_hash = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg(argv[i]);
        auto require_value = [&](std::string_view name) -> std::optional<std::string_view>
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << name << "\n";
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };

        if (arg == "--login")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto addr = parse_address(*value);
            if (!addr)
            {
                std::cerr << "Invalid --login address, expected host:port\n";
                return std::nullopt;
            }
            opts.login_addr = *addr;
            have_login = true;
        }
        else if (arg == "--password-hash")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            opts.password_hash = std::string(*value);
            have_password_hash = true;
        }
        else if (arg == "--clients")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<std::size_t>(*value);
            if (!parsed)
                return std::nullopt;
            opts.clients = *parsed;
        }
        else if (arg == "--account-pool")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<std::size_t>(*value);
            if (!parsed)
                return std::nullopt;
            opts.account_pool = *parsed;
        }
        else if (arg == "--username-prefix")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            opts.username_prefix = std::string(*value);
        }
        else if (arg == "--ramp-per-sec")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<std::size_t>(*value);
            if (!parsed)
                return std::nullopt;
            opts.ramp_per_sec = std::max<std::size_t>(1, *parsed);
        }
        else if (arg == "--duration-sec")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.duration_sec = *parsed;
        }
        else if (arg == "--connect-timeout-ms")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.connect_timeout_ms = *parsed;
        }
        else if (arg == "--retry-delay-ms")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.retry_delay_ms = *parsed;
        }
        else if (arg == "--hold-min-ms")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.hold_min_ms = *parsed;
        }
        else if (arg == "--hold-max-ms")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.hold_max_ms = *parsed;
        }
        else if (arg == "--shortline-pct")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.shortline_pct = std::clamp(*parsed, 0, 100);
        }
        else if (arg == "--shortline-min-ms")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.shortline_min_ms = *parsed;
        }
        else if (arg == "--shortline-max-ms")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<int>(*value);
            if (!parsed)
                return std::nullopt;
            opts.shortline_max_ms = *parsed;
        }
        else if (arg == "--seed")
        {
            auto value = require_value(arg);
            if (!value)
                return std::nullopt;
            auto parsed = parse_numeric<uint32_t>(*value);
            if (!parsed)
                return std::nullopt;
            opts.seed = *parsed;
        }
        else if (arg == "--verbose-failures")
        {
            opts.verbose_failures = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage();
            std::exit(0);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }

    if (!have_login || !have_password_hash)
    {
        return std::nullopt;
    }

    if (opts.account_pool == 0)
    {
        opts.account_pool = opts.clients;
    }

    if (opts.hold_max_ms < opts.hold_min_ms)
    {
        std::swap(opts.hold_max_ms, opts.hold_min_ms);
    }
    if (opts.shortline_max_ms < opts.shortline_min_ms)
    {
        std::swap(opts.shortline_max_ms, opts.shortline_min_ms);
    }
    return opts;
}

auto percentile_ms(std::vector<double> values, double pct) -> double
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::clamp<double>((pct / 100.0) * static_cast<double>(values.size() - 1), 0.0,
                           static_cast<double>(values.size() - 1)));
    return values[index];
}

void print_progress(const Options& opts, const Metrics& metrics,
                    const std::vector<Session>& sessions, TimePoint started_at, TimePoint now)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count();
    const auto online = static_cast<std::size_t>(std::count_if(
        sessions.begin(), sessions.end(), [](const Session& s) { return s.is_online(); }));
    const auto inflight = static_cast<std::size_t>(std::count_if(
        sessions.begin(), sessions.end(), [](const Session& s) { return s.is_inflight(); }));

    std::cout << std::format(
        "[{:>4}s] started={} login_ok={} auth_ok={} login_fail={} auth_fail={} timeouts={} "
        "online={} inflight={} planned_disc={} unexpected_disc={}\n",
        elapsed, metrics.login_started, metrics.login_result_success, metrics.auth_success,
        metrics.login_result_fail, metrics.auth_fail, metrics.timeout_fail, online, inflight,
        metrics.planned_disconnects, metrics.unexpected_disconnects);
    (void)opts;
}

void print_summary(const Options& opts, const Metrics& metrics,
                   const std::vector<Session>& sessions)
{
    const auto online = static_cast<std::size_t>(std::count_if(
        sessions.begin(), sessions.end(), [](const Session& s) { return s.is_online(); }));

    std::cout << "\nSummary\n";
    std::cout << std::format("  clients:            {}\n", opts.clients);
    std::cout << std::format("  account_pool:       {}\n", opts.account_pool);
    std::cout << std::format("  login_started:      {}\n", metrics.login_started);
    std::cout << std::format("  login_success:      {}\n", metrics.login_result_success);
    std::cout << std::format("  auth_success:       {}\n", metrics.auth_success);
    std::cout << std::format("  login_fail:         {}\n", metrics.login_result_fail);
    std::cout << std::format("  auth_fail:          {}\n", metrics.auth_fail);
    std::cout << std::format("  timeout_fail:       {}\n", metrics.timeout_fail);
    std::cout << std::format("  planned_disconnect: {}\n", metrics.planned_disconnects);
    std::cout << std::format("  unexpected_disc:    {}\n", metrics.unexpected_disconnects);
    std::cout << std::format("  online_at_end:      {}\n", online);

    if (!metrics.auth_latency_ms.empty())
    {
        std::cout << std::format("  auth_latency_p50:   {:.2f} ms\n",
                                 percentile_ms(metrics.auth_latency_ms, 50.0));
        std::cout << std::format("  auth_latency_p95:   {:.2f} ms\n",
                                 percentile_ms(metrics.auth_latency_ms, 95.0));
        std::cout << std::format("  auth_latency_p99:   {:.2f} ms\n",
                                 percentile_ms(metrics.auth_latency_ms, 99.0));
    }

    if (!metrics.failure_reasons.empty())
    {
        std::vector<std::pair<std::string, std::size_t>> failures(metrics.failure_reasons.begin(),
                                                                  metrics.failure_reasons.end());
        std::sort(failures.begin(), failures.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::cout << "  top_failures:\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(10, failures.size()); ++i)
        {
            std::cout << std::format("    {}: {}\n", failures[i].first, failures[i].second);
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    const auto opts = parse_options(argc, argv);
    if (!opts)
    {
        print_usage();
        return 1;
    }

    EventDispatcher dispatcher("login_stress");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    Metrics metrics;
    std::mt19937 rng(opts->seed);
    std::vector<Session> sessions;
    sessions.reserve(opts->clients);

    const auto started_at = SteadyClock::now();
    const auto ramp_interval_ms = std::max<std::size_t>(
        1, static_cast<std::size_t>(1000 / std::max<std::size_t>(1, opts->ramp_per_sec)));

    for (std::size_t i = 0; i < opts->clients; ++i)
    {
        const auto account_index = i % opts->account_pool;
        auto username = std::format("{}{}", opts->username_prefix, account_index);
        sessions.emplace_back(i, std::move(username), dispatcher, *opts, metrics, rng);
        sessions.back().schedule_initial(started_at +
                                         std::chrono::milliseconds(i * ramp_interval_ms));
    }

    std::cout << std::format(
        "Starting login_stress: login={} clients={} account_pool={} ramp/s={} duration={}s "
        "shortline={}%% seed={}\n",
        opts->login_addr.to_string(), opts->clients, opts->account_pool, opts->ramp_per_sec,
        opts->duration_sec, opts->shortline_pct, opts->seed);

    auto next_progress = started_at + std::chrono::seconds(1);
    const auto deadline = started_at + std::chrono::seconds(opts->duration_sec);

    while (SteadyClock::now() < deadline)
    {
        dispatcher.process_once();

        const auto now = SteadyClock::now();
        for (auto& session : sessions)
        {
            session.update(now);
        }

        if (now >= next_progress)
        {
            print_progress(*opts, metrics, sessions, started_at, now);
            next_progress += std::chrono::seconds(1);
        }
    }

    print_summary(*opts, metrics, sessions);
    return 0;
}
