#pragma once

#include "Scripting/BlueprintProgram.h"

#include <expected>
#include <span>
#include <vector>

namespace PulseForge
{
	inline constexpr uint32_t BlueprintAssetFormatVersion = 4;
	inline constexpr uint32_t BlueprintPreviousAssetFormatVersion = 3;
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
		[[nodiscard]] std::span<const BlueprintVerifiedEventProgram> GetEvents() const noexcept { return m_Events; }

	private:
		VerifiedBlueprintAsset(uint32_t Version, std::vector<BlueprintVerifiedEventProgram> Events)
			: m_Version(Version), m_Events(std::move(Events)) {}
		uint32_t m_Version = BlueprintAssetFormatVersion;
		std::vector<BlueprintVerifiedEventProgram> m_Events;
		friend PULSEFORGE_API std::expected<VerifiedBlueprintAsset, BlueprintProgramError>
			VerifyBlueprintAsset(std::vector<BlueprintEventProgramSource> Events);
	};

	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintAsset, BlueprintProgramError> VerifyBlueprintAsset(
		std::vector<BlueprintEventProgramSource> Events);
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, BlueprintProgramError> SerializeBlueprintAsset(
		std::span<const BlueprintEventProgramSource> Events);
	[[nodiscard]] PULSEFORGE_API std::expected<VerifiedBlueprintAsset, BlueprintProgramError> DeserializeBlueprintAsset(
		std::span<const std::byte> Bytes);
}
