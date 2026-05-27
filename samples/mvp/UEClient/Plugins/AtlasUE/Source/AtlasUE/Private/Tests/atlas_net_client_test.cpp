#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <limits>
#include <memory>

#include "AtlasNetClient.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/span_writer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientLifecycleTest,
	"Atlas.NetClient.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientLifecycleTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestEqual(TEXT("initial state idle"), Client.GetState(), EAtlasNetClientState::Idle);
	TestNull(TEXT("no ctx pre-create"), Client.GetContext());

	TestTrue(TEXT("create"), Client.Create());
	TestNotNull(TEXT("ctx post-create"), Client.GetContext());

	// Idempotent re-create.
	TestTrue(TEXT("recreate is idempotent"), Client.Create());

	// Worker thread on an idle ctx; AtlasNetPoll returns immediately on no inbound
	// data; we just want to verify the thread starts and stops without race.
	Client.StartRunningThread();
	TestEqual(TEXT("state running"), Client.GetState(), EAtlasNetClientState::Running);

	FPlatformProcess::Sleep(0.05f);

	// TickGameThread on an empty queue with no entities should be a no-op.
	atlas::ClientEntityManager Mgr;
	Client.TickGameThread(Mgr);
	TestEqual(TEXT("manager untouched"), Mgr.Size(), static_cast<std::size_t>(0));

	Client.Destroy();
	TestEqual(TEXT("state idle after destroy"), Client.GetState(), EAtlasNetClientState::Idle);
	TestNull(TEXT("no ctx after destroy"), Client.GetContext());

	// Re-create cycle proves the static registry properly unregisters on Destroy.
	TestTrue(TEXT("create after destroy"), Client.Create());
	TestNotNull(TEXT("ctx post-second-create"), Client.GetContext());
	Client.Destroy();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientDisconnectSentinelTest,
	"Atlas.NetClient.DisconnectSentinel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientDisconnectSentinelTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	// Pre-queue a couple of harmless AoI envelopes, then a disconnect, then a
	// "stale" message that must be dropped without reaching the manager.
	const uint8 EmptyPayload[1] = {0};
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	Client.TriggerDisconnectForTest(/*Reason=*/7);
	Client.DeliverForTest(0xF003, EmptyPayload, 0);

	TestEqual(TEXT("queue depth pre-tick"), Client.GetInboundDepthForTest(), 4);

	atlas::ClientEntityManager Mgr;
	Client.TickGameThread(Mgr);

	TestEqual(TEXT("state Disconnected after sentinel"), Client.GetState(),
		EAtlasNetClientState::Disconnected);
	TestEqual(TEXT("queue drained past sentinel"), Client.GetInboundDepthForTest(), 0);

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientInboundOverflowTest,
	"Atlas.NetClient.InboundOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientInboundOverflowTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	// Fill exactly to cap; nothing should drop yet, latch stays clear.
	const uint8 EmptyPayload[1] = {0};
	for (int32 i = 0; i < kAtlasInboundQueueCap; ++i)
	{
		Client.DeliverForTest(0xF003, EmptyPayload, 0);
	}
	TestEqual(TEXT("depth at cap"), Client.GetInboundDepthForTest(), kAtlasInboundQueueCap);
	TestFalse(TEXT("overflow latch clear at cap"), Client.GetInboundOverflowLoggedForTest());

	// Two extra deliveries; both drop, but only the first warns.
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	Client.DeliverForTest(0xF003, EmptyPayload, 0);
	TestEqual(TEXT("depth unchanged past cap"), Client.GetInboundDepthForTest(),
		kAtlasInboundQueueCap);
	TestTrue(TEXT("overflow latch set"), Client.GetInboundOverflowLoggedForTest());

	// Oversize payload check: a buffer over the byte cap is rejected without
	// touching the queue at all.
	const int32 BeforeOversize = Client.GetInboundDepthForTest();
	TArray<uint8> Huge;
	Huge.SetNumZeroed(kAtlasMaxInboundPayloadBytes + 1);
	Client.DeliverForTest(0xF003, Huge.GetData(), Huge.Num());
	TestEqual(TEXT("oversize payload dropped"), Client.GetInboundDepthForTest(), BeforeOversize);

	Client.Destroy();
	// Destroy must reset the latch so a fresh session starts clean.
	TestTrue(TEXT("create after destroy"), Client.Create());
	TestFalse(TEXT("overflow latch cleared by Destroy"),
		Client.GetInboundOverflowLoggedForTest());
	TestEqual(TEXT("depth reset by Destroy"), Client.GetInboundDepthForTest(), 0);
	Client.Destroy();

	return true;
}

