#pragma once

#include "Scripting/BlueprintProgram.h"

#include <expected>
#include <span>
#include <vector>

namespace PulseForge
{
	inline constexpr uint32_t BlueprintAssetFormatVersion = 5;
	inline constexpr uint32_t BlueprintPreviousAssetFormatVersion = 4;
	inline constexpr uint32_t BlueprintLegacyTimerAssetFormatVersion = 3;
	inline constexpr uint32_t BlueprintLegacyActionAssetFormatVersion = 2;
	inline constexpr uint32_t BlueprintLegacyAssetFormatVersion = 1;
	inline constexpr size_t BlueprintMaximumEventPrograms = 14;
	inline constexpr size_t BlueprintMaximumAssetBytes = 8u * 1024u * 1024u;

	struct BlueprintEventProgramSource final
	{
		BlueprintEventKind Event = BlueprintEventKind::OnCreate;
		BlueprintProgramSource Program;
	};

	struct BlueprintVerifiedEventProgram final
	{
		BlueprintEventKind Event = BlueprintEventKind::OnCreate;
		VerifiedBlueprintProgram Program;
	};

	class PULSEFORGE_API VerifiedBlueprintAsset final
	{
	public:
		[[nodiscard]] uint32_t GetVersion() const noexcept { return m_Version; }
		[[nodiscard]] uint64_t GetSourceRevision() const noexcept { return m_SourceRevision; }
		[[nodiscard]] std::span<const BlueprintVerifiedEventProgram> GetEvents() const noexcept { return m_Events; }

	private:
		VerifiedBlueprintAsset(uint32_t Version, uint64_t SourceRevision, std::vector<BlueprintVerifiedEventProgram> Events)
			: m_Version(Version), m_SourceRevision(SourceRevision), m_Events(std::move(Events)) {}
		uint32_t m_Version = BlueprintAssetFormatVersion;
		uint64_t m_SourceRevision = 0;
		std::vector<BlueprintVerifiedEventProgram> m_Events;
		friend PULSEFORGE_API std::expected<VerifiedBlueprintAsset, BlueprintProgramError>
			VerifyBlueprintAsset(std::vector<BlueprintEventProgramSource> Events, uint64_t SourceRevision);
	};

	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintAsset, BlueprintProgramError> VerifyBlueprintAsset(
		std::vector<BlueprintEventProgramSource> Events, uint64_t SourceRevision = 0);
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, BlueprintProgramError> SerializeBlueprintAsset(
		std::span<const BlueprintEventProgramSource> Events, uint64_t SourceRevision = 0);
	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintAsset, BlueprintProgramError> DeserializeBlueprintAsset(
		std::span<const std::byte> Bytes);
	[[nodiscard]] PULSEFORGE_API uint64_t ComputeBlueprintSourceRevision(std::span<const std::byte> SourceBytes) noexcept;
}
