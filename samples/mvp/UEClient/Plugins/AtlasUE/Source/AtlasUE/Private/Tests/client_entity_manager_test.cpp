#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <memory>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/entity_view.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasClientEntityManagerTest,
	"Atlas.ClientEntityManager.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
class TestView : public atlas::EntityView
{
public:
	int TransformCount = 0;
	int PropertyCount = 0;
	uint16 LastFieldId = 0;

	void OnTransformReplicated(const atlas::Vec3&, const atlas::Quat&) override { ++TransformCount; }
	void OnPropertyChanged(uint16_t field_id) override
	{
		++PropertyCount;
		LastFieldId = static_cast<uint16>(field_id);
	}
};
}  // namespace

bool FAtlasClientEntityManagerTest::RunTest(const FString&)
{
	atlas::ClientEntityManager Manager;

	TestEqual(TEXT("empty manager size"), Manager.Size(), static_cast<std::size_t>(0));
	TestNull(TEXT("find on empty"), Manager.Find(1));

	auto Entity = std::make_unique<atlas::ClientEntity>(1, 100);
	atlas::ClientEntity* Raw = Entity.get();
	TestTrue(TEXT("register"), Manager.Register(std::move(Entity)));
	TestEqual(TEXT("size after register"), Manager.Size(), static_cast<std::size_t>(1));
	TestEqual(TEXT("find by id"), Manager.Find(1), Raw);

	auto Dup = std::make_unique<atlas::ClientEntity>(1, 100);
	TestFalse(TEXT("duplicate id rejected"), Manager.Register(std::move(Dup)));

	auto Invalid = std::make_unique<atlas::ClientEntity>(atlas::kInvalidEntityId, 100);
	TestFalse(TEXT("invalid id rejected"), Manager.Register(std::move(Invalid)));

	TestFalse(TEXT("register null"), Manager.Register(nullptr));

	auto View = std::make_unique<TestView>();
	TestView* ViewRaw = View.get();
	Raw->AttachView(std::move(View));
	TestEqual(TEXT("view attached"), Raw->View(), static_cast<atlas::EntityView*>(ViewRaw));

	ViewRaw->OnPropertyChanged(7);
	ViewRaw->OnTransformReplicated({}, {});
	TestEqual(TEXT("view recorded property"), ViewRaw->PropertyCount, 1);
	TestEqual(TEXT("view recorded transform"), ViewRaw->TransformCount, 1);
	TestEqual(TEXT("view recorded field id"), ViewRaw->LastFieldId, static_cast<uint16>(7));

	Raw->DetachView();
	TestNull(TEXT("view detached"), Raw->View());

	TestTrue(TEXT("remove existing"), Manager.Remove(1));
	TestEqual(TEXT("size after remove"), Manager.Size(), static_cast<std::size_t>(0));
	TestFalse(TEXT("remove missing"), Manager.Remove(1));

	Manager.Register(std::make_unique<atlas::ClientEntity>(2, 100));
	Manager.Register(std::make_unique<atlas::ClientEntity>(3, 100));
	Manager.Register(std::make_unique<atlas::ClientEntity>(4, 100));
	TestEqual(TEXT("size before clear"), Manager.Size(), static_cast<std::size_t>(3));

	int IterCount = 0;
	Manager.ForEach([&](const atlas::ClientEntity& E) { (void)E; ++IterCount; });
	TestEqual(TEXT("ForEach count"), IterCount, 3);

	Manager.Clear();
	TestEqual(TEXT("size after clear"), Manager.Size(), static_cast<std::size_t>(0));

	return true;
}

namespace
{
class CapturingEntity : public atlas::ClientEntity
{
public:
	CapturingEntity(atlas::EntityId id, atlas::EntityTypeId t = 100)
		: atlas::ClientEntity(id, t) {}

	double LastServerTime = -1.0;
	atlas::Vec3 LastPos{};
	bool LastOnGround = false;
	int PositionCalls = 0;
	double TickDtSum = 0.0;
	int TickCalls = 0;

