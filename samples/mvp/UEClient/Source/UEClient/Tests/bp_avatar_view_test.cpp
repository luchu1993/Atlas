#include "Tests/bp_avatar_view_test.h"

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstdint>
#include <cstring>
#include <vector>

#include "AtlasAvatarView.h"
#include "AtlasCore/span_reader.h"
#include "AtlasUE.h"
#include "BpAvatarEntity.h"
#include "entitydef/entitydef_api.h"

namespace
{
template <typename T>
void AppendBp(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasBpAvatarViewTest,
	"Atlas.BpView.AvatarDelegatesFireOnDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasBpAvatarViewTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = FAtlasUEModule::GetEdrContext();
	if (!TestNotNull(TEXT("edr context"), Ctx)) return false;
	const AtlasEdrEntity* Desc = AtlasEdrFindEntityByName(Ctx, "Avatar");
	if (!TestNotNull(TEXT("avatar descriptor"), Desc)) return false;

	UAtlasAvatarView* View = NewObject<UAtlasAvatarView>();
	UAtlasBpAvatarListener* Listener = NewObject<UAtlasBpAvatarListener>();
	View->OnHpChanged.AddDynamic(Listener, &UAtlasBpAvatarListener::OnHp);
	View->OnLevelChanged.AddDynamic(Listener, &UAtlasBpAvatarListener::OnLevel);

	FBpAvatarEntity Avatar(7, atlas::mvp::Avatar::kTypeId);
	Avatar.BindDescriptor(Desc, Ctx);
	Avatar.BindView(View);
	TestEqual(TEXT("view bound to entity"), View->GetEntity(),
		static_cast<atlas::mvp::Avatar*>(&Avatar));

	// First delta: Hp=120, Level=3 → both delegates fire once with 0→new.
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint8_t>(Buf, 0x01);
		AppendBp<uint8_t>(Buf, (1 << 0) | (1 << 4));  // Hp + Level bits
		AppendBp<int32_t>(Buf, 120);
		AppendBp<int32_t>(Buf, 3);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 1"), Avatar.ApplyDelta(R));
		TestEqual(TEXT("hp delegate fired"), Listener->HpCalls, 1);
		TestEqual(TEXT("hp old"), Listener->LastOld, 0);
		TestEqual(TEXT("hp new"), Listener->LastNew, 120);
		TestEqual(TEXT("level delegate fired"), Listener->LevelCalls, 1);
		TestEqual(TEXT("level new"), Listener->LastLevelNew, 3);
		// BP getter mirrors current entity state.
		TestEqual(TEXT("view GetHp matches"), View->GetHp(), 120);
		TestEqual(TEXT("view GetLevel matches"), View->GetLevel(), 3);
	}

	// Second delta: Hp dirty bit set but value unchanged (120) → no delegate.
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint8_t>(Buf, 0x01);
		AppendBp<uint8_t>(Buf, 1 << 0);
		AppendBp<int32_t>(Buf, 120);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 2"), Avatar.ApplyDelta(R));
		TestEqual(TEXT("hp delegate not refired"), Listener->HpCalls, 1);
	}

	// Detach: BindView(null) drops the link; subsequent deltas don't fire.
	{
		Avatar.BindView(nullptr);
		std::vector<uint8_t> Buf;
		AppendBp<uint8_t>(Buf, 0x01);
		AppendBp<uint8_t>(Buf, 1 << 0);
		AppendBp<int32_t>(Buf, 99);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 3"), Avatar.ApplyDelta(R));
		TestEqual(TEXT("hp delegate silent after detach"), Listener->HpCalls, 1);
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
