#include "Scripting/BlueprintProgram.h"
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
		CorruptBytes[4] = std::byte{ 2 };
		const auto UnsupportedEncoded = DeserializeBlueprintProgram(CorruptBytes);
		PF_CHECK(Tests, !UnsupportedEncoded && UnsupportedEncoded.error().Code == BlueprintProgramErrorCode::UnsupportedVersion);
		const auto TruncatedEncoded = DeserializeBlueprintProgram(std::span<const std::byte>(*Encoded).first(12));
		PF_CHECK(Tests, !TruncatedEncoded && TruncatedEncoded.error().Code == BlueprintProgramErrorCode::InvalidByteStream);
		std::vector<std::byte> TrailingBytes = *Encoded;
		TrailingBytes.push_back(std::byte{ 0 });
		const auto TrailingEncoded = DeserializeBlueprintProgram(TrailingBytes);
		PF_CHECK(Tests, !TrailingEncoded && TrailingEncoded.error().Code == BlueprintProgramErrorCode::InvalidByteStream);
		std::vector<std::byte> InvalidOpcodeBytes = *Encoded;
		InvalidOpcodeBytes[20 + 2 * sizeof(uint32_t) + sizeof(uint16_t) + std::string("health").size() + 3 * 25] = std::byte{ 0xff };
		const auto InvalidOpcodeEncoded = DeserializeBlueprintProgram(InvalidOpcodeBytes);
		PF_CHECK(Tests, !InvalidOpcodeEncoded && InvalidOpcodeEncoded.error().Code == BlueprintProgramErrorCode::InvalidOpcode &&
			InvalidOpcodeEncoded.error().Instruction == 3 && InvalidOpcodeEncoded.error().SourceNode == UUID(10, 4));
		std::array<float, 1> State{ 0.0f };
		const auto Executed = ExecuteBlueprintProgram(*Verified, State);
		PF_CHECK(Tests, Executed && Executed->InstructionsExecuted == 5);
		PF_CHECK(Tests, Executed->PropertyWrites.size() == 1 && Executed->PropertyWrites[0].PropertyId == "health" &&
			Executed->PropertyWrites[0].Value == 6.0f);

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
}

void RunBlueprintProgramTests(TestRunner& Tests)
{
	TestBlueprintProgramVerificationAndExecution(Tests);
}
