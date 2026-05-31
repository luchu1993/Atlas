#ifndef ATLAS_LIB_SERVER_REVIVER_SUBJECT_H_
#define ATLAS_LIB_SERVER_REVIVER_SUBJECT_H_

#include <cstdint>
#include <iterator>
#include <unordered_map>

#include "foundation/clock.h"
#include "network/address.h"

namespace atlas {

// BigWorld-style reviver arbitration (lib/server/reviver_subject.cpp): the
// monitored Manager is the subject. Each Reviver pings it carrying a static
// priority; the subject designates the highest-priority pinger seen within the
// timeout window as the single active monitor and echoes that verdict back.
// No fencing token — a dead/lagging Reviver simply ages out of the window.
class ReviverSubject {
 public:
  static constexpr Duration kDefaultTimeout =
      std::chrono::duration_cast<Duration>(std::chrono::seconds(3));

  void SetTimeout(Duration timeout) { timeout_ = timeout; }

  // Records a ping and returns whether `reviver` is now the active monitor —
  // the live pinger with the highest priority (ties broken by lowest address
  // so all subjects converge on the same choice).
  [[nodiscard]] auto RecordPingAndIsActive(const Address& reviver, uint8_t priority, TimePoint now)
      -> bool {
    pingers_.insert_or_assign(reviver, Pinger{priority, now});
    Prune(now);
    const Address* best = nullptr;
    uint8_t best_priority = 0;
    for (const auto& [addr, p] : pingers_) {
      if (best == nullptr || p.priority > best_priority ||
          (p.priority == best_priority && addr < *best)) {
        best = &addr;
        best_priority = p.priority;
      }
    }
    return best != nullptr && *best == reviver;
  }

  [[nodiscard]] auto LivePingerCount(TimePoint now) -> std::size_t {
    Prune(now);
    return pingers_.size();
  }

 private:
  struct Pinger {
    uint8_t priority{0};
    TimePoint last_ping{};
  };

  void Prune(TimePoint now) {
    for (auto it = pingers_.begin(); it != pingers_.end();) {
      it = (now - it->second.last_ping >= timeout_) ? pingers_.erase(it) : std::next(it);
    }
  }

  std::unordered_map<Address, Pinger> pingers_;
  Duration timeout_{kDefaultTimeout};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_REVIVER_SUBJECT_H_
