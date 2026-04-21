#ifndef ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_
#define ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_

#include <cstdint>
#include <string_view>

namespace atlas::world_stress {

// ============================================================================
// ClientEventCounters / ParseAndCount — stdout line → event-kind counter.
//
// The Phase C2 harness launches real atlas_client.exe children that load
// samples/client/. Every Phase-B script hook there logs a parse-stable line
// like
//     "[StressAvatar:42] OnHpChanged old=100 new=99"
// — see samples/client/StressAvatar.cs. This tap turns those lines into
// per-child counters the harness prints at the end of a run and uses to
// decide whether the Phase-B event contract was honoured end-to-end.
//
// Deliberately permissive: anything that isn't a recognized event line just
// bumps `unparsed_lines` — C# breadcrumbs, log noise, stack traces all flow
// through without crashing the harness.
// ============================================================================

struct ClientEventCounters {
  uint64_t on_init{0};
  uint64_t on_enter_world{0};
  uint64_t on_destroy{0};
  uint64_t on_hp_changed{0};
  uint64_t on_position_updated{0};
  // Phase D2'.3: how many property deltas the client missed in the
  // middle of a reliable stream. Populated from the `event_seq gap:
  // last=A got=B missed=N` warning lines that ClientEntity emits to
  // Console.Error when the incoming seq jumps by more than 1. A single
  // log line contributes N to the counter — operators care about lost
  // events, not the count of warning lines.
  uint64_t event_seq_gaps{0};
  uint64_t unparsed_lines{0};
};

// Inspect `line` and bump the matching counter in `out`. Returns true if
// `line` was recognized as a tracked event. The line contract matches
// samples/client/StressAvatar.cs — update both sides together.
auto ParseAndCountClientEventLine(std::string_view line, ClientEventCounters& out) -> bool;

}  // namespace atlas::world_stress

#endif  // ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_
