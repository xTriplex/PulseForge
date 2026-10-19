#pragma once

#include "Editor/EditorDocumentManager.h"

#include <algorithm>
#include <cmath>

namespace PulseForgeEditor
{
	[[nodiscard]] inline bool IsValidNativeWindowBounds(
		const EditorDocumentHost::WindowBounds& Bounds) noexcept
	{
		if (!Bounds.HasValue)
			return true;
		return std::isfinite(Bounds.X) && std::isfinite(Bounds.Y) &&
			std::isfinite(Bounds.Width) && std::isfinite(Bounds.Height) &&
			std::isfinite(Bounds.DpiScale) && std::abs(Bounds.X) <= 100000.0f &&
			std::abs(Bounds.Y) <= 100000.0f && Bounds.Width >= 160.0f && Bounds.Width <= 16384.0f &&
			Bounds.Height >= 120.0f && Bounds.Height <= 16384.0f &&
			Bounds.DpiScale >= 0.5f && Bounds.DpiScale <= 8.0f && Bounds.MonitorName.size() <= 256;
	}

	struct EditorNativeWindowRect
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 640.0f;
		float Height = 480.0f;
	};

	struct EditorNativeWindowWorkArea
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
	};

	[[nodiscard]] inline double NativeWindowWorkAreaIntersectionArea(
		float X, float Y, float Width, float Height, const EditorNativeWindowWorkArea& WorkArea) noexcept
	{
		if (!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Width) || !std::isfinite(Height) || !std::isfinite(WorkArea.X) ||
			!std::isfinite(WorkArea.Y) || !std::isfinite(WorkArea.Width) || !std::isfinite(WorkArea.Height) || Width <= 0.0f ||
			Height <= 0.0f || WorkArea.Width <= 0.0f || WorkArea.Height <= 0.0f)
			return 0.0;

		const double IntersectionWidth =
			(std::max)(0.0, (std::min)(static_cast<double>(X + Width), static_cast<double>(WorkArea.X + WorkArea.Width)) -
								(std::max)(static_cast<double>(X), static_cast<double>(WorkArea.X)));
		const double IntersectionHeight =
			(std::max)(0.0, (std::min)(static_cast<double>(Y + Height), static_cast<double>(WorkArea.Y + WorkArea.Height)) -
								(std::max)(static_cast<double>(Y), static_cast<double>(WorkArea.Y)));
		return IntersectionWidth * IntersectionHeight;
	}

	[[nodiscard]] inline EditorNativeWindowRect ResolveNativeWindowRect(
		const EditorDocumentHost::WindowBounds& Saved, const EditorNativeWindowWorkArea& WorkArea, float CurrentDpiScale) noexcept
	{
		EditorNativeWindowRect Result{ Saved.X, Saved.Y, Saved.Width, Saved.Height };
		if (std::isfinite(CurrentDpiScale) && std::isfinite(Saved.DpiScale) && CurrentDpiScale > 0.0f && Saved.DpiScale > 0.0f)
		{
			const float ScaleRatio = std::clamp(CurrentDpiScale / Saved.DpiScale, 0.5f, 2.0f);
			Result.Width *= ScaleRatio;
			Result.Height *= ScaleRatio;
		}

		if (std::isfinite(WorkArea.X) && std::isfinite(WorkArea.Y) &&
			std::isfinite(WorkArea.Width) && std::isfinite(WorkArea.Height) &&
			WorkArea.Width > 0.0f && WorkArea.Height > 0.0f)
		{
			Result.Width = std::min(Result.Width, WorkArea.Width);
			Result.Height = std::min(Result.Height, WorkArea.Height);
			const float VisibleTitleWidth = std::min(64.0f, Result.Width);
			const float VisibleTitleHeight = std::min(40.0f, Result.Height);
			Result.X = std::clamp(Result.X, WorkArea.X - Result.Width + VisibleTitleWidth,
				WorkArea.X + WorkArea.Width - VisibleTitleWidth);
			Result.Y = std::clamp(Result.Y, WorkArea.Y,
				WorkArea.Y + WorkArea.Height - VisibleTitleHeight);
		}

		Result.Width = std::max(Result.Width, 320.0f);
		Result.Height = std::max(Result.Height, 240.0f);
		return Result;
	}
}