	void OnPositionReceived(double t, const atlas::Vec3& p, const atlas::Vec3&, bool g) override
	{
		LastServerTime = t;
		LastPos = p;
		LastOnGround = g;
		++PositionCalls;
	}
	void TickInterpolation(double dt) override
	{
		TickDtSum += dt;
		++TickCalls;
	}
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasClientEntityHooksTest,
	"Atlas.ClientEntity.PositionHookAndTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasClientEntityHooksTest::RunTest(const FString&)
{
	atlas::ClientEntityManager Manager;
	auto Entity = std::make_unique<CapturingEntity>(42);
	CapturingEntity* Raw = Entity.get();
	Manager.Register(std::move(Entity));

	Manager.HandlePositionUpdate(42, 5.0, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, true);
	TestEqual(TEXT("hook calls"), Raw->PositionCalls, 1);
	TestEqual(TEXT("server_time forwarded"), Raw->LastServerTime, 5.0);
	TestEqual(TEXT("pos.x forwarded"), Raw->LastPos.x, 1.0f);
	TestEqual(TEXT("on_ground forwarded"), Raw->LastOnGround, true);

	Manager.HandlePositionUpdate(999, 6.0, {}, {}, false);
	TestEqual(TEXT("unknown id is silent"), Raw->PositionCalls, 1);

	Manager.TickAll(0.016);
	Manager.TickAll(0.016);
	TestEqual(TEXT("tick called"), Raw->TickCalls, 2);
	TestEqual(TEXT("tick dt sum"), Raw->TickDtSum, 0.032);

	// Default base class hooks are no-op.
	atlas::ClientEntity Plain(7, 0);
	Plain.OnPositionReceived(0.0, {}, {}, false);
	Plain.TickInterpolation(1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasClientEntityManagerHandleCreateTest,
	"Atlas.ClientEntityManager.HandleCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasClientEntityManagerHandleCreateTest::RunTest(const FString&)
{
	atlas::ClientEntityManager Mgr;
	Mgr.RegisterFactory(7, [](atlas::EntityId id, atlas::EntityTypeId t) {
		return std::make_unique<CapturingEntity>(id, t);
	});

	TestTrue(TEXT("create"), Mgr.HandleCreate(42, 7));
	auto* E = static_cast<CapturingEntity*>(Mgr.Find(42));
	TestNotNull(TEXT("entity registered"), E);
	if (E)
	{
		TestEqual(TEXT("type id"), E->TypeId(), static_cast<atlas::EntityTypeId>(7));
		TestEqual(TEXT("position hook NOT fired"), E->PositionCalls, 0);
	}

	TestFalse(TEXT("duplicate id rejected"), Mgr.HandleCreate(42, 7));
	TestFalse(TEXT("invalid id rejected"), Mgr.HandleCreate(atlas::kInvalidEntityId, 7));

	TestFalse(TEXT("unknown type rejected"), Mgr.HandleCreate(43, 999));
	TestNull(TEXT("no entity for unknown type"), Mgr.Find(43));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasClientEntityManagerHandleEnterReusesTest,
	"Atlas.ClientEntityManager.HandleEnterReusesExisting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasClientEntityManagerHandleEnterReusesTest::RunTest(const FString&)
{
	atlas::ClientEntityManager Mgr;
	Mgr.RegisterFactory(7, [](atlas::EntityId id, atlas::EntityTypeId t) {
		return std::make_unique<CapturingEntity>(id, t);
	});

	// Pre-create via HandleCreate — simulates the EntityTransferred path that
	// produces an entity before its first AoI envelope arrives.
	TestTrue(TEXT("HandleCreate"), Mgr.HandleCreate(42, 7));
	auto* E = static_cast<CapturingEntity*>(Mgr.Find(42));
	TestNotNull(TEXT("entity registered"), E);
	TestEqual(TEXT("no position seeded by HandleCreate"), E->PositionCalls, 0);

	// HandleEnter on the same id must reuse + seed, not double-create.
	TestTrue(TEXT("HandleEnter reuses"),
		Mgr.HandleEnter(42, 7, 12.0, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, true));
	TestEqual(TEXT("position hook fired"), E->PositionCalls, 1);
	TestEqual(TEXT("server_time forwarded"), E->LastServerTime, 12.0);
	TestEqual(TEXT("pos.x forwarded"), E->LastPos.x, 1.0f);
	TestEqual(TEXT("on_ground forwarded"), E->LastOnGround, true);
	TestEqual(TEXT("entity not duplicated"), Mgr.Size(), static_cast<std::size_t>(1));

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
