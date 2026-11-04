#include "TestEngineSupport.h"

#include "Audio/AudioSession.h"
#include "Runtime/PlayerSession.h"
#include "Scripting/ScriptRuntime.h"

#include <chrono>
#include <cmath>
#include <limits>

namespace
{
	struct AudioSampleScriptCapture final
	{
		size_t DiagnosticCount = 0;
		size_t UIActionCount = 0;
		PulseForge::ScriptDiagnosticCode LastDiagnostic = PulseForge::ScriptDiagnosticCode::InvalidComponent;
	};

	void CaptureAudioSampleScriptObservation(
		void* Context,
		const PulseForge::ScriptRuntimeObservation& Observation) noexcept
	{
		auto& Capture = *static_cast<AudioSampleScriptCapture*>(Context);
		if (Observation.Kind == PulseForge::ScriptRuntimeObservationKind::Diagnostic)
		{
			++Capture.DiagnosticCount;
			Capture.LastDiagnostic = Observation.Diagnostic;
		}
		else if (Observation.Kind == PulseForge::ScriptRuntimeObservationKind::CallbackEntered &&
			Observation.Callback == PulseForge::ScriptRuntimeCallback::OnUIAction)
			++Capture.UIActionCount;
	}

	std::vector<std::byte> MakeSilentWave(uint32_t SampleCount)
	{
		constexpr uint32_t Channels = 1;
		constexpr uint32_t SampleRate = 44100;
		constexpr uint16_t BitsPerSample = 16;
		constexpr uint16_t BlockAlign = Channels * (BitsPerSample / 8);
		const uint32_t DataSize = SampleCount * BlockAlign;
		std::vector<std::byte> Data(44 + DataSize);

		auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		auto Write32 = [&Data](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Data[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};

		WriteFourCC(0, "RIFF");
		Write32(4, static_cast<uint32_t>(Data.size() - 8));
		WriteFourCC(8, "WAVE");
		WriteFourCC(12, "fmt ");
		Write32(16, 16);
		Write16(20, 1);
		Write16(22, Channels);
		Write32(24, SampleRate);
		Write32(28, SampleRate * BlockAlign);
		Write16(32, BlockAlign);
		Write16(34, BitsPerSample);
		WriteFourCC(36, "data");
		Write32(40, DataSize);
		return Data;
	}

	std::vector<std::byte> MakeToneWave(uint32_t SampleCount, float Amplitude, uint32_t Frequency, uint32_t SampleRate = 48000)
	{
		constexpr uint16_t Channels = 2;
		constexpr uint16_t BitsPerSample = 16;
		constexpr uint16_t BlockAlign = Channels * (BitsPerSample / 8);
		const uint32_t DataSize = SampleCount * BlockAlign;
		std::vector<std::byte> Data(44 + DataSize);

		auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		auto Write32 = [&Data](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Data[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};

		WriteFourCC(0, "RIFF");
		Write32(4, static_cast<uint32_t>(Data.size() - 8));
		WriteFourCC(8, "WAVE");
		WriteFourCC(12, "fmt ");
		Write32(16, 16);
		Write16(20, 1);
		Write16(22, Channels);
		Write32(24, SampleRate);
		Write32(28, SampleRate * BlockAlign);
		Write16(32, BlockAlign);
		Write16(34, BitsPerSample);
		WriteFourCC(36, "data");
		Write32(40, DataSize);

		constexpr double TwoPi = 6.28318530717958647692;
		for (uint32_t Frame = 0; Frame < SampleCount; ++Frame)
		{
			const double Phase = TwoPi * static_cast<double>(Frequency) * Frame / SampleRate;
			const int16_t Sample = static_cast<int16_t>(std::lround(std::sin(Phase) * Amplitude * 32767.0));
			const uint16_t Bits = static_cast<uint16_t>(Sample);
			Write16(44 + static_cast<size_t>(Frame) * BlockAlign, Bits);
			Write16(44 + static_cast<size_t>(Frame) * BlockAlign + 2, Bits);
		}
		return Data;
	}

	float MeasureRMS(std::span<const float> Samples)
	{
		if (Samples.empty())
			return 0.0f;
		double SumSquares = 0.0;
		for (const float Sample : Samples)
			SumSquares += static_cast<double>(Sample) * Sample;
		return static_cast<float>(std::sqrt(SumSquares / Samples.size()));
	}

	void TestOfflineAudioBusMixing(TestRunner& Tests)
	{
		using namespace PulseForge;
		auto CreatedEngine = AudioEngine::Create({ AudioOutputBackend::Offline });
		PF_CHECK(Tests, CreatedEngine.has_value());
		if (!CreatedEngine)
			return;
		AudioEngine Engine = std::move(*CreatedEngine);
		const std::vector<std::byte> Tone = MakeToneWave(48000 * 3, 0.4f, 750);
		const auto Clip = Engine.CreateClip("Deterministic bus mix tone", Tone);
		PF_CHECK(Tests, Clip.has_value());
		if (!Clip)
			return;
		const auto InitialMemory = Engine.GetMemoryStats();
		PF_CHECK(Tests, InitialMemory.RetainedClipCount == 1);
		PF_CHECK(Tests, InitialMemory.RetainedEncodedClipBytes == Tone.size());
		PF_CHECK(Tests, InitialMemory.LivePlaybackCount == 0);
		PF_CHECK(Tests, !InitialMemory.BackendAllocatedBytes.has_value());

		// Keep one short and one one-minute fixture to distinguish shared encoded storage from per-voice objects.
		const std::vector<std::byte> LongTone = MakeToneWave(48000 * 60, 0.15f, 220);
		const auto LongClip = Engine.CreateClip("One-minute memory fixture", LongTone);
		PF_CHECK(Tests, LongClip.has_value());
		if (!LongClip)
			return;
		const auto LongMemory = Engine.GetMemoryStats();
		PF_CHECK(Tests, LongMemory.RetainedClipCount == 2);
		PF_CHECK(Tests, LongMemory.RetainedEncodedClipBytes == Tone.size() + LongTone.size());

		std::vector<AudioPlayback> MeasuredVoices;
		uint64_t OneVoiceObjectBytes = 0;
		uint64_t OneVoiceDecoderBytes = 0;
		for (const size_t TargetCount : { 1u, 8u, 32u })
		{
			MeasuredVoices.clear();
			MeasuredVoices.reserve(TargetCount);
			AudioPlaybackDesc MeasurementDescription;
			MeasurementDescription.StartPaused = true;
			const auto Start = std::chrono::steady_clock::now();
			for (size_t Index = 0; Index < TargetCount; ++Index)
			{
				auto Voice = Engine.Play(*LongClip, MeasurementDescription);
				PF_CHECK(Tests, Voice.has_value());
				if (!Voice)
					break;
				MeasuredVoices.push_back(std::move(*Voice));
			}
			const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - Start).count();
			const auto Memory = Engine.GetMemoryStats();
			PF_CHECK(Tests, Memory.LivePlaybackCount == MeasuredVoices.size());
			PF_CHECK(Tests, Memory.RetainedClipCount == 2);
			PF_CHECK(Tests, Memory.RetainedEncodedClipBytes == Tone.size() + LongTone.size());
			PF_CHECK(Tests, Memory.KnownDecoderObjectBytes <= Memory.KnownPlaybackObjectBytes);
			if (TargetCount == 1)
			{
				OneVoiceObjectBytes = Memory.KnownPlaybackObjectBytes;
				OneVoiceDecoderBytes = Memory.KnownDecoderObjectBytes;
			}
			PF_CHECK(Tests, Memory.KnownPlaybackObjectBytes == OneVoiceObjectBytes * TargetCount);
			PF_CHECK(Tests, Memory.KnownDecoderObjectBytes == OneVoiceDecoderBytes * TargetCount);
			std::cout << "Audio sizing fixture: encoded_short_bytes=" << Tone.size()
				<< " encoded_60s_bytes=" << LongTone.size()
				<< " live_voices=" << Memory.LivePlaybackCount
				<< " known_playback_object_bytes=" << Memory.KnownPlaybackObjectBytes
				<< " known_decoder_object_bytes=" << Memory.KnownDecoderObjectBytes
				<< " create_microseconds=" << Elapsed
				<< " backend_allocation_bytes=unknown\n";
		}
		MeasuredVoices.clear();
		PF_CHECK(Tests, Engine.GetMemoryStats().LivePlaybackCount == 0);

		AudioPlaybackDesc MusicDescription;
		MusicDescription.Volume = 0.5f;
		MusicDescription.Looping = true;
		MusicDescription.Bus = AudioBus::Music;
		auto MusicPlayback = Engine.Play(*Clip, MusicDescription);
		PF_CHECK(Tests, MusicPlayback.has_value());
		if (!MusicPlayback)
			return;
		AudioPlaybackDesc EffectsDescription = MusicDescription;
		EffectsDescription.Volume = 0.25f;
		EffectsDescription.Bus = AudioBus::Effects;
		auto EffectsPlayback = Engine.Play(*Clip, EffectsDescription);
		PF_CHECK(Tests, EffectsPlayback.has_value());
		if (!EffectsPlayback)
			return;
		AudioPlaybackDesc UIDescription = MusicDescription;
		UIDescription.Volume = 0.75f;
		UIDescription.Bus = AudioBus::UI;
		auto UIPlayback = Engine.Play(*Clip, UIDescription);
		PF_CHECK(Tests, UIPlayback.has_value());
		if (!UIPlayback)
			return;

		constexpr size_t FramesPerBlock = 4096;
		std::vector<float> Samples(FramesPerBlock * 2);
		struct OfflineBlockMetrics final
		{
			float RMS = 0.0f;
			float Peak = 0.0f;
			size_t NonzeroSamples = 0;
		};
		auto RenderBlock = [&Tests, &Engine, &Samples]() -> OfflineBlockMetrics
		{
			const auto Rendered = Engine.RenderOfflineFrames(Samples);
			PF_CHECK(Tests, Rendered.has_value() && *Rendered == Samples.size() / 2);
			OfflineBlockMetrics Metrics;
			Metrics.RMS = Rendered ? MeasureRMS(Samples) : 0.0f;
			for (const float Sample : Samples)
			{
				Metrics.Peak = std::max(Metrics.Peak, std::abs(Sample));
				if (std::abs(Sample) > 1.0e-8f)
					++Metrics.NonzeroSamples;
			}
			return Metrics;
		};
		auto RenderRMS = [&RenderBlock]() { return RenderBlock().RMS; };
		auto CheckSettledSilence = [&Tests, &RenderBlock]()
		{
			// The pinned miniaudio pull graph can return a few already-mixed samples after a boundary control.
			// Bound that one block, then require exact silence under the unchanged strict RMS threshold.
			const OfflineBlockMetrics Boundary = RenderBlock();
			PF_CHECK(Tests, Boundary.NonzeroSamples <= 4 && Boundary.Peak <= 0.1f);
			const OfflineBlockMetrics Settled = RenderBlock();
			PF_CHECK(Tests, Settled.RMS < 0.00001f);
		};

		const auto InitialMusicMix = Engine.GetBusMix(AudioBus::Music);
		PF_CHECK(Tests, InitialMusicMix && InitialMusicMix->EffectiveGain == 1.0f);
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Effects, 0.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::UI, 0.0f).has_value());
		const float MusicOnlyRMS = RenderRMS();
		PF_CHECK(Tests, MusicOnlyRMS > 0.02f);

