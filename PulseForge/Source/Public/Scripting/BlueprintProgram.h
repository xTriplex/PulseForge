#pragma once

#include "Core/Core.h"
#include "Scene/UUID.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace PulseForge
{
	inline constexpr uint32_t BlueprintProgramFormatVersion = 1;
	inline constexpr size_t BlueprintMaximumInstructionCount = 4096;
	inline constexpr size_t BlueprintMaximumScalarCount = 64;
	inline constexpr size_t BlueprintMaximumPropertyIdentifierLength = 64;
	inline constexpr uint32_t BlueprintDefaultInstructionBudget = 10000;

	enum class BlueprintEventKind : uint8_t
	{
		OnCreate,
		OnUpdate,
		OnCollisionBegin,
		OnCollisionStay,
		OnCollisionEnd,
		OnTriggerEnter,
		OnTriggerStay,
		OnTriggerExit,
		OnUIAction
	};

	enum class BlueprintOpcode : uint8_t
	{
		PushConstant,
		LoadScalar,
		StoreScalar,
		Add,
		Multiply,
		Jump,
		JumpIfZero,
		WriteScalarProperty,
		Return
	};

	struct BlueprintInstruction
	{
		BlueprintOpcode Opcode = BlueprintOpcode::Return;
		uint32_t Operand = 0;
		uint32_t Target = 0;
		UUID SourceNode;
	};

	struct BlueprintProgramSource
	{
		uint32_t Version = BlueprintProgramFormatVersion;
		std::vector<float> Constants;
		std::vector<std::string> ScalarPropertyIds;
		std::vector<BlueprintInstruction> Instructions;
	};

	enum class BlueprintProgramErrorCode : uint8_t
	{
		UnsupportedVersion,
		ProgramTooLarge,
		InvalidConstant,
		InvalidPropertyIdentifier,
		InvalidOpcode,
		InvalidOperand,
		InvalidControlFlow,
		InvalidByteStream,
		StackUnderflow,
		StackMismatch,
		MissingReturn,
		InstructionBudgetExceeded,
		HostRejectedPropertyWrite,
		HostFailure
	};

	struct BlueprintProgramError
	{
		BlueprintProgramErrorCode Code;
		uint32_t Instruction = 0;
		UUID SourceNode;
		std::string Message;
	};

	struct VerifiedBlueprintProgram final
	{
		[[nodiscard]] const BlueprintProgramSource& GetSource() const noexcept { return m_Source; }

	private:
		explicit VerifiedBlueprintProgram(BlueprintProgramSource Source) : m_Source(std::move(Source)) {}
		BlueprintProgramSource m_Source;
		friend PULSEFORGE_API std::expected<VerifiedBlueprintProgram, struct BlueprintProgramError>
			VerifyBlueprintProgram(BlueprintProgramSource Source);
	};

	struct BlueprintScalarPropertyWrite
	{
		std::string PropertyId;
		float Value = 0.0f;
		UUID SourceNode;
	};

	struct BlueprintExecutionResult
	{
		uint32_t InstructionsExecuted = 0;
		std::vector<BlueprintScalarPropertyWrite> PropertyWrites;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintProgram, BlueprintProgramError>
		VerifyBlueprintProgram(BlueprintProgramSource Source);
	[[nodiscard]] PULSEFORGE_API std::expected<BlueprintExecutionResult, BlueprintProgramError> ExecuteBlueprintProgram(
		const VerifiedBlueprintProgram& Program,
		std::span<float> ScalarState,
		uint32_t InstructionBudget = BlueprintDefaultInstructionBudget);
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, BlueprintProgramError>
		SerializeBlueprintProgram(const BlueprintProgramSource& Source);
	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintProgram, BlueprintProgramError>
		DeserializeBlueprintProgram(std::span<const std::byte> Bytes);
}
