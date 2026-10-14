#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace PulseForgeEditor
{
	[[nodiscard]] inline bool AssetSelectorMatchesSearch(
		std::string_view AssetPath,
		std::string_view Query) noexcept
	{
		if (Query.empty())
			return true;

		const auto Fold = [](unsigned char Character) noexcept
		{
			return static_cast<unsigned char>(std::tolower(Character));
		};
		return std::search(AssetPath.begin(), AssetPath.end(), Query.begin(), Query.end(),
			[&](char PathCharacter, char QueryCharacter)
			{
				return Fold(static_cast<unsigned char>(PathCharacter)) == Fold(static_cast<unsigned char>(QueryCharacter));
			}) != AssetPath.end();
	}
}
