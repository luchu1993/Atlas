#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AtlasUEActorView.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasUEActorViewNullSafeTest,
	"Atlas.UEActorView.NullSafeLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasUEActorViewNullSafeTest::RunTest(const FString&)
{
	// Null weak handle no-ops on every call; real Destroy() is integration-tested.
	{
		FAtlasUEActorView View(nullptr);
		View.OnTransformReplicated({1.0f, 2.0f, 3.0f}, {});
		View.OnPropertyChanged(0);
		TestNull(TEXT("GetActor returns null"), View.GetActor());
	}
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
