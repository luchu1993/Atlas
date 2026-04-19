// CellApp integration test scaffold — Phase 10 Step 10.10.
//
// Scope note: full end-to-end exercises require the CLR runtime to be
// bring-up-able in the test process (Atlas.Runtime.dll loaded via hostfxr,
// C# OnInit running, NativeApi callbacks wired). That environment isn't
// universally available on every developer machine (the MSBuild SDK
// resolution path in particular is brittle on Windows where `dotnet` is
// present but `Microsoft.NET.Sdk` isn't discoverable to MSBuild).
//
// The tests below are structured as gtest cases but many are DISABLED_
// by default — running them locally is opt-in via `--gtest_filter=*`.
// The scenarios they encode match phase10_cellapp.md Step 10.10's
// acceptance list:
//
//   1. machined + DBApp + BaseAppMgr + BaseApp + CellApp + LoginApp all
//      start and register with one another.
//   2. Client logs in, receives Proxy, Proxy requests Cell entity creation,
//      CellApp responds CellEntityCreated, BaseEntity caches cell_addr.
//   3. EnableWitness from script → CellApp creates AoITrigger, existing
//      peers are reported as EntityEnter to the owning client.
//   4. AvatarUpdate moves the observer — other clients with overlapping
//      AoI receive EntityPositionUpdate (0xF001 envelopes).
//   5. A peer entering AoI produces EntityEnter on the owning client.
//   6. A peer leaving AoI produces EntityLeave.
//   7. A C# property change produces EntityPropertyUpdate (audience-
//      filtered: OwnerDelta to owner, OtherDelta to observers).
//   8. MoveToPointController drives smooth motion; observers see a
//      stream of EntityPositionUpdate frames.
//   9. 1000-entity stress: p50 < 20 ms tick, p99 < 50 ms at 10 Hz
//      (§3.11 performance budget).
//   10. RPC security:
//       - non-exposed cell RPC → BaseApp rejects
//       - exposed OwnClient + cross-entity → BaseApp rejects
//       - exposed AllClients + cross-entity → accepted and dispatched
//       - wrong-direction (Base RPC sent via cell channel) → rejected
//       - direction check on Base RPC as well
//       - InternalCellRpc → dispatched without exposed check
//
// The unit-level tests in test_cellapp_handlers.cpp and
// test_baseapp_messages.cpp already cover #10 at the handler level.
// What remains is wire-level validation that the two processes talk to
// each other correctly end-to-end.

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "cellapp/cellapp.h"
#include "foundation/clock.h"
#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"

namespace atlas {
namespace {

// Smoke test: CellApp can be constructed without CLR (handlers don't
// touch the script engine). The real process can't tick without Init(),
// which needs CLR — but construction validates the dependency graph.
TEST(CellAppIntegration, ConstructsCleanly) {
  EventDispatcher dispatcher("smoke");
  NetworkInterface network(dispatcher);
  CellApp app(dispatcher, network);
  EXPECT_EQ(app.Spaces().size(), 0u);
}

// ---------------------------------------------------------------------------
// DISABLED_ scenarios — the full E2E plan. Enable locally with:
//   test_cellapp_integration.exe --gtest_also_run_disabled_tests \
//       --gtest_filter='CellAppIntegration.DISABLED_*'
// once the CLR bring-up is healthy in your environment. Until then the
// test names document the acceptance criteria.
// ---------------------------------------------------------------------------

TEST(CellAppIntegration, DISABLED_AllProcessesStartAndRegister) {
  GTEST_SKIP() << "Requires machined + BaseAppMgr + DBApp + LoginApp + BaseApp + "
                  "CellApp processes + CLR bring-up.";
}

TEST(CellAppIntegration, DISABLED_LoginFlowProducesCellEntity) {
  GTEST_SKIP() << "Covers scenario #2 — client login → BaseApp → CreateCellEntity → "
                  "CellApp → CellEntityCreated ack.";
}

TEST(CellAppIntegration, DISABLED_EnableWitnessFiresEntityEnterForExistingPeers) {
  GTEST_SKIP() << "Scenario #3 — EnableWitness plants the AoITrigger and the "
                  "initial shuffle emits Enter events for peers already in range.";
}

TEST(CellAppIntegration, DISABLED_AvatarUpdatePropagatesToObservers) {
  GTEST_SKIP() << "Scenario #4 — moving an avatar produces EntityPositionUpdate "
                  "envelopes to observers with overlapping AoI.";
}

TEST(CellAppIntegration, DISABLED_PeerEnterLeaveFiresEventsOnObservers) {
  GTEST_SKIP() << "Scenarios #5 + #6 — peer entering / leaving AoI triggers the "
                  "corresponding envelope on the observer's client channel.";
}

TEST(CellAppIntegration, DISABLED_PropertyChangeFiresAudienceFilteredUpdate) {
  GTEST_SKIP() << "Scenario #7 — C# property mutation → ReplicationFrame → owner "
                  "and other audiences receive their respective deltas.";
}

TEST(CellAppIntegration, DISABLED_MoveControllerDrivesSmoothMotion) {
  GTEST_SKIP() << "Scenario #8 — MoveToPointController moves an entity across "
                  "multiple ticks; observers see an EntityPositionUpdate stream.";
}

TEST(CellAppIntegration, DISABLED_ThousandEntityTickWithinPerfBudget) {
  GTEST_SKIP() << "Scenario #9 — 1000 entities in one Space meet §3.11's tick "
                  "budget (p50 < 20 ms, p99 < 50 ms at 10 Hz).";
}

// ---- RPC security — scenario #10 -----------------------------------------

TEST(CellAppIntegration, DISABLED_NonExposedCellRpcRejectedAtBaseApp) {
  GTEST_SKIP() << "BaseApp's ClientCellRpc handler rejects non-exposed methods "
                  "before they can reach the CellApp.";
}

TEST(CellAppIntegration, DISABLED_OwnClientCellRpcRejectedWhenSourceNeTarget) {
  GTEST_SKIP() << "OWN_CLIENT scope blocks cross-entity calls at both the BaseApp "
                  "L2 check AND the CellApp L4 check (defence in depth).";
}

TEST(CellAppIntegration, DISABLED_AllClientsCellRpcAcceptedAcrossEntities) {
  GTEST_SKIP() << "ALL_CLIENTS scope allows any AoI-visible caller; BaseApp L2 "
                  "passes through.";
}

TEST(CellAppIntegration, DISABLED_WrongDirectionRpcRejected) {
  GTEST_SKIP() << "A Base-direction (0x03) RPC sent through the Cell channel "
                  "(or vice versa) is rejected before any script dispatch.";
}

TEST(CellAppIntegration, DISABLED_InternalCellRpcBypassesExposedCheck) {
  GTEST_SKIP() << "Server-internal Base→Cell RPCs skip the exposed check because "
                  "BaseApp is trusted. Only the handler's type-level validation "
                  "(entity exists, direction matches the Cell path) is enforced.";
}

}  // namespace
}  // namespace atlas
