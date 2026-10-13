#pragma once

#include "Editor/EditorDocumentManager.h"

#include <algorithm>
#include <cmath>

namespace PulseForgeEditor
{
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

	[[nodiscard]] inline EditorNativeWindowRect ResolveNativeWindowRect(
		const EditorDocumentHost::WindowBounds& Saved,
		const EditorNativeWindowWorkArea& WorkArea,
		float CurrentDpiScale) noexcept
	{
		EditorNativeWindowRect Result{ Saved.X, Saved.Y, Saved.Width, Saved.Height };
		if (std::isfinite(CurrentDpiScale) && std::isfinite(Saved.DpiScale) &&
			CurrentDpiScale > 0.0f && Saved.DpiScale > 0.0f)
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
