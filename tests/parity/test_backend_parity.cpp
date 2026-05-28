#include <map>

#include <gtest/gtest.h>

#include "parity_runner.h"
#include "parity_scenarios.h"

#ifdef ATLAS_PARITY_HAS_JOLT
#include "physics_jolt/jolt_init.h"
#endif

namespace atlas::physics::parity {

namespace {

class JoltEnv : public ::testing::Environment {
 public:
  void SetUp() override {
#ifdef ATLAS_PARITY_HAS_JOLT
    ASSERT_TRUE(jolt::Initialize());
#endif
  }
  void TearDown() override {
#ifdef ATLAS_PARITY_HAS_JOLT
    jolt::Shutdown();
#endif
  }
};

constexpr BackendKind kAllBackends[] = {BackendKind::kFlat, BackendKind::kStatic,
                                        BackendKind::kJolt};

class BackendParityTest : public ::testing::TestWithParam<ParityScenario> {};

TEST_P(BackendParityTest, ScenarioPassesAllPairs) {
  const auto& scenario = GetParam();

  std::map<BackendKind, std::vector<PerTickRecord>> runs;
  for (auto kind : kAllBackends) {
    auto records = RunScenario(scenario, kind);
    if (!records.empty()) runs[kind] = std::move(records);
  }
  if (runs.size() < 2) {
    GTEST_SKIP() << scenario.id
                 << ": fewer than 2 viable backends in this build (typical when "
                    "ATLAS_ENABLE_JOLT=OFF and the scenario excludes Flat)";
  }

  bool any_failure = false;
  for (auto a_it = runs.begin(); a_it != runs.end(); ++a_it) {
    for (auto b_it = std::next(a_it); b_it != runs.end(); ++b_it) {
      auto result = ComparePair(scenario, a_it->second, b_it->second);
      if (!result.passed) {
        any_failure = true;
        ADD_FAILURE() << scenario.id << " " << Name(a_it->first) << " vs "
                      << Name(b_it->first) << ": " << result.diff_summary;
      }
    }
  }
  EXPECT_FALSE(any_failure);
}

INSTANTIATE_TEST_SUITE_P(All, BackendParityTest,
                         ::testing::ValuesIn(AllScenarios()),
                         [](const ::testing::TestParamInfo<ParityScenario>& info) {
                           return std::string(info.param.id);
                         });

// Registered via static init so the default gtest_main picks it up before
// RUN_ALL_TESTS — keeps the test under the standard atlas_add_test helper.
const auto* g_jolt_env [[maybe_unused]] =
    ::testing::AddGlobalTestEnvironment(new JoltEnv);

}  // namespace
}  // namespace atlas::physics::parity
