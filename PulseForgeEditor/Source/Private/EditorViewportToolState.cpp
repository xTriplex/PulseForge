#include "Editor/EditorViewportToolState.h"

namespace PulseForgeEditor
{
	bool EditorViewportToolState::SetTool(ViewportTool Tool) noexcept
	{
		if (m_Tool == Tool)
			return false;

		m_Tool = Tool;
		switch (Tool)
		{
		case ViewportTool::Move:
			m_Operation = TransformGizmoOperation::Translate;
			break;
		case ViewportTool::Rotate:
			m_Operation = TransformGizmoOperation::Rotate;
			break;
		case ViewportTool::Scale:
			m_Operation = TransformGizmoOperation::Scale;
			break;
		case ViewportTool::Select:
			break;
		}
		return true;
	}
}
