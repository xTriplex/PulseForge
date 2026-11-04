#include "Editor/EditorAudioPreviewController.h"

#include "Assets/AudioAssetCache.h"
#include "Audio/AudioSession.h"

#include <exception>
#include <optional>
#include <utility>

namespace PulseForgeEditor
{
	struct EditorAudioPreviewController::Impl final
	{
		PulseForge::AudioSession* Session = nullptr;
		std::optional<PulseForge::AudioPlayback> Playback;
		std::optional<PulseForge::AssetID> ActiveAsset;
		uint64_t Generation = 0;
		uint64_t ObservedDeviceGeneration = 0;
	};

	EditorAudioPreviewController::EditorAudioPreviewController()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	EditorAudioPreviewController::~EditorAudioPreviewController()
	{
		ClearSession();
	}

	void EditorAudioPreviewController::SetSession(PulseForge::AudioSession* Session) noexcept
	{
		if (m_Impl->Session == Session)
		{
			ReconcileDeviceGeneration();
			return;
		}
		Cancel();
		m_Impl->Session = Session;
		m_Impl->ObservedDeviceGeneration = Session ? Session->GetEngine().GetDeviceGeneration() : 0;
	}

	void EditorAudioPreviewController::ClearSession() noexcept
	{
		Cancel();
		m_Impl->Session = nullptr;
		m_Impl->ObservedDeviceGeneration = 0;
	}

	void EditorAudioPreviewController::ReconcileDeviceGeneration() noexcept
	{
		if (!m_Impl->Session)
		{
			m_Impl->ObservedDeviceGeneration = 0;
			return;
		}

		const uint64_t CurrentDeviceGeneration = m_Impl->Session->GetEngine().GetDeviceGeneration();
		if (CurrentDeviceGeneration == m_Impl->ObservedDeviceGeneration)
			return;

		Cancel();
		m_Impl->ObservedDeviceGeneration = CurrentDeviceGeneration;
	}

	std::expected<void, std::string> EditorAudioPreviewController::Play(
		const PulseForge::AssetID& Asset,
		PulseForge::AudioBus Bus)
	{
		if (!m_Impl->Session)
			return std::unexpected("Open a project with an active audio session before previewing an audio asset");

		const auto RestorePreviousPlayback = [this]() -> std::optional<std::string>
		{
			if (!m_Impl->Playback) return std::nullopt;
			try
			{
				auto Resumed = m_Impl->Playback->Resume();
				if (!Resumed) return Resumed.error().Message;
				return std::nullopt;
			}
			catch (const std::exception& Exception)
			{
				return std::string("prior preview resume threw: ") + Exception.what();
			}
			catch (...)
			{
				return "prior preview resume failed unexpectedly";
			}
		};
		bool PreviousPlaybackMayBePaused = false;

		try
		{
			auto Clip = m_Impl->Session->GetAssets().GetOrLoad(Asset);
			if (!Clip) return std::unexpected("Could not load audio preview asset: " + Clip.error().Message);

			PulseForge::AudioPlaybackDesc Description;
			Description.Volume = 1.0f;
			Description.Looping = false;
			Description.Spatialized = false;
			Description.Bus = Bus;
			Description.StartPaused = true;
			auto CandidatePlayback = m_Impl->Session->GetEngine().Play(*Clip, Description);
			if (!CandidatePlayback)
				return std::unexpected("Could not prepare audio preview: " + CandidatePlayback.error().Message);

			std::optional<PulseForge::AudioPlayback> CandidateVoice;
			CandidateVoice.emplace(std::move(*CandidatePlayback));

			if (m_Impl->Playback)
			{
				PreviousPlaybackMayBePaused = true;
				auto Paused = m_Impl->Playback->Pause();
				if (!Paused)
				{
					CandidateVoice.reset();
					auto Restored = RestorePreviousPlayback();
					PreviousPlaybackMayBePaused = false;
					std::string Message = "Could not pause the active audio preview: " + Paused.error().Message;
					if (Restored) Message += "; restoring it also failed: " + *Restored;
					return std::unexpected(std::move(Message));
				}
			}

			auto Started = CandidateVoice->Resume();
			if (!Started)
			{
				const std::string StartFailure = Started.error().Message;
				(void)CandidateVoice->Stop();
				CandidateVoice.reset();
				auto Restored = RestorePreviousPlayback();
				PreviousPlaybackMayBePaused = false;
				std::string Message = "Could not start replacement audio preview: " + StartFailure;
				if (Restored) Message += "; restoring the previous preview also failed: " + *Restored;
				return std::unexpected(std::move(Message));
			}

			if (m_Impl->Playback)
			{
				auto Stopped = m_Impl->Playback->Stop();
				if (!Stopped)
				{
					const std::string StopFailure = Stopped.error().Message;
					(void)CandidateVoice->Stop();
					CandidateVoice.reset();
					auto Restored = RestorePreviousPlayback();
					PreviousPlaybackMayBePaused = false;
					std::string Message = "Could not retire the previous audio preview: " + StopFailure;
					if (Restored) Message += "; restoring it also failed: " + *Restored;
					return std::unexpected(std::move(Message));
				}
				m_Impl->Playback.reset();
			}

			m_Impl->Playback.emplace(std::move(*CandidateVoice));
			m_Impl->ActiveAsset = Asset;
			++m_Impl->Generation;
			return {};
		}
		catch (const std::exception& Exception)
		{
			if (PreviousPlaybackMayBePaused)
				(void)RestorePreviousPlayback();
			return std::unexpected(std::string("Could not prepare audio preview: ") + Exception.what());
		}
		catch (...)
		{
			if (PreviousPlaybackMayBePaused)
				(void)RestorePreviousPlayback();
			return std::unexpected("Could not prepare audio preview");
		}
	}

	std::expected<void, std::string> EditorAudioPreviewController::Stop()
	{
		if (m_Impl->Playback)
		{
			auto Stopped = m_Impl->Playback->Stop();
			if (!Stopped) return std::unexpected("Could not stop audio preview: " + Stopped.error().Message);
			m_Impl->Playback.reset();
		}
		m_Impl->ActiveAsset.reset();
		++m_Impl->Generation;
		return {};
	}

	void EditorAudioPreviewController::Cancel() noexcept
	{
		if (m_Impl->Playback)
		{
			try
			{
				(void)m_Impl->Playback->Stop();
			}
			catch (...)
			{
			}
			m_Impl->Playback.reset();
		}
		m_Impl->ActiveAsset.reset();
		++m_Impl->Generation;
	}

	uint64_t EditorAudioPreviewController::GetGeneration() const noexcept
	{
		return m_Impl->Generation;
	}

	bool EditorAudioPreviewController::IsCurrentGeneration(uint64_t Generation) const noexcept
	{
		return Generation == m_Impl->Generation;
	}

	std::optional<PulseForge::AssetID> EditorAudioPreviewController::GetActiveAsset() const noexcept
	{
		return m_Impl->ActiveAsset;
	}

	bool EditorAudioPreviewController::IsPlaying() const noexcept
	{
		return m_Impl->Playback && m_Impl->Playback->IsPlaying();
	}
}
