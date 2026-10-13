#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace PulseForgeEditor
{
	inline constexpr uint32_t DefaultDocumentOpeningPreference = 0;

	[[nodiscard]] constexpr bool ShouldBuildDefaultDockspace(bool HasRestorableDockspace) noexcept
	{
		return !HasRestorableDockspace;
	}

	[[nodiscard]] inline std::optional<uint32_t> ParseDocumentOpeningPreference(
		std::string_view Value) noexcept
	{
		uint32_t Parsed = 0;
		const auto [End, Error] = std::from_chars(Value.data(), Value.data() + Value.size(), Parsed);
		if (Error != std::errc{} || End != Value.data() + Value.size() || Parsed > 2)
			return std::nullopt;
		return Parsed;
	}

	[[nodiscard]] constexpr uint32_t ResolveDocumentOpeningPreference(
		bool HasRecognizedState,
		bool StateFormatWasValid,
		std::optional<uint32_t> PersistedPreference) noexcept
	{
		if (!HasRecognizedState || !StateFormatWasValid || !PersistedPreference ||
			*PersistedPreference > 2)
			return DefaultDocumentOpeningPreference;

		return *PersistedPreference;
	}
}
