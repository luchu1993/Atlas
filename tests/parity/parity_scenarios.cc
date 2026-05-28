#include "parity_scenarios.h"

#include "physics/physics_query.h"

#ifdef ATLAS_PARITY_HAS_JOLT
#include "physics_jolt/jolt_physics_query.h"
#endif

#include "math/vector3.h"

namespace atlas::physics::parity {

namespace {

// Atlas Capsule convention: character foot at y=0 means MovementState.position.y=0.
constexpr float kDefaultGroundY = 0.0f;
constexpr float kGiantBoxHalfExtentXZ = 500.0f;
constexpr float kGiantBoxThickness = 100.0f;

[[nodiscard]] auto MakeFlatBackend(BackendKind kind, float ground_y)
    -> std::unique_ptr<PhysicsQuery> {
  switch (kind) {
    case BackendKind::kFlat:
      return std::make_unique<FlatPhysicsQuery>(ground_y);
    case BackendKind::kStatic:
      return std::make_unique<StaticPhysicsQuery>(StaticGroundMode::kEnabled, ground_y);
    case BackendKind::kJolt: {
#ifdef ATLAS_PARITY_HAS_JOLT
      auto query = std::make_unique<JoltPhysicsQuery>();
      StaticBox box;
      box.min = {-kGiantBoxHalfExtentXZ, ground_y - kGiantBoxThickness,
                 -kGiantBoxHalfExtentXZ};
      box.max = {kGiantBoxHalfExtentXZ, ground_y, kGiantBoxHalfExtentXZ};
      query->AddBox(box);
      return query;
#else
      return nullptr;
#endif
    }
  }
  return nullptr;
}

[[nodiscard]] auto MakeBoxBackend(BackendKind kind, const StaticBox& box,
                                  float flat_ground_y = -1000.0f)
    -> std::unique_ptr<PhysicsQuery> {
  switch (kind) {
    case BackendKind::kFlat:
      // Box scenarios are not representable with FlatPhysicsQuery.
      return nullptr;
    case BackendKind::kStatic: {
      auto query =
          std::make_unique<StaticPhysicsQuery>(StaticGroundMode::kDisabled, flat_ground_y);
      query->AddBox(box);
      return query;
    }
    case BackendKind::kJolt: {
#ifdef ATLAS_PARITY_HAS_JOLT
      auto query = std::make_unique<JoltPhysicsQuery>();
      query->AddBox(box);
      return query;
#else
      return nullptr;
#endif
    }
  }
  return nullptr;
}

[[nodiscard]] auto ForwardInput() -> movement::InputFrame {
  movement::InputFrame f;
  f.move_z = 127;
  f.client_dt_ms = 33;
  return f;
}

[[nodiscard]] auto IdleInput() -> movement::InputFrame {
  movement::InputFrame f;
  f.client_dt_ms = 33;
  return f;
}

[[nodiscard]] auto JumpInput() -> movement::InputFrame {
  movement::InputFrame f;
  f.buttons = movement::kInputButtonJump;
  f.client_dt_ms = 33;
  return f;
}

[[nodiscard]] auto InitialStateOnGround(float y) -> movement::MovementState {
  movement::MovementState s;
  s.position = {0.0f, y, 0.0f};
  s.direction = {0.0f, 0.0f, 1.0f};
  s.flags = movement::kMovementFlagGrounded;
  return s;
}

[[nodiscard]] auto InitialStateInAir(float y) -> movement::MovementState {
  movement::MovementState s;
  s.position = {0.0f, y, 0.0f};
  s.direction = {0.0f, 0.0f, 1.0f};
  s.flags = 0;
  return s;
}

// ── Flat scenarios ────────────────────────────────────────────────────────

[[nodiscard]] auto FlatWalkForward() -> ParityScenario {
  ParityScenario s;
  s.id = "flat_walk_forward";
  s.initial_state = InitialStateOnGround(kDefaultGroundY);
  s.inputs = {ForwardInput()};
  s.tick_count = 150;
  s.tolerance = kNormalTolerance;
  s.backends = {BackendKind::kFlat, BackendKind::kStatic, BackendKind::kJolt};
  s.make_query = [](BackendKind kind) { return MakeFlatBackend(kind, kDefaultGroundY); };
  return s;
}

[[nodiscard]] auto FlatStopStart() -> ParityScenario {
  ParityScenario s;
  s.id = "flat_stop_start";
  s.initial_state = InitialStateOnGround(kDefaultGroundY);
  // 30 ticks forward / 30 ticks idle / 30 ticks forward / 30 ticks idle.
  std::vector<movement::InputFrame> inputs;
  for (int i = 0; i < 30; ++i) inputs.push_back(ForwardInput());
  for (int i = 0; i < 30; ++i) inputs.push_back(IdleInput());
  for (int i = 0; i < 30; ++i) inputs.push_back(ForwardInput());
  for (int i = 0; i < 30; ++i) inputs.push_back(IdleInput());
  s.inputs = std::move(inputs);
  s.tick_count = 120;
  s.tolerance = kNormalTolerance;
  s.backends = {BackendKind::kFlat, BackendKind::kStatic, BackendKind::kJolt};
  s.make_query = [](BackendKind kind) { return MakeFlatBackend(kind, kDefaultGroundY); };
  return s;
}

[[nodiscard]] auto FlatJumpFall() -> ParityScenario {
  ParityScenario s;
  s.id = "flat_jump_fall";
  s.initial_state = InitialStateOnGround(kDefaultGroundY);
  std::vector<movement::InputFrame> inputs;
  inputs.push_back(JumpInput());  // single-tick jump press
  for (int i = 0; i < 119; ++i) inputs.push_back(IdleInput());
  s.inputs = std::move(inputs);
  s.tick_count = 120;
  s.tolerance = kNormalTolerance;
  s.backends = {BackendKind::kFlat, BackendKind::kStatic, BackendKind::kJolt};
  s.make_query = [](BackendKind kind) { return MakeFlatBackend(kind, kDefaultGroundY); };
  return s;
}

// ── Box scenarios (Static ↔ Jolt) ────────────────────────────────────────

[[nodiscard]] auto BoxDropToTop() -> ParityScenario {
  ParityScenario s;
  s.id = "box_drop_to_top";
  // 10×2×10 box centered at origin: top face at y=1.
  StaticBox box;
  box.min = {-5.0f, -1.0f, -5.0f};
  box.max = {5.0f, 1.0f, 5.0f};
  s.initial_state = InitialStateInAir(5.0f);  // released above box top
  s.inputs = {IdleInput()};
  s.tick_count = 180;
  s.tolerance = kNormalTolerance;
  s.backends = {BackendKind::kStatic, BackendKind::kJolt};
  s.make_query = [box](BackendKind kind) { return MakeBoxBackend(kind, box); };
  return s;
}

// Mesh scenario: Static approximates the mesh ground with a thin slab box.
// Jolt loads an actual 100x100 XZ quad. The parity check tolerates more drift
// (kMeshTolerance) because sphere-cast contact resolution against a triangle
// face differs from sphere-cast against a box face by a few millimetres even
// on perfectly flat geometry.
[[nodiscard]] auto MakeMeshGroundBackend(BackendKind kind) -> std::unique_ptr<PhysicsQuery> {
  constexpr float kHalfExtent = 50.0f;
  switch (kind) {
    case BackendKind::kFlat:
      return nullptr;
    case BackendKind::kStatic: {
      auto query =
          std::make_unique<StaticPhysicsQuery>(StaticGroundMode::kDisabled, -1000.0f);
      StaticBox box;
      box.min = {-kHalfExtent, -1.0f, -kHalfExtent};
      box.max = {kHalfExtent, 0.0f, kHalfExtent};
      query->AddBox(box);
      return query;
    }
    case BackendKind::kJolt: {
#ifdef ATLAS_PARITY_HAS_JOLT
      auto query = std::make_unique<JoltPhysicsQuery>();
      const math::Vector3 verts[] = {
          {-kHalfExtent, 0.0f, -kHalfExtent},
          { kHalfExtent, 0.0f, -kHalfExtent},
          { kHalfExtent, 0.0f,  kHalfExtent},
          {-kHalfExtent, 0.0f,  kHalfExtent},
      };
      const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
      query->AddMesh(verts, indices, 0);
      return query;
#else
      return nullptr;
#endif
    }
  }
  return nullptr;
}

[[nodiscard]] auto MeshWalkLongPath() -> ParityScenario {
  ParityScenario s;
  s.id = "mesh_walk_long_path";
  s.initial_state = InitialStateOnGround(0.0f);
  s.inputs = {ForwardInput()};
  s.tick_count = 150;  // 5s at 30Hz → ~25m forward, inside the ±50 quad
  s.tolerance = kMeshTolerance;
  s.backends = {BackendKind::kStatic, BackendKind::kJolt};
  s.make_query = [](BackendKind kind) { return MakeMeshGroundBackend(kind); };
  return s;
}

[[nodiscard]] auto BoxWalkSteady() -> ParityScenario {
  ParityScenario s;
  s.id = "box_walk_steady";
  // 100×2×100 box; 5s forward walk at max_speed=5m/s reaches z=25, well within
  // the +50 z boundary so the parity check only exercises the interior, not the
  // edge — Static's radius-grace and Jolt's true geometry diverge near corners
  // (a known and acceptable cross-backend difference; see backend_parity_testing
  // §4.4).
  StaticBox box;
  box.min = {-50.0f, -1.0f, -50.0f};
  box.max = {50.0f, 1.0f, 50.0f};
  s.initial_state = InitialStateOnGround(1.0f);
  s.inputs = {ForwardInput()};
  s.tick_count = 150;
  s.tolerance = kNormalTolerance;
  s.backends = {BackendKind::kStatic, BackendKind::kJolt};
  s.make_query = [box](BackendKind kind) { return MakeBoxBackend(kind, box); };
  return s;
}

}  // namespace

auto AllScenarios() -> std::vector<ParityScenario> {
  return {
      FlatWalkForward(),
      FlatStopStart(),
      FlatJumpFall(),
      BoxDropToTop(),
      BoxWalkSteady(),
      MeshWalkLongPath(),
  };
}

}  // namespace atlas::physics::parity
