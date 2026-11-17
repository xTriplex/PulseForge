#include "Scripting/BlueprintProgram.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <optional>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		constexpr size_t MaximumBlueprintStackDepth = 128;
		constexpr size_t MaximumBlueprintProgramBytes = 1024 * 1024;
		constexpr std::array<std::byte, 4> BlueprintMagic{
			std::byte{ 'P' }, std::byte{ 'F' }, std::byte{ 'B' }, std::byte{ 'P' } };

		BlueprintProgramError MakeError(
			BlueprintProgramErrorCode Code,
			uint32_t Instruction,
			UUID SourceNode,
			std::string Message)
		{
			return { Code, Instruction, SourceNode, std::move(Message) };
		}

		template<typename T>
		void AppendInteger(std::vector<std::byte>& Bytes, T Value)
		{
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
		}

		template<typename T>
		bool ReadInteger(std::span<const std::byte> Bytes, size_t& Offset, T& Value)
		{
			if (Bytes.size() - Offset < sizeof(T)) return false;
			Value = 0;
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Value |= static_cast<T>(std::to_integer<uint8_t>(Bytes[Offset++])) << (Index * 8);
			return true;
		}

		bool IsValidBlueprintPropertyId(std::string_view Identifier) noexcept
		{
			if (Identifier.empty() || Identifier.size() > BlueprintMaximumPropertyIdentifierLength) return false;
			const auto IsFirst = [](unsigned char Character)
			{
				return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') || Character == '_';
			};
			const auto IsRemaining = [&](unsigned char Character)
			{
				return IsFirst(Character) || (Character >= '0' && Character <= '9') || Character == '.' || Character == '-';
			};
			return IsFirst(static_cast<unsigned char>(Identifier.front())) && std::all_of(Identifier.begin() + 1, Identifier.end(),
				[&](char Character) { return IsRemaining(static_cast<unsigned char>(Character)); });
		}
	}

	std::expected<VerifiedBlueprintProgram, BlueprintProgramError> VerifyBlueprintProgram(BlueprintProgramSource Source)
	{
		if (Source.Version != BlueprintProgramFormatVersion)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, 0, {},
				"Blueprint program version is not supported"));
		if (Source.Instructions.empty() || Source.Instructions.size() > BlueprintMaximumInstructionCount ||
			Source.Constants.size() > BlueprintMaximumInstructionCount || Source.ScalarPropertyIds.size() > BlueprintMaximumScalarCount ||
			Source.ActionNames.size() > BlueprintMaximumActionNameCount ||
			Source.SceneTransitionAssets.size() > BlueprintMaximumSceneTransitionAssetCount ||
			Source.TimerNames.size() > BlueprintMaximumTimerNameCount)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, 0, {},
				"Blueprint program exceeds a structural limit or has no instructions"));

		for (size_t Index = 0; Index < Source.Constants.size(); ++Index)
		{
			if (!std::isfinite(Source.Constants[Index]))
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidConstant, 0, {},
					"Blueprint constants must be finite"));
		}
		std::unordered_set<std::string> PropertyIds;
		for (const std::string& PropertyId : Source.ScalarPropertyIds)
		{
			if (!IsValidBlueprintPropertyId(PropertyId) ||
				!PropertyIds.emplace(PropertyId).second)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidPropertyIdentifier, 0, {},
					"Blueprint property identifiers must be unique and bounded"));
		}
		std::unordered_set<std::string> ActionNames;
		for (const std::string& Name : Source.ActionNames)
			if (Name.empty() || Name.size() > BlueprintMaximumActionNameLength || !ActionNames.emplace(Name).second)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, 0, {},
					"Blueprint UI action names must be unique, non-empty, and bounded"));
		std::unordered_set<std::string> TimerNames;
		for (const std::string& Name : Source.TimerNames)
			if (Name.empty() || Name.size() > BlueprintMaximumTimerNameLength || !TimerNames.emplace(Name).second)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, 0, {},
					"Blueprint timer names must be unique, non-empty, and bounded"));
		std::vector<AssetID> SceneTransitionAssets;
		SceneTransitionAssets.reserve(Source.SceneTransitionAssets.size());
		for (const AssetID& Asset : Source.SceneTransitionAssets)
		{
			if (Asset.IsNil() || std::find(SceneTransitionAssets.begin(), SceneTransitionAssets.end(), Asset) !=
				SceneTransitionAssets.end())
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, 0, {},
					"Blueprint scene transition targets must be unique and have stable asset identities"));
			SceneTransitionAssets.push_back(Asset);
		}

		std::vector<std::optional<size_t>> StackDepth(Source.Instructions.size());
		std::deque<uint32_t> Pending;
		size_t DeferredPhysicsCallCount = 0;
		size_t DeferredTimerCount = 0;
		size_t EntitySpawnCount = 0;
		bool HasAudioControlCall = false;
		StackDepth[0] = 0;
		Pending.push_back(0);
		bool HasReachableReturn = false;
		const auto Enqueue = [&](uint32_t Target, size_t Depth, uint32_t From) -> std::optional<BlueprintProgramError>
		{
			if (Target >= Source.Instructions.size())
				return MakeError(BlueprintProgramErrorCode::InvalidControlFlow, From, Source.Instructions[From].SourceNode,
					"Blueprint control flow leaves the program");
			if (StackDepth[Target] && *StackDepth[Target] != Depth)
				return MakeError(BlueprintProgramErrorCode::StackMismatch, Target, Source.Instructions[Target].SourceNode,
					"Blueprint control-flow paths disagree on stack depth");
			if (!StackDepth[Target])
			{
				StackDepth[Target] = Depth;
				Pending.push_back(Target);
			}
			return {};
		};

		while (!Pending.empty())
		{
			const uint32_t Index = Pending.front();
			Pending.pop_front();
			const BlueprintInstruction& Instruction = Source.Instructions[Index];
			size_t Depth = *StackDepth[Index];
			bool FallsThrough = true;
			switch (Instruction.Opcode)
			{
			case BlueprintOpcode::PushConstant:
				if (Instruction.Operand >= Source.Constants.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint constant index is out of range"));
				++Depth;
				break;
			case BlueprintOpcode::LoadScalar:
				if (Instruction.Operand >= Source.ScalarPropertyIds.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint scalar index is out of range"));
				++Depth;
				break;
			case BlueprintOpcode::StoreScalar:
			case BlueprintOpcode::WriteScalarProperty:
				if (Instruction.Operand >= Source.ScalarPropertyIds.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint scalar property index is out of range"));
				if (Depth == 0)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackUnderflow, Index, Instruction.SourceNode,
						"Blueprint instruction requires a scalar value"));
				--Depth;
				break;
			case BlueprintOpcode::Add:
			case BlueprintOpcode::Multiply:
				if (Depth < 2)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackUnderflow, Index, Instruction.SourceNode,
						"Blueprint arithmetic requires two values"));
				--Depth;
				break;
			case BlueprintOpcode::Jump:
				if (const auto Error = Enqueue(Instruction.Target, Depth, Index)) return std::unexpected(*Error);
				FallsThrough = false;
				break;
			case BlueprintOpcode::JumpIfZero:
				if (Depth == 0)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackUnderflow, Index, Instruction.SourceNode,
						"Blueprint conditional branch requires a value"));
				--Depth;
				if (const auto Error = Enqueue(Instruction.Target, Depth, Index)) return std::unexpected(*Error);
				break;
			case BlueprintOpcode::JumpIfAction:
				if (Instruction.Operand >= Source.ActionNames.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint action name index is out of range"));
				if (const auto Error = Enqueue(Instruction.Target, Depth, Index)) return std::unexpected(*Error);
				break;
			case BlueprintOpcode::JumpIfTimer:
				if (Instruction.Operand >= Source.TimerNames.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint timer name index is out of range"));
				if (const auto Error = Enqueue(Instruction.Target, Depth, Index)) return std::unexpected(*Error);
				break;
			case BlueprintOpcode::RequestStop:
				break;
			case BlueprintOpcode::RequestPause:
				if (Instruction.Operand > 1)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint pause request must contain a boolean operand"));
				break;
			case BlueprintOpcode::RequestSceneTransition:
				if (Instruction.Operand >= Source.SceneTransitionAssets.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint scene transition asset index is out of range"));
				break;
			case BlueprintOpcode::RequestDestroySelf:
				break;
			case BlueprintOpcode::RequestApplyForceSelf:
				if (Depth < 3)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackUnderflow, Index, Instruction.SourceNode,
						"Blueprint physics force request requires three vector components"));
				Depth -= 3;
				if (++DeferredPhysicsCallCount > BlueprintMaximumDeferredPhysicsCallsPerEvent)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, Index, Instruction.SourceNode,
						"Blueprint deferred physics calls exceed the per-event limit"));
				break;
			case BlueprintOpcode::RequestAudioControlSelf:
				if (Instruction.Operand > static_cast<uint32_t>(BlueprintAudioControl::Stop))
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint audio control operation is invalid"));
				if (HasAudioControlCall)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, Index, Instruction.SourceNode,
						"A Blueprint event may request at most one audio control operation"));
				HasAudioControlCall = true;
				break;
			case BlueprintOpcode::RequestTimerSelf:
				if (Instruction.Operand >= Source.TimerNames.size() || Instruction.Target >= Source.Constants.size() ||
					Source.Constants[Instruction.Target] < BlueprintMinimumTimerDelaySeconds ||
					Source.Constants[Instruction.Target] > BlueprintMaximumTimerDelaySeconds)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint timer requires a known name and a bounded positive delay"));
				if (++DeferredTimerCount > BlueprintMaximumTimersPerEvent)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, Index, Instruction.SourceNode,
						"Blueprint timer requests exceed the per-event limit"));
				break;
			case BlueprintOpcode::RequestSpawnEntity:
				if (Depth < 3)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackUnderflow, Index, Instruction.SourceNode,
						"Blueprint entity spawn requires three position components"));
				Depth -= 3;
				if (++EntitySpawnCount > BlueprintMaximumEntitySpawnsPerEvent)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, Index, Instruction.SourceNode,
						"Blueprint entity spawn requests exceed the per-event limit"));
				break;
			case BlueprintOpcode::Raycast:
				if (Depth < 7)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackUnderflow, Index, Instruction.SourceNode,
						"Blueprint physics raycast requires origin, direction, and distance values"));
				Depth -= 6;
				break;
			case BlueprintOpcode::Return:
				if (Depth != 0)
					return std::unexpected(MakeError(BlueprintProgramErrorCode::StackMismatch, Index, Instruction.SourceNode,
						"Blueprint return requires an empty evaluation stack"));
				HasReachableReturn = true;
				FallsThrough = false;
				break;
			default:
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOpcode, Index, Instruction.SourceNode,
					"Blueprint opcode is unknown"));
			}
			if (Depth > MaximumBlueprintStackDepth)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, Index, Instruction.SourceNode,
					"Blueprint evaluation stack exceeds its limit"));
			if (FallsThrough)
			{
				if (Index + 1 >= Source.Instructions.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::MissingReturn, Index, Instruction.SourceNode,
						"Blueprint execution can fall off the end of the program"));
				if (const auto Error = Enqueue(Index + 1, Depth, Index)) return std::unexpected(*Error);
			}
		}
		if (!HasReachableReturn)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::MissingReturn, 0, Source.Instructions[0].SourceNode,
				"Blueprint program has no reachable return"));
		return VerifiedBlueprintProgram(std::move(Source));
	}

	std::expected<BlueprintExecutionResult, BlueprintProgramError> ExecuteBlueprintProgram(
		const VerifiedBlueprintProgram& Program,
		std::span<float> ScalarState,
		uint32_t InstructionBudget,
		std::string_view EventName,
		const BlueprintExecutionServices& Services)
	{
		const BlueprintProgramSource& Source = Program.GetSource();
		if (ScalarState.size() != Source.ScalarPropertyIds.size() || ScalarState.size() > BlueprintMaximumScalarCount)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, 0, {},
				"Blueprint instance state does not match the declared scalar slots"));
		if (!std::all_of(ScalarState.begin(), ScalarState.end(), [](float Value) { return std::isfinite(Value); }))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidConstant, 0, {},
				"Blueprint instance scalar state contains a non-finite value"));
		std::vector<float> Stack;
		Stack.reserve(MaximumBlueprintStackDepth);
		std::vector<float> CandidateState(ScalarState.begin(), ScalarState.end());
		uint32_t ProgramCounter = 0;
		BlueprintExecutionResult Result;
		Result.PropertyWrites.reserve((std::min)(Source.Instructions.size(), size_t{ 32 }));
		Result.ExecutedNodes.reserve((std::min)(Source.Instructions.size(), BlueprintMaximumTraceNodesPerEvent));
		uint32_t PhysicsQueriesExecuted = 0;
		const auto MakeExecutionError = [&](BlueprintProgramErrorCode Code, uint32_t Instruction, UUID SourceNode,
			std::string Message)
		{
			BlueprintProgramError Error = MakeError(Code, Instruction, SourceNode, std::move(Message));
			Error.InstructionsExecuted = Result.InstructionsExecuted;
			return Error;
		};
		while (ProgramCounter < Source.Instructions.size())
		{
			if (Result.InstructionsExecuted >= InstructionBudget)
				return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::InstructionBudgetExceeded, ProgramCounter,
					Source.Instructions[ProgramCounter].SourceNode, "Blueprint event exceeded its instruction budget"));
			++Result.InstructionsExecuted;
			const BlueprintInstruction& Instruction = Source.Instructions[ProgramCounter];
			if (!Instruction.SourceNode.IsNil())
			{
				if (Result.ExecutedNodes.size() < BlueprintMaximumTraceNodesPerEvent)
					Result.ExecutedNodes.push_back(Instruction.SourceNode);
				else
					Result.ExecutedNodeTraceTruncated = true;
			}
			switch (Instruction.Opcode)
			{
			case BlueprintOpcode::PushConstant: Stack.push_back(Source.Constants[Instruction.Operand]); ++ProgramCounter; break;
			case BlueprintOpcode::LoadScalar: Stack.push_back(CandidateState[Instruction.Operand]); ++ProgramCounter; break;
			case BlueprintOpcode::StoreScalar:
				CandidateState[Instruction.Operand] = Stack.back(); Stack.pop_back(); ++ProgramCounter; break;
			case BlueprintOpcode::Add:
			{
				const float Right = Stack.back(); Stack.pop_back();
				const float Left = Stack.back(); Stack.pop_back();
				const float Value = Left + Right;
				if (!std::isfinite(Value))
					return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::InvalidConstant, ProgramCounter,
						Instruction.SourceNode, "Blueprint arithmetic produced a non-finite value"));
				Stack.push_back(Value); ++ProgramCounter; break;
			}
			case BlueprintOpcode::Multiply:
			{
				const float Right = Stack.back(); Stack.pop_back();
				const float Left = Stack.back(); Stack.pop_back();
				const float Value = Left * Right;
				if (!std::isfinite(Value))
					return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::InvalidConstant, ProgramCounter,
						Instruction.SourceNode, "Blueprint arithmetic produced a non-finite value"));
				Stack.push_back(Value); ++ProgramCounter; break;
			}
			case BlueprintOpcode::Jump: ProgramCounter = Instruction.Target; break;
			case BlueprintOpcode::JumpIfZero:
			{
				const float Condition = Stack.back(); Stack.pop_back();
				ProgramCounter = Condition == 0.0f ? Instruction.Target : ProgramCounter + 1;
				break;
			}
			case BlueprintOpcode::JumpIfAction:
				ProgramCounter = EventName == Source.ActionNames[Instruction.Operand]
					? Instruction.Target : ProgramCounter + 1;
				break;
			case BlueprintOpcode::JumpIfTimer:
				ProgramCounter = EventName == Source.TimerNames[Instruction.Operand]
					? Instruction.Target : ProgramCounter + 1;
				break;
			case BlueprintOpcode::WriteScalarProperty:
			{
				const float Value = Stack.back(); Stack.pop_back();
				Result.PropertyWrites.push_back({ Source.ScalarPropertyIds[Instruction.Operand], Value, Instruction.SourceNode });
				++ProgramCounter;
				break;
			}
			case BlueprintOpcode::RequestStop:
				Result.RequestsStop = true;
				Result.StopRequestSourceNode = Instruction.SourceNode;
				++ProgramCounter;
				break;
			case BlueprintOpcode::RequestPause:
				Result.RequestedPause = Instruction.Operand != 0;
				Result.PauseRequestSourceNode = Instruction.SourceNode;
				++ProgramCounter;
				break;
			case BlueprintOpcode::RequestSceneTransition:
				Result.RequestedSceneTransition = Source.SceneTransitionAssets[Instruction.Operand];
				Result.SceneTransitionSourceNode = Instruction.SourceNode;
				++ProgramCounter;
				break;
			case BlueprintOpcode::RequestDestroySelf:
				Result.RequestsEntityDestroy = true;
				Result.EntityDestroySourceNode = Instruction.SourceNode;
				++ProgramCounter;
				break;
			case BlueprintOpcode::RequestApplyForceSelf:
			{
				const float Z = Stack.back(); Stack.pop_back();
				const float Y = Stack.back(); Stack.pop_back();
				const float X = Stack.back(); Stack.pop_back();
				Result.PhysicsForceRequests.push_back({ X, Y, Z, Instruction.SourceNode });
				++ProgramCounter;
				break;
			}
			case BlueprintOpcode::RequestAudioControlSelf:
				Result.AudioControlRequest = BlueprintAudioControlRequest{
					static_cast<BlueprintAudioControl>(Instruction.Operand), Instruction.SourceNode };
				++ProgramCounter;
				break;
			case BlueprintOpcode::RequestTimerSelf:
				Result.TimerRequests.push_back({ Source.TimerNames[Instruction.Operand],
					Source.Constants[Instruction.Target], Instruction.SourceNode });
				++ProgramCounter;
				break;
			case BlueprintOpcode::RequestSpawnEntity:
			{
				const float Z = Stack.back(); Stack.pop_back();
				const float Y = Stack.back(); Stack.pop_back();
				const float X = Stack.back(); Stack.pop_back();
				Result.EntitySpawnRequests.push_back({ X, Y, Z, Instruction.SourceNode });
				++ProgramCounter;
				break;
			}
			case BlueprintOpcode::Raycast:
			{
				BlueprintRaycastQuery Query;
				Query.MaxDistance = Stack.back(); Stack.pop_back();
				Query.DirectionZ = Stack.back(); Stack.pop_back();
				Query.DirectionY = Stack.back(); Stack.pop_back();
				Query.DirectionX = Stack.back(); Stack.pop_back();
				Query.OriginZ = Stack.back(); Stack.pop_back();
				Query.OriginY = Stack.back(); Stack.pop_back();
				Query.OriginX = Stack.back(); Stack.pop_back();
				Query.SourceNode = Instruction.SourceNode;
				if (PhysicsQueriesExecuted >= BlueprintMaximumPhysicsQueriesPerEvent)
					return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::EventBudgetExceeded, ProgramCounter,
						Instruction.SourceNode, "Blueprint physics query budget was exhausted for this event"));
				++PhysicsQueriesExecuted;
				const std::array<float, 7> Values{ Query.OriginX, Query.OriginY, Query.OriginZ,
					Query.DirectionX, Query.DirectionY, Query.DirectionZ, Query.MaxDistance };
				if (!std::all_of(Values.begin(), Values.end(), [](float Value) { return std::isfinite(Value); }) ||
					Query.MaxDistance <= 0.0f ||
					Query.DirectionX * Query.DirectionX + Query.DirectionY * Query.DirectionY +
						Query.DirectionZ * Query.DirectionZ <= 1.0e-12f)
					return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::InvalidOperand, ProgramCounter,
						Instruction.SourceNode, "Blueprint physics raycast input is invalid"));
				if (!Services.Raycast)
					return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::HostFailure, ProgramCounter,
						Instruction.SourceNode, "Blueprint physics raycast service is unavailable"));
				auto Hit = Services.Raycast(Query);
				if (!Hit)
				{
					BlueprintProgramError Error = std::move(Hit.error());
					Error.Instruction = ProgramCounter;
					Error.SourceNode = Instruction.SourceNode;
					Error.InstructionsExecuted = Result.InstructionsExecuted;
					return std::unexpected(std::move(Error));
				}
				Stack.push_back(*Hit ? 1.0f : 0.0f);
				++ProgramCounter;
				break;
			}
			case BlueprintOpcode::Return:
				std::copy(CandidateState.begin(), CandidateState.end(), ScalarState.begin());
				return Result;
			default:
				return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::InvalidOpcode, ProgramCounter,
					Instruction.SourceNode, "Verified Blueprint program contains an unknown opcode"));
			}
		}
		return std::unexpected(MakeExecutionError(BlueprintProgramErrorCode::InvalidControlFlow, ProgramCounter, {},
			"Blueprint execution left the verified instruction range"));
	}

	std::expected<std::vector<std::byte>, BlueprintProgramError> SerializeBlueprintProgram(const BlueprintProgramSource& Source)
	{
		if (const auto Verified = VerifyBlueprintProgram(Source); !Verified) return std::unexpected(Verified.error());
		std::vector<std::byte> Bytes;
		Bytes.reserve(28 + Source.Constants.size() * sizeof(uint32_t) + Source.SceneTransitionAssets.size() * 16 +
			Source.Instructions.size() * 25);
		Bytes.insert(Bytes.end(), BlueprintMagic.begin(), BlueprintMagic.end());
		AppendInteger(Bytes, Source.Version);
		AppendInteger(Bytes, static_cast<uint32_t>(Source.Constants.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.ScalarPropertyIds.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.ActionNames.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.SceneTransitionAssets.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.TimerNames.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.Instructions.size()));
		for (const float Constant : Source.Constants) AppendInteger(Bytes, std::bit_cast<uint32_t>(Constant));
		for (const std::string& PropertyId : Source.ScalarPropertyIds)
		{
			AppendInteger(Bytes, static_cast<uint16_t>(PropertyId.size()));
			for (const char Character : PropertyId) Bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(Character)));
		}
		for (const std::string& ActionName : Source.ActionNames)
		{
			AppendInteger(Bytes, static_cast<uint16_t>(ActionName.size()));
			for (const char Character : ActionName) Bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(Character)));
		}
		for (const AssetID& Asset : Source.SceneTransitionAssets)
		{
			AppendInteger(Bytes, Asset.GetHigh());
			AppendInteger(Bytes, Asset.GetLow());
		}
		for (const std::string& TimerName : Source.TimerNames)
		{
			AppendInteger(Bytes, static_cast<uint16_t>(TimerName.size()));
			for (const char Character : TimerName) Bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(Character)));
		}
		for (const BlueprintInstruction& Instruction : Source.Instructions)
		{
			Bytes.push_back(static_cast<std::byte>(Instruction.Opcode));
			AppendInteger(Bytes, Instruction.Operand);
			AppendInteger(Bytes, Instruction.Target);
			AppendInteger(Bytes, Instruction.SourceNode.GetHigh());
			AppendInteger(Bytes, Instruction.SourceNode.GetLow());
		}
		if (Bytes.size() > MaximumBlueprintProgramBytes)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, 0, {},
				"Serialized Blueprint program exceeds its byte limit"));
		return Bytes;
	}

	std::expected<VerifiedBlueprintProgram, BlueprintProgramError> DeserializeBlueprintProgram(std::span<const std::byte> Bytes)
	{
		if (Bytes.size() < 20 || Bytes.size() > MaximumBlueprintProgramBytes ||
			!std::equal(BlueprintMagic.begin(), BlueprintMagic.end(), Bytes.begin()))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is invalid"));
		size_t Offset = BlueprintMagic.size();
		BlueprintProgramSource Source;
		uint32_t ConstantCount = 0;
		uint32_t PropertyCount = 0;
		uint32_t InstructionCount = 0;
		uint32_t SceneTransitionAssetCount = 0;
		uint32_t TimerNameCount = 0;
		if (!ReadInteger(Bytes, Offset, Source.Version) || !ReadInteger(Bytes, Offset, ConstantCount) ||
			!ReadInteger(Bytes, Offset, PropertyCount))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		const uint32_t EncodedVersion = Source.Version;
		if (EncodedVersion < BlueprintLegacyProgramFormatVersion || EncodedVersion > BlueprintProgramFormatVersion)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, 0, {},
				"Blueprint program version is not supported"));
		uint32_t ActionNameCount = 0;
		if (EncodedVersion >= BlueprintActionProgramFormatVersion && !ReadInteger(Bytes, Offset, ActionNameCount))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		if (EncodedVersion >= BlueprintSceneTransitionProgramFormatVersion &&
			!ReadInteger(Bytes, Offset, SceneTransitionAssetCount))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		if (EncodedVersion >= BlueprintTimerProgramFormatVersion && !ReadInteger(Bytes, Offset, TimerNameCount))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		if (!ReadInteger(Bytes, Offset, InstructionCount))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		if (ConstantCount > BlueprintMaximumInstructionCount || PropertyCount > BlueprintMaximumScalarCount ||
			ActionNameCount > BlueprintMaximumActionNameCount ||
			SceneTransitionAssetCount > BlueprintMaximumSceneTransitionAssetCount ||
			TimerNameCount > BlueprintMaximumTimerNameCount ||
			InstructionCount == 0 || InstructionCount > BlueprintMaximumInstructionCount)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, 0, {},
				"Blueprint program counts exceed structural limits"));
		Source.Constants.reserve(ConstantCount);
		Source.ScalarPropertyIds.reserve(PropertyCount);
		Source.ActionNames.reserve(ActionNameCount);
		Source.SceneTransitionAssets.reserve(SceneTransitionAssetCount);
		Source.TimerNames.reserve(TimerNameCount);
		Source.Instructions.reserve(InstructionCount);
		for (uint32_t Index = 0; Index < ConstantCount; ++Index)
		{
			uint32_t Bits = 0;
			if (!ReadInteger(Bytes, Offset, Bits))
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
					"Blueprint constant table is truncated"));
			Source.Constants.push_back(std::bit_cast<float>(Bits));
		}
		for (uint32_t Index = 0; Index < PropertyCount; ++Index)
		{
			uint16_t Length = 0;
			if (!ReadInteger(Bytes, Offset, Length) || Length == 0 || Length > BlueprintMaximumPropertyIdentifierLength ||
				Bytes.size() - Offset < Length)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
					"Blueprint property table is truncated or invalid"));
			std::string PropertyId;
			PropertyId.resize(Length);
			for (char& Character : PropertyId) Character = static_cast<char>(std::to_integer<uint8_t>(Bytes[Offset++]));
			Source.ScalarPropertyIds.push_back(std::move(PropertyId));
		}
		for (uint32_t Index = 0; Index < ActionNameCount; ++Index)
		{
			uint16_t Length = 0;
			if (!ReadInteger(Bytes, Offset, Length) || Length == 0 || Length > BlueprintMaximumActionNameLength ||
				Offset > Bytes.size() || Bytes.size() - Offset < Length)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
					"Blueprint action name table is truncated or invalid"));
			std::string ActionName;
			ActionName.resize(Length);
			for (char& Character : ActionName) Character = static_cast<char>(std::to_integer<uint8_t>(Bytes[Offset++]));
			Source.ActionNames.push_back(std::move(ActionName));
		}
		for (uint32_t Index = 0; Index < SceneTransitionAssetCount; ++Index)
		{
			uint64_t High = 0;
			uint64_t Low = 0;
			if (!ReadInteger(Bytes, Offset, High) || !ReadInteger(Bytes, Offset, Low))
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
					"Blueprint scene transition asset table is truncated"));
			Source.SceneTransitionAssets.emplace_back(High, Low);
		}
		for (uint32_t Index = 0; Index < TimerNameCount; ++Index)
		{
			uint16_t Length = 0;
			if (!ReadInteger(Bytes, Offset, Length) || Length == 0 || Length > BlueprintMaximumTimerNameLength ||
				Offset > Bytes.size() || Bytes.size() - Offset < Length)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
					"Blueprint timer name table is truncated or invalid"));
			std::string TimerName;
			TimerName.resize(Length);
			for (char& Character : TimerName) Character = static_cast<char>(std::to_integer<uint8_t>(Bytes[Offset++]));
			Source.TimerNames.push_back(std::move(TimerName));
		}
		for (uint32_t Index = 0; Index < InstructionCount; ++Index)
		{
			if (Bytes.size() - Offset < 25)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, Index, {},
					"Blueprint instruction stream is truncated"));
			BlueprintInstruction Instruction;
			Instruction.Opcode = static_cast<BlueprintOpcode>(std::to_integer<uint8_t>(Bytes[Offset++]));
			uint64_t NodeHigh = 0;
			uint64_t NodeLow = 0;
			if (!ReadInteger(Bytes, Offset, Instruction.Operand) || !ReadInteger(Bytes, Offset, Instruction.Target) ||
				!ReadInteger(Bytes, Offset, NodeHigh) || !ReadInteger(Bytes, Offset, NodeLow))
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, Index, {},
					"Blueprint instruction stream is truncated"));
			Instruction.SourceNode = UUID(NodeHigh, NodeLow);
			if (EncodedVersion < BlueprintDeferredServiceProgramFormatVersion &&
				(Instruction.Opcode == BlueprintOpcode::RequestApplyForceSelf ||
					Instruction.Opcode == BlueprintOpcode::RequestAudioControlSelf))
				return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, Index,
					Instruction.SourceNode, "Blueprint deferred service opcode requires a newer program version"));
			if (EncodedVersion < BlueprintTimerProgramFormatVersion &&
				(Instruction.Opcode == BlueprintOpcode::RequestTimerSelf || Instruction.Opcode == BlueprintOpcode::JumpIfTimer))
				return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, Index,
					Instruction.SourceNode, "Blueprint timer opcode requires a newer program version"));
			if (EncodedVersion < BlueprintSpawnProgramFormatVersion && Instruction.Opcode == BlueprintOpcode::RequestSpawnEntity)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, Index,
					Instruction.SourceNode, "Blueprint entity spawn opcode requires a newer program version"));
			if (EncodedVersion < BlueprintRaycastProgramFormatVersion && Instruction.Opcode == BlueprintOpcode::Raycast)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, Index,
					Instruction.SourceNode, "Blueprint physics query opcode requires a newer program version"));
			Source.Instructions.push_back(Instruction);
		}
		if (Offset != Bytes.size())
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, InstructionCount, {},
				"Blueprint program contains trailing bytes"));
		Source.Version = BlueprintProgramFormatVersion;
		return VerifyBlueprintProgram(std::move(Source));
	}
}
