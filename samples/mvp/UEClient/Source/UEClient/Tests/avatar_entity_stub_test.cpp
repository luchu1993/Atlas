#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AvatarEntityStub.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvatarEntityStubTest,
	"Atlas.AvatarEntityStub.PositionFeedsFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvatarEntityStubTest::RunTest(const FString&)
{
	FAvatarEntityStub Stub(123, 2);
	TestEqual(TEXT("filter empty"), Stub.GetFilter().SampleCount(),
		static_cast<std::size_t>(0));

	Stub.OnPositionReceived(10.0, {1.0f, 0.0f, 0.0f}, {}, true);
	TestEqual(TEXT("filter has 1"), Stub.GetFilter().SampleCount(),
		static_cast<std::size_t>(1));

	Stub.OnPositionReceived(10.1, {2.0f, 0.0f, 0.0f}, {}, false);
	TestEqual(TEXT("filter has 2"), Stub.GetFilter().SampleCount(),
		static_cast<std::size_t>(2));

	// Out-of-order is dropped by AvatarFilter.Input (already covered by its own
	// tests; verified here as the stub's forwarding actually reaches it).
	Stub.OnPositionReceived(10.05, {99.0f, 0.0f, 0.0f}, {}, false);
	TestEqual(TEXT("out-of-order dropped"), Stub.GetFilter().SampleCount(),
		static_cast<std::size_t>(2));

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
