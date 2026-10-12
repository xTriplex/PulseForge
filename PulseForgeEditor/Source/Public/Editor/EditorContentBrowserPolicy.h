#pragma once

#include <algorithm>
#include <cmath>

namespace PulseForgeEditor
{
	struct ContentBrowserVerticalLayout
	{
		float AssetItemsHeight = 0.0f;
		float DetailsHeight = 0.0f;
		float SelectionSummaryHeight = 0.0f;
	};

	namespace ContentBrowserLayoutMetrics
	{
		inline constexpr float CompactDetailsThreshold = 164.0f;
		inline constexpr float CompactSelectionSummaryHeight = 28.0f;
		inline constexpr float MinimumAssetItemsHeight = 72.0f;
		inline constexpr float RegularDetailsMaximumHeight = 108.0f;
	}

	[[nodiscard]] inline ContentBrowserVerticalLayout GetContentBrowserVerticalLayout(
		float AvailableHeight,
		bool HasSelection,
		bool IsMaterial,
		bool ExpandCompactDetails = false) noexcept
	{
		AvailableHeight = std::isfinite(AvailableHeight) ? std::max(AvailableHeight, 0.0f) : 0.0f;
		if (!HasSelection)
			return { .AssetItemsHeight = AvailableHeight };
		if (IsMaterial)
		{
			const float SummaryHeight = std::min(
				ContentBrowserLayoutMetrics::CompactSelectionSummaryHeight, AvailableHeight);
			return {
				.AssetItemsHeight = AvailableHeight - SummaryHeight,
				.SelectionSummaryHeight = SummaryHeight
			};
		}

		if (AvailableHeight < ContentBrowserLayoutMetrics::CompactDetailsThreshold)
		{
			const float SummaryHeight = std::min(
				ContentBrowserLayoutMetrics::CompactSelectionSummaryHeight, AvailableHeight);
			if (ExpandCompactDetails)
			{
				const float RemainingHeight = AvailableHeight - SummaryHeight;
				const float DetailsHeight = RemainingHeight * 0.55f;
				return {
					.AssetItemsHeight = RemainingHeight - DetailsHeight,
					.DetailsHeight = DetailsHeight,
					.SelectionSummaryHeight = SummaryHeight
				};
			}
			return {
				.AssetItemsHeight = AvailableHeight - SummaryHeight,
				.SelectionSummaryHeight = SummaryHeight
			};
		}

		const float MinimumItems = std::min(
			ContentBrowserLayoutMetrics::MinimumAssetItemsHeight, AvailableHeight * 0.45f);
		const float MaximumDetails = std::max(AvailableHeight - MinimumItems, 0.0f);
		const float DetailsHeight = std::min({
			ContentBrowserLayoutMetrics::RegularDetailsMaximumHeight,
			AvailableHeight * 0.36f,
			MaximumDetails });
		return {
			.AssetItemsHeight = AvailableHeight - DetailsHeight,
			.DetailsHeight = DetailsHeight
		};
	}
}
