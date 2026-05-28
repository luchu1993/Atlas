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

struct ScriptClientOptions {
  std::filesystem::path exe;             // atlas_client.exe
  std::filesystem::path assembly;        // Atlas.ClientSample.dll
  std::filesystem::path runtime_config;  // hostfxr runtime.json (optional)
  std::string login_host;                // forwarded to atlas_client --loginapp-host
  uint16_t login_port{0};
  std::string password_hash;    // forwarded as --password
  std::string username_prefix;  // each child gets `<prefix><N>` for N in 0..count
  std::size_t username_index_base{0};
  std::size_t count{0};  // zero means the harness is inert
  bool verify{false};    // PrintSummary returns false on threshold miss.
  // Forwarded to each child as `--drop-inbound-ms start duration`. 0/0 = off.
  int drop_inbound_start_ms{0};
  int drop_inbound_duration_ms{0};
  // Forwarded as `--drop-transport-ms start duration`. Validates RUDP recovery;
  // use drop_inbound_* for app-level gap detection + baseline fallback.
  int drop_transport_start_ms{0};
  int drop_transport_duration_ms{0};
  int transport_impairment_latency_ms{0};
  int transport_impairment_loss_permyriad{0};
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

  // Returns true when verify is false or every child passed the script smoke gate.
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
