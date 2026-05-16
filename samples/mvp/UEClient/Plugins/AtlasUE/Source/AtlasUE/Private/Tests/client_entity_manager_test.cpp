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

#endif  // WITH_DEV_AUTOMATION_TESTS
