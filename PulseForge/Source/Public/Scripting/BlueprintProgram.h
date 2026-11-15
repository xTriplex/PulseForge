#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"
#include "Scene/UUID.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PulseForge
{
	inline constexpr uint32_t BlueprintProgramFormatVersion = 4;
	inline constexpr uint32_t BlueprintPreviousProgramFormatVersion = 3;
	inline constexpr uint32_t BlueprintActionProgramFormatVersion = 2;
	inline constexpr uint32_t BlueprintLegacyProgramFormatVersion = 1;
	inline constexpr size_t BlueprintMaximumInstructionCount = 4096;
	inline constexpr size_t BlueprintMaximumScalarCount = 64;
	inline constexpr size_t BlueprintMaximumPropertyIdentifierLength = 64;
	inline constexpr size_t BlueprintMaximumActionNameCount = 64;
	inline constexpr size_t BlueprintMaximumActionNameLength = 128;
	inline constexpr size_t BlueprintMaximumSceneTransitionAssetCount = 64;
	inline constexpr uint32_t BlueprintDefaultInstructionBudget = 10000;
	inline constexpr uint32_t BlueprintMaximumEventDispatchesPerFrame = 4096;

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
		OnUIAction,
		OnDestroy,
		OnActionPressed,
		OnActionReleased,
		OnActionCancelled
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
		Return,
		JumpIfAction,
		RequestStop,
		RequestPause,
		RequestSceneTransition
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
		std::vector<std::string> ActionNames;
		std::vector<AssetID> SceneTransitionAssets;
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
		HostFailure,
		EventBudgetExceeded,
		HostRejectedDeferredCall
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
		bool RequestsStop = false;
		UUID StopRequestSourceNode;
		std::optional<bool> RequestedPause;
		UUID PauseRequestSourceNode;
		std::optional<AssetID> RequestedSceneTransition;
		UUID SceneTransitionSourceNode;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintProgram, BlueprintProgramError>
		VerifyBlueprintProgram(BlueprintProgramSource Source);
	[[nodiscard]] PULSEFORGE_API std::expected<BlueprintExecutionResult, BlueprintProgramError> ExecuteBlueprintProgram(
		const VerifiedBlueprintProgram& Program,
		std::span<float> ScalarState,
		uint32_t InstructionBudget = BlueprintDefaultInstructionBudget,
		std::string_view ActionName = {});
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, BlueprintProgramError>
		SerializeBlueprintProgram(const BlueprintProgramSource& Source);
	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintProgram, BlueprintProgramError>
		DeserializeBlueprintProgram(std::span<const std::byte> Bytes);
}
