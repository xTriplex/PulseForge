#include "TestEngineSupport.h"

namespace
{
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
			auto Imported = AssetOperations::CreateAssetFromBytes(Registry, ProjectRoot, WaveData, "Assets/Audio/test.wav");
			PF_CHECK(Tests, Imported.has_value());
			if (!Imported) return;

			AudioAssetCache Cache(Engine, ProjectRoot, Registry);
			const auto Loaded = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, Loaded && Loaded->IsValid());
			PF_CHECK(Tests, Cache.GetLoadedCount() == 1);
			const auto CachedAgain = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, CachedAgain && Cache.GetLoadedCount() == 1);

			const auto Moved = AssetOperations::Move(Registry, ProjectRoot, Imported->ID, "Assets/Audio/renamed.wav");
			PF_CHECK(Tests, Moved && Moved->ID == Imported->ID);
			Cache.Clear();
			const auto LoadedAfterMove = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, LoadedAfterMove && LoadedAfterMove->IsValid());
			const auto MovedRecord = Registry.Find(Imported->ID);
			PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == "Assets/Audio/renamed.wav");

			Scene AudioScene;
			auto EmitterResult = AudioScene.CreateEntity("Audio emitter and listener");
			PF_CHECK(Tests, EmitterResult.has_value());
			if (!EmitterResult) return;
			Entity Emitter = *EmitterResult;
			PF_CHECK(Tests, Emitter.SetAudioSource(AudioSourceComponent{ Imported->ID, 0.5f, true, true, true }).has_value());
			PF_CHECK(Tests, Emitter.SetAudioListener(AudioListenerComponent{}).has_value());

			AudioSceneRuntime Runtime(Engine, Cache);
			PF_CHECK(Tests, Runtime.Start(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			Scene OtherAudioScene;
			PF_CHECK(Tests, !Runtime.Advance(OtherAudioScene));
			PF_CHECK(Tests, Runtime.Pause(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.Resume(Emitter.GetUUID()).has_value());

			AudioSourceComponent UpdatedSource{ Imported->ID, 0.25f, false, true, false };
			PF_CHECK(Tests, Emitter.SetAudioSource(UpdatedSource).has_value());
			TransformComponent MovedEmitter;
			MovedEmitter.Translation = { 3.0f, 2.0f, 1.0f };
			PF_CHECK(Tests, Emitter.SetTransform(MovedEmitter).has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.StopPlayback(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 0);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 0);
			PF_CHECK(Tests, Runtime.Play(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);

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
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);
			AudioControlRuntime.Stop();
			PF_CHECK(Tests, Emitter.RemoveScript().has_value());

			PF_CHECK(Tests, Emitter.RemoveAudioSource().has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 0);
			Runtime.Stop();
			PF_CHECK(Tests, !Runtime.IsRunning());

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
	TestAudioEngineAndAssetCache(Tests);
}
