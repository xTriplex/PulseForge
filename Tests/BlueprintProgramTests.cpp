#include "Scripting/BlueprintAsset.h"
#include "TestSupport.h"

#include <array>

using namespace PulseForge;

namespace
{
	BlueprintProgramSource MakeScalarWriteProgram()
	{
		BlueprintProgramSource Program;
		Program.Constants = { 2.0f, 3.0f };
		Program.ScalarPropertyIds = { "health" };
		Program.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(10, 1) },
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(10, 2) },
			{ BlueprintOpcode::Multiply, 0, 0, UUID(10, 3) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(10, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(10, 5) }
		};
		return Program;
	}

	void TestBlueprintProgramVerificationAndExecution(TestRunner& Tests)
	{
		const auto Verified = VerifyBlueprintProgram(MakeScalarWriteProgram());
		PF_CHECK(Tests, Verified.has_value());
		const auto Encoded = SerializeBlueprintProgram(Verified->GetSource());
		PF_CHECK(Tests, Encoded.has_value());
		const auto EncodedAgain = SerializeBlueprintProgram(Verified->GetSource());
		PF_CHECK(Tests, EncodedAgain && *EncodedAgain == *Encoded);
		const auto Decoded = DeserializeBlueprintProgram(*Encoded);
		PF_CHECK(Tests, Decoded && Decoded->GetSource().Instructions.size() == Verified->GetSource().Instructions.size() &&
			Decoded->GetSource().Instructions[3].SourceNode == UUID(10, 4) &&
			Decoded->GetSource().ScalarPropertyIds == Verified->GetSource().ScalarPropertyIds);
		std::vector<std::byte> CorruptBytes = *Encoded;
		CorruptBytes[4] = std::byte{ 4 };
		const auto UnsupportedEncoded = DeserializeBlueprintProgram(CorruptBytes);
		PF_CHECK(Tests, !UnsupportedEncoded && UnsupportedEncoded.error().Code == BlueprintProgramErrorCode::UnsupportedVersion);
		const auto TruncatedEncoded = DeserializeBlueprintProgram(std::span<const std::byte>(*Encoded).first(12));
		PF_CHECK(Tests, !TruncatedEncoded && TruncatedEncoded.error().Code == BlueprintProgramErrorCode::InvalidByteStream);
		std::vector<std::byte> TrailingBytes = *Encoded;
		TrailingBytes.push_back(std::byte{ 0 });
		const auto TrailingEncoded = DeserializeBlueprintProgram(TrailingBytes);
		PF_CHECK(Tests, !TrailingEncoded && TrailingEncoded.error().Code == BlueprintProgramErrorCode::InvalidByteStream);
		std::vector<std::byte> InvalidOpcodeBytes = *Encoded;
		InvalidOpcodeBytes[24 + 2 * sizeof(uint32_t) + sizeof(uint16_t) + std::string("health").size() + 3 * 25] = std::byte{ 0xff };
		const auto InvalidOpcodeEncoded = DeserializeBlueprintProgram(InvalidOpcodeBytes);
		PF_CHECK(Tests, !InvalidOpcodeEncoded && InvalidOpcodeEncoded.error().Code == BlueprintProgramErrorCode::InvalidOpcode &&
			InvalidOpcodeEncoded.error().Instruction == 3 && InvalidOpcodeEncoded.error().SourceNode == UUID(10, 4));
		std::array<float, 1> State{ 0.0f };
		const auto Executed = ExecuteBlueprintProgram(*Verified, State);
		PF_CHECK(Tests, Executed && Executed->InstructionsExecuted == 5);
		PF_CHECK(Tests, Executed->PropertyWrites.size() == 1 && Executed->PropertyWrites[0].PropertyId == "health" &&
			Executed->PropertyWrites[0].Value == 6.0f);
		std::vector<std::byte> LegacyProgramBytes = *Encoded;
		LegacyProgramBytes[4] = std::byte{ 1 };
		LegacyProgramBytes.erase(LegacyProgramBytes.begin() + 16, LegacyProgramBytes.begin() + 20);
		const auto LegacyProgram = DeserializeBlueprintProgram(LegacyProgramBytes);
		PF_CHECK(Tests, LegacyProgram && LegacyProgram->GetSource().Version == BlueprintProgramFormatVersion &&
			LegacyProgram->GetSource().Instructions.size() == Verified->GetSource().Instructions.size());
		std::vector<std::byte> PreviousProgramBytes = *Encoded;
		PreviousProgramBytes[4] = std::byte{ static_cast<uint8_t>(BlueprintPreviousProgramFormatVersion) };
		const auto PreviousProgram = DeserializeBlueprintProgram(PreviousProgramBytes);
		PF_CHECK(Tests, PreviousProgram && PreviousProgram->GetSource().Version == BlueprintProgramFormatVersion &&
			PreviousProgram->GetSource().Instructions.size() == Verified->GetSource().Instructions.size());

		BlueprintProgramSource Unsupported = MakeScalarWriteProgram();
		Unsupported.Version++;
		const auto UnsupportedResult = VerifyBlueprintProgram(std::move(Unsupported));
		PF_CHECK(Tests, !UnsupportedResult && UnsupportedResult.error().Code == BlueprintProgramErrorCode::UnsupportedVersion);

		BlueprintProgramSource BadTarget;
		BadTarget.Instructions = { { BlueprintOpcode::Jump, 0, 17, UUID(11, 1) } };
		const auto BadTargetResult = VerifyBlueprintProgram(std::move(BadTarget));
		PF_CHECK(Tests, !BadTargetResult && BadTargetResult.error().Code == BlueprintProgramErrorCode::InvalidControlFlow &&
			BadTargetResult.error().SourceNode == UUID(11, 1));

		BlueprintProgramSource BadStack;
		BadStack.Instructions = {
			{ BlueprintOpcode::Add, 0, 0, UUID(12, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(12, 2) }
		};
		const auto BadStackResult = VerifyBlueprintProgram(std::move(BadStack));
		PF_CHECK(Tests, !BadStackResult && BadStackResult.error().Code == BlueprintProgramErrorCode::StackUnderflow &&
			BadStackResult.error().SourceNode == UUID(12, 1));

		BlueprintProgramSource BadOpcode;
		BadOpcode.Instructions = {
			{ static_cast<BlueprintOpcode>(0xff), 0, 0, UUID(13, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(13, 2) }
		};
		const auto BadOpcodeResult = VerifyBlueprintProgram(std::move(BadOpcode));
		PF_CHECK(Tests, !BadOpcodeResult && BadOpcodeResult.error().Code == BlueprintProgramErrorCode::InvalidOpcode);

		BlueprintProgramSource BadProperty = MakeScalarWriteProgram();
		BadProperty.ScalarPropertyIds = { "../translation" };
		const auto BadPropertyResult = VerifyBlueprintProgram(std::move(BadProperty));
		PF_CHECK(Tests, !BadPropertyResult && BadPropertyResult.error().Code == BlueprintProgramErrorCode::InvalidPropertyIdentifier);

		BlueprintProgramSource UIActionProgram;
		UIActionProgram.Constants = { 1.0f };
		UIActionProgram.ScalarPropertyIds = { "activated" };
		UIActionProgram.ActionNames = { "Open" };
		UIActionProgram.Instructions = {
			{ BlueprintOpcode::JumpIfAction, 0, 2, UUID(16, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(16, 2) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(16, 3) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(16, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(16, 5) }
		};
		const auto VerifiedUIAction = VerifyBlueprintProgram(std::move(UIActionProgram));
		PF_CHECK(Tests, VerifiedUIAction.has_value());
		std::array<float, 1> ActionState{ 0.0f };
		const auto OpenAction = ExecuteBlueprintProgram(*VerifiedUIAction, ActionState,
			BlueprintDefaultInstructionBudget, "Open");
		PF_CHECK(Tests, OpenAction && OpenAction->PropertyWrites.size() == 1 && OpenAction->PropertyWrites[0].Value == 1.0f);
		const auto OtherAction = ExecuteBlueprintProgram(*VerifiedUIAction, ActionState,
			BlueprintDefaultInstructionBudget, "Close");
		PF_CHECK(Tests, OtherAction && OtherAction->PropertyWrites.empty());

		BlueprintProgramSource StopProgram;
		StopProgram.Instructions = {
			{ BlueprintOpcode::RequestStop, 0, 0, UUID(17, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(17, 2) }
		};
		const auto VerifiedStop = VerifyBlueprintProgram(std::move(StopProgram));
		PF_CHECK(Tests, VerifiedStop.has_value());
		if (VerifiedStop)
		{
			std::array<float, 0> NoState{};
			const auto StopResult = ExecuteBlueprintProgram(*VerifiedStop, NoState);
			PF_CHECK(Tests, StopResult && StopResult->RequestsStop && StopResult->InstructionsExecuted == 2 &&
				StopResult->StopRequestSourceNode == UUID(17, 1));
		}

		BlueprintProgramSource Loop;
		Loop.Constants = { 1.0f };
		Loop.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(14, 1) },
			{ BlueprintOpcode::JumpIfZero, 0, 3, UUID(14, 2) },
			{ BlueprintOpcode::Jump, 0, 2, UUID(14, 3) },
			{ BlueprintOpcode::Return, 0, 0, UUID(14, 4) }
		};
		const auto VerifiedLoop = VerifyBlueprintProgram(std::move(Loop));
		PF_CHECK(Tests, VerifiedLoop.has_value());
		const auto BudgetResult = ExecuteBlueprintProgram(*VerifiedLoop, {}, 7);
		PF_CHECK(Tests, !BudgetResult && BudgetResult.error().Code == BlueprintProgramErrorCode::InstructionBudgetExceeded &&
			BudgetResult.error().Instruction == 2 && BudgetResult.error().SourceNode == UUID(14, 3));

		BlueprintProgramSource StateLoop;
		StateLoop.Constants = { 4.0f, 1.0f };
		StateLoop.ScalarPropertyIds = { "health" };
		StateLoop.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(15, 1) },
			{ BlueprintOpcode::StoreScalar, 0, 0, UUID(15, 2) },
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(15, 3) },
			{ BlueprintOpcode::JumpIfZero, 0, 6, UUID(15, 4) },
			{ BlueprintOpcode::Jump, 0, 4, UUID(15, 5) },
			{ BlueprintOpcode::Return, 0, 0, UUID(15, 6) },
			{ BlueprintOpcode::Return, 0, 0, UUID(15, 7) }
		};
		const auto VerifiedStateLoop = VerifyBlueprintProgram(std::move(StateLoop));
		PF_CHECK(Tests, VerifiedStateLoop.has_value());
		std::array<float, 1> StateBeforeFailure{ 3.0f };
		const auto StateBudgetResult = ExecuteBlueprintProgram(*VerifiedStateLoop, StateBeforeFailure, 6);
		PF_CHECK(Tests, !StateBudgetResult && StateBudgetResult.error().Code == BlueprintProgramErrorCode::InstructionBudgetExceeded &&
			StateBeforeFailure[0] == 3.0f);
	}

	void TestBlueprintAssetCodec(TestRunner& Tests)
	{
		const BlueprintProgramSource CreateProgram = MakeScalarWriteProgram();
		BlueprintProgramSource UpdateProgram = MakeScalarWriteProgram();
		UpdateProgram.Constants = { 7.0f };
		UpdateProgram.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(20, 1) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(20, 2) },
			{ BlueprintOpcode::Return, 0, 0, UUID(20, 3) }
		};
		const std::array<BlueprintEventProgramSource, 2> Events{
			BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, UpdateProgram },
			BlueprintEventProgramSource{ BlueprintEventKind::OnCreate, CreateProgram }
		};
		const auto Encoded = SerializeBlueprintAsset(Events);
		PF_CHECK(Tests, Encoded.has_value());
		if (!Encoded) return;
		const std::array<BlueprintEventProgramSource, 2> ReverseEvents{ Events[1], Events[0] };
		const auto EncodedReverse = SerializeBlueprintAsset(ReverseEvents);
		PF_CHECK(Tests, EncodedReverse && *EncodedReverse == *Encoded);
		const auto Decoded = DeserializeBlueprintAsset(*Encoded);
		PF_CHECK(Tests, Decoded && Decoded->GetVersion() == BlueprintAssetFormatVersion && Decoded->GetEvents().size() == 2);
		if (Decoded && Decoded->GetEvents().size() == 2)
		{
			PF_CHECK(Tests, Decoded->GetEvents()[0].Event == BlueprintEventKind::OnCreate);
			PF_CHECK(Tests, Decoded->GetEvents()[1].Event == BlueprintEventKind::OnUpdate);
			PF_CHECK(Tests, Decoded->GetEvents()[0].Program.GetSource().Instructions[3].SourceNode == UUID(10, 4));
		}

		std::vector<std::byte> LegacyVersion = *Encoded;
		LegacyVersion[4] = std::byte{ 1 };
		const auto LegacyDecoded = DeserializeBlueprintAsset(LegacyVersion);
		PF_CHECK(Tests, LegacyDecoded && LegacyDecoded->GetEvents().size() == 2);

		std::vector<std::byte> UnsupportedVersion = *Encoded;
		UnsupportedVersion[4] = std::byte{ 4 };
		const auto Unsupported = DeserializeBlueprintAsset(UnsupportedVersion);
		PF_CHECK(Tests, !Unsupported && Unsupported.error().Code == BlueprintProgramErrorCode::UnsupportedVersion);
		const auto Truncated = DeserializeBlueprintAsset(std::span<const std::byte>(*Encoded).first(8));
		PF_CHECK(Tests, !Truncated && Truncated.error().Code == BlueprintProgramErrorCode::InvalidByteStream);
		std::vector<std::byte> Trailing = *Encoded;
		Trailing.push_back(std::byte{ 0 });
		const auto TrailingResult = DeserializeBlueprintAsset(Trailing);
		PF_CHECK(Tests, !TrailingResult && TrailingResult.error().Code == BlueprintProgramErrorCode::InvalidByteStream);

		const std::array<BlueprintEventProgramSource, 2> DuplicateEvents{
			Events[0], BlueprintEventProgramSource{ Events[0].Event, CreateProgram }
		};
		const auto Duplicate = SerializeBlueprintAsset(DuplicateEvents);
		PF_CHECK(Tests, !Duplicate && Duplicate.error().Code == BlueprintProgramErrorCode::InvalidOperand);
		const std::array<BlueprintEventProgramSource, 1> InvalidEvent{
			BlueprintEventProgramSource{ static_cast<BlueprintEventKind>(0xff), CreateProgram }
		};
		const auto InvalidEventResult = SerializeBlueprintAsset(InvalidEvent);
		PF_CHECK(Tests, !InvalidEventResult && InvalidEventResult.error().Code == BlueprintProgramErrorCode::InvalidOperand);
		const std::array<BlueprintEventProgramSource, 1> DestroyEvent{
			BlueprintEventProgramSource{ BlueprintEventKind::OnDestroy, CreateProgram }
		};
		const auto DestroyEncoded = SerializeBlueprintAsset(DestroyEvent);
		PF_CHECK(Tests, DestroyEncoded.has_value());
		if (DestroyEncoded)
		{
			const auto DestroyDecoded = DeserializeBlueprintAsset(*DestroyEncoded);
			PF_CHECK(Tests, DestroyDecoded && DestroyDecoded->GetEvents().size() == 1 &&
				DestroyDecoded->GetEvents()[0].Event == BlueprintEventKind::OnDestroy);
			std::vector<std::byte> LegacyDestroy = *DestroyEncoded;
			LegacyDestroy[4] = std::byte{ 1 };
			const auto LegacyDestroyDecoded = DeserializeBlueprintAsset(LegacyDestroy);
			PF_CHECK(Tests, !LegacyDestroyDecoded &&
				LegacyDestroyDecoded.error().Code == BlueprintProgramErrorCode::InvalidOperand);
			std::vector<std::byte> PreviousDestroy = *DestroyEncoded;
			PreviousDestroy[4] = std::byte{ 2 };
			PF_CHECK(Tests, DeserializeBlueprintAsset(PreviousDestroy).has_value());
		}
		const std::array<BlueprintEventProgramSource, 1> ActionEvent{
			BlueprintEventProgramSource{ BlueprintEventKind::OnActionPressed, CreateProgram }
		};
		const auto ActionEncoded = SerializeBlueprintAsset(ActionEvent);
		PF_CHECK(Tests, ActionEncoded.has_value());
		if (ActionEncoded)
		{
			const auto ActionDecoded = DeserializeBlueprintAsset(*ActionEncoded);
			PF_CHECK(Tests, ActionDecoded && ActionDecoded->GetEvents().size() == 1 &&
				ActionDecoded->GetEvents()[0].Event == BlueprintEventKind::OnActionPressed);
			std::vector<std::byte> PreviousAction = *ActionEncoded;
			PreviousAction[4] = std::byte{ 2 };
			PF_CHECK(Tests, !DeserializeBlueprintAsset(PreviousAction).has_value());
		}
	}
}

void RunBlueprintProgramTests(TestRunner& Tests)
{
	TestBlueprintProgramVerificationAndExecution(Tests);
	TestBlueprintAssetCodec(Tests);
}
