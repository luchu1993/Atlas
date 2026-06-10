#include <filesystem>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include "navigation/nav_query.h"
#include "navigation_recast/recast_nav_backend.h"
#include "platform/filesystem.h"
#include "space.h"
#include "space/controllers.h"
#include "space/entity_motion.h"
#include "space/move_along_path_controller.h"

// End-to-end: collision asset + nav params on disk → Space::LoadNavMeshFromFiles
// through the Recast backend factory → Space::NavQuery() answers FindPath.
namespace atlas {
namespace {

constexpr std::string_view kFloorCollision = R"({
  "version": 1,
  "coordinate_system": "x_right_y_up_z_forward_meters",
  "source_hash": "navfloor",
  "objects": [
    {"shape": "box", "min": [-10, -1, -10], "max": [10, 0, 10], "layer": 0}
  ]
})";

constexpr std::string_view kNavParams = R"({
  "version": 1,
  "coordinate_system": "x_right_y_up_z_forward_meters",
  "source_hash": "navfloor"
})";

struct NavFixtureFiles {
  std::filesystem::path collision;
  std::filesystem::path params;

  NavFixtureFiles() {
    collision = fs::TempDirectory() / "atlas_nav_pipeline.collision.json";
    params = fs::TempDirectory() / "atlas_nav_pipeline.nav.json";
    EXPECT_TRUE(fs::WriteTextFile(collision, std::string(kFloorCollision)).HasValue());
    EXPECT_TRUE(fs::WriteTextFile(params, std::string(kNavParams)).HasValue());
  }
  ~NavFixtureFiles() {
    std::filesystem::remove(collision);
    std::filesystem::remove(params);
  }
};

TEST(NavPipeline, DefaultQueryIsNull) {
  Space space(1);
  const nav::NavQueryFilter filter;
  const auto path = space.NavQuery().FindPath({0, 0, 0}, {5, 0, 5}, filter);
  EXPECT_EQ(path.status, nav::NavPathStatus::kEmpty);
}

TEST(NavPipeline, LoadWithoutBackendIsRejected) {
  const NavFixtureFiles files;
  Space space(1);
  const auto result = space.LoadNavMeshFromFiles(files.collision, files.params);
  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kNotSupported);
}

TEST(NavPipeline, BakedSpaceAnswersFindPath) {
  const NavFixtureFiles files;
  Space space(1);
  space.SetNavBackendFactory(std::make_shared<nav::RecastNavBackendFactory>());
  const auto result = space.LoadNavMeshFromFiles(files.collision, files.params);
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_EQ(space.NavSourceHash(), "navfloor");

  const nav::NavQueryFilter filter;
  const auto path = space.NavQuery().FindPath({-8, 0, -8}, {8, 0, 8}, filter);
  EXPECT_EQ(path.status, nav::NavPathStatus::kReached);
  EXPECT_GT(path.length_m, 20.0f);
  EXPECT_LT(path.length_m, 40.0f);

  // Replacing the query manually clears the asset bookkeeping.
  space.SetNavQuery(std::make_unique<nav::NullNavQuery>());
  EXPECT_TRUE(space.NavSourceHash().empty());
  EXPECT_EQ(space.NavQuery().FindPath({-8, 0, -8}, {8, 0, 8}, filter).status,
            nav::NavPathStatus::kEmpty);
}

// The MoveTo consumption pattern: plan on the space's navmesh, then walk the
// returned waypoints with MoveAlongPathController.
TEST(NavPipeline, NavPathDrivesMoveAlongPathController) {
  const NavFixtureFiles files;
  Space space(1);
  space.SetNavBackendFactory(std::make_shared<nav::RecastNavBackendFactory>());
  ASSERT_TRUE(space.LoadNavMeshFromFiles(files.collision, files.params).HasValue());

  const nav::NavQueryFilter filter;
  auto path = space.NavQuery().FindPath({-8, 0, -8}, {8, 0, 8}, filter);
  ASSERT_EQ(path.status, nav::NavPathStatus::kReached);

  class MotionStub final : public IEntityMotion {
   public:
    [[nodiscard]] auto Position() const -> const math::Vector3& override { return pos_; }
    void SetPosition(const math::Vector3& p) override { pos_ = p; }
    [[nodiscard]] auto Direction() const -> const math::Vector3& override { return dir_; }
    void SetDirection(const math::Vector3& d) override { dir_ = d; }

   private:
    math::Vector3 pos_{-8, 0, -8};
    math::Vector3 dir_{0, 0, 1};
  } motion;

  Controllers ctrls;
  const auto id = ctrls.Add(
      std::make_unique<MoveAlongPathController>(std::move(path.waypoints), /*speed=*/5.f,
                                                /*face_movement=*/true),
      &motion, 0);
  for (int tick = 0; tick < 200 && ctrls.Contains(id); ++tick) ctrls.Update(0.033f);

  EXPECT_FALSE(ctrls.Contains(id)) << "controller never finished";
  EXPECT_NEAR(motion.Position().x, 8.f, 0.5f);
  EXPECT_NEAR(motion.Position().z, 8.f, 0.5f);
}

TEST(NavPipeline, MissingParamsFileFails) {
  const NavFixtureFiles files;
  Space space(1);
  space.SetNavBackendFactory(std::make_shared<nav::RecastNavBackendFactory>());
  const auto result =
      space.LoadNavMeshFromFiles(files.collision, fs::TempDirectory() / "atlas_nav_missing.json");
  EXPECT_FALSE(result.HasValue());
  EXPECT_TRUE(space.NavSourceHash().empty());
}

}  // namespace
}  // namespace atlas
