#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AtlasCore/moving_client_entity.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasMovingClientEntityTest,
	"Atlas.MovingClientEntity.PositionFeedsFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasMovingClientEntityTest::RunTest(const FString&)
{
	atlas::MovingClientEntity Entity(123, 2);
	TestEqual(TEXT("filter empty"), Entity.Filter().SampleCount(),
		static_cast<std::size_t>(0));

	Entity.OnPositionReceived(10.0, {1.0f, 0.0f, 0.0f}, {}, true);
	TestEqual(TEXT("filter has 1"), Entity.Filter().SampleCount(),
		static_cast<std::size_t>(1));

	Entity.OnPositionReceived(10.1, {2.0f, 0.0f, 0.0f}, {}, false);
	TestEqual(TEXT("filter has 2"), Entity.Filter().SampleCount(),
		static_cast<std::size_t>(2));

	// AvatarFilter drops out-of-order timestamps; this asserts the entity
	// hands every sample through without filtering before the filter sees it.
	Entity.OnPositionReceived(10.05, {99.0f, 0.0f, 0.0f}, {}, false);
	TestEqual(TEXT("out-of-order dropped"), Entity.Filter().SampleCount(),
		static_cast<std::size_t>(2));

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
