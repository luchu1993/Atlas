#ifndef ATLAS_TESTS_PARITY_PARITY_RUNNER_H_
#define ATLAS_TESTS_PARITY_PARITY_RUNNER_H_

#include <vector>

#include "parity_scenario.h"

namespace atlas::physics::parity {

// Drives movement_sim::Step through a PhysicsCharacterQuery on the given
// backend. Returns an empty vector if the scenario does not list the backend
// or its make_query returns nullptr (typically Jolt with ATLAS_ENABLE_JOLT=OFF).
[[nodiscard]] auto RunScenario(const ParityScenario& scenario, BackendKind backend)
    -> std::vector<PerTickRecord>;

// Per-tick + cumulative drift check between two runs of the same scenario.
[[nodiscard]] auto ComparePair(const ParityScenario& scenario,
                               const std::vector<PerTickRecord>& a,
                               const std::vector<PerTickRecord>& b) -> ParityResult;

}  // namespace atlas::physics::parity

#endif  // ATLAS_TESTS_PARITY_PARITY_RUNNER_H_
