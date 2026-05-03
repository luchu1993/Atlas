#include "script_clients.h"

#include <chrono>
#include <format>
#include <iostream>

namespace atlas::world_stress {

ScriptClientHarness::ScriptClientHarness(ScriptClientOptions opts) : opts_(std::move(opts)) {}

ScriptClientHarness::~ScriptClientHarness() = default;

auto ScriptClientHarness::Start() -> Result<void> {
  if (opts_.count == 0) return {};  // nothing to do

  if (opts_.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "--client-exe is required when --script-clients > 0"};
  }
  if (opts_.assembly.empty()) {
    return Error{ErrorCode::kInvalidArgument,
                 "--client-assembly is required when --script-clients > 0"};
  }

  children_.reserve(opts_.count);
  for (std::size_t i = 0; i < opts_.count; ++i) {
    const auto idx = opts_.username_index_base + i;
    auto username = std::format("{}{}", opts_.username_prefix, idx);

    ChildProcess::Options copts;
    copts.exe = opts_.exe;
    copts.args = {
        "--loginapp-host", opts_.login_host,
        "--loginapp-port", std::to_string(opts_.login_port),
        "--username",      username,
        "--password",      opts_.password_hash,
        "--assembly",      opts_.assembly.string(),
    };
    if (!opts_.runtime_config.empty()) {
      copts.args.emplace_back("--runtime-config");
      copts.args.emplace_back(opts_.runtime_config.string());
    }
    if (opts_.drop_inbound_duration_ms > 0) {
      copts.args.emplace_back("--drop-inbound-ms");
      copts.args.emplace_back(std::to_string(opts_.drop_inbound_start_ms));
      copts.args.emplace_back(std::to_string(opts_.drop_inbound_duration_ms));
    }
    if (opts_.drop_transport_duration_ms > 0) {
      copts.args.emplace_back("--drop-transport-ms");
      copts.args.emplace_back(std::to_string(opts_.drop_transport_start_ms));
      copts.args.emplace_back(std::to_string(opts_.drop_transport_duration_ms));
    }

    auto r = ChildProcess::Start(std::move(copts));
    if (!r.HasValue()) {
      // Best-effort: tear down any children we already spawned before
      // bubbling the error up so the harness doesn't leak zombies.
      children_.clear();
      return Error{
          ErrorCode::kInternalError,
          std::format("ScriptClientHarness: spawn child {} failed: {}", idx, r.Error().Message())};
    }
    children_.push_back(Child{std::move(username), std::move(*r), ClientEventCounters{}});
  }

  std::cout << std::format("[script-clients] launched {} subprocess(es)\n", children_.size());
  return {};
}

void ScriptClientHarness::Pump() {
  for (auto& child : children_) {
    while (auto line = child.proc.TryReadStdoutLine()) {
      ParseAndCountClientEventLine(*line, child.counters);
    }
  }
}

void ScriptClientHarness::ShutdownAndWait(std::chrono::milliseconds timeout) {
  for (auto& child : children_) child.proc.Kill();

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (auto& child : children_) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    (void)child.proc.Wait(ms.count() > 0 ? ms : std::chrono::milliseconds(0));
    // Drain any stdout that landed after Kill() but before reap.
    while (auto line = child.proc.TryReadStdoutLine()) {
      ParseAndCountClientEventLine(*line, child.counters);
    }
  }
}

auto ScriptClientHarness::PrintSummary() const -> bool {
  if (children_.empty()) return true;

  std::cout << "\n[script-clients] per-child event summary:\n";
  std::cout << std::format(
      "  {:<24}  {:>5}  {:>5}  {:>3}  {:>4}  {:>5}  {:>4}  {:>4}  {:>4}  {:>4}  {:>4}  {:>5}  "
      "{:>5}\n",
      "username", "init", "enter", "dst", "hp", "wepCh", "wepBr", "scrSn", "afxUp", "bcst", "pos",
      "gaps", "unprs");

  bool all_ok = true;
  for (const auto& child : children_) {
    const auto& c = child.counters;
    std::cout << std::format(
        "  {:<24}  {:>5}  {:>5}  {:>3}  {:>4}  {:>5}  {:>4}  {:>5}  {:>5}  {:>4}  {:>4}  {:>5}  "
        "{:>5}\n",
        child.username, c.on_init, c.on_enter_world, c.on_destroy, c.on_hp_changed,
        c.on_main_weapon_changed, c.on_weapon_broken, c.on_scores_snapshot, c.on_affixes_updated,
        c.on_area_broadcast, c.on_position_updated, c.event_seq_gaps, c.unparsed_lines);
    if (opts_.verify) {
      // Minimum bar: the child's own StressAvatar at least got created.
      // OnEnterWorld is nice-to-have (depends on AoI peer or baseline
      // delivery); we accept a child that OnInit'd even if no OnEnterWorld
      // fired to keep the harness useful at --script-clients 1.
      if (c.on_init == 0) {
        all_ok = false;
        std::cout << std::format("    └── FAIL: no OnInit observed for {}\n", child.username);
      }
    }
  }
  return all_ok;
}

}  // namespace atlas::world_stress
