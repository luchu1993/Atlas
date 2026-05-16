#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cmath>

#include "AtlasCore/avatar_filter.h"
#include "AtlasCore/entity_view.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasAvatarFilterTest,
	"Atlas.AvatarFilter.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasAvatarFilterTest::RunTest(const FString&)
{
	auto NearlyEqual = [](float A, float B, float Tol = 1e-5f) {
		return std::fabs(A - B) <= Tol;
	};

	// 1. Empty filter rejects TryEvaluate.
	{
		atlas::AvatarFilter F;
		atlas::Vec3 P{}, D{};
		bool G = false;
		TestFalse(TEXT("empty TryEvaluate"), F.TryEvaluate(P, D, G));
	}

	// 2. Single sample: latency_current snaps to target_latency, position echoes input.
	{
		double WallNow = 10.0;
		atlas::AvatarFilter F([&]() { return WallNow; });
		F.Input(10.0, {1.0f, 2.0f, 3.0f}, {1.0f, 0.0f, 0.0f}, true);
		TestEqual(TEXT("count after 1 input"), F.SampleCount(), static_cast<std::size_t>(1));
		TestTrue(TEXT("latency snapped to target"),
			std::fabs(F.CurrentLatency() - F.TargetLatency()) < 1e-9);

		atlas::Vec3 P{}, D{};
		bool G = false;
		WallNow = 10.0;  // target_time = 10.0 - 0.0 - 0.3 = 9.7; extrapolation with count<2 returns s.position.
		TestTrue(TEXT("single sample TryEvaluate"), F.TryEvaluate(P, D, G));
		TestTrue(TEXT("single sample pos.x"), NearlyEqual(P.x, 1.0f));
		TestTrue(TEXT("single sample pos.y"), NearlyEqual(P.y, 2.0f));
		TestTrue(TEXT("single sample pos.z"), NearlyEqual(P.z, 3.0f));
		TestTrue(TEXT("single sample dir.x"), NearlyEqual(D.x, 1.0f));
		TestTrue(TEXT("single sample on_ground"), G);
	}

	// 3. Two samples, exact midpoint lerp.
	// wall=10.0: Input(server=10.0, pos=0). offset=0, latency=0.3.
	// wall=11.0: Input(server=11.0, pos=10). offset=0.95*0+0.05*0=0.
	// wall=11.0 TryEvaluate: target=11.0-0-0.3=10.7. Lerp t=0.7. pos.x=0+10*0.7=7.0.
	{
		double WallNow = 10.0;
		atlas::AvatarFilter F([&]() { return WallNow; });
		F.Input(10.0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, false);
		WallNow = 11.0;
		F.Input(11.0, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, false);

		atlas::Vec3 P{}, D{};
		bool G = false;
		WallNow = 11.0;
		TestTrue(TEXT("two samples TryEvaluate"), F.TryEvaluate(P, D, G));
		TestTrue(TEXT("lerp at t=0.7 pos.x ≈ 7.0"), NearlyEqual(P.x, 7.0f));
	}

	// 4. Target_time after newest extrapolates with cap of max_extrapolation.
	{
		double WallNow = 10.0;
		atlas::AvatarFilter F([&]() { return WallNow; });
		F.max_extrapolation = 0.05;
		F.Input(10.0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, false);
		F.Input(10.1, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, false);
		// Force a target_time well past newest by zeroing latency + offset.
		// After 1st Input offset=10.0-10.0=0, latency=0.3. After 2nd Input offset=0 still.
		// To get target_time > 10.1, need wall - 0 - 0.3 > 10.1 → wall > 10.4.
		WallNow = 10.5;  // target_time = 10.2; ahead = 10.2-10.1 = 0.1, capped to 0.05.
		// span = 10.1-10.0 = 0.1, scale = 0.05/0.1 = 0.5, pos.x = 1.0 + (1.0-0.0)*0.5 = 1.5.
		atlas::Vec3 P{}, D{};
		bool G = false;
		TestTrue(TEXT("extrapolate TryEvaluate"), F.TryEvaluate(P, D, G));
		TestTrue(TEXT("extrapolation cap clamps at 1.5"), NearlyEqual(P.x, 1.5f));
	}

	// 5. Sample ordering: out-of-order Input discarded.
	{
		atlas::AvatarFilter F([]() { return 0.0; });
		F.Input(10.0, {0.0f, 0.0f, 0.0f}, {}, false);
		F.Input(9.5, {99.0f, 0.0f, 0.0f}, {}, false);
		TestEqual(TEXT("out-of-order discarded"), F.SampleCount(), static_cast<std::size_t>(1));
	}

	// 6. Reset clears state.
	{
		double WallNow = 0.0;
		atlas::AvatarFilter F([&]() { return WallNow; });
		F.Input(10.0, {1.0f, 0.0f, 0.0f}, {}, false);
		TestEqual(TEXT("count before reset"), F.SampleCount(), static_cast<std::size_t>(1));
		F.Reset();
		TestEqual(TEXT("count after reset"), F.SampleCount(), static_cast<std::size_t>(0));
		TestEqual(TEXT("latency after reset"), F.CurrentLatency(), 0.0);
	}

	// 7. Ring buffer wraps cleanly at 8 samples.
	{
		atlas::AvatarFilter F([]() { return 0.0; });
		for (int i = 0; i < 12; ++i) {
			F.Input(static_cast<double>(i), {static_cast<float>(i), 0.0f, 0.0f}, {}, false);
		}
		TestEqual(TEXT("ring caps at 8"), F.SampleCount(),
			static_cast<std::size_t>(atlas::AvatarFilter::kRingCapacity));
	}

	// 8. UpdateLatency converges toward target.
	{
		atlas::AvatarFilter F([]() { return 0.0; });
		F.Input(0.0, {0.0f, 0.0f, 0.0f}, {}, false);
		// After Input, latency_current = target (snapped). Manually perturb by
		// reseting and re-entering with a stale target via setter change.
		F.latency_frames = 6.0;  // new target = 0.6, but latency_current still 0.3.
		const double Before = F.CurrentLatency();
		F.UpdateLatency(1.0 / 60.0);
		TestTrue(TEXT("latency moves toward larger target"), F.CurrentLatency() > Before);
		TestTrue(TEXT("latency does not overshoot target"), F.CurrentLatency() <= F.TargetLatency());
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
