#include "Editor/Detail/EditorInputMappingPanel.h"

#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"
#include "Events/ApplicationEvent.h"
#include "Core/Log.h"
#include "Input/GameplayInputStore.h"

#include <imgui.h>

#include <algorithm>
#include <string_view>

namespace PulseForgeEditor::Detail
{
	namespace
	{
		const char* ValueTypeName(PulseForge::ActionValueType Type) noexcept
		{
			switch (Type)
			{
			case PulseForge::ActionValueType::Digital: return "Digital";
			case PulseForge::ActionValueType::Axis1D: return "Axis 1D";
			case PulseForge::ActionValueType::Axis2D: return "Axis 2D";
			}
			return "Unknown";
		}

		std::string ControlName(PulseForge::InputControl Control)
		{
			const std::string_view Name = PulseForge::GetInputControlName(Control);
			return Name.empty() ? "Unassigned" : std::string(Name);
		}
	}

	void EditorInputMappingPanel::DrawContents(
		const PulseForge::GameplayActionMap& Map,
		bool CanPersist,
		const std::string& PersistenceWarning)
	{
		m_FocusedLastFrame = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (m_Capturing)
		{
			ImGui::Text("Press a keyboard key or mouse button for %s / %s. Escape cancels.",
				m_CaptureActionId.c_str(), m_CaptureBindingId.c_str());
			if (ImGui::Button("Cancel capture"))
			{
				m_Capturing = false;
				m_CaptureActionId.clear();
				m_CaptureBindingId.clear();
				m_Status = "Binding capture canceled.";
			}
		}
		else if (!CanPersist)
			ImGui::TextWrapped("Bindings are read-only: %s", PersistenceWarning.c_str());
		else
			ImGui::TextUnformatted("Choose Rebind, then press a keyboard key or mouse button. Escape cancels.");

		if (!m_Status.empty()) ImGui::TextWrapped("%s", m_Status.c_str());
		ImGui::Separator();
		if (Map.Actions.empty())
		{
			ImGui::TextUnformatted("This project has no authored input actions.");
			return;
		}

		for (const PulseForge::GameplayActionDefinition& Action : Map.Actions)
		{
			const std::string Header = Action.Id + "  (" + ValueTypeName(Action.Type) + ")";
			if (!ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
			if (Action.Bindings.empty())
			{
				ImGui::TextUnformatted("No bindings");
				continue;
			}

			for (const PulseForge::ActionBinding& Binding : Action.Bindings)
			{
				ImGui::PushID(Action.Id.c_str());
				ImGui::PushID(Binding.Id.c_str());
				ImGui::TextUnformatted(Binding.Id.c_str());
				ImGui::SameLine();
				ImGui::Text("Primary: %s", ControlName(Binding.Primary).c_str());
				ImGui::SameLine();
				const bool CanCaptureBinding = CanPersist && !m_Capturing &&
					(Binding.Kind == PulseForge::ActionBindingKind::Button || Binding.Kind == PulseForge::ActionBindingKind::Axis);
				ImGui::BeginDisabled(!CanCaptureBinding);
				if (ImGui::SmallButton("Rebind primary"))
				{
					m_CaptureActionId = Action.Id;
					m_CaptureBindingId = Binding.Id;
					m_CaptureSecondary = false;
					m_Capturing = true;
					m_Status.clear();
				}
				ImGui::EndDisabled();
				if (Binding.Kind == PulseForge::ActionBindingKind::Axis)
				{
					ImGui::Text("Secondary: %s", ControlName(Binding.Secondary).c_str());
					ImGui::SameLine();
					ImGui::BeginDisabled(!CanCaptureBinding);
					if (ImGui::SmallButton("Rebind secondary"))
					{
						m_CaptureActionId = Action.Id;
						m_CaptureBindingId = Binding.Id;
						m_CaptureSecondary = true;
						m_Capturing = true;
						m_Status.clear();
					}
					ImGui::EndDisabled();
				}
				for (uint8_t Modifier = 0; Modifier < Binding.ModifierCount && Modifier < Binding.Modifiers.size(); ++Modifier)
				{
					ImGui::SameLine();
					ImGui::Text("+ %s", ControlName(Binding.Modifiers[Modifier]).c_str());
				}
				ImGui::PopID();
				ImGui::PopID();
			}
		}
	}

	void EditorInputMappingPanel::HandleEvent(
		PulseForge::Event& Event,
		PulseForge::GameplayInputService& Input,
		const std::filesystem::path& ProjectRoot,
		const PulseForge::UUID& ProjectId,
		const std::filesystem::path& UserDataRoot,
		bool CanPersist)
	{
		if (!m_Capturing) return;

		if (Event.GetEventType() == PulseForge::EEventType::WindowLostFocus)
		{
			m_Capturing = false;
			m_CapturedInputAwaitingPublish = true;
			m_CapturedInputPublished = true;
			m_CaptureActionId.clear();
			m_CaptureBindingId.clear();
			m_CaptureSecondary = false;
			m_Status = "Binding capture canceled because the owning window lost focus.";
			Event.bHandled = true;
			return;
		}

		PulseForge::InputControl Captured;
		bool HasCapturedControl = false;
		const bool IsKeyboardOrMouse = Event.IsInCategory(PulseForge::EventCategoryKeyboard) ||
			Event.IsInCategory(PulseForge::EventCategoryMouse);
		if (!IsKeyboardOrMouse) return;
		// Capture owns keyboard and mouse input even when the event is not a bindable press. This
		// prevents the initiating click, unsupported controls, or key repeats from reaching gameplay.
		Event.bHandled = true;
		if (Event.GetEventType() == PulseForge::EEventType::KeyPressed)
		{
			const auto& Key = static_cast<const PulseForge::KeyPressedEvent&>(Event);
			if (Key.GetRepeatCount() == 0 && Key.GetKeyCode() == static_cast<int>(PulseForge::InputKey::Escape))
			{
				m_Capturing = false;
				m_CapturedInputAwaitingPublish = true;
				m_CapturedInputPublished = true;
				m_CaptureActionId.clear();
				m_CaptureBindingId.clear();
				m_CaptureSecondary = false;
				m_Status = "Binding capture canceled.";
				return;
			}
			if (Key.GetRepeatCount() == 0 && Key.GetKeyCode() > 0)
			{
				Captured = { PulseForge::InputDevice::Keyboard, static_cast<uint16_t>(Key.GetKeyCode()) };
				HasCapturedControl = !PulseForge::GetInputControlName(Captured).empty();
			}
		}
		else if (Event.GetEventType() == PulseForge::EEventType::MouseButtonPressed)
		{
			const auto& Button = static_cast<const PulseForge::MouseButtonPressedEvent&>(Event);
			if (Button.GetMouseButton() >= 0)
			{
				Captured = { PulseForge::InputDevice::MouseButton, static_cast<uint16_t>(Button.GetMouseButton()) };
				HasCapturedControl = !PulseForge::GetInputControlName(Captured).empty();
			}
		}

		if (!HasCapturedControl) return;
		m_Capturing = false;
		m_CapturedInputAwaitingPublish = true;
		m_CapturedInputPublished = true;
		if (!CanPersist)
		{
			m_Status = "Could not save the binding because user data storage is unavailable.";
			return;
		}

		const auto Action = std::find_if(Input.GetActionMap().Actions.begin(), Input.GetActionMap().Actions.end(),
			[this](const PulseForge::GameplayActionDefinition& Candidate) { return Candidate.Id == m_CaptureActionId; });
		if (Action == Input.GetActionMap().Actions.end())
		{
			m_Status = "The selected action no longer exists.";
			return;
		}
		const auto Binding = std::find_if(Action->Bindings.begin(), Action->Bindings.end(),
			[this](const PulseForge::ActionBinding& Candidate) { return Candidate.Id == m_CaptureBindingId; });
		if (Binding == Action->Bindings.end())
		{
			m_Status = "The selected binding no longer exists.";
			return;
		}

		PulseForge::ActionBinding Replacement = *Binding;
		if (m_CaptureSecondary) Replacement.Secondary = Captured;
		else Replacement.Primary = Captured;
		auto Saved = PulseForge::SaveGameplayInputBindingOverride(
			UserDataRoot, ProjectId, m_CaptureActionId, m_CaptureBindingId, Replacement);
		if (!Saved)
		{
			m_Status = "Could not save binding override: " + Saved.error().Message;
			m_CaptureActionId.clear();
			m_CaptureBindingId.clear();
			return;
		}

		auto Reloaded = PulseForge::LoadGameplayInputMap(ProjectRoot, ProjectId, UserDataRoot);
		if (!Reloaded)
		{
			m_Status = "Saved the override, but could not reload the project map: " + Reloaded.error().Message;
			m_CaptureActionId.clear();
			m_CaptureBindingId.clear();
			return;
		}
		for (const std::string& Warning : Reloaded->Warnings) PF_WARN("{}", Warning);
		if (auto Applied = Input.SetActionMap(std::move(Reloaded->ActionMap)); !Applied)
			m_Status = "Saved the override, but the active map rejected it: " + Applied.error().Message;
		else
			m_Status = std::string(m_CaptureSecondary ? "Secondary" : "Primary") + " binding updated to " + ControlName(Captured) + ".";
		m_CaptureActionId.clear();
		m_CaptureBindingId.clear();
		m_CaptureSecondary = false;
	}
}
