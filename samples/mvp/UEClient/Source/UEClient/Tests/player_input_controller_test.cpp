#include "AtlasPlayerInputController.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BpAvatarEntity.h"
#include "MovementCommandCurves.h"
#include "OwnerMovementPredictor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasPlayerInputSequenceWrapTest,
	"Atlas.Mvp.PlayerInput.SequenceWrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasPlayerInputSequenceWrapTest::RunTest(const FString&)
{
	TestTrue(TEXT("forward seq is newer"),
	         FOwnerMovementPredictor::IsInputSeqNewer(2, 1));
	TestFalse(TEXT("same seq is not newer"),
	          FOwnerMovementPredictor::IsInputSeqNewer(1, 1));
	TestFalse(TEXT("older seq is not newer"),
	          FOwnerMovementPredictor::IsInputSeqNewer(1, 2));
	TestTrue(TEXT("wrapped zero is newer"),
	         FOwnerMovementPredictor::IsInputSeqNewer(0, UINT32_MAX));
	TestFalse(TEXT("pre-wrap max is older than zero"),
	          FOwnerMovementPredictor::IsInputSeqNewer(UINT32_MAX, 0));
	TestTrue(TEXT("newer ack seq wins"),
	         FOwnerMovementPredictor::IsMovementAckNewer(43, 1, 42, 100));
	TestTrue(TEXT("newer ack tick wins for same seq"),
	         FOwnerMovementPredictor::IsMovementAckNewer(42, 101, 42, 100));
	TestFalse(TEXT("same ack tick is duplicate"),
	          FOwnerMovementPredictor::IsMovementAckNewer(42, 100, 42, 100));
	TestFalse(TEXT("older ack tick loses for same seq"),
	          FOwnerMovementPredictor::IsMovementAckNewer(42, 90, 42, 100));
	TestEqual(TEXT("seed next from newer ack"),
	          FOwnerMovementPredictor::SeedNextInputSeqFromAck(2, 1000), 1001u);
	TestEqual(TEXT("seed next keeps newer local"),
	          FOwnerMovementPredictor::SeedNextInputSeqFromAck(43, 41), 43u);
	TestEqual(TEXT("seed next wraps"),
	          FOwnerMovementPredictor::SeedNextInputSeqFromAck(UINT32_MAX, UINT32_MAX), 0u);

	FOwnerMovementPredictor Predictor;
	AtlasMovementInputFrame Frame{};
	Frame.seq = UINT32_MAX - 1;
	Predictor.StoreInputForTest(Frame);
	Frame.seq = UINT32_MAX;
	Predictor.StoreInputForTest(Frame);
	Frame.seq = 0;
	Predictor.StoreInputForTest(Frame);
	Frame.seq = 1;
	Predictor.StoreInputForTest(Frame);

	Predictor.DropAckedForTest(UINT32_MAX - 1);
	TestEqual(TEXT("count after pre-wrap ack"), Predictor.HistoryCountForTest(), 3);
	TestEqual(TEXT("first after pre-wrap ack"),
	          Predictor.HistoryAtForTest(0).seq, UINT32_MAX);
	TestEqual(TEXT("second after pre-wrap ack"), Predictor.HistoryAtForTest(1).seq, 0u);
	TestEqual(TEXT("third after pre-wrap ack"), Predictor.HistoryAtForTest(2).seq, 1u);

	Predictor.DropAckedForTest(0);
	TestEqual(TEXT("count after wrapped ack"), Predictor.HistoryCountForTest(), 1);
	TestEqual(TEXT("remaining seq"), Predictor.HistoryAtForTest(0).seq, 1u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasPlayerInputCommandPlaybackTest,
	"Atlas.Mvp.PlayerInput.CommandPlayback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasPlayerInputCommandPlaybackTest::RunTest(const FString&)
{
	FOwnerMovementPredictor Predictor;
	Predictor.Reset(atlas::Vec3{0.0f, 0.0f, 0.0f}, atlas::Vec3{0.0f, 0.0f, 1.0f});

	atlas::MovementCommandFrame Command{};
	Command.command_id = 55;
	Command.start_position = atlas::Vec3{0.0f, 0.0f, 0.0f};
	Command.target_position = atlas::Vec3{10.0f, 0.0f, 0.0f};
	Command.duration_ms = 100;
	TestTrue(TEXT("command start accepted"), Predictor.ApplyMovementCommandStart(Command));
	TestTrue(TEXT("command active after start"), Predictor.HasActiveCommand());
	TestFalse(TEXT("suppress command blocks input"), Predictor.AcceptsInput());
	TestEqual(TEXT("command start position"), Predictor.StateForTest().position_x, 0.0f);

	Predictor.AdvanceActiveCommandForTest(0.05f);
	TestTrue(TEXT("command active midway"), Predictor.HasActiveCommand());
	TestEqual(TEXT("midway position"), Predictor.StateForTest().position_x, 5.0f);
	TestEqual(TEXT("midway velocity"), Predictor.StateForTest().velocity_x, 100.0f);

	AtlasMovementInputFrame Input{};
	Input.seq = 1;
	Input.input_tick = 1;
	TestFalse(TEXT("active command suppresses input"), Predictor.PushInput(Input));
	Predictor.AdvanceActiveCommandForTest(0.05f);
	TestFalse(TEXT("command playback complete"), Predictor.HasActiveCommand());
	TestFalse(TEXT("open suppress command blocks input"), Predictor.AcceptsInput());
	TestFalse(TEXT("open suppress command rejects input"), Predictor.PushInput(Input));

	FOwnerMovementPredictor InvalidPolicyPredictor;
	InvalidPolicyPredictor.Reset(atlas::Vec3{0.0f, 0.0f, 0.0f},
	                              atlas::Vec3{0.0f, 0.0f, 1.0f});
	atlas::MovementCommandFrame InvalidPolicyCommand = Command;
	InvalidPolicyCommand.input_policy = 2;
	TestFalse(TEXT("allow_full command rejected"),
	          InvalidPolicyPredictor.ApplyMovementCommandStart(InvalidPolicyCommand));

	FOwnerMovementPredictor TurnPredictor;
	TurnPredictor.Reset(atlas::Vec3{0.0f, 0.0f, 0.0f},
	                    atlas::Vec3{0.0f, 0.0f, 1.0f});
	atlas::MovementCommandFrame TurnCommand = Command;
	TurnCommand.input_policy = 1;
	TestTrue(TEXT("turn command start accepted"),
	         TurnPredictor.ApplyMovementCommandStart(TurnCommand));
	TestTrue(TEXT("turn command accepts input"), TurnPredictor.AcceptsInput());
	Input.view_yaw = 16384;
	TestTrue(TEXT("turn command consumes input"), TurnPredictor.PushInput(Input));
	TestEqual(TEXT("turn command keeps path x"),
	          TurnPredictor.StateForTest().position_x, 0.0f);
	TestTrue(TEXT("turn command updates dir x"),
	         TurnPredictor.StateForTest().direction_x > 0.99f);
	TurnPredictor.AdvanceActiveCommandForTest(0.1f);
	TestFalse(TEXT("turn command playback complete"), TurnPredictor.HasActiveCommand());
	TestTrue(TEXT("open turn command accepts input"), TurnPredictor.AcceptsInput());
	Input.seq = 2;
	TestTrue(TEXT("open turn command consumes input"), TurnPredictor.PushInput(Input));

	AtlasMovementStateFrame EndState{};
	EndState.position_x = 10.0f;
	EndState.direction_x = 1.0f;
	EndState.flags = 1;
	EndState.last_processed_input_seq = 4;
	TestTrue(TEXT("command end accepted"),
	         Predictor.ApplyMovementCommandEnd(
		         55, 90, atlas::MovementCommandEndReason::kCollision, EndState));
	TestFalse(TEXT("command inactive after end"), Predictor.HasActiveCommand());
	TestEqual(TEXT("end state position"), Predictor.StateForTest().position_x, 10.0f);
	TestEqual(TEXT("end ack seq"), Predictor.LastAckSeqForTest(), 4u);
	TestEqual(TEXT("end reason"),
	          static_cast<uint8>(Predictor.LastCommandEndReasonForTest()),
	          static_cast<uint8>(atlas::MovementCommandEndReason::kCollision));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasPlayerInputCommandCurveTest,
	"Atlas.Mvp.PlayerInput.CommandCurve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasPlayerInputCommandCurveTest::RunTest(const FString&)
{
	atlas::mvp::movement_curves::Register(77, {0.0f, 0.0f, 1.0f});
	FOwnerMovementPredictor Predictor;
	Predictor.Reset(atlas::Vec3{0.0f, 0.0f, 0.0f}, atlas::Vec3{0.0f, 0.0f, 1.0f});

	atlas::MovementCommandFrame Command{};
	Command.command_id = 56;
	Command.start_position = atlas::Vec3{0.0f, 0.0f, 0.0f};
	Command.target_position = atlas::Vec3{10.0f, 0.0f, 0.0f};
	Command.duration_ms = 100;
	Command.curve_id = 77;
	TestTrue(TEXT("command start accepted"), Predictor.ApplyMovementCommandStart(Command));

	Predictor.AdvanceActiveCommandForTest(0.05f);
	TestEqual(TEXT("curve holds first half"), Predictor.StateForTest().position_x, 0.0f);

	Predictor.AdvanceActiveCommandForTest(0.025f);
	TestEqual(TEXT("curve advances third quarter"), Predictor.StateForTest().position_x, 5.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasMvpAvatarMovementAckLatestWinsTest,
	"Atlas.Mvp.Avatar.MovementAckLatestWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasMvpAvatarMovementAckLatestWinsTest::RunTest(const FString&)
{
	FBpAvatarEntity Entity(100, 2);

	AtlasMovementStateFrame First{};
	First.position_x = 1.0f;
	Entity.OnMovementStateAck(42, 100, First, 1);

	AtlasMovementStateFrame OlderTick{};
	OlderTick.position_x = 2.0f;
	Entity.OnMovementStateAck(42, 90, OlderTick, 2);

	FBpAvatarEntity::FMovementAck Ack;
	TestTrue(TEXT("pending ack after first pair"), Entity.ConsumeMovementAck(Ack));
	TestEqual(TEXT("same seq keeps newer tick"), Ack.ServerTick, 100u);
	TestEqual(TEXT("same seq keeps first state"), Ack.State.position_x, 1.0f);
	TestEqual(TEXT("same seq keeps first flags"), Ack.CorrectionFlags, static_cast<uint16>(1));

	AtlasMovementStateFrame OldSeq{};
	OldSeq.position_x = 3.0f;
	Entity.OnMovementStateAck(100, 5, OldSeq, 1);

	AtlasMovementStateFrame NewSeq{};
	NewSeq.position_x = 4.0f;
	Entity.OnMovementStateAck(101, 1, NewSeq, 2);

	TestTrue(TEXT("pending ack after seq pair"), Entity.ConsumeMovementAck(Ack));
	TestEqual(TEXT("newer seq wins"), Ack.AckedInputSeq, 101u);
	TestEqual(TEXT("newer seq state wins"), Ack.State.position_x, 4.0f);

	AtlasMovementStateFrame Wrapped{};
	Wrapped.position_x = 5.0f;
	Entity.OnMovementStateAck(UINT32_MAX, 10, Wrapped, 1);

	AtlasMovementStateFrame Zero{};
	Zero.position_x = 6.0f;
	Entity.OnMovementStateAck(0, 1, Zero, 2);

	TestTrue(TEXT("pending ack after wrap pair"), Entity.ConsumeMovementAck(Ack));
	TestEqual(TEXT("wrapped seq wins"), Ack.AckedInputSeq, 0u);
	TestEqual(TEXT("wrapped seq state wins"), Ack.State.position_x, 6.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasMvpAvatarMovementCommandPendingTest,
	"Atlas.Mvp.Avatar.MovementCommandPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasMvpAvatarMovementCommandPendingTest::RunTest(const FString&)
{
	FBpAvatarEntity Entity(100, 2);

	atlas::MovementCommandFrame First{};
	First.command_id = 10;
	First.start_position = atlas::Vec3{0.0f, 0.0f, 0.0f};
	First.target_position = atlas::Vec3{1.0f, 0.0f, 0.0f};
	First.duration_ms = 100;
	Entity.OnMovementCommandStart(First);

	atlas::MovementCommandFrame Second = First;
	Second.command_id = 11;
	Second.target_position = atlas::Vec3{2.0f, 0.0f, 0.0f};
	Entity.OnMovementCommandStart(Second);

	atlas::MovementCommandFrame PendingStart;
	TestTrue(TEXT("pending command start"), Entity.ConsumeMovementCommandStart(PendingStart));
	TestEqual(TEXT("latest command start wins"), PendingStart.command_id, 11u);
	TestEqual(TEXT("latest command target wins"), PendingStart.target_position.x, 2.0f);
	TestFalse(TEXT("pending command start consumed"),
	          Entity.ConsumeMovementCommandStart(PendingStart));

	AtlasMovementStateFrame EndState{};
	EndState.position_x = 3.0f;
	EndState.direction_z = 1.0f;
	EndState.flags = 1;
	EndState.last_processed_input_seq = 42;
	Entity.OnMovementCommandEnd(
		11, 200, atlas::MovementCommandEndReason::kCancelled, EndState);

	FBpAvatarEntity::FMovementCommandEnd PendingEnd;
	TestTrue(TEXT("pending command end"), Entity.ConsumeMovementCommandEnd(PendingEnd));
	TestEqual(TEXT("command end id"), PendingEnd.CommandId, 11u);
	TestEqual(TEXT("command end tick"), PendingEnd.ServerTick, 200u);
	TestEqual(TEXT("command end reason"), static_cast<uint8>(PendingEnd.Reason),
	          static_cast<uint8>(atlas::MovementCommandEndReason::kCancelled));
	TestEqual(TEXT("command end position"), PendingEnd.State.position_x, 3.0f);
	TestEqual(TEXT("command end last processed"),
	          PendingEnd.State.last_processed_input_seq, 42u);
	TestFalse(TEXT("pending command end consumed"),
	          Entity.ConsumeMovementCommandEnd(PendingEnd));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasMvpAvatarRemoteCommandOpenUntilEndTest,
	"Atlas.Mvp.Avatar.RemoteCommandOpenUntilEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasMvpAvatarRemoteCommandOpenUntilEndTest::RunTest(const FString&)
{
	FBpAvatarEntity Entity(100, 2);
	Entity.OnPositionReceived(
		1.0, atlas::Vec3{0.0f, 0.0f, 0.0f}, atlas::Vec3{0.0f, 0.0f, 1.0f}, true);

	atlas::MovementCommandFrame Command{};
	Command.command_id = 20;
	Command.start_position = atlas::Vec3{0.0f, 0.0f, 0.0f};
	Command.target_position = atlas::Vec3{4.0f, 0.0f, 0.0f};
	Command.duration_ms = 100;
	Entity.OnMovementCommandStart(Command);

	TestTrue(TEXT("remote command open"), Entity.HasOpenRemoteCommandForTest());
	TestTrue(TEXT("remote command active"), Entity.HasActiveRemoteCommandForTest());

	Entity.OnPositionReceived(
		1.1, atlas::Vec3{4.0f, 0.0f, 0.0f}, atlas::Vec3{0.0f, 0.0f, 1.0f}, true);

	TestTrue(TEXT("remote command stays open"), Entity.HasOpenRemoteCommandForTest());
	TestFalse(TEXT("remote command playback complete"),
	          Entity.HasActiveRemoteCommandForTest());
	TestTrue(TEXT("remote command keeps overlay"),
	         Entity.HasRemoteCommandPositionForTest());
	TestEqual(TEXT("remote command target position"), Entity.InitialServerPos().x, 4.0f);

	Entity.OnPositionReceived(
		1.2, atlas::Vec3{4.25f, 0.0f, 0.0f}, atlas::Vec3{1.0f, 0.0f, 0.0f}, true);

	TestTrue(TEXT("remote command still open"), Entity.HasOpenRemoteCommandForTest());
	TestFalse(TEXT("remote command remains inactive"),
	          Entity.HasActiveRemoteCommandForTest());
	TestTrue(TEXT("server snapshot keeps overlay"),
	         Entity.HasRemoteCommandPositionForTest());
	TestEqual(TEXT("server snapshot position"), Entity.InitialServerPos().x, 4.25f);
	TestEqual(TEXT("server snapshot direction"), Entity.InitialServerDir().x, 1.0f);

	AtlasMovementStateFrame EndState{};
	EndState.position_x = 5.0f;
	EndState.direction_z = 1.0f;
	EndState.flags = 1;
	Entity.OnMovementCommandEnd(
		20, 300, atlas::MovementCommandEndReason::kCompleted, EndState);

	TestFalse(TEXT("remote command closed"), Entity.HasOpenRemoteCommandForTest());
	TestFalse(TEXT("remote command inactive after end"),
	          Entity.HasActiveRemoteCommandForTest());
	TestTrue(TEXT("command end keeps final overlay"),
	         Entity.HasRemoteCommandPositionForTest());
	TestEqual(TEXT("command end position"), Entity.InitialServerPos().x, 5.0f);
	return true;
}

#endif
