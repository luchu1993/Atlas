#include "client_event_tap.h"

namespace atlas::world_stress {

namespace {

// Match "] <Event>" where `<Event>` is the exact token after the closing
// bracket of "[<TypeName>:<EntityId>]". We don't parse the entity id itself
// — the harness only aggregates per-child counts today; the id is noise at
// this layer. Kept as positional substring lookup (no regex) so the tap
// stays header-only-friendly and has zero startup cost per line.
//
// L6: client logs now emit `[t=<seconds>] [<Type>:<Id>] <Event>` — the
// leading timestamp bracket is stripped here before the type:id bracket
// parse so existing counters work unchanged. The raw value is not kept
// by the tap (analysis scripts lift it back out with a regex) — the
// benefit is purely that convergence timing is recoverable post-run.
auto EventBegins(std::string_view line) -> std::string_view {
  // Strip optional leading timestamp bracket: `[t=12.345] ` (prefix +
  // exactly one space). If the line doesn't start with `[t=` the line
  // is pre-L6 format and drops through untouched.
  if (line.size() >= 4 && line[0] == '[' && line[1] == 't' && line[2] == '=') {
    auto end = line.find("] ");
    if (end == std::string_view::npos) return {};
    line = line.substr(end + 2);
  }
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

  // event_seq gap warning — Phase D2'.3. Format (from
  // ClientEntity.NoteIncomingEventSeq):
  //   [<Type>:<Id>] event_seq gap: last=A got=B missed=N
  // We add N to the counter so it reflects the number of lost deltas,
  // not the number of warning lines.
  if (StartsWithToken(rest, "event_seq gap")) {
    auto missed_pos = rest.find("missed=");
    if (missed_pos != std::string_view::npos) {
      auto digits = rest.substr(missed_pos + 7);
      uint64_t n = 0;
      for (char c : digits) {
        if (c < '0' || c > '9') break;
        n = n * 10 + static_cast<uint64_t>(c - '0');
      }
      out.event_seq_gaps += n;
      return true;
    }
    // Well-formed prefix but missing/mangled `missed=` → still count
    // the line as recognized so it doesn't inflate unparsed_lines, but
    // we can't add any missed count.
    return true;
  }

  ++out.unparsed_lines;
  return false;
}

}  // namespace atlas::world_stress
