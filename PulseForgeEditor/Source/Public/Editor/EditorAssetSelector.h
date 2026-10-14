#pragma once

#include "Assets/AssetRegistry.h"

#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	class EditorAssetSelector final
	{
	public:
		[[nodiscard]] bool Draw(
			const char* Label,
			std::optional<PulseForge::AssetID>& Current,
			const std::vector<PulseForge::AssetRecord>& Assets,
			std::initializer_list<std::string_view> Extensions,
			bool AllowNone);

	private:
		std::array<char, 256> m_Search{};
		std::string m_SearchOwner;
	};
}
