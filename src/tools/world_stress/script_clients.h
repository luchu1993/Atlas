#ifndef ATLAS_TOOLS_WORLD_STRESS_SCRIPT_CLIENTS_H_
#define ATLAS_TOOLS_WORLD_STRESS_SCRIPT_CLIENTS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "client_event_tap.h"
#include "foundation/error.h"
#include "platform/child_process.h"

namespace atlas::world_stress {

// ============================================================================
// ScriptClientHarness — runs N real atlas_client.exe children alongside the
// stress harness's virtual clients and verifies the Phase-B callback contract
// end-to-end via stdout greps.
//
// Lifecycle from main.cc's point of view:
//     harness.Start()            // before the main loop
//     while (running) {
//         harness.Pump();        // every iteration, after ProcessOnce
//         ...
//     }
//     harness.ShutdownAndWait(3s);
//     bool ok = harness.PrintSummary();
// ============================================================================

struct ScriptClientOptions {
  std::filesystem::path exe;             // atlas_client.exe
  std::filesystem::path assembly;        // Atlas.ClientSample.dll
  std::filesystem::path runtime_config;  // hostfxr runtime.json (optional)
  std::string login_host;                // forwarded to atlas_client --loginapp-host
  uint16_t login_port{0};
  std::string password_hash;    // forwarded as --password
  std::string username_prefix;  // each child gets `<prefix><N>` for N in 0..count
  std::size_t username_index_base{0};
  std::size_t count{0};  // zero → harness is inert
  bool verify{false};    // PrintSummary returns false on threshold miss
  // Phase C3: forwarded to each child as `--drop-inbound-ms start duration`.
  // 0/0 = off.
  int drop_inbound_start_ms{0};
  int drop_inbound_duration_ms{0};
};

class ScriptClientHarness {
 public:
  explicit ScriptClientHarness(ScriptClientOptions opts);
  ~ScriptClientHarness();

  ScriptClientHarness(const ScriptClientHarness&) = delete;
  ScriptClientHarness& operator=(const ScriptClientHarness&) = delete;

  [[nodiscard]] auto Start() -> Result<void>;

  // Drain buffered stdout lines from every child into its counters.
  // Non-blocking; safe to call from a hot loop.
  void Pump();

  // Signal SIGTERM / TerminateProcess to every child and wait up to
  // `timeout` total for them to reap. Idempotent.
  void ShutdownAndWait(std::chrono::milliseconds timeout);

  // True if every child met the observation threshold (>= 1 OnInit). Returns
  // trivially true when verify is false (the harness just reports, does not
  // gate). Called after ShutdownAndWait for the final exit-code decision.
  auto PrintSummary() const -> bool;

  [[nodiscard]] auto Count() const -> std::size_t { return children_.size(); }

 private:
  struct Child {
    std::string username;
    atlas::ChildProcess proc;
    ClientEventCounters counters;
  };

  ScriptClientOptions opts_;
  std::vector<Child> children_;
};

}  // namespace atlas::world_stress

#endif  // ATLAS_TOOLS_WORLD_STRESS_SCRIPT_CLIENTS_H_