		PF_CHECK(Tests, Engine.SetMasterGain(0.5f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Music, 0.25f).has_value());
		const auto MusicMix = Engine.GetBusMix(AudioBus::Music);
		PF_CHECK(Tests, MusicMix && MusicMix->MasterGain == 0.5f && MusicMix->BusGain == 0.25f);
		PF_CHECK(Tests, MusicMix && std::abs(MusicMix->EffectiveGain - 0.125f) < 0.0001f);
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Music, 0.25f).has_value());
		const float ComposedGainRMS = RenderRMS();
		PF_CHECK(
			Tests,
			ComposedGainRMS > 0.0f && std::abs(ComposedGainRMS / MusicOnlyRMS - 0.125f) < 0.0125f);

		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Music, 0.0f).has_value());
		CheckSettledSilence();
		PF_CHECK(Tests, Engine.SetMasterGain(1.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Music, 0.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Effects, 1.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::UI, 0.0f).has_value());
		const float EffectsOnlyRMS = RenderRMS();
		PF_CHECK(Tests, EffectsOnlyRMS > 0.008f);
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Effects, 0.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::UI, 1.0f).has_value());
		const float UIOnlyRMS = RenderRMS();
		PF_CHECK(Tests, UIOnlyRMS > 0.02f);
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::UI, 0.0f).has_value());
		CheckSettledSilence();

		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Music, 1.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::Effects, 1.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusGain(AudioBus::UI, 1.0f).has_value());
		PF_CHECK(Tests, Engine.SetBusMuted(AudioBus::Music, true).has_value());
		const float OtherBusesRMS = RenderRMS();
		PF_CHECK(Tests, OtherBusesRMS > UIOnlyRMS);
		PF_CHECK(Tests, MusicPlayback->IsPlaying());
		PF_CHECK(Tests, Engine.SetBusMuted(AudioBus::Music, false).has_value());
		const float UnmutedMusicRMS = RenderRMS();
		PF_CHECK(Tests, UnmutedMusicRMS > OtherBusesRMS);

		PF_CHECK(Tests, Engine.SetMasterMuted(true).has_value());
		PF_CHECK(Tests, RenderRMS() < 0.00001f);
		PF_CHECK(Tests, Engine.SetMasterMuted(false).has_value());
		PF_CHECK(Tests, EffectsPlayback->Pause().has_value());
		PF_CHECK(Tests, UIPlayback->Pause().has_value());
		const float BeforePauseRMS = RenderRMS();
		PF_CHECK(Tests, MusicPlayback->Pause().has_value());
		CheckSettledSilence();
		PF_CHECK(Tests, MusicPlayback->Resume().has_value());
		const float AfterResumeRMS = RenderRMS();
		PF_CHECK(Tests, BeforePauseRMS > 0.02f && AfterResumeRMS > 0.02f);
		PF_CHECK(Tests, std::abs(AfterResumeRMS / BeforePauseRMS - 1.0f) < 0.08f);

		PF_CHECK(Tests, !Engine.SetMasterGain(std::numeric_limits<float>::quiet_NaN()));
		PF_CHECK(Tests, !Engine.SetBusGain(AudioBus::Effects, -0.1f));
		PF_CHECK(Tests, !Engine.SetBusGain(AudioBus::UI, std::numeric_limits<float>::infinity()));
		PF_CHECK(Tests, MusicPlayback->Stop().has_value());
		PF_CHECK(Tests, EffectsPlayback->Stop().has_value());
		PF_CHECK(Tests, UIPlayback->Stop().has_value());
		CheckSettledSilence();

		const AssetID AudioAsset{ 0x5300000000000010ull, 3 };
		AudioAssetCache Assets(Engine, [AudioAsset, Tone](const AssetID& Requested)
			-> std::expected<std::vector<std::byte>, AudioAssetError>
		{
			if (Requested != AudioAsset)
				return std::unexpected(AudioAssetError{ AudioAssetErrorCode::AssetNotFound, Requested, "Test audio asset is missing" });
			return Tone;
		});
		Scene PauseScene;
		const std::array<std::pair<const char*, AudioBus>, 3> Sources = {
			std::pair{ "Music source", AudioBus::Music },
			std::pair{ "Effects source", AudioBus::Effects },
			std::pair{ "UI source", AudioBus::UI }
		};
		std::array<UUID, 3> SourceIDs{};
		for (size_t Index = 0; Index < Sources.size(); ++Index)
		{
			const auto Entity = PauseScene.CreateEntity(Sources[Index].first);
			PF_CHECK(Tests, Entity.has_value());
			if (!Entity)
				return;
			AudioSourceComponent Component;
			Component.AudioAsset = AudioAsset;
			Component.Volume = 0.2f;
			Component.Looping = true;
			Component.PlayOnStart = true;
			Component.Spatialized = false;
			Component.Bus = Sources[Index].second;
			PF_CHECK(Tests, Entity->SetAudioSource(Component).has_value());
			SourceIDs[Index] = Entity->GetUUID();
		}
		AudioSceneRuntime PauseRuntime(Engine, Assets, 80, 21);
		PF_CHECK(Tests, PauseRuntime.Start(PauseScene, true).has_value());
		PF_CHECK(Tests, PauseRuntime.GetPlaybackCount() == 3);
		for (const UUID SourceID : SourceIDs)
		{
			const auto PreparedPlaying = PauseRuntime.IsPlaybackPlaying(SourceID);
			PF_CHECK(Tests, PreparedPlaying && !*PreparedPlaying);
		}
		PF_CHECK(Tests, RenderRMS() < 0.00001f);
		PF_CHECK(Tests, PauseRuntime.Activate().has_value());
		const float BeforeGameplayPause = RenderRMS();
		PF_CHECK(Tests, BeforeGameplayPause > 0.02f);
		PF_CHECK(Tests, PauseRuntime.SetPaused(true).has_value());
		auto MusicPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[0]);
		auto EffectsPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[1]);
		auto UIPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[2]);
		PF_CHECK(Tests, MusicPlayingWhilePaused && !*MusicPlayingWhilePaused);
		PF_CHECK(Tests, EffectsPlayingWhilePaused && !*EffectsPlayingWhilePaused);
		PF_CHECK(Tests, UIPlayingWhilePaused && *UIPlayingWhilePaused);
		const float UIPauseMixRMS = RenderRMS();
		PF_CHECK(Tests, UIPauseMixRMS > 0.003f && UIPauseMixRMS < BeforeGameplayPause);

		PF_CHECK(Tests, PauseRuntime.SetPaused(false).has_value());
		PF_CHECK(Tests, PauseRuntime.Pause(SourceIDs[0]).has_value());
		PF_CHECK(Tests, PauseRuntime.SuspendForTransition().has_value());
		MusicPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[0]);
		EffectsPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[1]);
		UIPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[2]);
		PF_CHECK(Tests, MusicPlayingWhilePaused && !*MusicPlayingWhilePaused);
		PF_CHECK(Tests, EffectsPlayingWhilePaused && !*EffectsPlayingWhilePaused);
		PF_CHECK(Tests, UIPlayingWhilePaused && !*UIPlayingWhilePaused);
		CheckSettledSilence();
		const auto PauseDuringTransition = PauseRuntime.SetPaused(true);
		PF_CHECK(Tests, !PauseDuringTransition && PauseDuringTransition.error().Code == AudioSceneRuntimeErrorCode::InvalidCommand);
		PF_CHECK(Tests, PauseRuntime.ResumeAfterTransition().has_value());
		MusicPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[0]);
		EffectsPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[1]);
		UIPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[2]);
		PF_CHECK(Tests, MusicPlayingWhilePaused && !*MusicPlayingWhilePaused);
		PF_CHECK(Tests, EffectsPlayingWhilePaused && *EffectsPlayingWhilePaused);
		PF_CHECK(Tests, UIPlayingWhilePaused && *UIPlayingWhilePaused);
		PF_CHECK(Tests, RenderRMS() > 0.005f);

		PF_CHECK(Tests, PauseRuntime.Pause(SourceIDs[2]).has_value());
		PF_CHECK(Tests, PauseRuntime.SetPaused(true).has_value());
		PF_CHECK(Tests, PauseRuntime.SetPaused(false).has_value());
		UIPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[2]);
		MusicPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[0]);
		EffectsPlayingWhilePaused = PauseRuntime.IsPlaybackPlaying(SourceIDs[1]);
		PF_CHECK(Tests, UIPlayingWhilePaused && !*UIPlayingWhilePaused);
		PF_CHECK(Tests, MusicPlayingWhilePaused && !*MusicPlayingWhilePaused);
		PF_CHECK(Tests, EffectsPlayingWhilePaused && *EffectsPlayingWhilePaused);
		PauseRuntime.Stop();
	}

	void TestAudioSchemaCompatibility(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const auto EntityResult = Source.CreateEntity("Authored music source");
		PF_CHECK(Tests, EntityResult.has_value());
		if (!EntityResult)
			return;

		AudioSourceComponent SourceComponent;
		SourceComponent.AudioAsset = AssetID{ 0x5300000000000010ull, 1 };
		SourceComponent.Volume = 0.4f;
		SourceComponent.Looping = true;
		SourceComponent.PlayOnStart = true;
		SourceComponent.Spatialized = false;
		SourceComponent.Bus = AudioBus::Music;
		PF_CHECK(Tests, EntityResult->SetAudioSource(SourceComponent).has_value());
		const auto SerializedScene = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, SerializedScene.has_value());
		if (!SerializedScene)
			return;
		PF_CHECK(Tests, SerializedScene->find("\"version\": 13") != std::string::npos);
		PF_CHECK(Tests, SerializedScene->find("\"bus\": \"music\"") != std::string::npos);

		Scene LoadedScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*SerializedScene, LoadedScene).has_value());
		const auto LoadedEntity = LoadedScene.FindEntity(EntityResult->GetUUID());
		PF_CHECK(Tests, LoadedEntity.has_value());
		if (!LoadedEntity)
			return;
		const auto LoadedSource = LoadedEntity->GetAudioSource();
		PF_CHECK(Tests, LoadedSource && LoadedSource->has_value() && LoadedSource->value().Bus == AudioBus::Music);

		nlohmann::json LegacySceneDocument = nlohmann::json::parse(*SerializedScene);
		LegacySceneDocument["version"] = 12;
		bool RemovedBusField = false;
		for (auto& Record : LegacySceneDocument["entities"])
		{
			if (Record.value("uuid", std::string{}) != EntityResult->GetUUID().ToString())
				continue;
			auto AudioRecord = Record.find("audioSource");
			RemovedBusField = AudioRecord != Record.end() && AudioRecord->erase("bus") == 1;
			break;
		}
		PF_CHECK(Tests, RemovedBusField);
		Scene LegacyLoadedScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(LegacySceneDocument.dump(2), LegacyLoadedScene).has_value());
		const auto LegacyEntity = LegacyLoadedScene.FindEntity(EntityResult->GetUUID());
		PF_CHECK(Tests, LegacyEntity.has_value());
		if (LegacyEntity)
		{
			const auto LegacySource = LegacyEntity->GetAudioSource();
			PF_CHECK(Tests, LegacySource && LegacySource->has_value() && LegacySource->value().Bus == AudioBus::Effects);
		}

		const auto LegacyProject = ProjectSerializer::Deserialize(
			"{\"format\":\"PulseForgeProject\",\"version\":1,\"projectId\":\"b0530000-0000-4000-8000-000000000001\",\"name\":\"Legacy audio project\"}");
		PF_CHECK(Tests, LegacyProject.has_value());
		if (LegacyProject)
		{
			PF_CHECK(Tests, LegacyProject->AudioMixDefaults.Master == 1.0f);
			PF_CHECK(Tests, LegacyProject->AudioMixDefaults.Music == 1.0f);
			PF_CHECK(Tests, LegacyProject->AudioMixDefaults.Effects == 1.0f);
			PF_CHECK(Tests, LegacyProject->AudioMixDefaults.UI == 1.0f);
			const auto UpgradedProject = ProjectSerializer::Serialize(*LegacyProject);
			PF_CHECK(Tests, UpgradedProject && UpgradedProject->find("\"version\": 2") != std::string::npos);
		}

		ProjectDesc ProjectDescription;
		ProjectDescription.ID = UUID{ 0x5300000000000010ull, 2 };
		ProjectDescription.Name = "Audio mix defaults";
		ProjectDescription.AudioMixDefaults = { .Master = 0.8f, .Music = 0.7f, .Effects = 0.6f, .UI = 0.5f };
		const auto SerializedProject = ProjectSerializer::Serialize(ProjectDescription);
		PF_CHECK(Tests, SerializedProject.has_value());
		if (SerializedProject)
		{
			const auto LoadedProject = ProjectSerializer::Deserialize(*SerializedProject);
			PF_CHECK(Tests, LoadedProject && LoadedProject->AudioMixDefaults.Master == 0.8f &&
						LoadedProject->AudioMixDefaults.Music == 0.7f && LoadedProject->AudioMixDefaults.Effects == 0.6f &&
						LoadedProject->AudioMixDefaults.UI == 0.5f);
		}
	}

	void TestAudioSessionSettings(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID)
			return;
		std::error_code Error;
		const std::filesystem::path Temp = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error)
			return;
		const std::filesystem::path Root = Temp / std::filesystem::u8path("PulseForge Audio Session café Ω " + ID->ToString());
		const std::filesystem::path ProjectDirectory = Root / "project root";
		const std::filesystem::path UserData = Root / std::filesystem::u8path("user data 演習");
		const std::filesystem::path ProjectFile = ProjectDirectory / std::filesystem::u8path("Audio Session project.pfproj");
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Root, Error);
			}
		} CleanupRoot{ Root };
		std::filesystem::create_directories(ProjectDirectory, Error);
		PF_CHECK(Tests, !Error);
		if (Error)
			return;

		auto ProjectResult = Project::Create(ProjectFile, "Audio settings project");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult)
			return;
		Project AudioProject = std::move(*ProjectResult);
		const AudioMixGains Defaults{ .Master = 0.9f, .Music = 0.8f, .Effects = 0.7f, .UI = 0.6f };
		PF_CHECK(Tests, AudioProject.SetAudioMixDefaults(Defaults).has_value());
		PF_CHECK(Tests, AudioProject.Save().has_value());
		auto ReadText = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
				return std::nullopt;
			return std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
		};
		const auto ProjectBytesBefore = ReadText(ProjectFile);
		PF_CHECK(Tests, ProjectBytesBefore.has_value());
		if (!ProjectBytesBefore)
			return;

		AudioSessionDesc Description;
		Description.Engine.OutputBackend = AudioOutputBackend::Offline;
		Description.UserDataDirectory = UserData;
		auto FirstSessionResult = AudioSession::CreateForProject(AudioProject, Description);
		PF_CHECK(Tests, FirstSessionResult.has_value());
		if (!FirstSessionResult)
			return;
		auto FirstSession = std::move(*FirstSessionResult);
		PF_CHECK(Tests, FirstSession->GetDeviceState() == AudioDeviceState::OfflineTest);
		PF_CHECK(Tests, FirstSession->GetSettingsDiagnostic().empty());
		PF_CHECK(Tests, !FirstSession->HasUnsavedUserSettings());
		PF_CHECK(Tests, FirstSession->GetMixState().Gains.Music == Defaults.Music);
		PF_CHECK(Tests, FirstSession->SetUserMasterGain(0.55f).has_value());
		PF_CHECK(Tests, FirstSession->SetUserBusGain(AudioBus::Music, 0.35f).has_value());
		PF_CHECK(Tests, FirstSession->SetBusMuted(AudioBus::UI, true).has_value());
		PF_CHECK(Tests, FirstSession->HasUnsavedUserSettings());
		PF_CHECK(Tests, FirstSession->SaveUserSettings().has_value());
		PF_CHECK(Tests, !FirstSession->HasUnsavedUserSettings());
		const std::filesystem::path SettingsPath = FirstSession->GetSettingsPath();
		PF_CHECK(Tests, SettingsPath.is_absolute());
		PF_CHECK(Tests, SettingsPath.parent_path() == UserData / "PulseForge" / "AudioSettings");
		PF_CHECK(Tests, SettingsPath.filename() == std::filesystem::path(AudioProject.GetDescription().ID.ToString() + ".json"));
		PF_CHECK(Tests, std::filesystem::exists(SettingsPath));
		FirstSession.reset();

		auto ReopenedSessionResult = AudioSession::CreateForProject(AudioProject, Description);
		PF_CHECK(Tests, ReopenedSessionResult.has_value());
		if (!ReopenedSessionResult)
			return;
		auto ReopenedSession = std::move(*ReopenedSessionResult);
		const AudioMixState Restored = ReopenedSession->GetMixState();
		PF_CHECK(Tests, Restored.Gains.Master == 0.55f && Restored.Gains.Music == 0.35f && Restored.UIMuted);
		PF_CHECK(Tests, !ReopenedSession->HasUnsavedUserSettings());
		PF_CHECK(Tests, ProjectBytesBefore == ReadText(ProjectFile));

		// Inject default-output availability while keeping output synthetic and deterministic.
		bool DefaultOutputAvailable = false;
		AudioSessionDesc RecoveryDescription;
		RecoveryDescription.Engine.OutputBackend = AudioOutputBackend::Default;
		RecoveryDescription.UserDataDirectory = Root / "injected recovery settings";
		RecoveryDescription.EngineFactory = [&DefaultOutputAvailable](AudioOutputBackend Backend)
			-> std::expected<AudioEngine, AudioError>
		{
			if (Backend == AudioOutputBackend::Default && !DefaultOutputAvailable)
				return std::unexpected(AudioError{ AudioErrorCode::DeviceInitializationFailed, "injected default output unavailable" });
			return AudioEngine::Create({ Backend == AudioOutputBackend::Default ? AudioOutputBackend::Offline : Backend });
		};
		auto UnavailableSessionResult = AudioSession::CreateForProject(AudioProject, RecoveryDescription);
		PF_CHECK(Tests, UnavailableSessionResult.has_value());
		if (UnavailableSessionResult)
		{
			auto UnavailableSession = std::move(*UnavailableSessionResult);
			PF_CHECK(Tests, UnavailableSession->GetDeviceState() == AudioDeviceState::Unavailable);
			PF_CHECK(Tests, !UnavailableSession->GetDeviceDiagnostic().empty());
			PF_CHECK(Tests, UnavailableSession->SetUserMasterGain(0.42f).has_value());
			const auto FailedRetry = UnavailableSession->RetryDefaultOutput();
			PF_CHECK(Tests, !FailedRetry);
			PF_CHECK(Tests, UnavailableSession->GetDeviceState() == AudioDeviceState::RecoveryFailed);
			PF_CHECK(Tests, UnavailableSession->GetMixState().Gains.Master == 0.42f);
			PF_CHECK(Tests, UnavailableSession->HasUnsavedUserSettings());
			DefaultOutputAvailable = true;
			const auto SuccessfulSyntheticRetry = UnavailableSession->RetryDefaultOutput();
			PF_CHECK(Tests, SuccessfulSyntheticRetry.has_value());
			PF_CHECK(Tests, UnavailableSession->GetDeviceState() == AudioDeviceState::DefaultActive);
			PF_CHECK(Tests, UnavailableSession->GetMixState().Gains.Master == 0.42f);
			PF_CHECK(Tests, UnavailableSession->HasUnsavedUserSettings());
		}

		auto OtherProjectResult = Project::Create(ProjectDirectory / "AnotherProject.pfproj", "Independent audio settings project");
		PF_CHECK(Tests, OtherProjectResult.has_value());
		if (!OtherProjectResult)
			return;
		Project OtherProject = std::move(*OtherProjectResult);
		const AudioMixGains OtherDefaults{ .Master = 1.0f, .Music = 0.2f, .Effects = 0.4f, .UI = 0.8f };
		PF_CHECK(Tests, OtherProject.SetAudioMixDefaults(OtherDefaults).has_value());
		PF_CHECK(Tests, OtherProject.Save().has_value());
		auto OtherSessionResult = AudioSession::CreateForProject(OtherProject, Description);
		PF_CHECK(Tests, OtherSessionResult.has_value());
		if (OtherSessionResult)
		{
			PF_CHECK(Tests, (*OtherSessionResult)->GetProjectID() != ReopenedSession->GetProjectID());
			PF_CHECK(Tests, (*OtherSessionResult)->GetSettingsPath() != ReopenedSession->GetSettingsPath());
			PF_CHECK(Tests, (*OtherSessionResult)->GetMixState().Gains.Music == OtherDefaults.Music);
		}

		std::ofstream InvalidSettings(SettingsPath, std::ios::binary | std::ios::trunc);
		PF_CHECK(Tests, InvalidSettings.is_open());
		InvalidSettings << "{\"version\":999}";
		InvalidSettings.close();
		const auto InvalidBytes = ReadText(SettingsPath);
		auto InvalidSettingsSessionResult = AudioSession::CreateForProject(AudioProject, Description);
		PF_CHECK(Tests, InvalidSettingsSessionResult.has_value());
		if (InvalidSettingsSessionResult)
		{
			const auto& InvalidSettingsSession = *InvalidSettingsSessionResult;
			PF_CHECK(Tests, !InvalidSettingsSession->GetSettingsDiagnostic().empty());
			PF_CHECK(Tests, InvalidSettingsSession->GetMixState().Gains.Music == Defaults.Music);
			PF_CHECK(Tests, InvalidBytes == ReadText(SettingsPath));
		}

		const std::filesystem::path BlockedUserData = Root / "not-a-directory";
		std::ofstream Blocker(BlockedUserData, std::ios::binary);
		Blocker << "preserve this file";
		Blocker.close();
		AudioSessionDesc FailedSaveDescription = Description;
		FailedSaveDescription.UserDataDirectory = BlockedUserData;
		auto FailedSaveSessionResult = AudioSession::CreateForProject(AudioProject, FailedSaveDescription);
		PF_CHECK(Tests, FailedSaveSessionResult.has_value());
		if (FailedSaveSessionResult)
		{
			auto FailedSaveSession = std::move(*FailedSaveSessionResult);
			PF_CHECK(Tests, FailedSaveSession->SetUserMasterGain(0.4f).has_value());
			const auto FailedSave = FailedSaveSession->SaveUserSettings();
			PF_CHECK(Tests, !FailedSave);
			PF_CHECK(Tests, FailedSaveSession->HasUnsavedUserSettings());
			PF_CHECK(Tests, ReadText(BlockedUserData) == std::optional<std::string>{ "preserve this file" });

			std::filesystem::remove(BlockedUserData, Error);
			PF_CHECK(Tests, !Error);
			if (!Error)
			{
				const auto RetriedSave = FailedSaveSession->SaveUserSettings();
				PF_CHECK(Tests, RetriedSave.has_value());
				PF_CHECK(Tests, !FailedSaveSession->HasUnsavedUserSettings());
				const bool SettingsWerePublished = std::filesystem::exists(
					BlockedUserData / "PulseForge" / "AudioSettings" /
						(AudioProject.GetDescription().ID.ToString() + ".json"), Error);
				PF_CHECK(Tests, !Error && SettingsWerePublished);
				if (RetriedSave && SettingsWerePublished)
				{
					auto RestoredSessionResult = AudioSession::CreateForProject(AudioProject, FailedSaveDescription);
					PF_CHECK(Tests, RestoredSessionResult.has_value());
					if (RestoredSessionResult)
						PF_CHECK(Tests, std::abs((*RestoredSessionResult)->GetMixState().Gains.Master - 0.4f) < 0.001f);
				}
			}
		}

		AudioSessionDesc NullDescription = Description;
		NullDescription.Engine.OutputBackend = AudioOutputBackend::Null;
		const std::filesystem::path NullUserData = Root / "null settings";
		NullDescription.UserDataDirectory = NullUserData;
		auto NullSessionResult = AudioSession::CreateForProject(AudioProject, NullDescription);
		PF_CHECK(Tests, NullSessionResult.has_value());
		if (NullSessionResult)
		{
			PF_CHECK(Tests, (*NullSessionResult)->GetDeviceState() == AudioDeviceState::ExplicitNull);
			PF_CHECK(Tests, !(*NullSessionResult)->RetryDefaultOutput());
			PF_CHECK(Tests, (*NullSessionResult)->GetDeviceState() == AudioDeviceState::ExplicitNull);
		}
	}

	void TestAudioMixingSamplePlayerSession(TestRunner& Tests)
	{
#if defined(PF_AUDIO_MIXING_SAMPLE_PROJECT)
		using namespace PulseForge;
		const auto ProjectID = UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID)
			return;

		const std::filesystem::path SampleProjectFile(PF_AUDIO_MIXING_SAMPLE_PROJECT);
		std::error_code Error;
		const std::filesystem::path TemporaryRoot = std::filesystem::temp_directory_path(Error) /
			std::filesystem::u8path("PulseForge-AudioMixing-Player-" + ProjectID->ToString());
		PF_CHECK(Tests, !Error);
		if (Error)
			return;
		const bool RootAlreadyExists = std::filesystem::exists(TemporaryRoot, Error);
		PF_CHECK(Tests, !Error && !RootAlreadyExists);
		if (Error || RootAlreadyExists)
			return;

		struct TemporaryProjectCleanup final
		{
			std::filesystem::path Root;
			~TemporaryProjectCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Root, CleanupError);
			}
		} Cleanup{ TemporaryRoot };

		std::filesystem::copy(SampleProjectFile.parent_path(), TemporaryRoot,
			std::filesystem::copy_options::recursive, Error);
		PF_CHECK(Tests, !Error);
		if (Error)
			return;

		const std::filesystem::path ProjectFile = TemporaryRoot / SampleProjectFile.filename();
		std::ifstream ProjectInput(ProjectFile, std::ios::binary);
		PF_CHECK(Tests, ProjectInput.is_open());
		if (!ProjectInput.is_open())
			return;
		const std::string ProjectBytes(
			std::istreambuf_iterator<char>{ ProjectInput }, std::istreambuf_iterator<char>{});
		ProjectInput.close();
		auto ProjectJson = nlohmann::json::parse(ProjectBytes, nullptr, false);
		PF_CHECK(Tests, !ProjectJson.is_discarded() && ProjectJson.is_object());
		if (ProjectJson.is_discarded() || !ProjectJson.is_object())
			return;
		ProjectJson["projectId"] = ProjectID->ToString();
		std::ofstream ProjectOutput(ProjectFile, std::ios::binary | std::ios::trunc);
		PF_CHECK(Tests, ProjectOutput.is_open());
		if (!ProjectOutput.is_open())
			return;
		ProjectOutput << ProjectJson.dump(2) << '\n';
		ProjectOutput.close();
		PF_CHECK(Tests, ProjectOutput.good());
		if (!ProjectOutput.good())
			return;

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Offline;
		AudioSampleScriptCapture ScriptCapture;
		SceneRuntimeServices RuntimeServices;
		RuntimeServices.ScriptObserver = { &ScriptCapture, CaptureAudioSampleScriptObservation };
		PlayerSession Session(ProjectFile, RuntimeDescription, RuntimeServices);
		const auto Started = Session.Start();
		PF_CHECK(Tests, Started.has_value());
		if (!Started || !Session.IsRunning())
			return;
		PF_CHECK(Tests, Session.HasRuntimeUI());
		PF_CHECK(Tests, Session.GetRuntimeUIInputPolicy() == RuntimeUIInputPolicy::Modal);
		AudioSession* Audio = Session.GetAudioSession();
		PF_CHECK(Tests, Audio != nullptr);
		if (!Audio)
			return;
		PF_CHECK(Tests, Audio->GetProjectID() == *ProjectID);
		PF_CHECK(Tests, Audio->GetDeviceState() == AudioDeviceState::OfflineTest);
		PF_CHECK(Tests, std::abs(Audio->GetMixState().Gains.Music - 0.8f) < 0.001f);
		PF_CHECK(Tests, !Audio->HasUnsavedUserSettings());

		RuntimeUIInputAdapter Adapter;
		const RuntimeUIViewport Viewport{ { 960.0f, 540.0f }, 960, 540 };
		auto SubmitFrame = [&](std::optional<glm::vec2> Pointer, bool Pressed, bool Released)
		{
			RuntimeUIHostInputSample Sample;
			Sample.SessionGeneration = Session.GetRuntimeUISessionGeneration();
			Sample.TreeGeneration = Session.GetRuntimeUIInputGeneration();
			Sample.InputOwner = 0xA17u;
			Sample.Viewport = Viewport;
			Sample.DeltaSeconds = 1.0 / 60.0;
			Sample.Focused = true;
			Sample.PointerPosition = Pointer;
			Sample.PointerPressed = Pressed;
			Sample.PointerHeld = Pressed;
			Sample.PointerReleased = Released;
			RuntimeUIHostFrame Frame = BuildRuntimeUIHostFrame(Adapter, std::move(Sample), Session.GetRuntimeUIInputPolicy());
			const bool Submitted = Session.SubmitRuntimeUIFrame(Frame.Viewport, Frame.Events);
			PF_CHECK(Tests, Submitted);
			if (!Submitted)
				return false;
			const auto Advanced = Session.Advance(Timestep(1.0 / 60.0));
			PF_CHECK(Tests, Advanced.has_value());
			return Advanced.has_value();
		};
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, ScriptCapture.DiagnosticCount == 0);
		auto FindButtonCenter = [&](std::string_view Label) -> std::optional<glm::vec2>
		{
			const auto Commands = Session.GetRuntimeUIDrawCommands();
			const auto Button = std::find_if(Commands.begin(), Commands.end(), [Label](const RuntimeUIDrawCommand& Command)
			{
				return Command.Kind == RuntimeUIElementKind::Button && Command.Text == Label;
			});
			PF_CHECK(Tests, Button != Commands.end());
			if (Button == Commands.end())
				return std::nullopt;
			return glm::vec2{ (Button->Bounds.Left + Button->Bounds.Right) * 0.5f,
				(Button->Bounds.Top + Button->Bounds.Bottom) * 0.5f };
		};
		auto ClickButton = [&](std::string_view Label)
		{
			const std::optional<glm::vec2> Position = FindButtonCenter(Label);
			if (!Position)
				return false;
			return SubmitFrame(Position, true, false) && SubmitFrame(Position, false, true);
		};
		auto FindText = [&](std::string_view Text)
		{
			const auto Commands = Session.GetRuntimeUIDrawCommands();
			return std::any_of(Commands.begin(), Commands.end(), [Text](const RuntimeUIDrawCommand& Command)
			{ return Command.Kind == RuntimeUIElementKind::Text && Command.Text == Text; });
		};
		PF_CHECK(Tests, FindText("Music: 80%"));
		PF_CHECK(Tests, FindText("Audio output: offline_test"));
		const size_t UIActionsBeforeRetry = ScriptCapture.UIActionCount;
		PF_CHECK(Tests, ClickButton("Retry output"));
		PF_CHECK(Tests, ScriptCapture.UIActionCount == UIActionsBeforeRetry + 1);
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, FindText("Audio output: offline_test (retry unavailable)"));
		PF_CHECK(Tests, ScriptCapture.DiagnosticCount == 0);
		PF_CHECK(Tests, ClickButton("Music +"));
		PF_CHECK(Tests, std::abs(Audio->GetMixState().Gains.Music - 0.9f) < 0.001f);
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, FindText("Music: 90%"));

		PF_CHECK(Tests, ClickButton("Pause gameplay"));
		PF_CHECK(Tests, Session.IsPaused());
		PF_CHECK(Tests, ClickButton("Music +"));
		PF_CHECK(Tests, std::abs(Audio->GetMixState().Gains.Music - 1.0f) < 0.001f);
		PF_CHECK(Tests, ClickButton("Resume"));
		PF_CHECK(Tests, !Session.IsPaused());
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, SubmitFrame(std::nullopt, false, false));
		PF_CHECK(Tests, FindText("Music: 100%"));
		Session.Stop();
