#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AtlasCoordinates.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasCoordinatesTest,
	"Atlas.Coordinates.AxisAndScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasCoordinatesTest::RunTest(const FString&)
{
	// Atlas (X=right, Y=up, Z=forward) -> UE (X=forward, Y=right, Z=up).
	const atlas::Vec3 In{1.5f, -2.0f, 3.25f};
	const FVector UE = AtlasToUE(In);
	TestEqual(TEXT("UE.X = Atlas.Z * 100"), UE.X, 325.0);
	TestEqual(TEXT("UE.Y = Atlas.X * 100"), UE.Y, 150.0);
	TestEqual(TEXT("UE.Z = Atlas.Y * 100"), UE.Z, -200.0);

	const atlas::Vec3 RoundTrip = UEToAtlas(UE);
	TestEqual(TEXT("round-trip X"), RoundTrip.x, In.x);
	TestEqual(TEXT("round-trip Y"), RoundTrip.y, In.y);
	TestEqual(TEXT("round-trip Z"), RoundTrip.z, In.z);

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