namespace
{
AtlasMovementStateFrame MakeMovementStateFrame()
{
	AtlasMovementStateFrame State{};
	State.position_x = 1.0f;
	State.position_y = 2.0f;
	State.position_z = 3.0f;
	State.velocity_x = 4.0f;
	State.velocity_y = 5.0f;
	State.velocity_z = 6.0f;
	State.direction_z = 1.0f;
	State.flags = 1;
	State.last_processed_input_seq = 42;
	return State;
}

void WriteMovementStateFrame(atlas::SpanWriter& Writer, const AtlasMovementStateFrame& State)
{
	Writer.Write(State.position_x);
	Writer.Write(State.position_y);
	Writer.Write(State.position_z);
	Writer.Write(State.velocity_x);
	Writer.Write(State.velocity_y);
	Writer.Write(State.velocity_z);
	Writer.Write(State.direction_x);
	Writer.Write(State.direction_y);
	Writer.Write(State.direction_z);
	Writer.Write(State.flags);
	Writer.Write(State.last_processed_input_seq);
}

atlas::MovementCommandFrame MakeMovementCommandFrame()
{
	atlas::MovementCommandFrame Command{};
	Command.command_id = 77;
	Command.skill_id = 12;
	Command.type = 1;
	Command.start_position = {1.0f, 2.0f, 3.0f};
	Command.target_position = {4.0f, 5.0f, 6.0f};
	Command.duration_ms = 300;
	Command.elapsed_ms = 33;
	Command.curve_id = 2;
	Command.input_policy = 1;
	Command.collision_policy = 2;
	Command.priority = 9;
	Command.server_tick = 9001;
	return Command;
}

void WriteMovementCommandFrame(atlas::SpanWriter& Writer,
                               const atlas::MovementCommandFrame& Command)
{
	Writer.Write(Command.command_id);
	Writer.Write(Command.skill_id);
	Writer.Write(Command.type);
	Writer.WriteVec3(Command.start_position);
	Writer.WriteVec3(Command.target_position);
	Writer.Write(Command.duration_ms);
	Writer.Write(Command.elapsed_ms);
	Writer.Write(Command.curve_id);
	Writer.Write(Command.input_policy);
	Writer.Write(Command.collision_policy);
	Writer.Write(Command.priority);
	Writer.Write(Command.server_tick);
}

TArray<uint8> ToPayload(const atlas::SpanWriter& Writer)
{
	TArray<uint8> Payload;
	Payload.Append(Writer.Bytes().data(), static_cast<int32>(Writer.Size()));
	return Payload;
}

TArray<uint8> BuildMovementStateAckPayload(uint32 EntityId,
                                           const AtlasMovementStateFrame& State)
{
	atlas::SpanWriter Writer;
	Writer.Write(EntityId);
	Writer.Write(uint32_t{42});
	Writer.Write(uint32_t{9001});
	WriteMovementStateFrame(Writer, State);
	Writer.Write(uint16_t{4});
	return ToPayload(Writer);
}

TArray<uint8> BuildMovementCommandStartPayload(
	uint32 EntityId, const atlas::MovementCommandFrame& Command)
{
	atlas::SpanWriter Writer;
	Writer.Write(EntityId);
	WriteMovementCommandFrame(Writer, Command);
	return ToPayload(Writer);
}

TArray<uint8> BuildMovementCommandEndPayload(uint32 EntityId, uint32 CommandId,
                                             uint32 ServerTick, uint8 Reason,
                                             const AtlasMovementStateFrame& State)
{
	atlas::SpanWriter Writer;
	Writer.Write(EntityId);
	Writer.Write(CommandId);
	Writer.Write(ServerTick);
	Writer.Write(Reason);
	WriteMovementStateFrame(Writer, State);
	return ToPayload(Writer);
}

void DeliverForTest(FAtlasNetClient& Client, atlas::ClientEntityManager& Mgr, uint16 MsgId,
                    const TArray<uint8>& Payload)
{
	Client.DeliverForTest(MsgId, Payload.GetData(), Payload.Num());
	Client.TickGameThread(Mgr);
}

class FMovementAckEntity : public atlas::ClientEntity
{
public:
	FMovementAckEntity(atlas::EntityId Id, atlas::EntityTypeId Type)
		: atlas::ClientEntity(Id, Type) {}

