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
			Source.UIActionNames.size() > BlueprintMaximumUIActionNameCount)
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
		std::unordered_set<std::string> UIActionNames;
		for (const std::string& Name : Source.UIActionNames)
			if (Name.empty() || Name.size() > BlueprintMaximumUIActionNameLength || !UIActionNames.emplace(Name).second)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, 0, {},
					"Blueprint UI action names must be unique, non-empty, and bounded"));

		std::vector<std::optional<size_t>> StackDepth(Source.Instructions.size());
		std::deque<uint32_t> Pending;
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
			case BlueprintOpcode::JumpIfUIAction:
				if (Instruction.Operand >= Source.UIActionNames.size())
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOperand, Index, Instruction.SourceNode,
						"Blueprint UI action name index is out of range"));
				if (const auto Error = Enqueue(Instruction.Target, Depth, Index)) return std::unexpected(*Error);
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
		std::string_view UIActionName)
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
		while (ProgramCounter < Source.Instructions.size())
		{
			if (Result.InstructionsExecuted >= InstructionBudget)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InstructionBudgetExceeded, ProgramCounter,
					Source.Instructions[ProgramCounter].SourceNode, "Blueprint event exceeded its instruction budget"));
			++Result.InstructionsExecuted;
			const BlueprintInstruction& Instruction = Source.Instructions[ProgramCounter];
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
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidConstant, ProgramCounter,
						Instruction.SourceNode, "Blueprint arithmetic produced a non-finite value"));
				Stack.push_back(Value); ++ProgramCounter; break;
			}
			case BlueprintOpcode::Multiply:
			{
				const float Right = Stack.back(); Stack.pop_back();
				const float Left = Stack.back(); Stack.pop_back();
				const float Value = Left * Right;
				if (!std::isfinite(Value))
					return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidConstant, ProgramCounter,
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
			case BlueprintOpcode::JumpIfUIAction:
				ProgramCounter = UIActionName == Source.UIActionNames[Instruction.Operand]
					? Instruction.Target : ProgramCounter + 1;
				break;
			case BlueprintOpcode::WriteScalarProperty:
			{
				const float Value = Stack.back(); Stack.pop_back();
				Result.PropertyWrites.push_back({ Source.ScalarPropertyIds[Instruction.Operand], Value, Instruction.SourceNode });
				++ProgramCounter;
				break;
			}
			case BlueprintOpcode::Return:
				std::copy(CandidateState.begin(), CandidateState.end(), ScalarState.begin());
				return Result;
			default:
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidOpcode, ProgramCounter,
					Instruction.SourceNode, "Verified Blueprint program contains an unknown opcode"));
			}
		}
		return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidControlFlow, ProgramCounter, {},
			"Blueprint execution left the verified instruction range"));
	}

	std::expected<std::vector<std::byte>, BlueprintProgramError> SerializeBlueprintProgram(const BlueprintProgramSource& Source)
	{
		if (const auto Verified = VerifyBlueprintProgram(Source); !Verified) return std::unexpected(Verified.error());
		std::vector<std::byte> Bytes;
		Bytes.reserve(24 + Source.Constants.size() * sizeof(uint32_t) + Source.Instructions.size() * 25);
		Bytes.insert(Bytes.end(), BlueprintMagic.begin(), BlueprintMagic.end());
		AppendInteger(Bytes, Source.Version);
		AppendInteger(Bytes, static_cast<uint32_t>(Source.Constants.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.ScalarPropertyIds.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.UIActionNames.size()));
		AppendInteger(Bytes, static_cast<uint32_t>(Source.Instructions.size()));
		for (const float Constant : Source.Constants) AppendInteger(Bytes, std::bit_cast<uint32_t>(Constant));
		for (const std::string& PropertyId : Source.ScalarPropertyIds)
		{
			AppendInteger(Bytes, static_cast<uint16_t>(PropertyId.size()));
			for (const char Character : PropertyId) Bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(Character)));
		}
		for (const std::string& ActionName : Source.UIActionNames)
		{
			AppendInteger(Bytes, static_cast<uint16_t>(ActionName.size()));
			for (const char Character : ActionName) Bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(Character)));
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
		if (!ReadInteger(Bytes, Offset, Source.Version) || !ReadInteger(Bytes, Offset, ConstantCount) ||
			!ReadInteger(Bytes, Offset, PropertyCount))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		const uint32_t EncodedVersion = Source.Version;
		if (EncodedVersion != BlueprintLegacyProgramFormatVersion && EncodedVersion != BlueprintProgramFormatVersion)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::UnsupportedVersion, 0, {},
				"Blueprint program version is not supported"));
		uint32_t UIActionNameCount = 0;
		if ((EncodedVersion == BlueprintProgramFormatVersion &&
			(!ReadInteger(Bytes, Offset, UIActionNameCount) || !ReadInteger(Bytes, Offset, InstructionCount))) ||
			(EncodedVersion == BlueprintLegacyProgramFormatVersion && !ReadInteger(Bytes, Offset, InstructionCount)))
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
				"Blueprint program header is truncated"));
		if (ConstantCount > BlueprintMaximumInstructionCount || PropertyCount > BlueprintMaximumScalarCount ||
			UIActionNameCount > BlueprintMaximumUIActionNameCount ||
			InstructionCount == 0 || InstructionCount > BlueprintMaximumInstructionCount)
			return std::unexpected(MakeError(BlueprintProgramErrorCode::ProgramTooLarge, 0, {},
				"Blueprint program counts exceed structural limits"));
		Source.Constants.reserve(ConstantCount);
		Source.ScalarPropertyIds.reserve(PropertyCount);
		Source.UIActionNames.reserve(UIActionNameCount);
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
		for (uint32_t Index = 0; Index < UIActionNameCount; ++Index)
		{
			uint16_t Length = 0;
			if (!ReadInteger(Bytes, Offset, Length) || Length == 0 || Length > BlueprintMaximumUIActionNameLength ||
				Offset > Bytes.size() || Bytes.size() - Offset < Length)
				return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, 0, {},
					"Blueprint UI action table is truncated or invalid"));
			std::string ActionName;
			ActionName.resize(Length);
			for (char& Character : ActionName) Character = static_cast<char>(std::to_integer<uint8_t>(Bytes[Offset++]));
			Source.UIActionNames.push_back(std::move(ActionName));
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
			Source.Instructions.push_back(Instruction);
		}
		if (Offset != Bytes.size())
			return std::unexpected(MakeError(BlueprintProgramErrorCode::InvalidByteStream, InstructionCount, {},
				"Blueprint program contains trailing bytes"));
		Source.Version = BlueprintProgramFormatVersion;
		return VerifyBlueprintProgram(std::move(Source));
	}
}
