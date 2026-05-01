#ifndef ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_
#define ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_

#include <cstdint>
#include <string_view>

namespace atlas::world_stress {

// ============================================================================
// ClientEventCounters / ParseAndCount — stdout line → event-kind counter.
//
// The harness launches atlas_client.exe children that load samples/client/.
// Script hooks emit parse-stable lines like
//     "[StressAvatar:42] OnHpChanged old=100 new=99"
// (see samples/client/StressAvatar.cs). This tap turns them into per-child
// counters the harness summarises and uses for pass/fail decisions.
//
// Deliberately permissive: unrecognized lines bump `unparsed_lines` so log
// noise and stack traces pass through without crashing the harness.
// ============================================================================

struct ClientEventCounters {
  uint64_t on_init{0};
  uint64_t on_enter_world{0};
  uint64_t on_destroy{0};
  uint64_t on_hp_changed{0};
  uint64_t on_position_updated{0};
  // Component-coverage counters. These fire from server-side periodic
  // RPCs that exercise the component RPC envelope (slot-encoded rpc_id)
  // and container/struct RPC arg encoding. Non-zero counts are the
  // success signal that component RPC end-to-end works.
  uint64_t on_main_weapon_changed{0};  // entity-level struct prop
  uint64_t on_weapon_broken{0};        // server→client struct-arg RPC
  uint64_t on_scores_snapshot{0};      // server→client list-arg RPC
  uint64_t on_affixes_updated{0};      // server→client component RPC
  uint64_t on_area_broadcast{0};       // server→client RpcTarget.All RPC
  // Count of property deltas missed in the middle of a reliable stream.
  // Populated from the `event_seq gap: last=A got=B missed=N` warning
  // lines ClientEntity emits when the incoming seq jumps by more than 1.
  // One log line contributes N (lost events, not warning count).
  uint64_t event_seq_gaps{0};
  uint64_t unparsed_lines{0};
};

// Inspect `line` and bump the matching counter in `out`. Returns true if
// `line` was recognized as a tracked event. The line contract matches
// samples/client/StressAvatar.cs — update both sides together.
auto ParseAndCountClientEventLine(std::string_view line, ClientEventCounters& out) -> bool;

}  // namespace atlas::world_stress

#endif  // ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_
