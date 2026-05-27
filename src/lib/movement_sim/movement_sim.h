#ifndef ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_SIM_H_
#define ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_SIM_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "math/vector3.h"
#include "physics/physics_query.h"

namespace atlas::movement {

inline constexpr uint32_t kMovementFlagGrounded = 1u << 0;
inline constexpr uint16_t kInputButtonJump = 1u << 0;
inline constexpr uint16_t kMinInputDtMs = 1;
inline constexpr uint16_t kMaxInputDtMs = 250;
inline constexpr uint32_t kMaxInputSequenceGap = 256;
inline constexpr float kCorrectionTier1DistanceM = 0.3f;
inline constexpr float kCorrectionTier2DistanceM = 1.5f;
inline constexpr float kCorrectionSnapDistanceM = 5.0f;
inline constexpr uint16_t kCorrectionFlagTier1 = 1u << 0;
inline constexpr uint16_t kCorrectionFlagTier2 = 1u << 1;
inline constexpr uint16_t kCorrectionFlagSnap = 1u << 2;
inline constexpr std::size_t kMaxMovementCurveSamples = 64;

enum class CorrectionTier : uint16_t {
  kNone,
  kTier1,
  kTier2,
  kSnap,
};

struct InputFrame {
  uint32_t seq{0};
  uint32_t input_tick{0};
  int8_t move_x{0};
  int8_t move_z{0};
  uint16_t view_yaw{0};
  int8_t view_pitch{0};
  uint16_t buttons{0};
  uint16_t client_dt_ms{0};
};

struct MovementConfig {
  float fixed_dt_s{1.0f / 30.0f};
  float max_speed_mps{5.0f};
  float acceleration_mps2{35.0f};
  float deceleration_mps2{45.0f};
  float gravity_mps2{25.0f};
  float jump_speed_mps{7.0f};
  float max_fall_speed_mps{60.0f};
  float max_position_abs_m{1000000.0f};
  float capsule_radius_m{0.35f};
  float capsule_half_height_m{0.9f};
  float ground_snap_distance_m{0.08f};
  float max_walkable_slope_degrees{50.0f};
  float step_height_m{0.35f};
  float max_depenetration_m{0.25f};
};

struct MovementState {
  math::Vector3 position{0.0f, 0.0f, 0.0f};
  math::Vector3 velocity{0.0f, 0.0f, 0.0f};
  math::Vector3 direction{0.0f, 0.0f, 1.0f};
  uint32_t flags{kMovementFlagGrounded};
  uint32_t last_processed_input_seq{0};
};

enum class MovementCommandType : uint8_t {
  kDash,
  kLaunch,
  kLaunchOther,
  kPull,
  kKnockback,
  kTeleport,
  kFollowEntity,
};

enum class MovementCommandInputPolicy : uint8_t {
  kSuppress,
  kAllowTurn,
  kAllowFull,
};

enum class MovementCommandCollisionPolicy : uint8_t {
  kStop,
  kContinue,
  kEndSkill,
};

enum class MovementCommandEndReason : uint8_t {
  kCompleted,
  kCancelled,
  kCollision,
  kInvalid,
};

struct MovementCurve {
  uint16_t id{0};
  uint16_t sample_count{0};
  std::array<float, kMaxMovementCurveSamples> samples{};
};

struct MovementCommand {
  uint32_t command_id{0};
  uint16_t skill_id{0};
  MovementCommandType type{MovementCommandType::kDash};
  math::Vector3 start_position{0.0f, 0.0f, 0.0f};
  math::Vector3 target_position{0.0f, 0.0f, 0.0f};
  uint16_t duration_ms{0};
  uint16_t elapsed_ms{0};
  uint16_t curve_id{0};
  MovementCommandInputPolicy input_policy{MovementCommandInputPolicy::kSuppress};
  MovementCommandCollisionPolicy collision_policy{MovementCommandCollisionPolicy::kStop};
  uint8_t priority{0};
  uint32_t server_tick{0};
};

struct MovementCommandStepResult {
  MovementState state;
  MovementCommand command;
  bool active{false};
  bool completed{false};
  bool blocked{false};
  bool collision_ended{false};
};

class MovementCommandSampler {
 public:
  virtual ~MovementCommandSampler() = default;

