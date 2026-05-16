#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AtlasCoordinates.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasCoordinatesTest,
	"Atlas.Coordinates.MetersToCm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasCoordinatesTest::RunTest(const FString&)
{
	const atlas::Vec3 In{1.5f, -2.0f, 3.25f};
	const FVector UE = AtlasToUE(In);
	TestEqual(TEXT("m -> cm X"), UE.X, 150.0);
	TestEqual(TEXT("m -> cm Y"), UE.Y, -200.0);
	TestEqual(TEXT("m -> cm Z"), UE.Z, 325.0);

	const atlas::Vec3 RoundTrip = UEToAtlas(UE);
	TestEqual(TEXT("round-trip X"), RoundTrip.x, In.x);
	TestEqual(TEXT("round-trip Y"), RoundTrip.y, In.y);
	TestEqual(TEXT("round-trip Z"), RoundTrip.z, In.z);

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
