#pragma once

#include "Assets/AssetRegistry.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string_view>

namespace PulseForgeEditor
{
	[[nodiscard]] inline bool AssetHasExtension(const PulseForge::AssetRecord& Asset, std::initializer_list<std::string_view> Extensions)
	{
		const std::string Extension = Asset.ProjectRelativePath.extension().string();
		return std::any_of(Extensions.begin(), Extensions.end(), [&](std::string_view Candidate) { return Extension == Candidate; });
	}

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
