#include "Core/PulseForgePCH.h"
#include "ImGui/UI.h"

#include <imgui.h>

namespace PulseForge::UI
{
	void BeginWindow(const std::string& Title)
	{
		ImGui::Begin(Title.c_str());
	}

	void EndWindow()
	{
		ImGui::End();
	}

	void Text(const std::string& Text)
	{
		ImGui::Text("%s", Text.c_str());
	}

	bool Button(const std::string& Label)
	{
		return ImGui::Button(Label.c_str());
	}

	void Separator()
	{
		ImGui::Separator();
	}

	void SameLine()
	{
		ImGui::SameLine();
	}

	void ShowDemoWindow()
	{
		ImGui::ShowDemoWindow();
	}
}
