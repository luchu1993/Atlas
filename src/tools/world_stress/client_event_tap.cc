#include "client_event_tap.h"

namespace atlas::world_stress {

namespace {

// Match "] <Event>" where `<Event>` is the exact token after the closing
// bracket of "[<TypeName>:<EntityId>]". We don't parse the entity id itself
// — the harness only aggregates per-child counts today; the id is noise at
// this layer. Kept as positional substring lookup (no regex) so the tap
// stays header-only-friendly and has zero startup cost per line.
auto EventBegins(std::string_view line) -> std::string_view {
  // Find the first closing bracket. If this line doesn't have the
  // "[<Type>:<Id>] <Event>" shape, return empty.
  auto close = line.find(']');
  if (close == std::string_view::npos) return {};
  auto rest = line.substr(close + 1);
  // Skip exactly one space after the bracket (the log format always emits
  // one — anything else is treated as unparsed).
  if (rest.empty() || rest.front() != ' ') return {};
  rest.remove_prefix(1);
  return rest;
}

auto StartsWithToken(std::string_view rest, std::string_view token) -> bool {
  // Matches when `rest` starts with `token` AND the next byte is absent or
  // not part of an identifier (so "OnInit" doesn't match "OnInitExtra").
  if (rest.size() < token.size()) return false;
  if (rest.substr(0, token.size()) != token) return false;
  if (rest.size() == token.size()) return true;
  char c = rest[token.size()];
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':';
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

  ++out.unparsed_lines;
  return false;
}

}  // namespace atlas::world_stress
