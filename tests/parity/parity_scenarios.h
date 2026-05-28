#ifndef ATLAS_TESTS_PARITY_PARITY_SCENARIOS_H_
#define ATLAS_TESTS_PARITY_PARITY_SCENARIOS_H_

#include <vector>

#include "parity_scenario.h"

namespace atlas::physics::parity {

// Quick-gate scenarios, in execution order.
[[nodiscard]] auto AllScenarios() -> std::vector<ParityScenario>;

}  // namespace atlas::physics::parity

#endif  // ATLAS_TESTS_PARITY_PARITY_SCENARIOS_H_