	int32 AckCalls = 0;
	uint32 AckedInputSeq = 0;
	uint32 ServerTick = 0;
	AtlasMovementStateFrame State{};
	uint16 CorrectionFlags = 0;
	int32 CommandStartCalls = 0;
	atlas::MovementCommandFrame CommandStart{};
	int32 CommandEndCalls = 0;
	uint32 CommandEndId = 0;
	uint32 CommandEndServerTick = 0;
	atlas::MovementCommandEndReason CommandEndReason =
		atlas::MovementCommandEndReason::kCompleted;
	AtlasMovementStateFrame CommandEndState{};

	void OnMovementStateAck(uint32_t InAckedInputSeq, uint32_t InServerTick,
	                        const AtlasMovementStateFrame& InState,
	                        uint16_t InCorrectionFlags) override
	{
		++AckCalls;
		AckedInputSeq = InAckedInputSeq;
		ServerTick = InServerTick;
		State = InState;
		CorrectionFlags = InCorrectionFlags;
	}

	void OnMovementCommandStart(const atlas::MovementCommandFrame& InCommand) override
	{
		++CommandStartCalls;
		CommandStart = InCommand;
	}

	void OnMovementCommandEnd(uint32_t InCommandId, uint32_t InServerTick,
	                          atlas::MovementCommandEndReason InReason,
	                          const AtlasMovementStateFrame& InState) override
	{
		++CommandEndCalls;
		CommandEndId = InCommandId;
		CommandEndServerTick = InServerTick;
		CommandEndReason = InReason;
		CommandEndState = InState;
	}
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientMovementStateAckTest,
	"Atlas.NetClient.MovementStateAck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientMovementStateAckTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	atlas::ClientEntityManager Mgr;
	auto Entity = std::make_unique<FMovementAckEntity>(100, 7);
	FMovementAckEntity* Raw = Entity.get();
	TestTrue(TEXT("register entity"), Mgr.Register(std::move(Entity)));

	atlas::SpanWriter Writer;
	Writer.Write(uint32_t{100});
	Writer.Write(uint32_t{42});
	Writer.Write(uint32_t{9001});
	Writer.Write(1.0f);
	Writer.Write(2.0f);
	Writer.Write(3.0f);
	Writer.Write(4.0f);
	Writer.Write(5.0f);
	Writer.Write(6.0f);
	Writer.Write(0.0f);
	Writer.Write(0.0f);
	Writer.Write(1.0f);
	Writer.Write(uint32_t{1});
	Writer.Write(uint32_t{42});
	Writer.Write(uint16_t{4});

	Client.DeliverForTest(0xF005, reinterpret_cast<const uint8*>(Writer.Bytes().data()),
		static_cast<int32>(Writer.Size()));
	Client.TickGameThread(Mgr);

	TestEqual(TEXT("ack calls"), Raw->AckCalls, 1);
	TestEqual(TEXT("acked seq"), Raw->AckedInputSeq, static_cast<uint32>(42));
	TestEqual(TEXT("server tick"), Raw->ServerTick, static_cast<uint32>(9001));
	TestEqual(TEXT("position x"), Raw->State.position_x, 1.0f);
	TestEqual(TEXT("velocity y"), Raw->State.velocity_y, 5.0f);
	TestEqual(TEXT("direction z"), Raw->State.direction_z, 1.0f);
	TestEqual(TEXT("state flags"), Raw->State.flags, static_cast<uint32>(1));
	TestEqual(TEXT("last processed"), Raw->State.last_processed_input_seq,
		static_cast<uint32>(42));
	TestEqual(TEXT("correction flags"), Raw->CorrectionFlags, static_cast<uint16>(4));

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientMovementStateAckRejectsInvalidPayloadTest,
	"Atlas.NetClient.MovementStateAckRejectsInvalidPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientMovementStateAckRejectsInvalidPayloadTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	atlas::ClientEntityManager Mgr;
	auto Entity = std::make_unique<FMovementAckEntity>(100, 7);
	FMovementAckEntity* Raw = Entity.get();
	TestTrue(TEXT("register entity"), Mgr.Register(std::move(Entity)));

	const auto ValidState = MakeMovementStateFrame();
	DeliverForTest(Client, Mgr, 0xF005, BuildMovementStateAckPayload(0, ValidState));

	auto NonFiniteState = MakeMovementStateFrame();
	NonFiniteState.position_x = std::numeric_limits<float>::infinity();
	DeliverForTest(Client, Mgr, 0xF005, BuildMovementStateAckPayload(100, NonFiniteState));