#else
		(void)Tests;
#endif
	}

	void TestAudioSceneCommandIdentity(
		TestRunner& Tests,
		PulseForge::AudioEngine& Engine,
		PulseForge::AudioAssetCache& Assets,
		PulseForge::AssetID AudioAsset)
	{
		using namespace PulseForge;
		Scene Source;
		const UUID EntityID{ 0x5300000000000000ull, 0x91 };
		const auto OriginalResult = Source.CreateEntityWithUUID(EntityID, "Queued audio source");
		PF_CHECK(Tests, OriginalResult.has_value());
		if (!OriginalResult)
			return;
		Entity Original = *OriginalResult;
		AudioSourceComponent SourceComponent;
		SourceComponent.AudioAsset = AudioAsset;
		SourceComponent.Looping = true;
		SourceComponent.Spatialized = false;
		SourceComponent.Bus = AudioBus::Music;
		PF_CHECK(Tests, Original.SetAudioSource(SourceComponent).has_value());

		AudioSceneRuntime Runtime(Engine, Assets, 71, 19);
		PF_CHECK(Tests, Runtime.Start(Source, true).has_value());
		PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);
		auto PreparedPlaying = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, PreparedPlaying && !*PreparedPlaying);
		const auto CandidateControl = Runtime.QueueControl(Original, AudioControlOperation::Pause);
		PF_CHECK(Tests, CandidateControl.has_value());
		PF_CHECK(Tests, Runtime.Activate().has_value());
		auto ActivatedPlaying = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, ActivatedPlaying && *ActivatedPlaying);
		PF_CHECK(Tests, Runtime.ProcessPendingCommands().has_value());
		ActivatedPlaying = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, ActivatedPlaying && !*ActivatedPlaying);

		// A queued control belongs to the original entity incarnation, not a later entity reusing its UUID.
		PF_CHECK(Tests, Runtime.QueueControl(Original, AudioControlOperation::Pause).has_value());
		PF_CHECK(Tests, Source.DestroyEntity(Original).has_value());
		const auto ReplacementResult = Source.CreateEntityWithUUID(EntityID, "Replacement audio source");
		PF_CHECK(Tests, ReplacementResult.has_value());
		if (!ReplacementResult)
			return;
		Entity Replacement = *ReplacementResult;
		PF_CHECK(Tests, Replacement.SetAudioSource(SourceComponent).has_value());
		PF_CHECK(Tests, Runtime.Advance(Source).has_value());
		PF_CHECK(Tests, Runtime.Play(EntityID).has_value());
		PF_CHECK(Tests, Runtime.ProcessPendingCommands().has_value());
		auto ReplacementPlaying = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, ReplacementPlaying && *ReplacementPlaying);

		// A command queued for a retired voice must not pause a new voice on the same live entity.
		PF_CHECK(Tests, Runtime.QueueControl(Replacement, AudioControlOperation::Pause).has_value());
		PF_CHECK(Tests, Runtime.StopPlayback(EntityID).has_value());
		PF_CHECK(Tests, Runtime.Play(EntityID).has_value());
		PF_CHECK(Tests, Runtime.ProcessPendingCommands().has_value());
		auto ReplacedVoicePlaying = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, ReplacedVoicePlaying && *ReplacedVoicePlaying);

		// Commands are bounded and an overflow is explicit; draining the accepted queue preserves later use.
		for (size_t Index = 0; Index < 256; ++Index)
			PF_CHECK(Tests, Runtime.QueueControl(Replacement, AudioControlOperation::Pause).has_value());
		const auto Overflow = Runtime.QueueControl(Replacement, AudioControlOperation::Pause);
		PF_CHECK(Tests, !Overflow && Overflow.error().Code == AudioSceneRuntimeErrorCode::CommandQueueFull);
		PF_CHECK(Tests, Runtime.ProcessPendingCommands().has_value());
		auto QueuePaused = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, QueuePaused && !*QueuePaused);
		PF_CHECK(Tests, Runtime.QueueControl(Replacement, AudioControlOperation::Resume).has_value());
		PF_CHECK(Tests, Runtime.ProcessPendingCommands().has_value());
		auto QueueResumed = Runtime.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, QueueResumed && *QueueResumed);

		PF_CHECK(Tests, Runtime.QueueControl(Replacement, AudioControlOperation::Pause).has_value());
		Runtime.Stop();
		AudioSceneRuntime NewSession(Engine, Assets, 72, 20);
		PF_CHECK(Tests, NewSession.Start(Source).has_value());
		auto NewSessionPlaying = NewSession.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, NewSessionPlaying && *NewSessionPlaying);
		PF_CHECK(Tests, !Runtime.ProcessPendingCommands());
		PF_CHECK(Tests, NewSession.ProcessPendingCommands().has_value());
		NewSessionPlaying = NewSession.IsPlaybackPlaying(EntityID);
		PF_CHECK(Tests, NewSessionPlaying && *NewSessionPlaying);
		NewSession.Stop();
	}

	void TestAudioEngineAndAssetCache(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAudio-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Audio", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		auto AudioProject = Project::Create(ProjectRoot / "AudioProject.pfproj", "Audio Script Tests");
		PF_CHECK(Tests, AudioProject.has_value());
		if (!AudioProject) return;

		const std::vector<std::byte> WaveData = MakeSilentWave(44100 * 5);
		AudioPlayback Playback;
		{
			auto CreatedEngine = AudioEngine::Create({ AudioOutputBackend::Null });
			PF_CHECK(Tests, CreatedEngine.has_value());
			if (!CreatedEngine) return;
			AudioEngine Engine = std::move(*CreatedEngine);
			PF_CHECK(Tests, Engine.IsInitialized());
			std::array<float, 2> NullOutput{};
			PF_CHECK(Tests, !Engine.RenderOfflineFrames(NullOutput));

			const std::array<std::byte, 4> InvalidData{};
			const auto InvalidClip = Engine.CreateClip("Invalid", InvalidData);
			PF_CHECK(Tests, !InvalidClip && InvalidClip.error().Code == AudioErrorCode::InvalidClip);
			const auto Clip = Engine.CreateClip("Test WAV", WaveData);
			PF_CHECK(Tests, Clip.has_value());
			if (!Clip) return;
			PF_CHECK(Tests, Clip->IsValid() && Clip->GetDebugName() == "Test WAV");

			AudioPlaybackDesc InvalidDescription;
			InvalidDescription.Volume = 1.1f;
			PF_CHECK(Tests, !InvalidDescription.Validate());
			PF_CHECK(Tests, !Engine.Play(*Clip, InvalidDescription));
			AudioPlaybackDesc Description;
			Description.Spatialized = true;
			Description.Position = { 2.0f, 0.0f, -1.0f };
			Description.Volume = 0.5f;
			auto StartedPlayback = Engine.Play(*Clip, Description);
			PF_CHECK(Tests, StartedPlayback.has_value());
			if (!StartedPlayback) return;
			Playback = std::move(*StartedPlayback);
			PF_CHECK(Tests, Playback.IsValid());
			auto ConcurrentPlayback = Engine.Play(*Clip, Description);
			PF_CHECK(Tests, ConcurrentPlayback.has_value() && ConcurrentPlayback->IsValid());
			if (ConcurrentPlayback) PF_CHECK(Tests, ConcurrentPlayback->Stop().has_value());
			PF_CHECK(Tests, Playback.Pause().has_value());
			PF_CHECK(Tests, Playback.Resume().has_value());
			PF_CHECK(Tests, Playback.SetVolume(0.25f).has_value());
			PF_CHECK(Tests, Playback.SetLooping(true).has_value());
			PF_CHECK(Tests, Playback.SetSpatialized(false).has_value());
			PF_CHECK(Tests, Playback.SetPosition({ 0.0f, 1.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Playback.Stop().has_value());
			PF_CHECK(Tests, Engine.SetListenerPosition({ 0.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Engine.SetListenerDirection({ 0.0f, 0.0f, -1.0f }).has_value());
			PF_CHECK(Tests, !Engine.SetListenerDirection({ 0.0f, 0.0f, 0.0f }));

			AssetRegistry& Registry = AudioProject->GetAssetRegistry();
			auto Imported = AssetOperations::CreateAssetFromBytes(Registry, ProjectRoot, WaveData, "Assets/Audio/test.WAV");
			PF_CHECK(Tests, Imported.has_value());
			if (!Imported) return;

			AudioAssetCache Cache(Engine, ProjectRoot, Registry);
			const auto Loaded = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, Loaded && Loaded->IsValid());
			PF_CHECK(Tests, Cache.GetLoadedCount() == 1);
			const auto CachedAgain = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, CachedAgain && Cache.GetLoadedCount() == 1);

			const auto Moved = AssetOperations::Move(Registry, ProjectRoot, Imported->ID, "Assets/Audio/renamed.WAV");
			PF_CHECK(Tests, Moved && Moved->ID == Imported->ID);
			Cache.Clear();
			const auto LoadedAfterMove = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, LoadedAfterMove && LoadedAfterMove->IsValid());
			const auto MovedRecord = Registry.Find(Imported->ID);
			PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == "Assets/Audio/renamed.WAV");
			TestAudioSceneCommandIdentity(Tests, Engine, Cache, Imported->ID);

			Scene AudioScene;
			auto EmitterResult = AudioScene.CreateEntity("Audio emitter and listener");
			PF_CHECK(Tests, EmitterResult.has_value());
			if (!EmitterResult) return;
			Entity Emitter = *EmitterResult;
			PF_CHECK(Tests, Emitter.SetAudioSource(AudioSourceComponent{ Imported->ID, 0.5f, true, true, true }).has_value());
			PF_CHECK(Tests, Emitter.SetAudioListener(AudioListenerComponent{}).has_value());
			auto SecondEmitterResult = AudioScene.CreateEntity("Second audio emitter");
			PF_CHECK(Tests, SecondEmitterResult.has_value());
			if (!SecondEmitterResult) return;
			Entity SecondEmitter = *SecondEmitterResult;
			PF_CHECK(Tests, SecondEmitter.SetAudioSource(AudioSourceComponent{ Imported->ID, 0.5f, true, true, false }).has_value());

			AudioSceneRuntime Runtime(Engine, Cache);
			PF_CHECK(Tests, Runtime.Start(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetPlaybackCount() == 2);
			auto CheckPlaybackState = [&Tests, &Runtime](UUID Identifier, bool Expected)
			{
				const auto Playing = Runtime.IsPlaybackPlaying(Identifier);
				PF_CHECK(Tests, Playing.has_value());
				if (Playing) PF_CHECK(Tests, *Playing == Expected);
			};
			CheckPlaybackState(Emitter.GetUUID(), true);
			CheckPlaybackState(SecondEmitter.GetUUID(), true);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			Scene OtherAudioScene;
			PF_CHECK(Tests, !Runtime.Advance(OtherAudioScene));
			PF_CHECK(Tests, Runtime.Pause(Emitter.GetUUID()).has_value());
			CheckPlaybackState(Emitter.GetUUID(), false);
			PF_CHECK(Tests, Runtime.SetPaused(true).has_value());
			CheckPlaybackState(Emitter.GetUUID(), false);
			CheckPlaybackState(SecondEmitter.GetUUID(), false);
			PF_CHECK(Tests, Runtime.SetPaused(false).has_value());
			CheckPlaybackState(Emitter.GetUUID(), false);
			CheckPlaybackState(SecondEmitter.GetUUID(), true);
			PF_CHECK(Tests, Runtime.Resume(Emitter.GetUUID()).has_value());
			CheckPlaybackState(Emitter.GetUUID(), true);

			AudioSourceComponent UpdatedSource{ Imported->ID, 0.25f, false, true, false };
			PF_CHECK(Tests, Emitter.SetAudioSource(UpdatedSource).has_value());
			TransformComponent MovedEmitter;
			MovedEmitter.Translation = { 3.0f, 2.0f, 1.0f };
			PF_CHECK(Tests, Emitter.SetTransform(MovedEmitter).has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.StopPlayback(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, Runtime.Play(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 2);

			std::filesystem::create_directories(ProjectRoot / "Assets" / "Scripts", FileError);
			PF_CHECK(Tests, !FileError);
			if (FileError) return;
			constexpr std::string_view AudioControlSource = R"(
function OnUpdate()
  assert(entity:stop_audio())
  assert(entity:play_audio())
  assert(entity:pause_audio())
  assert(entity:resume_audio())
end
)";
			const auto AudioControlBytes = std::as_bytes(std::span(AudioControlSource.data(), AudioControlSource.size()));
			const auto AudioControlAsset =
				AssetOperations::CreateAssetFromBytes(Registry, ProjectRoot, AudioControlBytes, "Assets/Scripts/audio_controls.lua");
			PF_CHECK(Tests, AudioControlAsset.has_value());
			if (!AudioControlAsset) return;
			PF_CHECK(Tests, Emitter.SetScript(ScriptComponent{ AudioControlAsset->ID }).has_value());
			ScriptRuntimeServices AudioServices;
			AudioServices.Audio = &Runtime;
			ScriptRuntime AudioControlRuntime(*AudioProject, {}, AudioServices);
			PF_CHECK(Tests, AudioControlRuntime.Start(AudioScene).has_value());
			PF_CHECK(Tests, AudioControlRuntime.Advance(AudioScene, Timestep(1.0 / 60.0)).has_value());
			PF_CHECK(Tests, AudioControlRuntime.GetDiagnostics().empty());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 2);
			AudioControlRuntime.Stop();
			PF_CHECK(Tests, Emitter.RemoveScript().has_value());

			PF_CHECK(Tests, Emitter.RemoveAudioSource().has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 1);
			Runtime.Stop();
			PF_CHECK(Tests, !Runtime.IsRunning());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 0);

			Scene ReusedEmitterScene;
			const UUID ReusedEmitterID{ 0x5300000000000000ull, 8 };
			const auto OriginalEmitter = ReusedEmitterScene.CreateEntityWithUUID(ReusedEmitterID, "Original emitter incarnation");
			PF_CHECK(Tests, OriginalEmitter.has_value());
			if (!OriginalEmitter) return;
			AudioSourceComponent ReusedEmitterSource;
			ReusedEmitterSource.AudioAsset = Imported->ID;
			ReusedEmitterSource.PlayOnStart = true;
			ReusedEmitterSource.Spatialized = false;
			PF_CHECK(Tests, OriginalEmitter->SetAudioSource(ReusedEmitterSource).has_value());
			AudioSceneRuntime ReusedEmitterRuntime(Engine, Cache);
			PF_CHECK(Tests, ReusedEmitterRuntime.Start(ReusedEmitterScene).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, ReusedEmitterScene.DestroyEntity(*OriginalEmitter).has_value());
			const auto ReplacementEmitter = ReusedEmitterScene.CreateEntityWithUUID(ReusedEmitterID, "Replacement emitter incarnation");
			PF_CHECK(Tests, ReplacementEmitter.has_value());
			if (!ReplacementEmitter) return;
			PF_CHECK(Tests, ReplacementEmitter->SetAudioSource(ReusedEmitterSource).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.Advance(ReusedEmitterScene).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.GetPlaybackCount() == 0);
			const auto StalePause = ReusedEmitterRuntime.Pause(ReusedEmitterID);
			PF_CHECK(Tests, !StalePause && StalePause.error().Code == AudioSceneRuntimeErrorCode::MissingPlayback);
			PF_CHECK(Tests, ReusedEmitterRuntime.GetPlaybackCount() == 0);
			const auto StaleResume = ReusedEmitterRuntime.Resume(ReusedEmitterID);
			PF_CHECK(Tests, !StaleResume && StaleResume.error().Code == AudioSceneRuntimeErrorCode::MissingPlayback);
			PF_CHECK(Tests, ReusedEmitterRuntime.Play(ReusedEmitterID).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, ReusedEmitterRuntime.Pause(ReusedEmitterID).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.Resume(ReusedEmitterID).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.StopPlayback(ReusedEmitterID).has_value());
			PF_CHECK(Tests, ReusedEmitterRuntime.GetPlaybackCount() == 0);
			ReusedEmitterRuntime.Stop();

			Scene PartialStartScene;
			const UUID ValidEmitterId{ 0x5300000000000000ull, 1 };
			const UUID MissingEmitterId{ 0x5300000000000000ull, 2 };
			const auto ValidEmitterResult = PartialStartScene.CreateEntityWithUUID(ValidEmitterId, "Valid audio source");
			const auto MissingEmitterResult = PartialStartScene.CreateEntityWithUUID(MissingEmitterId, "Missing audio source");
			PF_CHECK(Tests, ValidEmitterResult && MissingEmitterResult);
			if (ValidEmitterResult && MissingEmitterResult)
			{
				PF_CHECK(Tests, ValidEmitterResult->SetAudioSource(AudioSourceComponent{ Imported->ID }).has_value());
				PF_CHECK(
					Tests, MissingEmitterResult->SetAudioSource(AudioSourceComponent{ AssetID{ 0x5300000000000000ull, 3 } }).has_value());
				AudioSceneRuntime RollbackRuntime(Engine, Cache);
				const auto FailedStart = RollbackRuntime.Start(PartialStartScene);
				PF_CHECK(Tests, !FailedStart && FailedStart.error().Code == AudioSceneRuntimeErrorCode::AssetLoadFailed);
				PF_CHECK(Tests, !RollbackRuntime.IsRunning() && RollbackRuntime.GetPlaybackCount() == 0);
				PF_CHECK(Tests, MissingEmitterResult->RemoveAudioSource().has_value());
				PF_CHECK(Tests, RollbackRuntime.Start(PartialStartScene).has_value());
				PF_CHECK(Tests, RollbackRuntime.GetPlaybackCount() == 1);
				RollbackRuntime.Stop();
			}

			Scene InvalidListenerScene;
			auto FirstListener = InvalidListenerScene.CreateEntity("First listener");
			auto SecondListener = InvalidListenerScene.CreateEntity("Second listener");
			PF_CHECK(Tests, FirstListener && SecondListener);
			if (FirstListener && SecondListener)
			{
				PF_CHECK(Tests, FirstListener->SetAudioListener(AudioListenerComponent{}).has_value());
				PF_CHECK(Tests, SecondListener->SetAudioListener(AudioListenerComponent{}).has_value());
				AudioSceneRuntime InvalidRuntime(Engine, Cache);
				const auto InvalidStart = InvalidRuntime.Start(InvalidListenerScene);
				PF_CHECK(Tests, !InvalidStart && InvalidStart.error().Code == AudioSceneRuntimeErrorCode::MultiplePrimaryListeners);
				PF_CHECK(Tests, !InvalidRuntime.IsRunning());
			}

			Scene DuplicatedPrimaryListenerScene;
			auto PrimaryListener = DuplicatedPrimaryListenerScene.CreateEntity("Primary listener");
			PF_CHECK(Tests, PrimaryListener.has_value());
			if (PrimaryListener)
			{
				PF_CHECK(Tests, PrimaryListener->SetAudioListener(AudioListenerComponent{ true }).has_value());
				auto DuplicatedListener = DuplicatedPrimaryListenerScene.DuplicateEntity(*PrimaryListener);
				PF_CHECK(Tests, DuplicatedListener.has_value());
				if (DuplicatedListener)
				{
					const auto OriginalListenerComponent = PrimaryListener->GetAudioListener();
					const auto DuplicatedListenerComponent = DuplicatedListener->GetAudioListener();
					PF_CHECK(Tests, OriginalListenerComponent && OriginalListenerComponent->has_value() &&
										OriginalListenerComponent->value().IsPrimary);
					PF_CHECK(Tests, DuplicatedListenerComponent && DuplicatedListenerComponent->has_value() &&
										!DuplicatedListenerComponent->value().IsPrimary);
					AudioSceneRuntime RuntimeAfterListenerDuplication(Engine, Cache);
					PF_CHECK(Tests, RuntimeAfterListenerDuplication.Start(DuplicatedPrimaryListenerScene).has_value());
					PF_CHECK(Tests, RuntimeAfterListenerDuplication.Advance(DuplicatedPrimaryListenerScene).has_value());
				}
			}
		}

		// A playback retains the initialized device state even after its AudioEngine owner goes away.
		PF_CHECK(Tests, Playback.SetVolume(0.75f).has_value());
		PF_CHECK(Tests, Playback.Stop().has_value());
	}
} // namespace

void RunAudioRuntimeTests(TestRunner& Tests)
{
	TestAudioSchemaCompatibility(Tests);
	TestAudioSessionSettings(Tests);
	TestOfflineAudioBusMixing(Tests);
	TestAudioEngineAndAssetCache(Tests);
	TestAudioMixingSamplePlayerSession(Tests);
}
