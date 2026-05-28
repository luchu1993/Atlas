#ifndef ATLAS_TESTS_PARITY_PARITY_RUNNER_H_
#define ATLAS_TESTS_PARITY_PARITY_RUNNER_H_

#include <vector>

#include "parity_scenario.h"

namespace atlas::physics::parity {

// Returns an empty vector when the backend is not viable in this build
// (typically Jolt with ATLAS_ENABLE_JOLT=OFF).
[[nodiscard]] auto RunScenario(const ParityScenario& scenario, BackendKind backend)
    -> std::vector<PerTickRecord>;

[[nodiscard]] auto ComparePair(const ParityScenario& scenario,
                               const std::vector<PerTickRecord>& a,
                               const std::vector<PerTickRecord>& b) -> ParityResult;

}  // namespace atlas::physics::parity

#endif  // ATLAS_TESTS_PARITY_PARITY_RUNNER_H_
