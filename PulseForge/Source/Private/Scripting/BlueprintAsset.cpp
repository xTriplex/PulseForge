#include "Scripting/BlueprintAsset.h"

#include <algorithm>
#include <array>
#include <limits>

namespace PulseForge
{
	namespace
	{
		constexpr std::array<std::byte, 4> Magic{
			std::byte{ 'P' }, std::byte{ 'F' }, std::byte{ 'B' }, std::byte{ 'A' }
		};
		constexpr size_t HeaderBytes = 9;

		BlueprintProgramError MakeAssetError(BlueprintProgramErrorCode Code, std::string Message)
		{
			return { Code, 0, {}, std::move(Message) };
		}

		void AppendInteger(std::vector<std::byte>& Bytes, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				Bytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xffu));
		}

		bool ReadInteger(std::span<const std::byte> Bytes, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < sizeof(Value)) return false;
			Value = 0;
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset++])) << (Byte * 8);
			return true;
		}
	}

	std::expected<VerifiedBlueprintAsset, BlueprintProgramError> VerifyBlueprintAsset(
		std::vector<BlueprintEventProgramSource> Events)
	{
		if (Events.size() > BlueprintMaximumEventPrograms)
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::ProgramTooLarge,
				"Blueprint asset exceeds its event program limit"));

		std::sort(Events.begin(), Events.end(), [](const BlueprintEventProgramSource& Left, const BlueprintEventProgramSource& Right)
		{
			return static_cast<uint8_t>(Left.Event) < static_cast<uint8_t>(Right.Event);
		});
		std::array<bool, BlueprintMaximumEventPrograms> Seen{};
		std::vector<BlueprintVerifiedEventProgram> Verified;
		Verified.reserve(Events.size());
		for (BlueprintEventProgramSource& Event : Events)
		{
			const size_t EventIndex = static_cast<size_t>(Event.Event);
			if (EventIndex >= Seen.size() || Seen[EventIndex])
				return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidOperand,
					"Blueprint asset contains an invalid or duplicate event binding"));
			Seen[EventIndex] = true;
			auto Program = VerifyBlueprintProgram(std::move(Event.Program));
			if (!Program) return std::unexpected(Program.error());
			Verified.push_back({ Event.Event, std::move(*Program) });
		}
		const auto TimerHandler = std::find_if(Verified.begin(), Verified.end(), [](const BlueprintVerifiedEventProgram& Event)
		{
			return Event.Event == BlueprintEventKind::OnTimer;
		});
		for (const BlueprintVerifiedEventProgram& Event : Verified)
		{
			const BlueprintProgramSource& Program = Event.Program.GetSource();
			for (size_t InstructionIndex = 0; InstructionIndex < Program.Instructions.size(); ++InstructionIndex)
			{
				const BlueprintInstruction& Instruction = Program.Instructions[InstructionIndex];
				if (Instruction.Opcode != BlueprintOpcode::RequestTimerSelf) continue;
				const std::string& TimerName = Program.TimerNames[Instruction.Operand];
				const bool HasHandler = TimerHandler != Verified.end() &&
					std::find(TimerHandler->Program.GetSource().TimerNames.begin(),
						TimerHandler->Program.GetSource().TimerNames.end(), TimerName) !=
						TimerHandler->Program.GetSource().TimerNames.end();
				if (!HasHandler)
					return std::unexpected(BlueprintProgramError{ BlueprintProgramErrorCode::InvalidOperand,
						static_cast<uint32_t>(InstructionIndex), Instruction.SourceNode,
						"Blueprint timer request has no matching OnTimer handler" });
			}
		}
		return VerifiedBlueprintAsset(BlueprintAssetFormatVersion, std::move(Verified));
	}

	std::expected<std::vector<std::byte>, BlueprintProgramError> SerializeBlueprintAsset(
		std::span<const BlueprintEventProgramSource> Events)
	{
		if (Events.size() > BlueprintMaximumEventPrograms)
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::ProgramTooLarge,
				"Blueprint asset exceeds its event program limit"));
		std::vector<BlueprintEventProgramSource> Candidate(Events.begin(), Events.end());
		std::sort(Candidate.begin(), Candidate.end(), [](const BlueprintEventProgramSource& Left, const BlueprintEventProgramSource& Right)
		{
			return static_cast<uint8_t>(Left.Event) < static_cast<uint8_t>(Right.Event);
		});
		if (auto Verified = VerifyBlueprintAsset(Candidate); !Verified)
			return std::unexpected(Verified.error());

		std::vector<std::byte> Bytes(Magic.begin(), Magic.end());
		AppendInteger(Bytes, BlueprintAssetFormatVersion);
		Bytes.push_back(static_cast<std::byte>(Events.size()));
		for (const BlueprintEventProgramSource& Event : Candidate)
		{
			auto Program = SerializeBlueprintProgram(Event.Program);
			if (!Program) return std::unexpected(Program.error());
			if (Program->size() > std::numeric_limits<uint32_t>::max() ||
				Bytes.size() > BlueprintMaximumAssetBytes - (sizeof(uint32_t) + 1) ||
				Program->size() > BlueprintMaximumAssetBytes - Bytes.size() - (sizeof(uint32_t) + 1))
				return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::ProgramTooLarge,
					"Serialized Blueprint asset exceeds its byte limit"));
			Bytes.push_back(static_cast<std::byte>(Event.Event));
			AppendInteger(Bytes, static_cast<uint32_t>(Program->size()));
			Bytes.insert(Bytes.end(), Program->begin(), Program->end());
		}
		if (Bytes.size() < HeaderBytes || Bytes.size() > BlueprintMaximumAssetBytes)
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::ProgramTooLarge,
				"Serialized Blueprint asset exceeds its byte limit"));
		return Bytes;
	}

	std::expected<VerifiedBlueprintAsset, BlueprintProgramError> DeserializeBlueprintAsset(std::span<const std::byte> Bytes)
	{
		if (Bytes.size() < HeaderBytes || Bytes.size() > BlueprintMaximumAssetBytes ||
			!std::equal(Magic.begin(), Magic.end(), Bytes.begin()))
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidByteStream,
				"Blueprint asset header is invalid"));

		size_t Offset = Magic.size();
		uint32_t Version = 0;
		if (!ReadInteger(Bytes, Offset, Version))
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidByteStream,
				"Blueprint asset header is truncated"));
		if (Version != BlueprintLegacyAssetFormatVersion && Version != BlueprintLegacyActionAssetFormatVersion &&
			Version != BlueprintPreviousAssetFormatVersion && Version != BlueprintAssetFormatVersion)
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::UnsupportedVersion,
				"Blueprint asset version is not supported"));
		const size_t EventCount = std::to_integer<uint8_t>(Bytes[Offset++]);
		if (EventCount > BlueprintMaximumEventPrograms)
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::ProgramTooLarge,
				"Blueprint asset event count exceeds its limit"));

		std::vector<BlueprintEventProgramSource> Events;
		Events.reserve(EventCount);
		for (size_t Index = 0; Index < EventCount; ++Index)
		{
			if (Offset >= Bytes.size())
				return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidByteStream,
					"Blueprint event header is truncated"));
			const auto Event = static_cast<BlueprintEventKind>(std::to_integer<uint8_t>(Bytes[Offset++]));
			const size_t EventIndex = static_cast<size_t>(Event);
			const size_t SupportedEventCount = Version == BlueprintLegacyAssetFormatVersion
				? static_cast<size_t>(BlueprintEventKind::OnUIAction) + 1
				: Version == BlueprintLegacyActionAssetFormatVersion
					? static_cast<size_t>(BlueprintEventKind::OnDestroy) + 1
					: Version == BlueprintPreviousAssetFormatVersion
						? static_cast<size_t>(BlueprintEventKind::OnActionCancelled) + 1 : BlueprintMaximumEventPrograms;
			if (EventIndex >= SupportedEventCount)
				return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidOperand,
					"Blueprint asset contains an event unsupported by its format version"));
			uint32_t PayloadBytes = 0;
			if (!ReadInteger(Bytes, Offset, PayloadBytes) || PayloadBytes > Bytes.size() - Offset)
				return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidByteStream,
					"Blueprint event payload is truncated"));
			auto Program = DeserializeBlueprintProgram(Bytes.subspan(Offset, PayloadBytes));
			if (!Program) return std::unexpected(Program.error());
			Events.push_back({ Event, Program->GetSource() });
			Offset += PayloadBytes;
		}
		if (Offset != Bytes.size())
			return std::unexpected(MakeAssetError(BlueprintProgramErrorCode::InvalidByteStream,
				"Blueprint asset contains trailing bytes"));
		return VerifyBlueprintAsset(std::move(Events));
	}
}
