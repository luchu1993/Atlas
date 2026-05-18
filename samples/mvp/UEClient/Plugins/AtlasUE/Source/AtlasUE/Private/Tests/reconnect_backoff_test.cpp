#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AtlasReconnectBackoff.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasReconnectBackoffTest,
	"Atlas.Reconnect.BackoffSchedule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasReconnectBackoffTest::RunTest(const FString&)
{
	using namespace AtlasReconnect;

	TestEqual(TEXT("attempt 0 = 1s"), ComputeBackoffSec(0), 1.0);
	TestEqual(TEXT("attempt 1 = 2s"), ComputeBackoffSec(1), 2.0);
	TestEqual(TEXT("attempt 2 = 4s"), ComputeBackoffSec(2), 4.0);
	TestEqual(TEXT("attempt 4 = 16s"), ComputeBackoffSec(4), 16.0);
	TestEqual(TEXT("attempt 5 capped at 30s"), ComputeBackoffSec(5), kMaxBackoffSec);
	TestEqual(TEXT("attempt 20 still capped"), ComputeBackoffSec(20), kMaxBackoffSec);

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
