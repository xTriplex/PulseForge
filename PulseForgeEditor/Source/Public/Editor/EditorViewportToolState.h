#pragma once

#include "Editor/ViewportMath.h"

namespace PulseForgeEditor
{
	struct EditorViewportToolValues
	{
		bool LocalSpace = true;
		bool TransformSnapping = false;
		float TranslationSnapIncrement = 1.0f;
		float RotationSnapDegrees = 15.0f;
		float ScaleSnapIncrement = 0.1f;
		float ExposureEV = 0.0f;
	};

	class EditorViewportToolState final
	{
	public:
		[[nodiscard]] ViewportTool GetTool() const noexcept { return m_Tool; }
		[[nodiscard]] TransformGizmoOperation GetOperation() const noexcept { return m_Operation; }
		[[nodiscard]] const EditorViewportToolValues& GetValues() const noexcept { return m_Values; }
		[[nodiscard]] EditorViewportToolValues& GetValues() noexcept { return m_Values; }

		[[nodiscard]] bool SetTool(ViewportTool Tool) noexcept;
		void ToggleCoordinateSpace() noexcept { m_Values.LocalSpace = !m_Values.LocalSpace; }
		void ResetExposure() noexcept { m_Values.ExposureEV = 0.0f; }

	private:
		ViewportTool m_Tool = ViewportTool::Move;
		TransformGizmoOperation m_Operation = TransformGizmoOperation::Translate;
		EditorViewportToolValues m_Values;
	};
}
