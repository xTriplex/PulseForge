#include "Editor/EditorViewportToolbar.h"

#include "Editor/EditorStyle.h"
#include "Editor/EditorViewportToolbarPolicy.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace PulseForgeEditor
{
	EditorViewportToolbarActions EditorViewportToolbar::Draw(EditorStyle& Style, EditorViewportToolState& Tools, bool CanEditViewport) const
	{
		EditorViewportToolbarActions Actions;
		if (!Style.BeginToolbar("ViewportToolbar", EditorStyleMetrics::ViewportToolbarHeight))
		{
			Style.EndToolbar();
			return Actions;
		}

		const EditorViewportToolbarLayout Layout = GetEditorViewportToolbarLayout(ImGui::GetContentRegionAvail().x);
		if (Layout == EditorViewportToolbarLayout::Overflow)
		{
			ImGui::BeginDisabled(!CanEditViewport);
			ImGui::SetNextItemWidth(
				(std::max)(76.0f, ImGui::GetContentRegionAvail().x - EditorStyleMetrics::ViewportToolbarButtonSize - 5.0f));
			if (ImGui::BeginCombo("##ViewportToolOverflow", GetToolName(Tools.GetTool())))
			{
				DrawToolMenuItems(Style, Tools, Actions, CanEditViewport);
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();
			ImGui::SameLine(0.0f, 4.0f);
			DrawMoreButton(Style);
			DrawOptionsPopup(Style, Tools, Actions, true, true);
		}
		else
		{
			DrawToolButton(Style, Tools, Actions, ViewportTool::Select, EditorIcon::Select, "Select", "ViewportSelectTool",
				"Select entities (1 / Q).", CanEditViewport);
			ImGui::SameLine(0.0f, 3.0f);
			DrawToolButton(Style, Tools, Actions, ViewportTool::Move, EditorIcon::Translate, "Move", "ViewportMoveTool",
				"Move entities (2 / W).", CanEditViewport);
			ImGui::SameLine(0.0f, 3.0f);
			DrawToolButton(Style, Tools, Actions, ViewportTool::Rotate, EditorIcon::Rotate, "Rotate", "ViewportRotateTool",
				"Rotate entities (3 / E).", CanEditViewport);
			ImGui::SameLine(0.0f, 3.0f);
			DrawToolButton(Style, Tools, Actions, ViewportTool::Scale, EditorIcon::Scale, "Scale", "ViewportScaleTool",
				"Scale entities (4 / R).", CanEditViewport);

			DrawSeparator(Style);
			DrawCoordinateSpaceControl(Tools, Actions);
			DrawSeparator(Style);
			DrawSnapToggle(Tools.GetValues());

			if (Layout == EditorViewportToolbarLayout::Full)
			{
				ImGui::SameLine(0.0f, 5.0f);
				DrawSnapIncrementControl(Tools, false);
				DrawSeparator(Style);
				DrawExposureControl(Style, Tools.GetValues(), false);
			}
			else
			{
				ImGui::SameLine(0.0f, 4.0f);
				DrawMoreButton(Style);
				DrawOptionsPopup(Style, Tools, Actions, false, false);
			}
		}

		Style.EndToolbar();
		return Actions;
	}

	void EditorViewportToolbar::DrawToolButton(EditorStyle& Style, const EditorViewportToolState& Tools,
		EditorViewportToolbarActions& Actions, ViewportTool Tool, EditorIcon Icon, std::string_view Label, std::string_view StableID,
		std::string_view Tooltip, bool CanEditViewport)
	{
		if (Style.ToolIconButton(
				Icon, Label, StableID, Tooltip, Tools.GetTool() == Tool, CanEditViewport, EditorStyleMetrics::ViewportToolbarButtonSize))
			Actions.SelectedTool = Tool;
	}

	const char* EditorViewportToolbar::GetToolName(ViewportTool Tool) noexcept
	{
		switch (Tool)
		{
		case ViewportTool::Select:
			return "Select";
		case ViewportTool::Move:
			return "Move";
		case ViewportTool::Rotate:
			return "Rotate";
		case ViewportTool::Scale:
			return "Scale";
		}
		return "Select";
	}

	void EditorViewportToolbar::DrawCoordinateSpaceControl(const EditorViewportToolState& Tools, EditorViewportToolbarActions& Actions)
	{
		if (ImGui::SmallButton(Tools.GetValues().LocalSpace ? "Local##ViewportCoordinateSpace" : "World##ViewportCoordinateSpace"))
			Actions.ToggleCoordinateSpace = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Local and World gizmo axes.");
	}

	void EditorViewportToolbar::DrawSeparator(EditorStyle& Style)
	{
		ImGui::SameLine(0.0f, EditorStyleMetrics::ViewportToolbarGroupSpacing);
		const ImVec2 Cursor = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(Cursor.x + 0.5f, Cursor.y + 3.0f), ImVec2(Cursor.x + 0.5f, Cursor.y + 23.0f),
			ImGui::GetColorU32(Style.GetColor(EditorColorToken::BorderStrong)));
		ImGui::Dummy(ImVec2(1.0f, 26.0f));
		ImGui::SameLine(0.0f, EditorStyleMetrics::ViewportToolbarGroupSpacing);
	}

	void EditorViewportToolbar::DrawMoreButton(EditorStyle& Style)
	{
		if (Style.IconButton(EditorIcon::Settings, "More", "ViewportToolbarOptions", "More viewport and transform options", false,
				EditorStyleMetrics::ViewportToolbarButtonSize, EditorStyleMetrics::ViewportToolbarButtonSize))
			ImGui::OpenPopup("ViewportOptionsPopup");
	}

	void EditorViewportToolbar::DrawToolMenuItems(
		EditorStyle& Style, const EditorViewportToolState& Tools, EditorViewportToolbarActions& Actions, bool CanEditViewport)
	{
		const auto ToolItem = [&Style, &Tools, &Actions, CanEditViewport](
								  EditorIcon Icon, const char* Label, const char* Shortcut, ViewportTool Tool)
		{
			if (Style.MenuItem(Icon, Label, Shortcut, Tools.GetTool() == Tool, CanEditViewport)) Actions.SelectedTool = Tool;
		};
		ToolItem(EditorIcon::Select, "Select", "1", ViewportTool::Select);
		ToolItem(EditorIcon::Translate, "Move", "2", ViewportTool::Move);
		ToolItem(EditorIcon::Rotate, "Rotate", "3", ViewportTool::Rotate);
		ToolItem(EditorIcon::Scale, "Scale", "4", ViewportTool::Scale);
	}

	void EditorViewportToolbar::DrawOptionsPopup(EditorStyle& Style, EditorViewportToolState& Tools, EditorViewportToolbarActions& Actions,
		bool IncludeCoordinateSpace, bool IncludeSnapToggle)
	{
		if (!ImGui::BeginPopup("ViewportOptionsPopup")) return;

		if (IncludeCoordinateSpace) DrawCoordinateSpaceControl(Tools, Actions);
		if (IncludeCoordinateSpace && IncludeSnapToggle) ImGui::Separator();
		if (IncludeSnapToggle) DrawSnapToggle(Tools.GetValues());
		DrawSnapIncrementControl(Tools, true);
		ImGui::Separator();
		DrawExposureControl(Style, Tools.GetValues(), true);
		ImGui::EndPopup();
	}

	void EditorViewportToolbar::DrawSnapToggle(EditorViewportToolValues& Values)
	{
		ImGui::Checkbox("Snap", &Values.TransformSnapping);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap during gizmo drags. Hold Ctrl to temporarily enable snapping.");
	}

	void EditorViewportToolbar::DrawSnapIncrementControl(EditorViewportToolState& Tools, bool ShowLabel)
	{
		struct SnapPreset
		{
			const char* Label;
			float Value;
		};
		constexpr std::array<SnapPreset, 5> TranslationPresets{ { { "0.1 units", 0.1f }, { "0.5 units", 0.5f }, { "1 unit", 1.0f },
			{ "5 units", 5.0f }, { "10 units", 10.0f } } };
		constexpr std::array<SnapPreset, 5> RotationPresets{ { { "5 degrees", 5.0f }, { "10 degrees", 10.0f }, { "15 degrees", 15.0f },
			{ "45 degrees", 45.0f }, { "90 degrees", 90.0f } } };
		constexpr std::array<SnapPreset, 5> ScalePresets{ { { "0.05 scale", 0.05f }, { "0.1 scale", 0.1f }, { "0.25 scale", 0.25f },
			{ "0.5 scale", 0.5f }, { "1.0 scale", 1.0f } } };

		EditorViewportToolValues& Values = Tools.GetValues();
		float* Increment = &Values.TranslationSnapIncrement;
		const std::array<SnapPreset, 5>* Presets = &TranslationPresets;
		if (Tools.GetTool() == ViewportTool::Rotate)
		{
			Increment = &Values.RotationSnapDegrees;
			Presets = &RotationPresets;
		}
		else if (Tools.GetTool() == ViewportTool::Scale)
		{
			Increment = &Values.ScaleSnapIncrement;
			Presets = &ScalePresets;
		}

		const char* CurrentLabel = (*Presets)[0].Label;
		for (const SnapPreset& Preset : *Presets)
			if (std::abs(Preset.Value - *Increment) < 1.0e-5f) CurrentLabel = Preset.Label;
		ImGui::SetNextItemWidth(ShowLabel ? 156.0f : 112.0f);
		if (ImGui::BeginCombo(ShowLabel ? "Snap Increment" : "##TransformSnapIncrement", CurrentLabel))
		{
			for (const SnapPreset& Preset : *Presets)
			{
				const bool Selected = std::abs(Preset.Value - *Increment) < 1.0e-5f;
				if (ImGui::Selectable(Preset.Label, Selected)) *Increment = Preset.Value;
				if (Selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}

	void EditorViewportToolbar::DrawExposureControl(EditorStyle& Style, EditorViewportToolValues& Values, bool ShowLabel)
	{
		if (!ShowLabel)
		{
			Style.TextMuted("Exposure");
			ImGui::SameLine(0.0f, 4.0f);
		}
		ImGui::SetNextItemWidth(ShowLabel ? 144.0f : 98.0f);
		(void)ImGui::DragFloat(ShowLabel ? "Exposure (EV)" : "##ViewportExposureEV", &Values.ExposureEV, 0.05f, -8.0f, 8.0f, "%.2f EV",
			ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust viewport exposure from -8 to +8 EV. This does not modify the scene.");
	}
} // namespace PulseForgeEditor
