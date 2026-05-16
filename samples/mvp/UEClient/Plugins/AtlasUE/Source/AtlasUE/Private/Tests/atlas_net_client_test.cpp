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

#endif  // WITH_DEV_AUTOMATION_TESTS
