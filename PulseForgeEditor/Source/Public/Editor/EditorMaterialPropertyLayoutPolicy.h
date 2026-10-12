#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace PulseForgeEditor
{
	enum class MaterialPropertyLayout : uint8_t
	{
		Stacked,
		TwoColumn
	};

	struct MaterialValuePairLayout
	{
		bool Stacked = true;
		float FirstWidth = 0.0f;
		float SecondWidth = 0.0f;
	};

	namespace EditorMaterialPropertyLayoutMetrics
	{
		inline constexpr float TwoColumnBreakpoint = 560.0f;
		inline constexpr float LabelWidthRatio = 0.40f;
		inline constexpr float LabelWidthMinimum = 184.0f;
		inline constexpr float LabelWidthMaximum = 264.0f;
		inline constexpr float ValuePairControlMinimumWidth = 128.0f;
	}

	[[nodiscard]] constexpr float SanitizeMaterialPropertyWidth(float Width) noexcept
	{
		return Width >= 0.0f && Width <= (std::numeric_limits<float>::max)() ? Width : 0.0f;
	}

	[[nodiscard]] constexpr MaterialPropertyLayout GetMaterialPropertyLayout(float AvailableWidth) noexcept
	{
		AvailableWidth = SanitizeMaterialPropertyWidth(AvailableWidth);
		return AvailableWidth >= EditorMaterialPropertyLayoutMetrics::TwoColumnBreakpoint
			? MaterialPropertyLayout::TwoColumn
			: MaterialPropertyLayout::Stacked;
	}

	[[nodiscard]] constexpr float GetMaterialPropertyLabelWidth(float AvailableWidth) noexcept
	{
		AvailableWidth = SanitizeMaterialPropertyWidth(AvailableWidth);
		const float LowerBound = (std::min)(
			EditorMaterialPropertyLayoutMetrics::LabelWidthMinimum, AvailableWidth);
		const float UpperBound = (std::min)(
			EditorMaterialPropertyLayoutMetrics::LabelWidthMaximum, AvailableWidth);
		return (std::clamp)(
			AvailableWidth * EditorMaterialPropertyLayoutMetrics::LabelWidthRatio,
			LowerBound,
			UpperBound);
	}

	[[nodiscard]] constexpr MaterialValuePairLayout GetMaterialValuePairLayout(
		float AvailableWidth,
		float ItemSpacing) noexcept
	{
		AvailableWidth = SanitizeMaterialPropertyWidth(AvailableWidth);
		ItemSpacing = SanitizeMaterialPropertyWidth(ItemSpacing);
		const float WidthAfterSpacing = (std::max)(AvailableWidth - ItemSpacing, 0.0f);
		const float ControlWidth = WidthAfterSpacing * 0.5f;
		if (ControlWidth < EditorMaterialPropertyLayoutMetrics::ValuePairControlMinimumWidth)
			return { .Stacked = true, .FirstWidth = AvailableWidth, .SecondWidth = AvailableWidth };

		return { .Stacked = false, .FirstWidth = ControlWidth, .SecondWidth = ControlWidth };
	}
}
