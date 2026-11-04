#pragma once

#include "Assets/AssetID.h"
#include "Audio/AudioBus.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace PulseForge
{
	class AudioSession;
}

namespace PulseForgeEditor
{
	// Owns Editor-only preview playback while borrowing the active project AudioSession until ClearSession().
	class EditorAudioPreviewController final
	{
	public:
		EditorAudioPreviewController();
		~EditorAudioPreviewController();
		EditorAudioPreviewController(const EditorAudioPreviewController&) = delete;
		EditorAudioPreviewController& operator=(const EditorAudioPreviewController&) = delete;

		void SetSession(PulseForge::AudioSession* Session) noexcept;
		void ClearSession() noexcept;
		// Stops previews bound to a replaced engine/device generation. Call on the session owner thread.
		void ReconcileDeviceGeneration() noexcept;
		[[nodiscard]] std::expected<void, std::string> Play(const PulseForge::AssetID& Asset, PulseForge::AudioBus Bus);
		[[nodiscard]] std::expected<void, std::string> Stop();
		void Cancel() noexcept;
		[[nodiscard]] uint64_t GetGeneration() const noexcept;
		[[nodiscard]] bool IsCurrentGeneration(uint64_t Generation) const noexcept;
		[[nodiscard]] std::optional<PulseForge::AssetID> GetActiveAsset() const noexcept;
		[[nodiscard]] bool IsPlaying() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
