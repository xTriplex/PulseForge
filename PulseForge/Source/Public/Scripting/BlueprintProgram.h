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
	inline constexpr uint32_t BlueprintProgramFormatVersion = 8;
	inline constexpr uint32_t BlueprintPreviousProgramFormatVersion = 7;
	inline constexpr uint32_t BlueprintSpawnProgramFormatVersion = 8;
	inline constexpr uint32_t BlueprintDeferredServiceProgramFormatVersion = 6;
	inline constexpr uint32_t BlueprintTimerProgramFormatVersion = 7;
	inline constexpr uint32_t BlueprintSceneTransitionProgramFormatVersion = 4;
	inline constexpr uint32_t BlueprintIntermediateProgramFormatVersion = 3;
	inline constexpr uint32_t BlueprintActionProgramFormatVersion = 2;
	inline constexpr uint32_t BlueprintLegacyProgramFormatVersion = 1;
	inline constexpr size_t BlueprintMaximumInstructionCount = 4096;
	inline constexpr size_t BlueprintMaximumScalarCount = 64;
	inline constexpr size_t BlueprintMaximumPropertyIdentifierLength = 64;
	inline constexpr size_t BlueprintMaximumActionNameCount = 64;
	inline constexpr size_t BlueprintMaximumActionNameLength = 128;
	inline constexpr size_t BlueprintMaximumSceneTransitionAssetCount = 64;
	inline constexpr size_t BlueprintMaximumTimerNameCount = 64;
	inline constexpr size_t BlueprintMaximumTimerNameLength = 64;
	inline constexpr uint32_t BlueprintDefaultInstructionBudget = 10000;
	inline constexpr uint32_t BlueprintMaximumEventDispatchesPerFrame = 4096;
	inline constexpr size_t BlueprintMaximumDeferredPhysicsCallsPerEvent = 256;
	inline constexpr size_t BlueprintMaximumEntitySpawnsPerEvent = 64;
	inline constexpr size_t BlueprintMaximumScheduledTimers = 1024;
	inline constexpr size_t BlueprintMaximumTimersPerEvent = 64;
	inline constexpr float BlueprintMinimumTimerDelaySeconds = 0.001f;
	inline constexpr float BlueprintMaximumTimerDelaySeconds = 86400.0f;

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
		OnActionCancelled,
		OnTimer
	};

	enum class BlueprintAudioControl : uint8_t
	{
		Play,
		Pause,
		Resume,
		Stop
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
		RequestSceneTransition,
		RequestDestroySelf,
		RequestApplyForceSelf,
		RequestAudioControlSelf,
		RequestTimerSelf,
		JumpIfTimer,
		RequestSpawnEntity
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
		std::vector<std::string> TimerNames;
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

	struct BlueprintPhysicsForceRequest
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		UUID SourceNode;
	};

	struct BlueprintAudioControlRequest
	{
		BlueprintAudioControl Operation = BlueprintAudioControl::Play;
		UUID SourceNode;
	};

	struct BlueprintTimerRequest
	{
		std::string Name;
		float DelaySeconds = 0.0f;
		UUID SourceNode;
	};

	struct BlueprintEntitySpawnRequest
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
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
		bool RequestsEntityDestroy = false;
		UUID EntityDestroySourceNode;
		std::vector<BlueprintPhysicsForceRequest> PhysicsForceRequests;
		std::optional<BlueprintAudioControlRequest> AudioControlRequest;
		std::vector<BlueprintTimerRequest> TimerRequests;
		std::vector<BlueprintEntitySpawnRequest> EntitySpawnRequests;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintProgram, BlueprintProgramError>
		VerifyBlueprintProgram(BlueprintProgramSource Source);
	[[nodiscard]] PULSEFORGE_API std::expected<BlueprintExecutionResult, BlueprintProgramError> ExecuteBlueprintProgram(
		const VerifiedBlueprintProgram& Program,
		std::span<float> ScalarState,
		uint32_t InstructionBudget = BlueprintDefaultInstructionBudget,
		std::string_view EventName = {});
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, BlueprintProgramError>
		SerializeBlueprintProgram(const BlueprintProgramSource& Source);
	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintProgram, BlueprintProgramError>
		DeserializeBlueprintProgram(std::span<const std::byte> Bytes);
}
