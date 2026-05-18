#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AtlasNetClient.h"
#include "AtlasCore/client_entity_manager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientLifecycleTest,
	"Atlas.NetClient.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientLifecycleTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestEqual(TEXT("initial state idle"), Client.GetState(), EAtlasNetClientState::Idle);
	TestNull(TEXT("no ctx pre-create"), Client.GetContext());

	TestTrue(TEXT("create"), Client.Create());
	TestNotNull(TEXT("ctx post-create"), Client.GetContext());

	// Idempotent re-create.
	TestTrue(TEXT("recreate is idempotent"), Client.Create());

	// Worker thread on an idle ctx — AtlasNetPoll returns immediately on no inbound
	// data; we just want to verify the thread starts and stops without race.
	Client.StartRunningThread();
	TestEqual(TEXT("state running"), Client.GetState(), EAtlasNetClientState::Running);

	FPlatformProcess::Sleep(0.05f);

	// TickGameThread on an empty queue with no entities should be a no-op.
	atlas::ClientEntityManager Mgr;
	Client.TickGameThread(Mgr);
	TestEqual(TEXT("manager untouched"), Mgr.Size(), static_cast<std::size_t>(0));

	Client.Destroy();
	TestEqual(TEXT("state idle after destroy"), Client.GetState(), EAtlasNetClientState::Idle);
	TestNull(TEXT("no ctx after destroy"), Client.GetContext());

	// Re-create cycle proves the static registry properly unregisters on Destroy.
	TestTrue(TEXT("create after destroy"), Client.Create());
	TestNotNull(TEXT("ctx post-second-create"), Client.GetContext());
	Client.Destroy();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientDisconnectSentinelTest,
	"Atlas.NetClient.DisconnectSentinel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientDisconnectSentinelTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	// Pre-queue a couple of harmless AoI envelopes, then a disconnect, then a
	// "stale" message that must be dropped without reaching the manager.
	const uint8 EmptyPayload[1] = {0};
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	Client.TriggerDisconnectForTest(/*Reason=*/7);
	Client.DeliverForTest(0xF003, EmptyPayload, 0);

	TestEqual(TEXT("queue depth pre-tick"), Client.GetInboundDepthForTest(), 4);

	atlas::ClientEntityManager Mgr;
	Client.TickGameThread(Mgr);

	TestEqual(TEXT("state Disconnected after sentinel"), Client.GetState(),
		EAtlasNetClientState::Disconnected);
	TestEqual(TEXT("queue drained past sentinel"), Client.GetInboundDepthForTest(), 0);

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientInboundOverflowTest,
	"Atlas.NetClient.InboundOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientInboundOverflowTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	// Fill exactly to cap; nothing should drop yet, latch stays clear.
	const uint8 EmptyPayload[1] = {0};
	for (int32 i = 0; i < kAtlasInboundQueueCap; ++i)
	{
		Client.DeliverForTest(0xF003, EmptyPayload, 0);
	}
	TestEqual(TEXT("depth at cap"), Client.GetInboundDepthForTest(), kAtlasInboundQueueCap);
	TestFalse(TEXT("overflow latch clear at cap"), Client.GetInboundOverflowLoggedForTest());

	// Two extra deliveries — both drop, but only the first warns.
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	TestEqual(TEXT("depth unchanged past cap"), Client.GetInboundDepthForTest(),
		kAtlasInboundQueueCap);
	TestTrue(TEXT("overflow latch set"), Client.GetInboundOverflowLoggedForTest());

	// Oversize payload check: a buffer over the byte cap is rejected without
	// touching the queue at all.
	const int32 BeforeOversize = Client.GetInboundDepthForTest();
	TArray<uint8> Huge;
	Huge.SetNumZeroed(kAtlasMaxInboundPayloadBytes + 1);
	Client.DeliverForTest(0xF003, Huge.GetData(), Huge.Num());
	TestEqual(TEXT("oversize payload dropped"), Client.GetInboundDepthForTest(), BeforeOversize);

	Client.Destroy();
	// Destroy must reset the latch so a fresh session starts clean.
	TestTrue(TEXT("create after destroy"), Client.Create());
	TestFalse(TEXT("overflow latch cleared by Destroy"),
		Client.GetInboundOverflowLoggedForTest());
	TestEqual(TEXT("depth reset by Destroy"), Client.GetInboundDepthForTest(), 0);
	Client.Destroy();

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
