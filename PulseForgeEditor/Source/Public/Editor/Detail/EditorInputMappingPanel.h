#pragma once

#include "Input/GameplayInput.h"
#include "Scene/UUID.h"

#include <filesystem>
#include <string>

namespace PulseForge
{
	class Event;
}

namespace PulseForgeEditor::Detail
{
	class EditorInputMappingPanel final
	{
	public:
		void DrawContents(const PulseForge::GameplayActionMap& Map, bool CanPersist, const std::string& PersistenceWarning);
		void HandleEvent(
			PulseForge::Event& Event,
			PulseForge::GameplayInputService& Input,
			const std::filesystem::path& ProjectRoot,
			const PulseForge::UUID& ProjectId,
			const std::filesystem::path& UserDataRoot,
			bool CanPersist);
		void HandleInputEvent(
			const PulseForge::InputEvent& Event,
			PulseForge::GameplayInputService& Input,
			const std::filesystem::path& ProjectRoot,
			const PulseForge::UUID& ProjectId,
			const std::filesystem::path& UserDataRoot,
			bool CanPersist);
		void BeginCapture(std::string ActionId, std::string BindingId, bool Secondary = false, int ModifierIndex = -1);

		[[nodiscard]] bool IsCapturing() const noexcept { return m_Capturing; }
		[[nodiscard]] bool NeedsTextEntryRoute() const noexcept
		{
			return m_Capturing || m_CapturedInputAwaitingPublish || m_FocusedLastFrame;
		}
		[[nodiscard]] bool SuppressShortcutsThisFrame() const noexcept
		{
			return m_Capturing || m_CapturedInputAwaitingPublish || m_CapturedInputPublished || m_FocusedLastFrame;
		}
		void MarkInputPublished() noexcept { m_CapturedInputAwaitingPublish = false; }
		void FinishUiFrame() noexcept { m_CapturedInputPublished = false; }
		void CancelCapture() noexcept
		{
			m_Capturing = false;
			m_CapturedInputAwaitingPublish = false;
			m_CapturedInputPublished = false;
			m_CaptureActionId.clear();
			m_CaptureBindingId.clear();
			m_CaptureSecondary = false;
			m_CaptureModifierIndex = -1;
		}
		void MarkWindowUnfocused() noexcept { m_FocusedLastFrame = false; }

	private:
		void ApplyCapturedControl(
			PulseForge::InputControl Captured,
			PulseForge::GameplayInputService& Input,
			const std::filesystem::path& ProjectRoot,
			const PulseForge::UUID& ProjectId,
			const std::filesystem::path& UserDataRoot,
			bool CanPersist);
		void FinishCapture(std::string Status);

		std::string m_CaptureActionId;
		std::string m_CaptureBindingId;
		std::string m_Status;
		bool m_CaptureSecondary = false;
		int m_CaptureModifierIndex = -1;
		bool m_Capturing = false;
		bool m_CapturedInputAwaitingPublish = false;
		bool m_CapturedInputPublished = false;
		bool m_FocusedLastFrame = false;
	};
}
