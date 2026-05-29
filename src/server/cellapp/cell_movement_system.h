#ifndef ATLAS_SERVER_CELLAPP_CELL_MOVEMENT_SYSTEM_H_
#define ATLAS_SERVER_CELLAPP_CELL_MOVEMENT_SYSTEM_H_

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "cell_movement_metrics.h"
#include "foundation/clock.h"
#include "math/vector3.h"
#include "movement_command_store.h"
#include "movement_input_buffer.h"
#include "movement_input_rate_limiter.h"
#include "movement_position_history_store.h"
#include "movement_sim/movement_curve_store.h"
#include "movement_state_store.h"
#include "network/address.h"
#include "server/entity_types.h"

namespace atlas {

struct NativeMovementHistorySample;
class WatcherRegistry;
namespace physics {
class PhysicsQuery;
}

struct MovementActorSnapshot {
  math::Vector3 position;
  math::Vector3 direction;
  const physics::PhysicsQuery* physics_query{nullptr};
  bool on_ground{false};
};

class CellMovementHost {
 public:
  virtual ~CellMovementHost() = default;

  [[nodiscard]] virtual auto FindMovementActor(EntityID entity_id,
                                               MovementActorSnapshot& out) -> bool = 0;
  [[nodiscard]] virtual auto MovementNow() -> TimePoint = 0;
  [[nodiscard]] virtual auto MovementServerTick() const -> uint32_t = 0;
  virtual void PublishMovementState(EntityID entity_id,
                                    const movement::MovementState& state) = 0;
  virtual void SendMovementStateAck(EntityID entity_id, const movement::MovementState& state,
                                    uint32_t server_tick) = 0;
  virtual void SendMovementCommandStart(EntityID entity_id,
                                        const movement::MovementCommand& command) = 0;
  virtual void SendMovementCommandEnd(EntityID entity_id, uint32_t command_id,
                                      const movement::MovementState& state,
                                      uint32_t server_tick,
                                      movement::MovementCommandEndReason reason) = 0;
};

class CellMovementSystem {
 public:
  explicit CellMovementSystem(
      std::shared_ptr<const movement::MovementCommandResolver> command_resolver =
          movement::DefaultMovementCommandResolverPtr(),
      std::shared_ptr<const movement::MovementCommandPolicy> command_policy =
          movement::DefaultMovementCommandPolicyPtr());

  void Tick(CellMovementHost& host, float dt);
  void RegisterWatchers(WatcherRegistry& wr);

  void SetIntent(CellMovementHost& host, EntityID entity_id, float dir_x, float dir_z,
                 float speed_mps, uint16_t buttons);
  [[nodiscard]] auto SetCommand(CellMovementHost& host, EntityID entity_id,
                                const movement::MovementCommand& command) -> bool;
  [[nodiscard]] auto ClearCommand(CellMovementHost& host, EntityID entity_id,
                                  uint32_t command_id) -> bool;
  [[nodiscard]] auto EnqueueClientInput(CellMovementHost& host, EntityID entity_id,
                                        std::span<const movement::InputFrame> frames) -> bool;
  void RecordInputPacket();
  void RecordInputDrop();
  void RecordAckSent();

  [[nodiscard]] auto RestoreState(EntityID entity_id,
                                  const movement::MovementState& state) -> bool;
  // Cross-cell offload restore: samples carry the source cell's ticks
  // and get rebased so the latest sample lands at current_server_tick;
  // samples that would map to a negative tick after rebase are dropped.
  void RestorePositionHistoryFromOffload(EntityID entity_id, uint32_t current_server_tick,
                                         std::span<const MovementPositionSample> samples);
  // Same-cell revert (e.g. offload rejected by destination): samples were
  // captured in this cell's own tick frame and are restored verbatim.
  void RestorePositionHistoryAsIs(EntityID entity_id,
                                  std::span<const MovementPositionSample> samples);
  [[nodiscard]] auto RestoreCommand(EntityID entity_id,
                                    const movement::MovementCommand& command) -> bool;
  void ClearStoredCommand(EntityID entity_id);
  void EraseEntity(EntityID entity_id);

  [[nodiscard]] auto SetCurve(const movement::MovementCurve& curve) -> bool;
  [[nodiscard]] auto SampleHistory(EntityID entity_id, uint32_t server_tick,
                                   NativeMovementHistorySample& out) const -> bool;
  void CaptureOffloadState(EntityID entity_id, bool& has_state,
                           movement::MovementState& state,
                           std::vector<MovementPositionSample>& history,
                           bool& has_command,
                           movement::MovementCommand& command) const;

  [[nodiscard]] auto input_buffer() -> MovementInputBuffer& { return input_buffer_; }
  [[nodiscard]] auto state_store() -> MovementStateStore& { return state_store_; }
  [[nodiscard]] auto command_store() -> MovementCommandStore& { return command_store_; }
  [[nodiscard]] auto curve_store() -> movement::MovementCurveStore& { return curve_store_; }
  [[nodiscard]] auto position_history() -> MovementPositionHistoryStore& {
    return position_history_;
  }

 private:
  [[nodiscard]] auto ConsumeInputPacketToken(CellMovementHost& host, EntityID entity_id)
      -> bool;

  MovementInputBuffer input_buffer_;
  MovementCommandStore command_store_;
  movement::MovementCurveStore curve_store_;
  MovementPositionHistoryStore position_history_;
  MovementStateStore state_store_;
  std::shared_ptr<const movement::MovementCommandResolver> command_resolver_;
  std::shared_ptr<const movement::MovementCommandPolicy> command_policy_;
  CellMovementMetrics metrics_;
  MovementInputRateLimiter input_rate_limiter_;
  std::unordered_map<EntityID, movement::InputFrame> script_intents_;
  // Sub-millisecond dt remainder per entity; preserved across ticks so a
  // burst of sub-ms calls eventually advances the command by an integer ms
  // instead of either rounding up every call or freezing the command.
  std::unordered_map<EntityID, float> command_dt_residue_seconds_;
  movement::MovementConfig config_;
  std::vector<EntityID> tick_entity_scratch_;
};

}  // namespace atlas

#endif
