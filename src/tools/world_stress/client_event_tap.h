#ifndef ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_
#define ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_

#include <cstdint>
#include <string_view>

namespace atlas::world_stress {

struct ClientEventCounters {
  uint64_t on_init{0};
  uint64_t on_enter_world{0};
  uint64_t on_destroy{0};
  uint64_t on_hp_changed{0};
  uint64_t on_position_updated{0};
  // Component coverage from server-side periodic RPCs and prop changes.
  uint64_t on_main_weapon_changed{0};  // entity-level struct prop
  uint64_t on_weapon_broken{0};        // server-to-client struct-arg RPC
  uint64_t on_scores_snapshot{0};      // server-to-client list-arg RPC
  uint64_t on_affixes_updated{0};      // server-to-client component RPC
  uint64_t on_area_broadcast{0};       // server-to-client RpcTarget.All RPC
  uint64_t movement_input_sent{0};
  uint64_t movement_ack{0};
  uint64_t movement_report_sent{0};
  uint64_t movement_correction_tier1{0};
  uint64_t movement_correction_tier2{0};
  uint64_t movement_correction_snap{0};
  // Lost reliable deltas parsed from `event_seq gap` warnings.
  uint64_t event_seq_gaps{0};
  uint64_t unparsed_lines{0};
};

auto ParseAndCountClientEventLine(std::string_view line, ClientEventCounters& out) -> bool;
auto ClientEventCountersPassScriptVerify(const ClientEventCounters& counters) -> bool;

}  // namespace atlas::world_stress

#endif  // ATLAS_TOOLS_WORLD_STRESS_CLIENT_EVENT_TAP_H_
