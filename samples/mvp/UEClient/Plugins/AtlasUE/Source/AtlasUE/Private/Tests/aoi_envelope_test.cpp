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

void AppendPackedUInt32(std::vector<uint8_t>& buf, uint32_t v)
{
	if (v < 0xFE)
	{
		Append<uint8_t>(buf, static_cast<uint8_t>(v));
	}
	else if (v <= 0xFFFF)
	{
		Append<uint8_t>(buf, 0xFE);
		Append<uint16_t>(buf, static_cast<uint16_t>(v));
	}
	else
	{
		Append<uint8_t>(buf, 0xFF);
		Append<uint32_t>(buf, v);
	}
}

void AppendPackedXZ12(std::vector<uint8_t>& buf, int16_t x, int16_t z)
{
	const uint32_t packed = (static_cast<uint16_t>(x) & 0x0FFFu) |
		((static_cast<uint16_t>(z) & 0x0FFFu) << 12);
	Append<uint8_t>(buf, static_cast<uint8_t>(packed & 0xFFu));
	Append<uint8_t>(buf, static_cast<uint8_t>((packed >> 8) & 0xFFu));
	Append<uint8_t>(buf, static_cast<uint8_t>((packed >> 16) & 0xFFu));
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

	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPositionBatch));
		Append<uint32_t>(buf, 0);
		Append<float>(buf, 10.0f);
		Append<float>(buf, 1.0f);
		Append<float>(buf, -5.0f);
		Append<double>(buf, 21.0);
		Append<uint16_t>(buf, 2);
		Append<uint8_t>(buf, 0x01 | 0x02 | 0x08);
		AppendPackedUInt32(buf, 42);
		Append<uint8_t>(buf, 0x01);
		AppendPackedUInt32(buf, 0);
		AppendPackedXZ12(buf, 123, 50);
		Append<int16_t>(buf, 0);
		Append<uint8_t>(buf, 64);
		Append<uint16_t>(buf, 500);
		AppendPackedUInt32(buf, 999 - 42);
		AppendPackedXZ12(buf, 1, 3);
		Append<int16_t>(buf, 2);
		Append<uint8_t>(buf, 0);
		Append<uint16_t>(buf, 1000);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("position batch decode result"), Result, EnvelopeDecodeResult::kOk);
		auto* E = static_cast<CaptureEntity*>(Manager.Find(42));
		TestEqual(TEXT("position batch calls"), E->PositionCalls, 3);
		TestTrue(TEXT("position batch pos.x"), FMath::IsNearlyEqual(E->LastPos.x, 11.23f, 0.001f));
		TestTrue(TEXT("position batch pos.z"), FMath::IsNearlyEqual(E->LastPos.z, -4.5f, 0.001f));
		TestTrue(TEXT("position batch dir.x"), FMath::IsNearlyEqual(E->LastDir.x, 1.0f, 0.001f));
		TestTrue(TEXT("position batch dir.z"), FMath::IsNearlyZero(E->LastDir.z, 0.001f));
		TestEqual(TEXT("position batch server_time"), E->LastServerTime, 21.5);
		TestTrue(TEXT("position batch on_ground"), E->LastOnGround);
		TestNull(TEXT("position batch unknown ignored"), Manager.Find(999));
	}

	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityEnter));
		Append<uint32_t>(buf, 43);
		Append<uint16_t>(buf, 7);
		for (int i = 0; i < 6; ++i) Append<float>(buf, 0.0f);
		Append<uint8_t>(buf, 1);
		Append<double>(buf, 21.75);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("second enter decode result"), Result, EnvelopeDecodeResult::kOk);
		TestNotNull(TEXT("second entity created"), Manager.Find(43));
	}

	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPositionBatch));
		Append<uint32_t>(buf, 0);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 0.0f);
		Append<double>(buf, 22.0);
		Append<uint16_t>(buf, 2);
		Append<uint8_t>(buf, 0x04 | 0x20);
		AppendPackedUInt32(buf, 42);
		AppendPackedXZ12(buf, 100, 200);
		Append<uint8_t>(buf, 0);
		AppendPackedXZ12(buf, -100, -200);
		Append<uint8_t>(buf, 64);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("sequential batch decode result"), Result, EnvelopeDecodeResult::kOk);
		auto* E42 = static_cast<CaptureEntity*>(Manager.Find(42));
		auto* E43 = static_cast<CaptureEntity*>(Manager.Find(43));
		TestEqual(TEXT("sequential batch calls 42"), E42->PositionCalls, 4);
		TestEqual(TEXT("sequential batch calls 43"), E43->PositionCalls, 2);
		TestTrue(TEXT("sequential batch pos 43 x"), FMath::IsNearlyEqual(E43->LastPos.x, -1.0f, 0.001f));
		TestTrue(TEXT("sequential batch pos 43 z"), FMath::IsNearlyEqual(E43->LastPos.z, -2.0f, 0.001f));
		TestTrue(TEXT("sequential batch dir 43 x"), FMath::IsNearlyEqual(E43->LastDir.x, 1.0f, 0.001f));
	}

	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPositionBatch));
		Append<uint32_t>(buf, 0);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 0.0f);
		Append<float>(buf, 0.0f);
		Append<double>(buf, 23.0);
		Append<uint16_t>(buf, 1);
		Append<uint8_t>(buf, 0x80 | 0x04 | 0x20);
		AppendPackedUInt32(buf, 42);
		AppendPackedXZ12(buf, 123, 456);
		Append<uint8_t>(buf, 0);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("unknown batch flags"), Result, EnvelopeDecodeResult::kUnsupportedFlags);
		auto* E = static_cast<CaptureEntity*>(Manager.Find(42));
		TestEqual(TEXT("unknown flags do not apply"), E->PositionCalls, 4);
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

	// kEntityPropertyUpdate for an unknown entity (left AoI) drops cleanly.
	// Apply-against-bound-descriptor coverage lives in client_entity_apply_delta_test.
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPropertyUpdate));
		Append<uint32_t>(buf, 99);
		Append<uint64_t>(buf, 1);
		Append<uint8_t>(buf, 0xCD);

		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("property update for unknown entity drops"), Result,
			EnvelopeDecodeResult::kOk);
	}

	// Truncated property update header (event_seq missing).
	{
		std::vector<uint8_t> buf;
		Append<uint8_t>(buf, static_cast<uint8_t>(EnvelopeKind::kEntityPropertyUpdate));
		Append<uint32_t>(buf, 42);
		const auto Result = atlas::DecodeAoIEnvelope(buf.data(), buf.size(), Manager);
		TestEqual(TEXT("truncated property header"), Result, EnvelopeDecodeResult::kTruncated);
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