	auto Trailing = BuildMovementStateAckPayload(100, ValidState);
	Trailing.Add(0);
	DeliverForTest(Client, Mgr, 0xF005, Trailing);

	auto MultiBitFlags = BuildMovementStateAckPayload(100, ValidState);
	MultiBitFlags[MultiBitFlags.Num() - 2] = 0x03;
	MultiBitFlags[MultiBitFlags.Num() - 1] = 0x00;
	DeliverForTest(Client, Mgr, 0xF005, MultiBitFlags);

	auto ReservedFlag = BuildMovementStateAckPayload(100, ValidState);
	ReservedFlag[ReservedFlag.Num() - 2] = 0x20;
	ReservedFlag[ReservedFlag.Num() - 1] = 0x00;
	DeliverForTest(Client, Mgr, 0xF005, ReservedFlag);

	TestEqual(TEXT("ack calls"), Raw->AckCalls, 0);

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientMovementCommandStartTest,
	"Atlas.NetClient.MovementCommandStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientMovementCommandStartTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	atlas::ClientEntityManager Mgr;
	auto Entity = std::make_unique<FMovementAckEntity>(100, 7);
	FMovementAckEntity* Raw = Entity.get();
	TestTrue(TEXT("register entity"), Mgr.Register(std::move(Entity)));

	atlas::SpanWriter Writer;
	Writer.Write(uint32_t{100});
	Writer.Write(uint32_t{77});
	Writer.Write(uint16_t{12});
	Writer.Write(uint8_t{1});
	Writer.Write(1.0f);
	Writer.Write(2.0f);
	Writer.Write(3.0f);
	Writer.Write(4.0f);
	Writer.Write(5.0f);
	Writer.Write(6.0f);
	Writer.Write(uint16_t{300});
	Writer.Write(uint16_t{33});
	Writer.Write(uint16_t{2});
	Writer.Write(uint8_t{1});
	Writer.Write(uint8_t{2});
	Writer.Write(uint8_t{9});
	Writer.Write(uint32_t{9001});

	Client.DeliverForTest(0xF006, reinterpret_cast<const uint8*>(Writer.Bytes().data()),
		static_cast<int32>(Writer.Size()));
	Client.TickGameThread(Mgr);

	TestEqual(TEXT("command start calls"), Raw->CommandStartCalls, 1);
	TestEqual(TEXT("command id"), Raw->CommandStart.command_id, static_cast<uint32>(77));
	TestEqual(TEXT("skill id"), Raw->CommandStart.skill_id, static_cast<uint16>(12));
	TestEqual(TEXT("command type"), Raw->CommandStart.type, static_cast<uint8>(1));
	TestEqual(TEXT("start x"), Raw->CommandStart.start_position.x, 1.0f);
	TestEqual(TEXT("target z"), Raw->CommandStart.target_position.z, 6.0f);
	TestEqual(TEXT("duration"), Raw->CommandStart.duration_ms, static_cast<uint16>(300));
	TestEqual(TEXT("elapsed"), Raw->CommandStart.elapsed_ms, static_cast<uint16>(33));
	TestEqual(TEXT("curve id"), Raw->CommandStart.curve_id, static_cast<uint16>(2));
	TestEqual(TEXT("input policy"), Raw->CommandStart.input_policy, static_cast<uint8>(1));
	TestEqual(TEXT("collision policy"), Raw->CommandStart.collision_policy,
		static_cast<uint8>(2));
	TestEqual(TEXT("priority"), Raw->CommandStart.priority, static_cast<uint8>(9));
	TestEqual(TEXT("server tick"), Raw->CommandStart.server_tick, static_cast<uint32>(9001));

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientMovementCommandStartRejectsInvalidPayloadTest,
	"Atlas.NetClient.MovementCommandStartRejectsInvalidPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientMovementCommandStartRejectsInvalidPayloadTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	atlas::ClientEntityManager Mgr;
	auto Entity = std::make_unique<FMovementAckEntity>(100, 7);
	FMovementAckEntity* Raw = Entity.get();
	TestTrue(TEXT("register entity"), Mgr.Register(std::move(Entity)));

	const auto ValidCommand = MakeMovementCommandFrame();
	DeliverForTest(Client, Mgr, 0xF006, BuildMovementCommandStartPayload(0, ValidCommand));

	auto InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.command_id = 0;
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.duration_ms = 0;
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.elapsed_ms = InvalidCommand.duration_ms + 1;
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.type = 7;
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.input_policy = 3;
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.collision_policy = 3;
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	InvalidCommand = MakeMovementCommandFrame();
	InvalidCommand.start_position.x = std::numeric_limits<float>::infinity();
	DeliverForTest(Client, Mgr, 0xF006,
		BuildMovementCommandStartPayload(100, InvalidCommand));

