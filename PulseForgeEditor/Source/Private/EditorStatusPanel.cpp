#include "Editor/EditorStatusPanel.h"

#include "Core/Log.h"
#include "Editor/EditorStyle.h"

#include <imgui.h>

#include <utility>

namespace PulseForgeEditor
{
	void EditorStatusPanel::SetStatus(std::string Message)
	{
		m_Message = std::move(Message);
		m_Severity = EditorStatusSeverity::Information;
		PF_INFO("{}", m_Message);
	}

	void EditorStatusPanel::SetWarning(std::string Message)
	{
		m_Message = std::move(Message);
		m_Severity = EditorStatusSeverity::Warning;
		PF_WARN("{}", m_Message);
	}

	void EditorStatusPanel::SetError(std::string Message)
	{
		m_Message = std::move(Message);
		m_Severity = EditorStatusSeverity::Error;
		PF_ERROR("{}", m_Message);
	}

	void EditorStatusPanel::DrawMessage(EditorStyle& Style, EditorStatusMessagePresentation Presentation) const
	{
		const ImVec4 Color = m_Severity == EditorStatusSeverity::Error					? Style.GetColor(EditorColorToken::Error)
							 : m_Severity == EditorStatusSeverity::Warning				? Style.GetColor(EditorColorToken::Warning)
							 : Presentation == EditorStatusMessagePresentation::Wrapped ? Style.GetColor(EditorColorToken::Text)
																						: Style.GetColor(EditorColorToken::TextMuted);

		if (Presentation == EditorStatusMessagePresentation::Wrapped)
		{
			ImGui::TextWrapped("%s", m_Message.c_str());
			return;
		}

		const float MessageWidth = ImGui::GetContentRegionAvail().x;
		ImGui::TextColored(Color, "%s", m_Message.c_str());
		if (ImGui::IsItemHovered() && ImGui::CalcTextSize(m_Message.c_str()).x > MessageWidth) ImGui::SetTooltip("%s", m_Message.c_str());
	}

	void EditorStatusPanel::DrawStatusBarContents(
		EditorStyle& Style, bool HasScene, bool SceneDirty, bool RuntimeActive, std::string_view RendererName) const
	{
		if (!ImGui::BeginTable("GlobalStatusLayout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) return;

		ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 276.0f);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		DrawMessage(Style, EditorStatusMessagePresentation::SingleLine);

		ImGui::TableSetColumnIndex(1);
		if (SceneDirty) ImGui::TextColored(Style.GetColor(EditorColorToken::Warning), "Unsaved scene");
		else Style.TextMuted(!HasScene ? "No scene" : "Scene saved");
		if (RuntimeActive)
		{
			ImGui::SameLine();
			Style.TextMuted("|");
			ImGui::SameLine();
			Style.TextMuted("Runtime");
		}
		ImGui::SameLine();
		Style.TextMuted("|");
		ImGui::SameLine();
		Style.TextMuted(RendererName);
		ImGui::EndTable();
	}
} // namespace PulseForgeEditor
