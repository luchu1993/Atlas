#include "client_event_tap.h"

namespace atlas::world_stress {

namespace {

// Extract the event token after "[<TypeName>:<EntityId>] ".
// Optional "[t=<seconds>] " prefixes are stripped first.
auto EventBegins(std::string_view line) -> std::string_view {
  if (line.size() >= 4 && line[0] == '[' && line[1] == 't' && line[2] == '=') {
    auto end = line.find("] ");
    if (end == std::string_view::npos) return {};
    line = line.substr(end + 2);
  }
  auto close = line.find(']');
  if (close == std::string_view::npos) return {};
  auto rest = line.substr(close + 1);
  if (rest.empty() || rest.front() != ' ') return {};
  rest.remove_prefix(1);
  return rest;
}

auto StartsWithToken(std::string_view rest, std::string_view token) -> bool {
  if (rest.size() < token.size()) return false;
  if (rest.substr(0, token.size()) != token) return false;
  if (rest.size() == token.size()) return true;
  char c = rest[token.size()];
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':';
}

auto ParseUnsignedField(std::string_view rest, std::string_view field, uint64_t& out) -> bool {
  auto pos = rest.find(field);
  if (pos == std::string_view::npos) return false;
  auto digits = rest.substr(pos + field.size());
  uint64_t value = 0;
  bool has_digit = false;
  for (char c : digits) {
    if (c < '0' || c > '9') break;
    has_digit = true;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (!has_digit) return false;
  out = value;
  return true;
}

}  // namespace

auto ParseAndCountClientEventLine(std::string_view line, ClientEventCounters& out) -> bool {
  auto rest = EventBegins(line);
  if (rest.empty()) {
    ++out.unparsed_lines;
    return false;
  }

  if (StartsWithToken(rest, "OnInit")) {
    ++out.on_init;
    return true;
  }
  if (StartsWithToken(rest, "OnEnterWorld")) {
    ++out.on_enter_world;
    return true;
  }
  if (StartsWithToken(rest, "OnDestroy")) {
    ++out.on_destroy;
    return true;
  }
  if (StartsWithToken(rest, "OnHpChanged")) {
    ++out.on_hp_changed;
    return true;
  }
  if (StartsWithToken(rest, "OnPositionUpdated")) {
    ++out.on_position_updated;
    return true;
  }
  if (StartsWithToken(rest, "OnMainWeaponChanged")) {
    ++out.on_main_weapon_changed;
    return true;
  }
  if (StartsWithToken(rest, "OnWeaponBroken")) {
    ++out.on_weapon_broken;
    return true;
  }
  if (StartsWithToken(rest, "OnScoresSnapshot")) {
    ++out.on_scores_snapshot;
    return true;
  }
  if (StartsWithToken(rest, "OnAffixesUpdated")) {
    ++out.on_affixes_updated;
    return true;
  }
  if (StartsWithToken(rest, "OnAreaBroadcast")) {
    ++out.on_area_broadcast;
    return true;
  }
  if (StartsWithToken(rest, "OnMovementInputSent")) {
    ++out.movement_input_sent;
    return true;
  }
  if (StartsWithToken(rest, "OnMovementCorrectionReportSent")) {
    ++out.movement_report_sent;
    return true;
  }
  if (StartsWithToken(rest, "OnMovementCorrection")) {
    ++out.movement_ack;
    uint64_t tier = 0;
    if (ParseUnsignedField(rest, "tier=", tier)) {
      if (tier == 1) {
        ++out.movement_correction_tier1;
      } else if (tier == 2) {
        ++out.movement_correction_tier2;
      } else if (tier == 3) {
        ++out.movement_correction_snap;
      }
    }
    return true;
  }

  if (StartsWithToken(rest, "event_seq gap")) {
    uint64_t n = 0;
    if (ParseUnsignedField(rest, "missed=", n)) {
      out.event_seq_gaps += n;
      return true;
    }
    return true;
  }

  ++out.unparsed_lines;
  return false;
}

auto ClientEventCountersPassScriptVerify(const ClientEventCounters& counters) -> bool {
  return counters.on_init > 0 && counters.movement_input_sent > 0 && counters.movement_ack > 0 &&
      counters.movement_report_sent > 0;
}

}  // namespace atlas::world_stress