  [[nodiscard]] virtual auto Supports(MovementCommandType type) const -> bool = 0;
  [[nodiscard]] virtual auto Step(const MovementState& previous,
                                  const MovementCommand& command,
                                  const MovementCurve& curve,
                                  uint16_t dt_ms) const -> MovementCommandStepResult = 0;
};

class MovementCommandResolver {
 public:
  virtual ~MovementCommandResolver() = default;

  [[nodiscard]] virtual auto Find(MovementCommandType type) const
      -> const MovementCommandSampler* = 0;
};

class MovementCommandPolicy {
 public:
  virtual ~MovementCommandPolicy() = default;

  [[nodiscard]] virtual auto SuppressesInput(const MovementCommand& command) const
      -> bool = 0;
  [[nodiscard]] virtual auto AllowsTurnInput(const MovementCommand& command) const
      -> bool = 0;
  [[nodiscard]] virtual auto ChecksCollision(const MovementCommand& command) const
      -> bool = 0;
  [[nodiscard]] virtual auto EndReasonFor(const MovementCommandStepResult& result) const
      -> MovementCommandEndReason = 0;
};

struct GroundHit {
  bool hit{false};
  math::Vector3 position{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float distance_m{0.0f};
};

struct Capsule {
  math::Vector3 center{0.0f, 0.0f, 0.0f};
  float radius_m{0.0f};
  float half_height_m{0.0f};
};

struct CapsuleCast {
  Capsule capsule;
  math::Vector3 displacement{0.0f, 0.0f, 0.0f};
};

struct SweepHit {
  bool hit{false};
  float fraction{1.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
};

struct DepenetrationHit {
  bool hit{false};
  math::Vector3 offset{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float depth_m{0.0f};
};

class CharacterQuery {
 public:
  virtual ~CharacterQuery() = default;

  [[nodiscard]] virtual auto GroundProbe(const math::Vector3& position) const -> GroundHit = 0;
  [[nodiscard]] virtual auto SweepCapsule(const CapsuleCast& cast) const -> SweepHit = 0;
  [[nodiscard]] virtual auto OverlapCapsule(const Capsule& capsule) const -> bool = 0;
  [[nodiscard]] virtual auto DepenetrateCapsule(const Capsule& capsule) const
      -> DepenetrationHit;
};

class FlatGroundQuery final : public CharacterQuery {
 public:
  explicit FlatGroundQuery(float ground_y = 0.0f) : ground_y_(ground_y) {}

  [[nodiscard]] auto GroundProbe(const math::Vector3& position) const -> GroundHit override;
  [[nodiscard]] auto SweepCapsule(const CapsuleCast& cast) const -> SweepHit override;
  [[nodiscard]] auto OverlapCapsule(const Capsule& capsule) const -> bool override;
  [[nodiscard]] auto DepenetrateCapsule(const Capsule& capsule) const
      -> DepenetrationHit override;

 private:
  float ground_y_{0.0f};
};

class PhysicsCharacterQuery final : public CharacterQuery {
 public:
  explicit PhysicsCharacterQuery(const physics::PhysicsQuery& query,
                                 float ground_probe_distance_m = 2.0f,
                                 physics::LayerMask mask = {},
                                 float ground_probe_radius_m = 0.0f);

  [[nodiscard]] auto GroundProbe(const math::Vector3& position) const -> GroundHit override;
  [[nodiscard]] auto SweepCapsule(const CapsuleCast& cast) const -> SweepHit override;
  [[nodiscard]] auto OverlapCapsule(const Capsule& capsule) const -> bool override;
  [[nodiscard]] auto DepenetrateCapsule(const Capsule& capsule) const
      -> DepenetrationHit override;

