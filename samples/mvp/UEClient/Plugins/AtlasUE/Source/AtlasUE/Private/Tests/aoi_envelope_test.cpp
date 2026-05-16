#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstring>
#include <memory>
#include <vector>

#include "AtlasCore/aoi_envelope.h"
#include "AtlasCore/client_entity.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/entity_view.h"

namespace
{
class CaptureEntity : public atlas::ClientEntity
{
public:
	CaptureEntity(atlas::EntityId id, atlas::EntityTypeId t) : atlas::ClientEntity(id, t) {}

	int PositionCalls = 0;
	atlas::Vec3 LastPos{};
	atlas::Vec3 LastDir{};
	bool LastOnGround = false;
	double LastServerTime = 0.0;

	void OnPositionReceived(double t, const atlas::Vec3& p, const atlas::Vec3& d, bool g) override
	{
		LastServerTime = t;
		LastPos = p;
		LastDir = d;
		LastOnGround = g;
		++PositionCalls;
	}
};

template <typename T>
void Append(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasAoIEnvelopeTest,
	"Atlas.AoIEnvelope.Decode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasAoIEnvelopeTest::RunTest(const FString&)
{
	using atlas::EnvelopeDecodeResult;
	using atlas::EnvelopeKind;

	atlas::ClientEntityManager Manager;
	Manager.RegisterFactory(7, [](atlas::EntityId id, atlas::EntityTypeId t) {
		return std::make_unique<CaptureEntity>(id, t);
	});

	// kEntityEnter: kind=1, eid=42, typeId=7, pos=(1,2,3), dir=(0,0,1), onGround=1, t=10.5.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityEnter));
		Append<uint32_t>(buf, 42);
		Append<uint16_t>(buf, 7);
		Append<float>(buf, 1.0f);
		Append<float>(buf, 2.0f);
		Append<float>(buf, 3.0f);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 1.0f);
		Append<uint8_t>(buf, 1);
		Append<double>(buf, 10.5);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("enter decode result"), Result, EnvelopeDecodeResult::kOk);
		auto* E = static_cast<CaptureEntity*>(Manager.Find(42));
		TestNotNull(TEXT("entity created"), E);
		if (E)
		{
			TestEqual(TEXT("type id"), E->TypeId(), static_cast<atlas::EntityTypeId>(7));
			TestEqual(TEXT("enter pos.x"), E->LastPos.x, 1.0f);
			TestEqual(TEXT("enter pos.y"), E->LastPos.y, 2.0f);
			TestEqual(TEXT("enter dir.z"), E->LastDir.z, 1.0f);
			TestEqual(TEXT("enter server_time"), E->LastServerTime, 10.5);
			TestTrue(TEXT("enter on_ground"), E->LastOnGround);
		}
	}

	// kEntityPositionUpdate: kind=3, eid=42, pos=(4,5,6), dir=(0,1,0), g=0, t=20.25.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPositionUpdate));
		Append<uint32_t>(buf, 42);
		Append<float>(buf, 4.0f);
		Append<float>(buf, 5.0f);
		Append<float>(buf, 6.0f);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 1.0f);
		Append<float>(buf, 0.0f);
		Append<uint8_t>(buf, 0);
		Append<double>(buf, 20.25);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("pos decode result"), Result, EnvelopeDecodeResult::kOk);
		auto* E = static_cast<CaptureEntity*>(Manager.Find(42));
		TestEqual(TEXT("position calls"), E->PositionCalls, 2);  // enter seeded the first
		TestEqual(TEXT("update pos.x"), E->LastPos.x, 4.0f);
		TestEqual(TEXT("update dir.y"), E->LastDir.y, 1.0f);
		TestEqual(TEXT("update server_time"), E->LastServerTime, 20.25);
		TestFalse(TEXT("update on_ground"), E->LastOnGround);
	}

	// kEntityLeave: kind=2, eid=42. Header-only.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityLeave));
		Append<uint32_t>(buf, 42);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("leave decode result"), Result, EnvelopeDecodeResult::kOk);
		TestNull(TEXT("entity removed"), Manager.Find(42));
	}

	// kEntityPropertyUpdate: M0 silently skipped, manager unaffected.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPropertyUpdate));
		Append<uint32_t>(buf, 99);
		Append<uint64_t>(buf, 1);
		Append<uint8_t>(buf, 0xCD);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("property skipped"), Result, EnvelopeDecodeResult::kPropertyUpdateSkipped);
	}

	// Truncated header.
	{
		uint8_t Tiny[3] = {1, 0, 0};
		const auto Result = atlas::DecodeAoIEnvelope(Tiny, sizeof(Tiny), Manager);
		TestEqual(TEXT("truncated header"), Result, EnvelopeDecodeResult::kTruncated);
	}

	// Truncated enter body.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityEnter));
		Append<uint32_t>(buf, 43);
		Append<uint16_t>(buf, 7);
		// missing 6 floats + onGround + serverTime
		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("truncated enter"), Result, EnvelopeDecodeResult::kTruncated);
		TestNull(TEXT("no entity on truncated enter"), Manager.Find(43));
	}

	// Unknown kind.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, 99);
		Append<uint32_t>(buf, 100);
		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("unknown kind"), Result, EnvelopeDecodeResult::kUnknownKind);
	}

	// Enter for an unregistered type: returns kOk, but Find shows nothing.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityEnter));
		Append<uint32_t>(buf, 44);
		Append<uint16_t>(buf, 999);  // unregistered type
		for (int i = 0; i < 6; ++i) Append<float>(buf, 0.0f);
		Append<uint8_t>(buf, 0);
		Append<double>(buf, 0.0);
		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("unknown type still kOk"), Result, EnvelopeDecodeResult::kOk);
		TestNull(TEXT("no entity for unknown type"), Manager.Find(44));
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