	auto Trailing = BuildMovementCommandStartPayload(100, ValidCommand);
	Trailing.Add(0);
	DeliverForTest(Client, Mgr, 0xF006, Trailing);

	TestEqual(TEXT("command start calls"), Raw->CommandStartCalls, 0);

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientMovementCommandEndTest,
	"Atlas.NetClient.MovementCommandEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientMovementCommandEndTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	atlas::ClientEntityManager Mgr;
	auto Entity = std::make_unique<FMovementAckEntity>(100, 7);
	FMovementAckEntity* Raw = Entity.get();
	TestTrue(TEXT("register entity"), Mgr.Register(std::move(Entity)));

	AtlasMovementStateFrame State{};
	State.position_x = 7.0f;
	State.velocity_y = 8.0f;
	State.direction_z = 1.0f;
	State.flags = 1;
	State.last_processed_input_seq = 42;

	atlas::SpanWriter Writer;
	Writer.Write(uint32_t{100});
	Writer.Write(uint32_t{77});
	Writer.Write(uint32_t{9002});
	Writer.Write(static_cast<uint8_t>(atlas::MovementCommandEndReason::kCollision));
	WriteMovementStateFrame(Writer, State);

	Client.DeliverForTest(0xF007, reinterpret_cast<const uint8*>(Writer.Bytes().data()),
		static_cast<int32>(Writer.Size()));
	Client.TickGameThread(Mgr);

	TestEqual(TEXT("command end calls"), Raw->CommandEndCalls, 1);
	TestEqual(TEXT("command end id"), Raw->CommandEndId, static_cast<uint32>(77));
	TestEqual(TEXT("command end tick"), Raw->CommandEndServerTick, static_cast<uint32>(9002));
	TestEqual(TEXT("command end reason"), static_cast<uint8>(Raw->CommandEndReason),
		static_cast<uint8>(atlas::MovementCommandEndReason::kCollision));
	TestEqual(TEXT("end position x"), Raw->CommandEndState.position_x, 7.0f);
	TestEqual(TEXT("end velocity y"), Raw->CommandEndState.velocity_y, 8.0f);
	TestEqual(TEXT("end direction z"), Raw->CommandEndState.direction_z, 1.0f);
	TestEqual(TEXT("end flags"), Raw->CommandEndState.flags, static_cast<uint32>(1));
	TestEqual(TEXT("end last processed"), Raw->CommandEndState.last_processed_input_seq,
		static_cast<uint32>(42));

	Client.Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasNetClientMovementCommandEndRejectsInvalidPayloadTest,
	"Atlas.NetClient.MovementCommandEndRejectsInvalidPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasNetClientMovementCommandEndRejectsInvalidPayloadTest::RunTest(const FString&)
{
	FAtlasNetClient Client;
	TestTrue(TEXT("create"), Client.Create());

	atlas::ClientEntityManager Mgr;
	auto Entity = std::make_unique<FMovementAckEntity>(100, 7);
	FMovementAckEntity* Raw = Entity.get();
	TestTrue(TEXT("register entity"), Mgr.Register(std::move(Entity)));

	const auto ValidState = MakeMovementStateFrame();
	const uint8 ValidReason =
		static_cast<uint8>(atlas::MovementCommandEndReason::kCollision);
	DeliverForTest(Client, Mgr, 0xF007,
		BuildMovementCommandEndPayload(0, 77, 9002, ValidReason, ValidState));
	DeliverForTest(Client, Mgr, 0xF007,
		BuildMovementCommandEndPayload(100, 0, 9002, ValidReason, ValidState));
	DeliverForTest(Client, Mgr, 0xF007,
		BuildMovementCommandEndPayload(100, 77, 9002, 4, ValidState));

	auto NonFiniteState = MakeMovementStateFrame();
	NonFiniteState.velocity_y = std::numeric_limits<float>::infinity();
	DeliverForTest(Client, Mgr, 0xF007,
		BuildMovementCommandEndPayload(100, 77, 9002, ValidReason, NonFiniteState));

	auto Trailing =
		BuildMovementCommandEndPayload(100, 77, 9002, ValidReason, ValidState);
	Trailing.Add(0);
	DeliverForTest(Client, Mgr, 0xF007, Trailing);

	TestEqual(TEXT("command end calls"), Raw->CommandEndCalls, 0);

	Client.Destroy();
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