 private:
  const physics::PhysicsQuery& query_;
  float ground_probe_distance_m_{2.0f};
  float ground_probe_radius_m_{0.0f};
  physics::LayerMask mask_;
};

struct MovementStepResult {
  MovementState state;
  GroundHit ground;
  SweepHit sweep;
  uint32_t server_tick{0};
  bool blocked{false};
  bool jumped{false};
  bool stepped{false};
  bool snapped{false};
  bool depenetrated{false};
};

[[nodiscard]] auto IsFinite(const math::Vector3& v) -> bool;
[[nodiscard]] auto IsFinite(const MovementState& state) -> bool;
[[nodiscard]] auto IsStateWithinLimits(const MovementState& state,
                                       const MovementConfig& config) -> bool;
[[nodiscard]] auto IsHorizontalAccelerationWithinLimits(const MovementState& previous,
                                                        const MovementState& current,
                                                        const MovementConfig& config) -> bool;
[[nodiscard]] auto IsInputFrameValid(const InputFrame& input) -> bool;
[[nodiscard]] auto InputFacingDirection(const InputFrame& input) -> math::Vector3;
[[nodiscard]] auto InputSequenceDelta(uint32_t seq, uint32_t previous) -> uint32_t;
[[nodiscard]] auto IsInputSequenceNewer(uint32_t seq, uint32_t previous) -> bool;
[[nodiscard]] auto ClassifyCorrection(float distance_m) -> CorrectionTier;
[[nodiscard]] auto CorrectionFlagForTier(CorrectionTier tier) -> uint16_t;
[[nodiscard]] auto IsCorrectionFlagsValid(uint16_t flags) -> bool;
[[nodiscard]] auto IsMovementCurveValid(const MovementCurve& curve) -> bool;
[[nodiscard]] auto IsMovementCommandValid(const MovementCommand& command,
                                          const MovementCurve& curve) -> bool;
[[nodiscard]] auto SampleMovementCurve(const MovementCurve& curve,
                                       float normalized_time) -> float;
[[nodiscard]] auto DefaultMovementCommandResolver() -> const MovementCommandResolver&;
[[nodiscard]] auto DefaultMovementCommandResolverPtr()
    -> std::shared_ptr<const MovementCommandResolver>;
[[nodiscard]] auto DefaultMovementCommandPolicy() -> const MovementCommandPolicy&;
[[nodiscard]] auto DefaultMovementCommandPolicyPtr()
    -> std::shared_ptr<const MovementCommandPolicy>;
[[nodiscard]] auto ApplyMovementCommand(const MovementState& previous,
                                        const MovementCommand& command,
                                        const MovementCurve& curve,
                                        uint16_t dt_ms,
                                        const MovementCommandResolver& resolver)
    -> MovementCommandStepResult;
[[nodiscard]] auto ApplyMovementCommand(const MovementState& previous,
                                        const MovementCommand& command,
                                        const MovementCurve& curve,
                                        uint16_t dt_ms,
                                        const MovementConfig& config,
                                        const CharacterQuery& query,
                                        const MovementCommandResolver& resolver)
    -> MovementCommandStepResult;
[[nodiscard]] auto ApplyMovementCommand(const MovementState& previous,
                                        const MovementCommand& command,
                                        const MovementCurve& curve,
                                        uint16_t dt_ms,
                                        const MovementConfig& config,
                                        const CharacterQuery& query,
                                        const MovementCommandResolver& resolver,
                                        const MovementCommandPolicy& policy)
    -> MovementCommandStepResult;
[[nodiscard]] auto ApplyMovementCommandCurve(const MovementState& previous,
                                             const MovementCommand& command,
                                             const MovementCurve& curve,
                                             uint16_t dt_ms)
    -> MovementCommandStepResult;
[[nodiscard]] auto ApplyMovementCommandCurve(const MovementState& previous,
                                             const MovementCommand& command,
                                             const MovementCurve& curve,
                                             uint16_t dt_ms,
                                             const MovementConfig& config,
                                             const CharacterQuery& query)
    -> MovementCommandStepResult;
[[nodiscard]] auto Step(const MovementState& previous, const InputFrame& input,
                        const MovementConfig& config, const CharacterQuery& query,
                        uint32_t server_tick) -> MovementStepResult;

}

#endif
