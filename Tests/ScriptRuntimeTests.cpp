#include "TestEngineSupport.h"
#include "Core/Diagnostics.h"
#include "Input/GameplayInput.h"
#include "Scripting/Detail/ScriptFloatPropertyRead.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace
{
struct AudioBindingProbe;

#if defined(PF_DEBUG)
	struct LuaAllocationFaultProbe final
	{
		PulseForge::ScriptRuntimeAllocationPointForTesting TargetPoint =
			PulseForge::ScriptRuntimeAllocationPointForTesting::MainChunkReference;
		size_t TargetModuleIndex = 0;
		bool Armed = true;
		size_t RejectedAllocationCount = 0;
		size_t LastRequestedBytes = 0;
		AudioBindingProbe* AudioProbe = nullptr;
		bool RejectAudioHostErrorAllocation = false;
		size_t AudioHostErrorAllocationAttemptCount = 0;
		bool AudioErrorAllocationBeforeExceptionCleanup = false;
	};

	bool RejectLuaAllocation(void* Context, PulseForge::ScriptRuntimeAllocationPointForTesting Point,
		size_t ModuleIndex, size_t RequestedBytes) noexcept;
#endif

	enum class AudioBindingOperation : size_t
	{
		GetGain,
		SetGain,
		GetMuted,
		SetMuted,
		SaveSettings,
		GetOutputState,
		RetryOutput,
		Count
	};

	struct AudioBindingProbe final
	{
		std::array<size_t, static_cast<size_t>(AudioBindingOperation::Count)> Calls{};
		std::array<bool, static_cast<size_t>(AudioBindingOperation::Count)> Throw{};
		std::array<bool, static_cast<size_t>(AudioBindingOperation::Count)> Reject{};
		std::array<std::string_view, static_cast<size_t>(AudioBindingOperation::Count)> LastChannel{};
		size_t HostCallbackCount = 0;
		size_t ActiveExceptionCount = 0;
		size_t DestroyedExceptionCount = 0;
		float Gain = 0.625f;
		bool Muted = false;
		bool RetryResult = true;
	};

#if defined(PF_DEBUG)
	bool RejectLuaAllocation(void* Context, PulseForge::ScriptRuntimeAllocationPointForTesting Point,
		size_t ModuleIndex, size_t RequestedBytes) noexcept
	{
		auto& Probe = *static_cast<LuaAllocationFaultProbe*>(Context);
		if (Point == PulseForge::ScriptRuntimeAllocationPointForTesting::AudioHostErrorString)
		{
			++Probe.AudioHostErrorAllocationAttemptCount;
			if (!Probe.Armed || !Probe.RejectAudioHostErrorAllocation) return false;
			if (!Probe.AudioProbe || Probe.AudioProbe->HostCallbackCount == 0 ||
				Probe.AudioProbe->ActiveExceptionCount != 0 || Probe.AudioProbe->DestroyedExceptionCount == 0)
			{
				Probe.AudioErrorAllocationBeforeExceptionCleanup = true;
				return false;
			}
		}
		if (!Probe.Armed || Probe.TargetPoint != Point || Probe.TargetModuleIndex != ModuleIndex) return false;
		++Probe.RejectedAllocationCount;
		Probe.LastRequestedBytes = RequestedBytes;
		return true;
	}
#endif

	struct AudioBindingHostException final
	{
		explicit AudioBindingHostException(AudioBindingProbe& Probe) noexcept : m_Probe(&Probe)
		{
			++m_Probe->ActiveExceptionCount;
		}

		~AudioBindingHostException() noexcept
		{
			--m_Probe->ActiveExceptionCount;
			++m_Probe->DestroyedExceptionCount;
		}

		AudioBindingProbe* m_Probe;
	};

	bool EnterAudioHostOperation(AudioBindingProbe& Probe, AudioBindingOperation Operation)
	{
		const size_t Index = static_cast<size_t>(Operation);
		++Probe.Calls[Index];
		++Probe.HostCallbackCount;
		if (Probe.Throw[Index]) throw AudioBindingHostException(Probe);
		return !Probe.Reject[Index];
	}

	void InstallAudioBindingServices(PulseForge::ScriptRuntimeServices& Services, AudioBindingProbe& Probe)
	{
		using namespace PulseForge;
		Services.GetAudioGain = [&Probe](std::string_view Channel) -> std::optional<float>
		{
			Probe.LastChannel[static_cast<size_t>(AudioBindingOperation::GetGain)] = Channel;
			if (!EnterAudioHostOperation(Probe, AudioBindingOperation::GetGain)) return std::nullopt;
			return Probe.Gain;
		};
		Services.SetAudioGain = [&Probe](std::string_view Channel, float Gain)
		{
			Probe.LastChannel[static_cast<size_t>(AudioBindingOperation::SetGain)] = Channel;
			if (!EnterAudioHostOperation(Probe, AudioBindingOperation::SetGain)) return false;
			Probe.Gain = Gain;
			return true;
		};
		Services.GetAudioMuted = [&Probe](std::string_view Channel) -> std::optional<bool>
		{
			Probe.LastChannel[static_cast<size_t>(AudioBindingOperation::GetMuted)] = Channel;
			if (!EnterAudioHostOperation(Probe, AudioBindingOperation::GetMuted)) return std::nullopt;
			return Probe.Muted;
		};
		Services.SetAudioMuted = [&Probe](std::string_view Channel, bool Muted)
		{
			Probe.LastChannel[static_cast<size_t>(AudioBindingOperation::SetMuted)] = Channel;
			if (!EnterAudioHostOperation(Probe, AudioBindingOperation::SetMuted)) return false;
			Probe.Muted = Muted;
			return true;
		};
		Services.SaveAudioSettings = [&Probe]
		{
			return EnterAudioHostOperation(Probe, AudioBindingOperation::SaveSettings);
		};
		Services.GetAudioOutputState = [&Probe]() -> std::optional<std::string_view>
		{
			if (!EnterAudioHostOperation(Probe, AudioBindingOperation::GetOutputState)) return std::nullopt;
			return "DefaultActive";
		};
		Services.RetryAudioOutput = [&Probe]
		{
			if (!EnterAudioHostOperation(Probe, AudioBindingOperation::RetryOutput)) return false;
			return Probe.RetryResult;
		};
	}

	std::vector<std::byte> MakeSilentScriptTestWave()
	{
		constexpr uint32_t SampleCount = 44100 * 5;
		constexpr uint32_t DataSize = SampleCount * 2;
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
		Write16(22, 1);
		Write32(24, 44100);
		Write32(28, 88200);
		Write16(32, 2);
		Write16(34, 16);
		WriteFourCC(36, "data");
		Write32(40, DataSize);
		return Data;
	}

	void TestScriptRuntime(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeScriptRuntime-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		auto ProjectResult = Project::Create(ProjectRoot / "ScriptProject.pfproj", "Script Runtime Tests");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		std::filesystem::create_directories(ProjectRoot / "Assets/Scripts", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto CreateScriptAsset = [&](std::string_view FileName, std::string_view Source)
		{
			const auto Contents = std::as_bytes(std::span(Source.data(), Source.size()));
			return AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(), ProjectResult->GetRootPath(), Contents,
				std::filesystem::path("Assets/Scripts") / FileName);
		};

		constexpr std::string_view TransformScript = R"(
local elapsed = 0
function OnCreate()
  assert(io == nil and os == nil and debug == nil and package == nil)
  assert(load == nil and loadfile == nil and dofile == nil and collectgarbage == nil)
  assert(pcall == nil and xpcall == nil)
  assert(string.dump == nil)
  local found = scene.find_entity(entity:uuid())
  assert(found ~= nil and found:uuid() == entity:uuid())
end
function OnUpdate(dt)
  elapsed = elapsed + dt
  assert(entity:set_translation(elapsed, 2, 3))
end
function OnDestroy()
  assert(entity:set_translation(-1, 2, 3))
end
)";
		const auto TransformScriptAsset = CreateScriptAsset("transform.LUA", TransformScript);
		PF_CHECK(Tests, TransformScriptAsset.has_value());
		if (!TransformScriptAsset) return;

		const auto AuthoredPropertyScriptAsset =
			CreateScriptAsset("authored_property.lua", "function OnUpdate()\n"
													   "  local speed = entity:get_float_property('move.speed')\n"
													   "  assert(speed == 4.5)\n"
													   "  assert(entity:get_float_property('unknown') == nil)\n"
													   "  assert(entity:get_float_property('zero') == 0)\n"
													   "  assert(entity:get_float_property('negative') == -2.5)\n"
													   "  local invalid, invalidMessage = entity:get_float_property('bad id')\n"
													   "  assert(invalid == nil and type(invalidMessage) == 'string')\n"
													   "  local wrongType, typeMessage = entity:get_float_property(17)\n"
													   "  assert(wrongType == nil and type(typeMessage) == 'string')\n"
													   "  assert(entity:set_translation(speed, 0, 0))\n"
													   "end\n");
		PF_CHECK(Tests, AuthoredPropertyScriptAsset.has_value());
		if (!AuthoredPropertyScriptAsset) return;
		Scene AuthoredPropertyScene;
		const auto AuthoredPropertyEntity = AuthoredPropertyScene.CreateEntity("Authored script property");
		PF_CHECK(Tests, AuthoredPropertyEntity.has_value());
		if (!AuthoredPropertyEntity) return;
		ScriptComponent AuthoredPropertyComponent{ AuthoredPropertyScriptAsset->ID };
		AuthoredPropertyComponent.FloatProperties.emplace("move.speed", 4.5f);
		AuthoredPropertyComponent.FloatProperties.emplace("zero", 0.0f);
		AuthoredPropertyComponent.FloatProperties.emplace("negative", -2.5f);
		PF_CHECK(Tests, AuthoredPropertyEntity->SetScript(AuthoredPropertyComponent).has_value());
		ScriptRuntime AuthoredPropertyRuntime(*ProjectResult);
		PF_CHECK(Tests, AuthoredPropertyRuntime.Start(AuthoredPropertyScene).has_value());
		PF_CHECK(Tests, AuthoredPropertyRuntime.Advance(AuthoredPropertyScene, Timestep(1.0 / 60.0)).has_value());
		const auto AuthoredPropertyTransform = AuthoredPropertyEntity->GetTransform();
		PF_CHECK(Tests, AuthoredPropertyTransform && AuthoredPropertyTransform->Translation.x == 4.5f);
		PF_CHECK(Tests, AuthoredPropertyRuntime.GetDiagnostics().empty());
		AuthoredPropertyRuntime.Stop();

		const auto NonStringErrorAsset = CreateScriptAsset("non_string_error.lua", "function OnUpdate() error({}) end\n");
		PF_CHECK(Tests, NonStringErrorAsset.has_value());
		if (!NonStringErrorAsset) return;
		Scene NonStringErrorScene;
		const auto NonStringErrorEntity = NonStringErrorScene.CreateEntity("Non-string Lua error");
		PF_CHECK(Tests, NonStringErrorEntity &&
			NonStringErrorEntity->SetScript(ScriptComponent{ NonStringErrorAsset->ID }).has_value());
		if (!NonStringErrorEntity) return;
		ScriptRuntime NonStringErrorRuntime(*ProjectResult);
		PF_CHECK(Tests, NonStringErrorRuntime.Start(NonStringErrorScene).has_value());
		PF_CHECK(Tests, NonStringErrorRuntime.Advance(NonStringErrorScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, NonStringErrorRuntime.GetDiagnostics().size() == 1 &&
			NonStringErrorRuntime.GetDiagnostics().front().Message == "Lua returned a non-string error object");
		NonStringErrorRuntime.Stop();

		constexpr std::string_view AudioBindingSuccessSource = R"(
function OnUpdate()
  assert(audio.get_gain("music") == 0.625)
  assert(audio.set_gain("effects", 0.25) == true)
  assert(audio.is_muted("ui") == false)
  assert(audio.set_muted("music", true) == true)
  assert(audio.save_settings() == true)
  assert(audio.get_output_state() == "DefaultActive")
  assert(audio.retry_output() == true)
end
)";
		const auto AudioBindingSuccessAsset = CreateScriptAsset("audio_binding_success.lua", AudioBindingSuccessSource);
		PF_CHECK(Tests, AudioBindingSuccessAsset.has_value());
		if (!AudioBindingSuccessAsset) return;
		Scene AudioBindingSuccessScene;
		const auto AudioBindingSuccessEntity = AudioBindingSuccessScene.CreateEntity("Successful audio bindings");
		PF_CHECK(Tests, AudioBindingSuccessEntity &&
			AudioBindingSuccessEntity->SetScript(ScriptComponent{ AudioBindingSuccessAsset->ID }).has_value());
		if (!AudioBindingSuccessEntity) return;
		AudioBindingProbe AudioBindingSuccessProbe;
		ScriptRuntimeServices AudioBindingSuccessServices;
		InstallAudioBindingServices(AudioBindingSuccessServices, AudioBindingSuccessProbe);
		ScriptRuntime AudioBindingSuccessRuntime(*ProjectResult, {}, AudioBindingSuccessServices);
		PF_CHECK(Tests, AudioBindingSuccessRuntime.Start(AudioBindingSuccessScene).has_value());
		PF_CHECK(Tests, AudioBindingSuccessRuntime.Advance(AudioBindingSuccessScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, AudioBindingSuccessRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, AudioBindingSuccessProbe.Calls[0] == 1 && AudioBindingSuccessProbe.Calls[1] == 1 &&
			AudioBindingSuccessProbe.Calls[2] == 1 && AudioBindingSuccessProbe.Calls[3] == 1 &&
			AudioBindingSuccessProbe.Calls[4] == 1 && AudioBindingSuccessProbe.Calls[5] == 1 &&
			AudioBindingSuccessProbe.Calls[6] == 1);
		PF_CHECK(Tests, AudioBindingSuccessProbe.Gain == 0.25f && AudioBindingSuccessProbe.Muted);
		PF_CHECK(Tests, AudioBindingSuccessProbe.LastChannel[0] == "music" &&
			AudioBindingSuccessProbe.LastChannel[1] == "effects" && AudioBindingSuccessProbe.LastChannel[2] == "ui" &&
			AudioBindingSuccessProbe.LastChannel[3] == "music");
		AudioBindingSuccessRuntime.Stop();

		constexpr std::string_view AudioBindingRejectedSource = R"(
function OnUpdate()
  local gain, gainError = audio.get_gain("music")
  assert(gain == nil and gainError == "the requested audio mix channel is unavailable in this runtime session")
  local setGain, setGainError = audio.set_gain("effects", 0.25)
  assert(setGain == nil and setGainError == "the active audio session rejected the gain change")
  local muted, mutedError = audio.is_muted("ui")
  assert(muted == nil and mutedError == "the requested audio mix channel is unavailable in this runtime session")
  local setMuted, setMutedError = audio.set_muted("music", true)
  assert(setMuted == nil and setMutedError == "the active audio session rejected the mute change")
  local saved, saveError = audio.save_settings()
  assert(saved == nil and saveError == "user audio settings could not be saved")
  local output, outputError = audio.get_output_state()
  assert(output == nil and outputError == "audio output state is unavailable in this runtime session")
  assert(audio.retry_output() == false)
end
)";
		const auto AudioBindingRejectedAsset = CreateScriptAsset("audio_binding_rejected.lua", AudioBindingRejectedSource);
		PF_CHECK(Tests, AudioBindingRejectedAsset.has_value());
		if (!AudioBindingRejectedAsset) return;
		Scene AudioBindingRejectedScene;
		const auto AudioBindingRejectedEntity = AudioBindingRejectedScene.CreateEntity("Rejected audio bindings");
		PF_CHECK(Tests, AudioBindingRejectedEntity &&
			AudioBindingRejectedEntity->SetScript(ScriptComponent{ AudioBindingRejectedAsset->ID }).has_value());
		if (!AudioBindingRejectedEntity) return;
		AudioBindingProbe AudioBindingRejectedProbe;
		AudioBindingRejectedProbe.Reject.fill(true);
		ScriptRuntimeServices AudioBindingRejectedServices;
		InstallAudioBindingServices(AudioBindingRejectedServices, AudioBindingRejectedProbe);
		ScriptRuntime AudioBindingRejectedRuntime(*ProjectResult, {}, AudioBindingRejectedServices);
		PF_CHECK(Tests, AudioBindingRejectedRuntime.Start(AudioBindingRejectedScene).has_value());
		PF_CHECK(Tests, AudioBindingRejectedRuntime.Advance(AudioBindingRejectedScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, AudioBindingRejectedRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, AudioBindingRejectedProbe.Calls[0] == 1 && AudioBindingRejectedProbe.Calls[1] == 1 &&
			AudioBindingRejectedProbe.Calls[2] == 1 && AudioBindingRejectedProbe.Calls[3] == 1 &&
			AudioBindingRejectedProbe.Calls[4] == 1 && AudioBindingRejectedProbe.Calls[5] == 1 &&
			AudioBindingRejectedProbe.Calls[6] == 1);
		AudioBindingRejectedRuntime.Stop();

		constexpr std::string_view AudioBindingUnavailableSource = R"(
function OnUpdate()
  local gain, gainError = audio.get_gain("music")
  assert(gain == nil and type(gainError) == "string")
  local setGain, setGainError = audio.set_gain("effects", 0.25)
  assert(setGain == nil and type(setGainError) == "string")
  local muted, mutedError = audio.is_muted("ui")
  assert(muted == nil and type(mutedError) == "string")
  local setMuted, setMutedError = audio.set_muted("music", true)
  assert(setMuted == nil and type(setMutedError) == "string")
  local saved, saveError = audio.save_settings()
  assert(saved == nil and type(saveError) == "string")
  local output, outputError = audio.get_output_state()
  assert(output == nil and type(outputError) == "string")
  local retried, retryError = audio.retry_output()
  assert(retried == nil and type(retryError) == "string")
end
)";
		const auto AudioBindingUnavailableAsset = CreateScriptAsset("audio_binding_unavailable.lua", AudioBindingUnavailableSource);
		PF_CHECK(Tests, AudioBindingUnavailableAsset.has_value());
		if (!AudioBindingUnavailableAsset) return;
		Scene AudioBindingUnavailableScene;
		const auto AudioBindingUnavailableEntity = AudioBindingUnavailableScene.CreateEntity("Unavailable audio services");
		PF_CHECK(Tests, AudioBindingUnavailableEntity &&
			AudioBindingUnavailableEntity->SetScript(ScriptComponent{ AudioBindingUnavailableAsset->ID }).has_value());
		if (!AudioBindingUnavailableEntity) return;
		ScriptRuntime AudioBindingUnavailableRuntime(*ProjectResult);
		PF_CHECK(Tests, AudioBindingUnavailableRuntime.Start(AudioBindingUnavailableScene).has_value());
		PF_CHECK(Tests, AudioBindingUnavailableRuntime.Advance(AudioBindingUnavailableScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, AudioBindingUnavailableRuntime.GetDiagnostics().empty());
		AudioBindingUnavailableRuntime.Stop();

		constexpr std::string_view AudioBindingThrowSource = R"(
function OnUpdate()
  local gain, gainError = audio.get_gain("music")
  assert(gain == nil and gainError == "audio.get_gain failed in the audio session host")
  local setGain, setGainError = audio.set_gain("effects", 0.25)
  assert(setGain == nil and setGainError == "audio.set_gain failed in the audio session host")
  local muted, mutedError = audio.is_muted("ui")
  assert(muted == nil and mutedError == "audio.is_muted failed in the audio session host")
  local setMuted, setMutedError = audio.set_muted("music", true)
  assert(setMuted == nil and setMutedError == "audio.set_muted failed in the audio session host")
  local saved, saveError = audio.save_settings()
  assert(saved == nil and saveError == "audio.save_settings failed in the audio session host")
  local output, outputError = audio.get_output_state()
  assert(output == nil and outputError == "audio.get_output_state failed in the audio session host")
  local retried, retryError = audio.retry_output()
  assert(retried == nil and retryError == "audio.retry_output failed in the audio session host")
end
)";
		const auto AudioBindingThrowAsset = CreateScriptAsset("audio_binding_throw.lua", AudioBindingThrowSource);
		PF_CHECK(Tests, AudioBindingThrowAsset.has_value());
		if (!AudioBindingThrowAsset) return;
		Scene AudioBindingThrowScene;
		const auto AudioBindingThrowEntity = AudioBindingThrowScene.CreateEntity("Throwing audio services");
		PF_CHECK(Tests, AudioBindingThrowEntity &&
			AudioBindingThrowEntity->SetScript(ScriptComponent{ AudioBindingThrowAsset->ID }).has_value());
		if (!AudioBindingThrowEntity) return;
		AudioBindingProbe AudioBindingThrowProbe;
		AudioBindingThrowProbe.Throw.fill(true);
		ScriptRuntimeServices AudioBindingThrowServices;
		InstallAudioBindingServices(AudioBindingThrowServices, AudioBindingThrowProbe);
		ScriptRuntime AudioBindingThrowRuntime(*ProjectResult, {}, AudioBindingThrowServices);
		PF_CHECK(Tests, AudioBindingThrowRuntime.Start(AudioBindingThrowScene).has_value());
		PF_CHECK(Tests, AudioBindingThrowRuntime.Advance(AudioBindingThrowScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, AudioBindingThrowRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, AudioBindingThrowProbe.Calls[0] == 1 && AudioBindingThrowProbe.Calls[1] == 1 &&
			AudioBindingThrowProbe.Calls[2] == 1 && AudioBindingThrowProbe.Calls[3] == 1 &&
			AudioBindingThrowProbe.Calls[4] == 1 && AudioBindingThrowProbe.Calls[5] == 1 &&
			AudioBindingThrowProbe.Calls[6] == 1);
		PF_CHECK(Tests, AudioBindingThrowProbe.ActiveExceptionCount == 0 &&
			AudioBindingThrowProbe.DestroyedExceptionCount == 7);
		AudioBindingThrowRuntime.Stop();

#if defined(PF_DEBUG)
		constexpr std::string_view AudioBindingOomSource = R"(
function OnUpdate()
  local gain = audio.get_gain("music")
  if gain then
    assert(gain == 0.625)
    assert(entity:set_translation(gain, 0, 0))
  end
end
function OnDestroy()
  local allocationProbe = string.rep("x", 512)
  assert(#allocationProbe == 512)
end
)";
		const auto AudioBindingOomAsset = CreateScriptAsset("audio_binding_oom.lua", AudioBindingOomSource);
		PF_CHECK(Tests, AudioBindingOomAsset.has_value());
		if (!AudioBindingOomAsset) return;
		Scene AudioBindingOomScene;
		const auto AudioBindingOomEntity = AudioBindingOomScene.CreateEntity("Audio binding allocation failure");
		PF_CHECK(Tests, AudioBindingOomEntity &&
			AudioBindingOomEntity->SetScript(ScriptComponent{ AudioBindingOomAsset->ID }).has_value());
		if (!AudioBindingOomEntity) return;
		AudioBindingProbe AudioBindingOomProbe;
		AudioBindingOomProbe.Throw[static_cast<size_t>(AudioBindingOperation::GetGain)] = true;
		LuaAllocationFaultProbe AudioBindingOomFault;
		AudioBindingOomFault.TargetPoint = ScriptRuntimeAllocationPointForTesting::AudioHostErrorString;
		AudioBindingOomFault.RejectAudioHostErrorAllocation = true;
		AudioBindingOomFault.AudioProbe = &AudioBindingOomProbe;
		const ScriptRuntimeAllocationFaultHooksForTesting AudioBindingOomHooks{ &AudioBindingOomFault, RejectLuaAllocation };
		ScriptRuntimeDesc AudioBindingOomDescription;
		AudioBindingOomDescription.AllocationFaultHooksForTesting = &AudioBindingOomHooks;
		ScriptRuntimeServices AudioBindingOomServices;
		InstallAudioBindingServices(AudioBindingOomServices, AudioBindingOomProbe);
		ScriptRuntime AudioBindingOomRuntime(*ProjectResult, AudioBindingOomDescription, AudioBindingOomServices);
		PF_CHECK(Tests, AudioBindingOomRuntime.Start(AudioBindingOomScene).has_value());
		PF_CHECK(Tests, AudioBindingOomRuntime.Advance(AudioBindingOomScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, AudioBindingOomFault.AudioHostErrorAllocationAttemptCount > 0 &&
			AudioBindingOomFault.RejectedAllocationCount > 0 && AudioBindingOomFault.LastRequestedBytes > 0);
		PF_CHECK(Tests, !AudioBindingOomFault.AudioErrorAllocationBeforeExceptionCleanup &&
			AudioBindingOomProbe.ActiveExceptionCount == 0 && AudioBindingOomProbe.DestroyedExceptionCount == 1);
		PF_CHECK(Tests, AudioBindingOomRuntime.IsRunning() && AudioBindingOomRuntime.GetDiagnostics().size() == 1 &&
			AudioBindingOomRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::UpdateFailed);

		AudioBindingOomFault.Armed = false;
		AudioBindingOomFault.RejectAudioHostErrorAllocation = false;
		AudioBindingOomProbe.Throw[static_cast<size_t>(AudioBindingOperation::GetGain)] = false;
		const auto AudioBindingRecoveryEntity = AudioBindingOomScene.CreateEntity("Audio binding recovery");
		PF_CHECK(Tests, AudioBindingRecoveryEntity &&
			AudioBindingRecoveryEntity->SetScript(ScriptComponent{ AudioBindingOomAsset->ID }).has_value());
		if (!AudioBindingRecoveryEntity) return;
		PF_CHECK(Tests, AudioBindingOomRuntime.Advance(AudioBindingOomScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, AudioBindingOomRuntime.GetScriptCount() == 2 && AudioBindingOomRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, AudioBindingRecoveryEntity->GetTransform()->Translation.x == 0.625f &&
			AudioBindingOomProbe.Calls[static_cast<size_t>(AudioBindingOperation::GetGain)] == 2);
		AudioBindingOomRuntime.Stop();
		PF_CHECK(Tests, AudioBindingOomRuntime.GetDiagnostics().size() == 1 &&
			AudioBindingOomRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::UpdateFailed);
#endif

		const auto WriteScriptSource = [&](const AssetID& Asset, std::string_view Source)
		{
			const auto Record = ProjectResult->GetAssetRegistry().Find(Asset);
			if (!Record) return false;
			std::ofstream Output(ProjectRoot / Record->ProjectRelativePath, std::ios::binary | std::ios::trunc);
			Output.write(Source.data(), static_cast<std::streamsize>(Source.size()));
			return Output.good();
		};

#if defined(PF_DEBUG)
		LuaAllocationFaultProbe MainReferenceFault;
		const ScriptRuntimeAllocationFaultHooksForTesting MainReferenceHooks{ &MainReferenceFault, RejectLuaAllocation };
		const auto MainReferenceAsset = CreateScriptAsset("main_reference_fault.lua",
			"function OnUpdate() assert(entity:set_translation(11, 0, 0)) end\n");
		PF_CHECK(Tests, MainReferenceAsset.has_value());
		if (!MainReferenceAsset) return;
		Scene MainReferenceFaultScene;
		const auto MainReferenceEntity = MainReferenceFaultScene.CreateEntity("Main reference allocation failure");
		PF_CHECK(Tests, MainReferenceEntity &&
			MainReferenceEntity->SetScript(ScriptComponent{ MainReferenceAsset->ID }).has_value());
		if (!MainReferenceEntity) return;
		ScriptRuntimeDesc MainReferenceFaultDescription;
		MainReferenceFaultDescription.AllocationFaultHooksForTesting = &MainReferenceHooks;
		ScriptRuntime MainReferenceFaultRuntime(*ProjectResult, MainReferenceFaultDescription);
		PF_CHECK(Tests, MainReferenceFaultRuntime.Start(MainReferenceFaultScene).has_value());
		PF_CHECK(Tests, MainReferenceFault.RejectedAllocationCount > 0 && MainReferenceFault.LastRequestedBytes > 0);
		PF_CHECK(Tests, MainReferenceFaultRuntime.GetScriptCount() == 0 && MainReferenceFaultRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, MainReferenceFaultRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::LuaInitializationFailed &&
			MainReferenceFaultRuntime.GetDiagnostics().front().Message.find("Could not retain the compiled main script") != std::string::npos);
		MainReferenceFaultRuntime.Stop();
		MainReferenceFault.Armed = false;
		PF_CHECK(Tests, MainReferenceFaultRuntime.Start(MainReferenceFaultScene).has_value());
		PF_CHECK(Tests, MainReferenceFaultRuntime.GetScriptCount() == 1 && MainReferenceFaultRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, MainReferenceFaultRuntime.Advance(MainReferenceFaultScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, MainReferenceEntity->GetTransform()->Translation.x == 11.0f);
		MainReferenceFaultRuntime.Stop();

		std::filesystem::create_directories(ProjectRoot / "Assets/Scripts/fault_modules");
		std::ostringstream ModuleMetadata;
		ModuleMetadata << "--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[";
		AssetID ExpectedModuleDiagnosticAsset;
		for (size_t ModuleIndex = 0; ModuleIndex < 64; ++ModuleIndex)
		{
			const std::string ModuleName = "fault_modules/module_" + std::to_string(ModuleIndex);
			if (ModuleIndex != 0) ModuleMetadata << ',';
			ModuleMetadata << '\"' << ModuleName << '\"';
			const auto ModuleAsset = CreateScriptAsset(ModuleName + ".lua", "return {}\n");
			PF_CHECK(Tests, ModuleAsset.has_value());
			if (!ModuleAsset) return;
			if (ModuleIndex == 9) ExpectedModuleDiagnosticAsset = ModuleAsset->ID;
		}
		ModuleMetadata << "]}\n]]\nfunction OnUpdate() end\n";
		const auto ModuleReferenceAsset = CreateScriptAsset("module_reference_fault.lua", ModuleMetadata.str());
		PF_CHECK(Tests, ModuleReferenceAsset.has_value());
		if (!ModuleReferenceAsset) return;
		LuaAllocationFaultProbe ModuleReferenceFault;
		ModuleReferenceFault.TargetPoint = ScriptRuntimeAllocationPointForTesting::ModuleChunkReference;
		ModuleReferenceFault.TargetModuleIndex = 63;
		const ScriptRuntimeAllocationFaultHooksForTesting ModuleReferenceHooks{
			&ModuleReferenceFault, RejectLuaAllocation, 62 };
		ScriptRuntimeDesc ModuleReferenceFaultDescription;
		ModuleReferenceFaultDescription.AllocationFaultHooksForTesting = &ModuleReferenceHooks;
		Scene ModuleReferenceFaultScene;
		const auto ModuleReferenceEntity = ModuleReferenceFaultScene.CreateEntity("Module reference allocation failure");
		PF_CHECK(Tests, ModuleReferenceEntity &&
			ModuleReferenceEntity->SetScript(ScriptComponent{ ModuleReferenceAsset->ID }).has_value());
		if (!ModuleReferenceEntity) return;
		ScriptRuntime ModuleReferenceFaultRuntime(*ProjectResult, ModuleReferenceFaultDescription);
		PF_CHECK(Tests, ModuleReferenceFaultRuntime.Start(ModuleReferenceFaultScene).has_value());
		PF_CHECK(Tests, ModuleReferenceFault.RejectedAllocationCount > 0 && ModuleReferenceFault.LastRequestedBytes > 0);
		PF_CHECK(Tests, ModuleReferenceFaultRuntime.GetScriptCount() == 0 && ModuleReferenceFaultRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, ModuleReferenceFaultRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::LuaInitializationFailed &&
			ModuleReferenceFaultRuntime.GetDiagnostics().front().Message.find("Could not retain compiled module") != std::string::npos &&
			ModuleReferenceFaultRuntime.GetDiagnostics().front().SourceRelativePath == "Assets/Scripts/fault_modules/module_9.lua" &&
			ModuleReferenceFaultRuntime.GetDiagnostics().front().SourceAsset == ExpectedModuleDiagnosticAsset);
		ModuleReferenceFaultRuntime.Stop();

		const auto ReloadReferenceAsset = CreateScriptAsset("reload_reference_fault.lua",
			"local state = 10\nfunction OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n");
		PF_CHECK(Tests, ReloadReferenceAsset.has_value());
		if (!ReloadReferenceAsset) return;
		Scene ReloadReferenceFaultScene;
		const auto ReloadReferenceEntity = ReloadReferenceFaultScene.CreateEntity("Reload reference allocation failure");
		PF_CHECK(Tests, ReloadReferenceEntity &&
			ReloadReferenceEntity->SetScript(ScriptComponent{ ReloadReferenceAsset->ID }).has_value());
		if (!ReloadReferenceEntity) return;
		LuaAllocationFaultProbe ReloadReferenceFault;
		ReloadReferenceFault.Armed = false;
		const ScriptRuntimeAllocationFaultHooksForTesting ReloadReferenceHooks{ &ReloadReferenceFault, RejectLuaAllocation };
		ScriptRuntimeDesc ReloadReferenceDescription;
		ReloadReferenceDescription.AllocationFaultHooksForTesting = &ReloadReferenceHooks;
		ScriptRuntime ReloadReferenceRuntime(*ProjectResult, ReloadReferenceDescription);
		PF_CHECK(Tests, ReloadReferenceRuntime.Start(ReloadReferenceFaultScene).has_value());
		PF_CHECK(Tests, ReloadReferenceRuntime.Advance(ReloadReferenceFaultScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadReferenceEntity->GetTransform()->Translation.x == 11.0f);
		PF_CHECK(Tests, WriteScriptSource(ReloadReferenceAsset->ID,
			"local state = 100\nfunction OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n"));
		ReloadReferenceFault.Armed = true;
		const auto FailedReferenceReload = ReloadReferenceRuntime.RequestReload(*ReloadReferenceEntity);
		PF_CHECK(Tests, FailedReferenceReload.has_value());
		if (!FailedReferenceReload) return;
		const auto FailedReferenceReloadCount = ReloadReferenceRuntime.ServicePendingReloads(ReloadReferenceFaultScene);
		PF_CHECK(Tests, FailedReferenceReloadCount && *FailedReferenceReloadCount == 0);
		PF_CHECK(Tests, ReloadReferenceFault.RejectedAllocationCount > 0 && ReloadReferenceFault.LastRequestedBytes > 0);
		PF_CHECK(Tests, ReloadReferenceRuntime.GetDiagnostics().size() == 1 &&
			ReloadReferenceRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::LuaInitializationFailed &&
			ReloadReferenceRuntime.GetDiagnostics().front().Message.find("Could not retain the compiled main script") != std::string::npos);
		PF_CHECK(Tests, ReloadReferenceRuntime.Advance(ReloadReferenceFaultScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadReferenceEntity->GetTransform()->Translation.x == 12.0f);
		ReloadReferenceFault.Armed = false;
		const auto RetriedReferenceReload = ReloadReferenceRuntime.RequestReload(*ReloadReferenceEntity);
		PF_CHECK(Tests, RetriedReferenceReload.has_value());
		if (!RetriedReferenceReload) return;
		const auto RetriedReferenceReloadCount = ReloadReferenceRuntime.ServicePendingReloads(ReloadReferenceFaultScene);
		PF_CHECK(Tests, RetriedReferenceReloadCount && *RetriedReferenceReloadCount == 1);
		PF_CHECK(Tests, ReloadReferenceRuntime.Advance(ReloadReferenceFaultScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadReferenceEntity->GetTransform()->Translation.x == 101.0f);
		ReloadReferenceRuntime.Stop();
#endif

		constexpr std::string_view DeclaredPropertySource =
			"--[[PulseForgeScript:1\n"
			"{\"version\":1,\"properties\":[{\"id\":\"movement.speed\",\"label\":\"Move speed\",\"type\":\"float\",\"default\":3.5,\"min\":0.0,\"max\":20.0}]}\n"
			"]]\n"
			"function OnUpdate()\n"
			"  assert(entity:set_translation(entity:get_float_property('movement.speed'), 0, 0))\n"
			"end\n";
		const auto DeclaredPropertyAsset = CreateScriptAsset("declared_property.lua", DeclaredPropertySource);
		PF_CHECK(Tests, DeclaredPropertyAsset.has_value());
		if (!DeclaredPropertyAsset) return;
		Scene DeclaredPropertyScene;
		const auto DefaultPropertyEntity = DeclaredPropertyScene.CreateEntity("Default property");
		const auto OverriddenPropertyEntity = DeclaredPropertyScene.CreateEntity("Overridden property");
		PF_CHECK(Tests, DefaultPropertyEntity && OverriddenPropertyEntity);
		if (!DefaultPropertyEntity || !OverriddenPropertyEntity) return;
		PF_CHECK(Tests, DefaultPropertyEntity->SetScript(ScriptComponent{ DeclaredPropertyAsset->ID }).has_value());
		ScriptComponent PropertyOverride{ DeclaredPropertyAsset->ID };
		PropertyOverride.FloatProperties.emplace("movement.speed", 8.25f);
		PF_CHECK(Tests, OverriddenPropertyEntity->SetScript(PropertyOverride).has_value());
		ScriptRuntime DeclaredPropertyRuntime(*ProjectResult);
		PF_CHECK(Tests, DeclaredPropertyRuntime.Start(DeclaredPropertyScene).has_value());
		PF_CHECK(Tests, DeclaredPropertyRuntime.Advance(DeclaredPropertyScene, Timestep(1.0 / 60.0)).has_value());
		const auto DefaultPropertyTransform = DefaultPropertyEntity->GetTransform();
		const auto OverriddenPropertyTransform = OverriddenPropertyEntity->GetTransform();
		PF_CHECK(Tests, DefaultPropertyTransform && DefaultPropertyTransform->Translation.x == 3.5f);
		PF_CHECK(Tests, OverriddenPropertyTransform && OverriddenPropertyTransform->Translation.x == 8.25f);
		const auto UnmaterializedDefault = DefaultPropertyEntity->GetScript();
		PF_CHECK(Tests, UnmaterializedDefault && UnmaterializedDefault->has_value() && UnmaterializedDefault->value().FloatProperties.empty());
		PF_CHECK(Tests, DeclaredPropertyRuntime.GetDiagnostics().empty());
		DeclaredPropertyRuntime.Stop();

		std::filesystem::create_directories(ProjectRoot / "Assets/Scripts/shared", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		const auto ModuleAsset = CreateScriptAsset("shared/motion.lua",
			"local motion = { speed = 0 }\n"
			"function motion:next_speed() self.speed = self.speed + 1; return self.speed end\n"
			"return motion\n");
		PF_CHECK(Tests, ModuleAsset.has_value());
		if (!ModuleAsset) return;
		const auto ModuleConsumerAsset = CreateScriptAsset("module_consumer.lua",
			"--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[\"shared/motion\"]}\n]]\n"
			"local motion = require('shared/motion')\n"
			"function OnUpdate() assert(entity:set_translation(motion:next_speed(), 0, 0)) end\n");
		PF_CHECK(Tests, ModuleConsumerAsset.has_value());
		if (!ModuleConsumerAsset) return;
		Scene ModuleScene;
		const auto ModuleEntityA = ModuleScene.CreateEntity("Module A");
		const auto ModuleEntityB = ModuleScene.CreateEntity("Module B");
		PF_CHECK(Tests, ModuleEntityA && ModuleEntityB);
		if (!ModuleEntityA || !ModuleEntityB) return;
		PF_CHECK(Tests, ModuleEntityA->SetScript(ScriptComponent{ ModuleConsumerAsset->ID }).has_value());
		PF_CHECK(Tests, ModuleEntityB->SetScript(ScriptComponent{ ModuleConsumerAsset->ID }).has_value());
		ScriptRuntime ModuleRuntime(*ProjectResult);
		PF_CHECK(Tests, ModuleRuntime.Start(ModuleScene).has_value());
		PF_CHECK(Tests, ModuleRuntime.Advance(ModuleScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ModuleEntityA->GetTransform()->Translation.x == 1.0f &&
			ModuleEntityB->GetTransform()->Translation.x == 1.0f);
		PF_CHECK(Tests, ModuleRuntime.GetDiagnostics().empty());
		ModuleRuntime.Stop();

		const auto MissingModuleConsumer = CreateScriptAsset("missing_module.lua",
			"--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[\"shared/missing\"]}\n]]\n"
			"function OnCreate() end\n");
		PF_CHECK(Tests, MissingModuleConsumer.has_value());
		if (!MissingModuleConsumer) return;
		Scene MissingModuleScene;
		const auto MissingModuleEntity = MissingModuleScene.CreateEntity("Missing script module");
		PF_CHECK(Tests, MissingModuleEntity &&
			MissingModuleEntity->SetScript(ScriptComponent{ MissingModuleConsumer->ID }).has_value());
		if (!MissingModuleEntity) return;
		ScriptRuntime MissingModuleRuntime(*ProjectResult);
		PF_CHECK(Tests, MissingModuleRuntime.Start(MissingModuleScene).has_value());
		PF_CHECK(Tests, MissingModuleRuntime.GetDiagnostics().size() == 1 &&
			MissingModuleRuntime.GetDiagnostics().front().Message.find("not present in the source registry") != std::string::npos);
		MissingModuleRuntime.Stop();

		const auto CycleAAsset = CreateScriptAsset("shared/cycle_a.lua",
			"--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[\"cycle_b\"]}\n]]\n"
			"return require('cycle_b')\n");
		const auto CycleBAsset = CreateScriptAsset("shared/cycle_b.lua",
			"--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[\"cycle_a\"]}\n]]\n"
			"return require('cycle_a')\n");
		PF_CHECK(Tests, CycleAAsset.has_value() && CycleBAsset.has_value());
		if (!CycleAAsset || !CycleBAsset) return;
		const auto CycleConsumerAsset = CreateScriptAsset("cycle_consumer.lua",
			"--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[\"shared/cycle_a\"]}\n]]\n"
			"function OnCreate() require('shared/cycle_a') end\n");
		PF_CHECK(Tests, CycleConsumerAsset.has_value());
		if (!CycleConsumerAsset) return;
		Scene CycleScene;
		const auto CycleEntity = CycleScene.CreateEntity("Cyclic script modules");
		PF_CHECK(Tests, CycleEntity && CycleEntity->SetScript(ScriptComponent{ CycleConsumerAsset->ID }).has_value());
		if (!CycleEntity) return;
		ScriptRuntime CycleRuntime(*ProjectResult);
		PF_CHECK(Tests, CycleRuntime.Start(CycleScene).has_value());
		PF_CHECK(Tests, CycleRuntime.GetDiagnostics().size() == 1 &&
			CycleRuntime.GetDiagnostics().front().Message.find("dependency cycle") != std::string::npos);
		CycleRuntime.Stop();

		Scene BoundedDiagnosticsScene;
		for (size_t Index = 0; Index < 132; ++Index)
		{
			auto DiagnosticEntity = BoundedDiagnosticsScene.CreateEntity("Bounded diagnostic");
			PF_CHECK(Tests, DiagnosticEntity.has_value());
			if (!DiagnosticEntity) return;
			PF_CHECK(Tests, DiagnosticEntity->SetScript(ScriptComponent{ MissingModuleConsumer->ID }).has_value());
		}
		ScriptRuntime BoundedDiagnosticsRuntime(*ProjectResult);
		PF_CHECK(Tests, BoundedDiagnosticsRuntime.Start(BoundedDiagnosticsScene).has_value());
		PF_CHECK(Tests, BoundedDiagnosticsRuntime.GetDiagnostics().size() == 128);
		BoundedDiagnosticsRuntime.Stop();

		const auto ReloadAsset = CreateScriptAsset("reloadable.lua",
			"local state = 10\nfunction OnCreate() end\n"
			"function OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n"
			"function OnDestroy() assert(entity:set_translation(0, 1, 0)) end\n");
		PF_CHECK(Tests, ReloadAsset.has_value());
		if (!ReloadAsset) return;
		Scene ReloadScene;
		const auto ReloadEntity = ReloadScene.CreateEntity("Reload target");
		PF_CHECK(Tests, ReloadEntity && ReloadEntity->SetScript(ScriptComponent{ ReloadAsset->ID }).has_value());
		if (!ReloadEntity) return;
		SceneRuntime ReloadRuntime(*ProjectResult);
		PF_CHECK(Tests, ReloadRuntime.Start(ReloadScene).has_value());
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 11.0f);
		PF_CHECK(Tests, ReloadRuntime.SetPaused(true).has_value());
		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID, "function OnCreate(\n"));
		auto FailedReload = ReloadRuntime.RequestScriptReload(*ReloadEntity);
		PF_CHECK(Tests, FailedReload.has_value());
		if (!FailedReload) return;
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().size() == 1 &&
			ReloadRuntime.GetScriptDiagnostics().front().Code == ScriptDiagnosticCode::CompileFailed);
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().front().SourceRelativePath == "Assets/Scripts/reloadable.lua" &&
			ReloadRuntime.GetScriptDiagnostics().front().Line > 0);
		auto RepeatedFailedReload = ReloadRuntime.RequestScriptReload(*ReloadEntity);
		PF_CHECK(Tests, RepeatedFailedReload.has_value());
		if (!RepeatedFailedReload) return;
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().size() == 1);
		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID, "function OnCreate( -- another source generation\n"));
		auto NewGenerationFailedReload = ReloadRuntime.RequestScriptReload(*ReloadEntity);
		PF_CHECK(Tests, NewGenerationFailedReload.has_value());
		if (!NewGenerationFailedReload) return;
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().size() == 2 &&
			ReloadRuntime.GetScriptDiagnostics()[0].SourceFingerprint !=
			ReloadRuntime.GetScriptDiagnostics()[1].SourceFingerprint);
		const auto AfterRejectedReload = ReloadEntity->GetTransform();
		PF_CHECK(Tests, AfterRejectedReload && AfterRejectedReload->Translation.x == 11.0f);
		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID,
			"local state = 100\nfunction OnCreate() end\n"
			"function OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n"
			"function OnDestroy() assert(entity:set_translation(0, 2, 0)) end\n"));
		auto SuccessfulReload = ReloadRuntime.RequestScriptReload(*ReloadEntity);
		PF_CHECK(Tests, SuccessfulReload.has_value());
		if (!SuccessfulReload) return;
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().size() == 2);
		const auto ReloadedTransform = ReloadEntity->GetTransform();
		PF_CHECK(Tests, ReloadedTransform && ReloadedTransform->Translation.x == 0.0f && ReloadedTransform->Translation.y == 1.0f);
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 0.0f);
		PF_CHECK(Tests, ReloadRuntime.SetPaused(false).has_value());
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 101.0f);
		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID,
			"function OnCreate()\n"
			"  assert(entity:set_translation(9, 9, 9))\n"
			"  error('intentional activation failure')\n"
			"end\n"));
		auto ActivationFailureRequest = ReloadRuntime.RequestScriptReload(*ReloadEntity);
		PF_CHECK(Tests, ActivationFailureRequest.has_value());
		if (!ActivationFailureRequest) return;
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().size() == 3 &&
			ReloadRuntime.GetScriptDiagnostics().back().Code == ScriptDiagnosticCode::ReloadActivationFailed);
		const auto FailedActivationTransform = ReloadEntity->GetTransform();
		PF_CHECK(Tests, FailedActivationTransform && FailedActivationTransform->Translation.x == 9.0f &&
			FailedActivationTransform->Translation.y == 9.0f);
		PF_CHECK(Tests, ReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadRuntime.GetScriptDiagnostics().size() == 3);
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 9.0f);
		ReloadRuntime.Stop();

		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID,
			"local state = 200\nfunction OnCreate() end\n"
			"function OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n"
			"function OnDestroy() end\n"));
		ScriptRuntime QueuedReloadRuntime(*ProjectResult);
		PF_CHECK(Tests, QueuedReloadRuntime.Start(ReloadScene).has_value());
		PF_CHECK(Tests, QueuedReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 201.0f);
		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID,
			"local state = 300\nfunction OnCreate() end\n"
			"function OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n"
			"function OnDestroy() end\n"));
		const auto SupersededReload = QueuedReloadRuntime.RequestReload(*ReloadEntity);
		const auto LatestReload = QueuedReloadRuntime.RequestReload(*ReloadEntity);
		PF_CHECK(Tests, SupersededReload && LatestReload && *SupersededReload != *LatestReload);
		const auto ActivatedReloads = QueuedReloadRuntime.ServicePendingReloads(ReloadScene);
		PF_CHECK(Tests, ActivatedReloads && *ActivatedReloads == 1);
		PF_CHECK(Tests, QueuedReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 301.0f);
		PF_CHECK(Tests, WriteScriptSource(ReloadAsset->ID,
			"local state = 400\nfunction OnCreate() end\n"
			"function OnUpdate() state = state + 1; assert(entity:set_translation(state, 0, 0)) end\n"
			"function OnDestroy() end\n"));
		const auto CancelledReload = QueuedReloadRuntime.RequestReload(*ReloadEntity);
		PF_CHECK(Tests, CancelledReload && QueuedReloadRuntime.CancelReload(*CancelledReload));
		PF_CHECK(Tests, QueuedReloadRuntime.CancelReload(*CancelledReload));
		const auto CancelledReloadCount = QueuedReloadRuntime.ServicePendingReloads(ReloadScene);
		PF_CHECK(Tests, CancelledReloadCount && *CancelledReloadCount == 0);
		PF_CHECK(Tests, QueuedReloadRuntime.Advance(ReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReloadEntity->GetTransform()->Translation.x == 302.0f);
		QueuedReloadRuntime.Stop();

		const auto StaleReloadAsset = CreateScriptAsset("stale_reload.lua",
			"function OnCreate() assert(entity:set_translation(12, 0, 0)) end\nfunction OnUpdate() end\n");
		PF_CHECK(Tests, StaleReloadAsset.has_value());
		if (!StaleReloadAsset) return;
		Scene StaleReloadScene;
		const UUID ReusedReloadEntityID{ 0x4300000000004000ull, 0x8000000000000001ull };
		const auto OldReloadEntity = StaleReloadScene.CreateEntityWithUUID(ReusedReloadEntityID, "Old reload incarnation");
		PF_CHECK(Tests, OldReloadEntity && OldReloadEntity->SetScript(ScriptComponent{ StaleReloadAsset->ID }).has_value());
		if (!OldReloadEntity) return;
		ScriptRuntime StaleReloadRuntime(*ProjectResult);
		PF_CHECK(Tests, StaleReloadRuntime.Start(StaleReloadScene).has_value());
		const auto StaleReloadTicket = StaleReloadRuntime.RequestReload(*OldReloadEntity);
		PF_CHECK(Tests, StaleReloadTicket.has_value());
		if (!StaleReloadTicket) return;
		PF_CHECK(Tests, StaleReloadScene.DestroyEntity(*OldReloadEntity).has_value());
		const auto NewReloadEntity = StaleReloadScene.CreateEntityWithUUID(ReusedReloadEntityID, "New reload incarnation");
		PF_CHECK(Tests, NewReloadEntity && NewReloadEntity->SetScript(ScriptComponent{ StaleReloadAsset->ID }).has_value());
		if (!NewReloadEntity) return;
		const auto StaleReloadCount = StaleReloadRuntime.ServicePendingReloads(StaleReloadScene);
		PF_CHECK(Tests, StaleReloadCount && *StaleReloadCount == 0);
		PF_CHECK(Tests, StaleReloadRuntime.GetDiagnostics().size() == 1 &&
			StaleReloadRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::ReloadRejected);
		PF_CHECK(Tests, StaleReloadRuntime.Advance(StaleReloadScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, NewReloadEntity->GetTransform()->Translation.x == 12.0f);
		StaleReloadRuntime.Stop();

		const auto TeardownFailureAsset = CreateScriptAsset("reload_teardown_failure.lua",
			"function OnUpdate() assert(entity:set_translation(5, 0, 0)) end\n"
			"function OnDestroy() error('intentional teardown failure') end\n");
		PF_CHECK(Tests, TeardownFailureAsset.has_value());
		if (!TeardownFailureAsset) return;
		Scene TeardownFailureScene;
		const auto TeardownFailureEntity = TeardownFailureScene.CreateEntity("Teardown failure reload");
		PF_CHECK(Tests, TeardownFailureEntity &&
			TeardownFailureEntity->SetScript(ScriptComponent{ TeardownFailureAsset->ID }).has_value());
		if (!TeardownFailureEntity) return;
		ScriptRuntime TeardownFailureRuntime(*ProjectResult);
		PF_CHECK(Tests, TeardownFailureRuntime.Start(TeardownFailureScene).has_value());
		PF_CHECK(Tests, TeardownFailureRuntime.Advance(TeardownFailureScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, TeardownFailureEntity->GetTransform()->Translation.x == 5.0f);
		PF_CHECK(Tests, WriteScriptSource(TeardownFailureAsset->ID,
			"function OnCreate() assert(entity:set_translation(0, 8, 0)) end\nfunction OnUpdate() end\n"));
		const auto TeardownFailureTicket = TeardownFailureRuntime.RequestReload(*TeardownFailureEntity);
		PF_CHECK(Tests, TeardownFailureTicket.has_value());
		if (!TeardownFailureTicket) return;
		const auto TeardownFailureCount = TeardownFailureRuntime.ServicePendingReloads(TeardownFailureScene);
		PF_CHECK(Tests, TeardownFailureCount && *TeardownFailureCount == 0);
		PF_CHECK(Tests, TeardownFailureRuntime.GetDiagnostics().size() == 2 &&
			TeardownFailureRuntime.GetDiagnostics().back().Code == ScriptDiagnosticCode::ReloadActivationFailed);
		PF_CHECK(Tests, TeardownFailureRuntime.Advance(TeardownFailureScene, Timestep(1.0 / 60.0)).has_value());
		const auto TeardownFailureTransform = TeardownFailureEntity->GetTransform();
		PF_CHECK(Tests, TeardownFailureTransform && TeardownFailureTransform->Translation.x == 5.0f &&
			TeardownFailureTransform->Translation.y == 0.0f);
		TeardownFailureRuntime.Stop();

		const auto ThrownPropertyRead = ScriptRuntimeDetail::CaptureScriptFloatPropertyRead(
			[]() -> std::expected<PropertyValue, PropertyAccessError> { throw std::runtime_error("deterministic host lookup failure"); });
		PF_CHECK(Tests, ThrownPropertyRead.State == ScriptRuntimeDetail::ScriptFloatPropertyReadState::Failed);
		PF_CHECK(Tests, std::string_view(ThrownPropertyRead.Failure.data()) == "deterministic host lookup failure");
		const auto UnknownPropertyRead = ScriptRuntimeDetail::CaptureScriptFloatPropertyRead(
			[]()
			{
				return std::expected<PropertyValue, PropertyAccessError>{ std::unexpected(
					PropertyAccessError{ PropertyAccessErrorCode::UnknownProperty, "unknown authored float" }) };
			});
		PF_CHECK(Tests, UnknownPropertyRead.State == ScriptRuntimeDetail::ScriptFloatPropertyReadState::Missing);

		const auto RelativeMotionScriptAsset = CreateScriptAsset("relative_motion.lua",
			"local elapsed = 0\n"
			"local initialX, initialY, initialZ = 0, 0, 0\n"
			"function OnCreate()\n"
			"  initialX, initialY, initialZ = entity:get_translation()\n"
			"  assert(initialX ~= nil and initialY ~= nil and initialZ ~= nil)\n"
			"end\n"
			"function OnUpdate(dt)\n"
			"  elapsed = elapsed + dt\n"
			"  assert(entity:set_translation(initialX + math.sin(elapsed) * 0.25, initialY, initialZ))\n"
			"end\n");
		PF_CHECK(Tests, RelativeMotionScriptAsset.has_value());
		if (!RelativeMotionScriptAsset) return;

		Scene RelativeMotionScene;
		const auto OriginalMover = RelativeMotionScene.CreateEntity("Original mover");
		PF_CHECK(Tests, OriginalMover.has_value());
		if (!OriginalMover) return;
		TransformComponent OriginalTransform;
		OriginalTransform.Translation = { 3.0f, 2.0f, -4.0f };
		PF_CHECK(Tests, OriginalMover->SetTransform(OriginalTransform).has_value());
		PF_CHECK(Tests, OriginalMover->SetScript(ScriptComponent{ RelativeMotionScriptAsset->ID }).has_value());
		const auto DuplicatedMover = RelativeMotionScene.DuplicateEntity(*OriginalMover);
		PF_CHECK(Tests, DuplicatedMover.has_value());
		if (!DuplicatedMover) return;
		TransformComponent DuplicatedTransform;
		DuplicatedTransform.Translation = { -7.0f, 5.0f, 11.0f };
		PF_CHECK(Tests, DuplicatedMover->SetTransform(DuplicatedTransform).has_value());
		ScriptRuntime RelativeMotionRuntime(*ProjectResult);
		PF_CHECK(Tests, RelativeMotionRuntime.Start(RelativeMotionScene).has_value());
		PF_CHECK(Tests, RelativeMotionRuntime.Advance(RelativeMotionScene, Timestep(0.5)).has_value());
		const auto OriginalMovedTransform = OriginalMover->GetTransform();
		const auto DuplicatedMovedTransform = DuplicatedMover->GetTransform();
		const float ExpectedOscillation = std::sin(0.5f) * 0.25f;
		PF_CHECK(Tests, OriginalMovedTransform &&
							std::abs(OriginalMovedTransform->Translation.x - (3.0f + ExpectedOscillation)) < 0.0001f &&
							std::abs(OriginalMovedTransform->Translation.y - 2.0f) < 0.0001f &&
							std::abs(OriginalMovedTransform->Translation.z + 4.0f) < 0.0001f);
		PF_CHECK(Tests, DuplicatedMovedTransform &&
							std::abs(DuplicatedMovedTransform->Translation.x - (-7.0f + ExpectedOscillation)) < 0.0001f &&
							std::abs(DuplicatedMovedTransform->Translation.y - 5.0f) < 0.0001f &&
							std::abs(DuplicatedMovedTransform->Translation.z - 11.0f) < 0.0001f);
		PF_CHECK(Tests, RelativeMotionRuntime.GetDiagnostics().empty());
		RelativeMotionRuntime.Stop();

		std::filesystem::create_directories(ProjectRoot / "Assets/Prefabs", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		const auto PrefabChildScriptAsset =
			CreateScriptAsset("prefab_child.lua", "function OnCreate() assert(entity:set_translation(9, 8, 7)) end\n");
		PF_CHECK(Tests, PrefabChildScriptAsset.has_value());
		if (!PrefabChildScriptAsset) return;

		Scene ScriptSpawnPrefabSource;
		const auto ScriptSpawnPrefabRoot = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab root");
		const auto ScriptSpawnPrefabChild = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab child");
		PF_CHECK(Tests, ScriptSpawnPrefabRoot && ScriptSpawnPrefabChild);
		if (!ScriptSpawnPrefabRoot || !ScriptSpawnPrefabChild) return;
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetParent(*ScriptSpawnPrefabRoot).has_value());
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetScript(ScriptComponent{ PrefabChildScriptAsset->ID }).has_value());
		const auto ScriptSpawnPrefab = PrefabAssetService::Create(ProjectResult->GetAssetRegistry(), ProjectResult->GetRootPath(),
			"Assets/Prefabs/ScriptSpawn.prefab", ScriptSpawnPrefabSource, *ScriptSpawnPrefabRoot);
		PF_CHECK(Tests, ScriptSpawnPrefab.has_value());
		if (!ScriptSpawnPrefab) return;

		const std::string SceneMutationScript = "local CreatedEntity\n"
												"local SpawnedPrefab\n"
												"local Destroyed = false\n"
												"function OnCreate()\n"
												"  local invalidName, nameMessage = scene.create_entity(17)\n"
												"  assert(invalidName == nil and nameMessage ~= nil)\n"
												"  CreatedEntity, nameMessage = scene.create_entity('Lua-created entity')\n"
												"  assert(CreatedEntity, nameMessage)\n"
												"  local found = scene.find_entity(CreatedEntity:uuid())\n"
												"  assert(found ~= nil and found:uuid() == CreatedEntity:uuid())\n"
												"  local absent, absentMessage = CreatedEntity:get_float_property('missing')\n"
												"  assert(absent == nil and type(absentMessage) == 'string')\n"
												"  local invalidPrefab, prefabMessage = scene.spawn_prefab('not-a-uuid')\n"
												"  assert(invalidPrefab == nil and prefabMessage ~= nil)\n"
												"  local wrongType = scene.spawn_prefab('" +
												TransformScriptAsset->ID.ToString() +
												"')\n"
												"  assert(wrongType == nil)\n"
												"  SpawnedPrefab, prefabMessage = scene.spawn_prefab('" +
												ScriptSpawnPrefab->ID.ToString() +
												"')\n"
												"  assert(SpawnedPrefab, prefabMessage)\n"
												"end\n"
												"function OnUpdate()\n"
												"  if not Destroyed then\n"
												"    assert(CreatedEntity:destroy())\n"
												"    Destroyed = true\n"
												"    assert(scene.find_entity(CreatedEntity:uuid()) == nil)\n"
												"    local x, staleMessage = CreatedEntity:get_translation()\n"
												"    assert(x == nil and staleMessage ~= nil)\n"
												"    local value, propertyMessage = CreatedEntity:get_float_property('missing')\n"
												"    assert(value == nil and type(propertyMessage) == 'string')\n"
												"    local transient, message = scene.create_entity('Transient Lua entity')\n"
												"    assert(transient, message)\n"
												"    assert(transient:destroy())\n"
												"    assert(entity:destroy())\n"
												"  end\n"
												"  assert(SpawnedPrefab:set_translation(4, 5, 6))\n"
												"end\n"
												"function OnDestroy()\n"
												"  local x, staleMessage = entity:get_translation()\n"
												"  assert(x == nil and staleMessage ~= nil)\n"
												"end\n";
		const auto SceneMutationScriptAsset = CreateScriptAsset("scene_mutation.lua", SceneMutationScript);
		PF_CHECK(Tests, SceneMutationScriptAsset.has_value());
		if (!SceneMutationScriptAsset) return;

		Scene SceneMutationTestScene;
		const auto SceneMutationController = SceneMutationTestScene.CreateEntity("Scene mutation controller");
		PF_CHECK(Tests, SceneMutationController.has_value());
		if (!SceneMutationController) return;
		PF_CHECK(Tests, SceneMutationController->SetScript(ScriptComponent{ SceneMutationScriptAsset->ID }).has_value());
		ScriptRuntime SceneMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, SceneMutationRuntime.Start(SceneMutationTestScene).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 4);

		std::optional<Entity> SpawnedPrefabRoot;
		std::optional<Entity> SpawnedPrefabChild;
		for (const Entity& Current : SceneMutationTestScene.GetEntities())
		{
			const auto Tag = Current.GetTag();
			if (Tag && Tag->Name == "Script-spawned prefab root") SpawnedPrefabRoot = Current;
			else if (Tag && Tag->Name == "Script-spawned prefab child") SpawnedPrefabChild = Current;
		}
		PF_CHECK(Tests, SpawnedPrefabRoot.has_value() && SpawnedPrefabChild.has_value());
		if (SpawnedPrefabRoot && SpawnedPrefabChild)
		{
			const auto SpawnedChildParent = SpawnedPrefabChild->GetParent();
			PF_CHECK(Tests,
				SpawnedChildParent && SpawnedChildParent->has_value() && (**SpawnedChildParent).GetUUID() == SpawnedPrefabRoot->GetUUID());
		}

		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 2);
		if (SpawnedPrefabRoot)
		{
			const auto SpawnedTransform = SpawnedPrefabRoot->GetTransform();
			PF_CHECK(Tests, SpawnedTransform && std::abs(SpawnedTransform->Translation.x - 4.0f) < 0.0001f &&
								std::abs(SpawnedTransform->Translation.y - 5.0f) < 0.0001f &&
								std::abs(SpawnedTransform->Translation.z - 6.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 2);
		if (SpawnedPrefabChild)
		{
			const auto SpawnedChildTransform = SpawnedPrefabChild->GetTransform();
			PF_CHECK(Tests, SpawnedChildTransform && std::abs(SpawnedChildTransform->Translation.x - 9.0f) < 0.0001f &&
								std::abs(SpawnedChildTransform->Translation.y - 8.0f) < 0.0001f &&
								std::abs(SpawnedChildTransform->Translation.z - 7.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		SceneMutationRuntime.Stop();

		const UUID LifecycleControllerID{ 0x5500000000000000ull, 1 };
		const UUID LifecycleVictimID{ 0x5500000000000000ull, 2 };
		const auto DestroyLaterScript = CreateScriptAsset("destroy_later.lua", "function OnUpdate()\n"
																			   "  local victim = scene.find_entity('" +
																				   LifecycleVictimID.ToString() +
																				   "')\n"
																				   "  if victim then assert(victim:destroy()) end\n"
																				   "end\n");
		const auto LifecycleVictimScript =
			CreateScriptAsset("lifecycle_victim.lua", "function OnUpdate()\n"
													  "  local marker, message = scene.create_entity('victim-updated')\n"
													  "  assert(marker, message)\n"
													  "end\n"
													  "function OnDestroy()\n"
													  "  local marker, message = scene.create_entity('victim-destroyed')\n"
													  "  assert(marker, message)\n"
													  "end\n");
		PF_CHECK(Tests, DestroyLaterScript && LifecycleVictimScript);
		if (!DestroyLaterScript || !LifecycleVictimScript) return;
		Scene CallbackMutationScene;
		const auto LifecycleController = CallbackMutationScene.CreateEntityWithUUID(LifecycleControllerID, "Lifecycle controller");
		const auto LifecycleVictim = CallbackMutationScene.CreateEntityWithUUID(LifecycleVictimID, "Lifecycle victim");
		PF_CHECK(Tests, LifecycleController && LifecycleVictim);
		if (!LifecycleController || !LifecycleVictim) return;
		PF_CHECK(Tests, LifecycleController->SetScript(ScriptComponent{ DestroyLaterScript->ID }).has_value());
		PF_CHECK(Tests, LifecycleVictim->SetScript(ScriptComponent{ LifecycleVictimScript->ID }).has_value());
		ScriptRuntime CallbackMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, CallbackMutationRuntime.Start(CallbackMutationScene).has_value());
		PF_CHECK(Tests, CallbackMutationRuntime.GetScriptCount() == 2);
		PF_CHECK(Tests, CallbackMutationRuntime.Advance(CallbackMutationScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, !CallbackMutationScene.FindEntity(LifecycleVictimID).has_value());
		PF_CHECK(Tests, CallbackMutationRuntime.GetScriptCount() == 1);
		bool SawVictimDestroyed = false;
		bool SawVictimUpdated = false;
		for (const Entity& Current : CallbackMutationScene.GetEntities())
		{
			const auto Tag = Current.GetTag();
			if (!Tag) continue;
			SawVictimDestroyed = SawVictimDestroyed || Tag->Name == "victim-destroyed";
			SawVictimUpdated = SawVictimUpdated || Tag->Name == "victim-updated";
		}
		PF_CHECK(Tests, SawVictimDestroyed && !SawVictimUpdated);
		PF_CHECK(Tests, CallbackMutationRuntime.GetDiagnostics().empty());
		CallbackMutationRuntime.Stop();

		const UUID StartupControllerID{ 0x5500000000000000ull, 3 };
		const UUID StartupVictimID{ 0x5500000000000000ull, 4 };
		const auto DestroyAtStartupScript =
			CreateScriptAsset("destroy_at_startup.lua", "function OnCreate()\n"
														"  local victim = scene.find_entity('" +
															StartupVictimID.ToString() +
															"')\n"
															"  if victim then assert(victim:destroy()) end\n"
															"end\n");
		PF_CHECK(Tests, DestroyAtStartupScript.has_value());
		if (!DestroyAtStartupScript) return;
		Scene StartupMutationScene;
		const auto StartupController = StartupMutationScene.CreateEntityWithUUID(StartupControllerID, "Startup controller");
		const auto StartupVictim = StartupMutationScene.CreateEntityWithUUID(StartupVictimID, "Startup victim");
		PF_CHECK(Tests, StartupController && StartupVictim);
		if (!StartupController || !StartupVictim) return;
		PF_CHECK(Tests, StartupController->SetScript(ScriptComponent{ DestroyAtStartupScript->ID }).has_value());
		PF_CHECK(Tests, StartupVictim->SetScript(ScriptComponent{ LifecycleVictimScript->ID }).has_value());
		ScriptRuntime StartupMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, StartupMutationRuntime.Start(StartupMutationScene).has_value());
		PF_CHECK(Tests, !StartupMutationScene.FindEntity(StartupVictimID).has_value());
		PF_CHECK(Tests, StartupMutationRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, StartupMutationRuntime.GetDiagnostics().empty());
		StartupMutationRuntime.Stop();

		const UUID ReusedScriptEntityID{ 0x5500000000000000ull, 5 };
		const auto OldIncarnationScript =
			CreateScriptAsset("old_incarnation.lua", "function OnDestroy()\n"
													 "  local oldValue, propertyMessage = entity:get_float_property('speed')\n"
													 "  assert(oldValue == nil and propertyMessage ~= nil)\n"
													 "  local replacement = scene.find_entity(entity:uuid())\n"
													 "  if replacement then assert(replacement:set_translation(99, 0, 0)) end\n"
													 "  local marker, message = scene.create_entity('old-incarnation-destroyed')\n"
													 "  assert(marker, message)\n"
													 "end\n");
		const auto ReplacementIncarnationScript =
			CreateScriptAsset("replacement_incarnation.lua", "local updates = 0\n"
															 "function OnCreate()\n"
															 "  local x = entity:get_translation()\n"
															 "  assert(x == 10)\n"
															 "end\n"
															 "function OnUpdate()\n"
															 "  updates = updates + 1\n"
															 "  assert(entity:set_translation(updates, 0, 0))\n"
															 "end\n");
		PF_CHECK(Tests, OldIncarnationScript && ReplacementIncarnationScript);
		if (!OldIncarnationScript || !ReplacementIncarnationScript) return;
		Scene ReusedScriptScene;
		const auto OldIncarnation = ReusedScriptScene.CreateEntityWithUUID(ReusedScriptEntityID, "Old script incarnation");
		PF_CHECK(Tests, OldIncarnation.has_value());
		if (!OldIncarnation) return;
		PF_CHECK(Tests, OldIncarnation->SetScript(ScriptComponent{ OldIncarnationScript->ID }).has_value());
		ScriptRuntime ReusedScriptRuntime(*ProjectResult);
		PF_CHECK(Tests, ReusedScriptRuntime.Start(ReusedScriptScene).has_value());
		PF_CHECK(Tests, ReusedScriptRuntime.Advance(ReusedScriptScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedScriptScene.DestroyEntity(*OldIncarnation).has_value());
		const auto NewIncarnation = ReusedScriptScene.CreateEntityWithUUID(ReusedScriptEntityID, "New script incarnation");
		PF_CHECK(Tests, NewIncarnation.has_value());
		if (!NewIncarnation) return;
		TransformComponent NewIncarnationTransform;
		NewIncarnationTransform.Translation.x = 10.0f;
		PF_CHECK(Tests, NewIncarnation->SetTransform(NewIncarnationTransform).has_value());
		PF_CHECK(Tests, NewIncarnation->SetScript(ScriptComponent{ ReplacementIncarnationScript->ID }).has_value());
		PF_CHECK(Tests, ReusedScriptRuntime.Advance(ReusedScriptScene, Timestep(1.0 / 60.0)).has_value());
		const auto NewIncarnationResult = NewIncarnation->GetTransform();
		PF_CHECK(Tests, NewIncarnationResult && std::abs(NewIncarnationResult->Translation.x - 1.0f) < 0.0001f);
		PF_CHECK(Tests, ReusedScriptScene.GetEntityCount() == 2);
		PF_CHECK(Tests, ReusedScriptRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, ReusedScriptRuntime.GetDiagnostics().empty());
		ReusedScriptRuntime.Stop();

		const UUID FailedThenReusedID{ 0x5500000000000000ull, 6 };
		const UUID AttachmentBlockerID{ 0x5500000000000000ull, 7 };
		const auto RetryOnNewIncarnationScript =
			CreateScriptAsset("retry_new_incarnation.lua", "function OnCreate()\n"
														   "  if scene.find_entity('" +
															   AttachmentBlockerID.ToString() +
															   "') then error('blocked attachment') end\n"
															   "end\n"
															   "function OnUpdate() assert(entity:set_translation(7, 0, 0)) end\n");
		PF_CHECK(Tests, RetryOnNewIncarnationScript.has_value());
		if (!RetryOnNewIncarnationScript) return;
		Scene FailedAttachmentScene;
		const auto AttachmentBlocker = FailedAttachmentScene.CreateEntityWithUUID(AttachmentBlockerID, "Attachment blocker");
		const auto FailedIncarnation = FailedAttachmentScene.CreateEntityWithUUID(FailedThenReusedID, "Failed incarnation");
		PF_CHECK(Tests, AttachmentBlocker && FailedIncarnation);
		if (!AttachmentBlocker || !FailedIncarnation) return;
		PF_CHECK(Tests, FailedIncarnation->SetScript(ScriptComponent{ RetryOnNewIncarnationScript->ID }).has_value());
		ScriptRuntime FailedAttachmentRuntime(*ProjectResult);
		PF_CHECK(Tests, FailedAttachmentRuntime.Start(FailedAttachmentScene).has_value());
		PF_CHECK(Tests, FailedAttachmentRuntime.GetScriptCount() == 0);
		PF_CHECK(Tests, FailedAttachmentRuntime.GetDiagnostics().size() == 1);
		FailedAttachmentRuntime.ClearDiagnostics();
		PF_CHECK(Tests, FailedAttachmentScene.DestroyEntity(*AttachmentBlocker).has_value());
		PF_CHECK(Tests, FailedAttachmentScene.DestroyEntity(*FailedIncarnation).has_value());
		const auto ReusedFailedIncarnation = FailedAttachmentScene.CreateEntityWithUUID(FailedThenReusedID, "Recovered incarnation");
		PF_CHECK(Tests, ReusedFailedIncarnation.has_value());
		if (!ReusedFailedIncarnation) return;
		PF_CHECK(Tests, ReusedFailedIncarnation->SetScript(ScriptComponent{ RetryOnNewIncarnationScript->ID }).has_value());
		PF_CHECK(Tests, FailedAttachmentRuntime.Advance(FailedAttachmentScene, Timestep(1.0 / 60.0)).has_value());
		const auto RecoveredTransform = ReusedFailedIncarnation->GetTransform();
		PF_CHECK(Tests, RecoveredTransform && std::abs(RecoveredTransform->Translation.x - 7.0f) < 0.0001f);
		PF_CHECK(Tests, FailedAttachmentRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, FailedAttachmentRuntime.GetDiagnostics().empty());
		FailedAttachmentRuntime.Stop();

		Scene TestScene;
		const auto FirstEntity = TestScene.CreateEntity("First scripted entity");
		const auto SecondEntity = TestScene.CreateEntity("Second scripted entity");
		const auto DisabledEntity = TestScene.CreateEntity("Disabled scripted entity");
		PF_CHECK(Tests, FirstEntity && SecondEntity && DisabledEntity);
		if (!FirstEntity || !SecondEntity || !DisabledEntity) return;
		PF_CHECK(Tests, FirstEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, SecondEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, DisabledEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID, false }).has_value());

		ScriptRuntime Runtime(*ProjectResult);
		const auto BeforeStart = Runtime.Advance(TestScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeStart && BeforeStart.error().Code == ScriptRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetScriptCount() == 2);
		PF_CHECK(Tests, Runtime.GetDiagnostics().empty());
		const auto DuplicateStart = Runtime.Start(TestScene);
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == ScriptRuntimeErrorCode::AlreadyRunning);
		Scene OtherScene;
		const auto DifferentScene = Runtime.Advance(OtherScene, Timestep(0.1));
		PF_CHECK(Tests, !DifferentScene && DifferentScene.error().Code == ScriptRuntimeErrorCode::DifferentScene);
		const auto NegativeDelta = Runtime.Advance(TestScene, Timestep(-0.1));
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == ScriptRuntimeErrorCode::InvalidDeltaTime);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.25)).has_value());
		const auto FirstTransform = FirstEntity->GetTransform();
		const auto SecondTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, FirstTransform && std::abs(FirstTransform->Translation.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, SecondTransform && std::abs(SecondTransform->Translation.x - 0.25f) < 0.0001f);

		PF_CHECK(Tests, FirstEntity->RemoveScript().has_value());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, Runtime.GetScriptCount() == 1);
		const auto DetachedTransform = FirstEntity->GetTransform();
		const auto ActiveTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, DetachedTransform && DetachedTransform->Translation.x == -1.0f);
		PF_CHECK(Tests, ActiveTransform && std::abs(ActiveTransform->Translation.x - 0.35f) < 0.0001f);
		Runtime.Stop();
		PF_CHECK(Tests, !Runtime.IsRunning() && Runtime.GetScriptCount() == 0);
		const auto StoppedTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, StoppedTransform && StoppedTransform->Translation.x == -1.0f);
		Runtime.Stop();

		Scene CoordinatedScene;
		const auto CoordinatedScriptEntity = CoordinatedScene.CreateEntity("Coordinated script");
		const auto CoordinatedPhysicsEntity = CoordinatedScene.CreateEntity("Coordinated body");
		PF_CHECK(Tests, CoordinatedScriptEntity && CoordinatedPhysicsEntity);
		if (!CoordinatedScriptEntity || !CoordinatedPhysicsEntity) return;
		PF_CHECK(Tests, CoordinatedScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		RuntimeDescription.Physics.FixedStepSeconds = 0.1;
		RuntimeDescription.Physics.MaxSubsteps = 2;
		RuntimeDescription.Physics.MaxFrameDeltaSeconds = 0.25;
		DiagnosticRecorder RuntimeDiagnostics(32);
		SceneRuntimeServices RuntimeServices{ .Diagnostics = &RuntimeDiagnostics };
		SceneRuntime CoordinatedRuntime(*ProjectResult, RuntimeDescription, RuntimeServices);
		const auto BeforeRuntimeStart = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeRuntimeStart && BeforeRuntimeStart.error().Code == SceneRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, CoordinatedRuntime.Start(CoordinatedScene).has_value());
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning());
		PF_CHECK(Tests, RuntimeDiagnostics.CaptureSnapshot().Events.empty());
		SceneRuntime* const RunningRuntimeIdentity = &CoordinatedRuntime;
		RuntimeDiagnostics.SetCaptureEnabled(true);
		const auto DuplicateRuntimeStart = CoordinatedRuntime.Start(CoordinatedScene);
		PF_CHECK(Tests, !DuplicateRuntimeStart && DuplicateRuntimeStart.error().Code == SceneRuntimeErrorCode::AlreadyRunning);
		const auto CoordinatedNegativeDelta = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(-0.1));
		PF_CHECK(Tests, !CoordinatedNegativeDelta && CoordinatedNegativeDelta.error().Code == SceneRuntimeErrorCode::InvalidDeltaTime);
		const auto CoordinatedUpdate = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.25));
		PF_CHECK(Tests, CoordinatedUpdate && CoordinatedUpdate->PhysicsSteps == 2);
		const auto RuntimeDiagnosticsSnapshot = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, RuntimeDiagnosticsSnapshot.Events.size() == 3);
		const uint64_t FirstCaptureSessionId = RuntimeDiagnosticsSnapshot.SessionId;
		bool SawScriptAdvance = false;
		bool SawPhysicsAdvance = false;
		bool SawAudioAdvance = false;
		for (const DiagnosticEvent& Event : RuntimeDiagnosticsSnapshot.Events)
		{
			SawScriptAdvance |= Event.Owner == "ScriptRuntime" && Event.Name == "ScriptRuntime::Advance";
			SawPhysicsAdvance |= Event.Owner == "PhysicsSceneRuntime" && Event.Name == "PhysicsSceneRuntime::Advance";
			SawAudioAdvance |= Event.Owner == "AudioSceneRuntime" && Event.Name == "AudioSceneRuntime::Advance";
		}
		PF_CHECK(Tests, SawScriptAdvance && SawPhysicsAdvance && SawAudioAdvance);
		RuntimeDiagnostics.SetCaptureEnabled(false);
		const DiagnosticSnapshot FirstStoppedCapture = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, FirstStoppedCapture.CaptureEndTimestampNanoseconds != 0);
		PF_CHECK(Tests, CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, RuntimeDiagnostics.CaptureSnapshot().Events.size() == RuntimeDiagnosticsSnapshot.Events.size());
		const auto GameplayAtCaptureRestart = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsAtCaptureRestart = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, GameplayAtCaptureRestart && std::abs(GameplayAtCaptureRestart->Translation.x - 0.35f) < 0.0001f);
		PF_CHECK(Tests, PhysicsAtCaptureRestart.has_value());
		RuntimeDiagnostics.SetCaptureEnabled(true);
		const DiagnosticSnapshot SecondCaptureStarted = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, SecondCaptureStarted.SessionId > FirstCaptureSessionId);
		PF_CHECK(Tests, SecondCaptureStarted.Events.empty());
		PF_CHECK(Tests, SecondCaptureStarted.CaptureEndTimestampNanoseconds == 0);
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning() && &CoordinatedRuntime == RunningRuntimeIdentity);
		PF_CHECK(Tests, CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1)).has_value());
		const DiagnosticSnapshot SecondCaptureEvents = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, SecondCaptureEvents.Events.size() == 3);
		for (const DiagnosticEvent& Event : SecondCaptureEvents.Events)
			PF_CHECK(Tests, Event.SessionId == SecondCaptureStarted.SessionId);
		const auto GameplayAfterCaptureRestart = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsAfterCaptureRestart = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, GameplayAfterCaptureRestart && std::abs(GameplayAfterCaptureRestart->Translation.x - 0.45f) < 0.0001f);
		PF_CHECK(Tests, PhysicsAtCaptureRestart && PhysicsAfterCaptureRestart &&
			PhysicsAfterCaptureRestart->Translation.y < PhysicsAtCaptureRestart->Translation.y);
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning() && &CoordinatedRuntime == RunningRuntimeIdentity);
		const auto CoordinatedTransform = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsTransform = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedTransform && std::abs(CoordinatedTransform->Translation.x - 0.45f) < 0.0001f);
		PF_CHECK(Tests, PhysicsTransform && PhysicsTransform->Translation.y < 0.0f);
		Scene OtherRuntimeScene;
		const auto WrongRuntimeScene = CoordinatedRuntime.Advance(OtherRuntimeScene, Timestep(0.1));
		PF_CHECK(Tests, !WrongRuntimeScene && WrongRuntimeScene.error().Code == SceneRuntimeErrorCode::DifferentScene);
		CoordinatedRuntime.Stop();
		PF_CHECK(Tests, !CoordinatedRuntime.IsRunning());
		const auto CoordinatedStoppedTransform = CoordinatedScriptEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedStoppedTransform && CoordinatedStoppedTransform->Translation.x == -1.0f);

		const auto PhysicsPoseScriptAsset =
			CreateScriptAsset("physics_pose.lua", "function OnCreate() assert(entity:set_translation(4, 10, 0)) end\n"
												  "function OnUpdate() assert(entity:set_translation(6, 10, 0)) end\n");
		PF_CHECK(Tests, PhysicsPoseScriptAsset.has_value());
		if (!PhysicsPoseScriptAsset) return;
		Scene PhysicsPoseScene;
		const auto PhysicsPoseEntity = PhysicsPoseScene.CreateEntity("Script-teleported body");
		PF_CHECK(Tests, PhysicsPoseEntity.has_value());
		if (!PhysicsPoseEntity) return;
		PF_CHECK(Tests, PhysicsPoseEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, PhysicsPoseEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, PhysicsPoseEntity->SetScript(ScriptComponent{ PhysicsPoseScriptAsset->ID }).has_value());
		SceneRuntime PhysicsPoseRuntime(*ProjectResult, RuntimeDescription);
		PF_CHECK(Tests, PhysicsPoseRuntime.Start(PhysicsPoseScene).has_value());
		const auto OnCreatePose = PhysicsPoseEntity->GetTransform();
		PF_CHECK(Tests, OnCreatePose && std::abs(OnCreatePose->Translation.x - 4.0f) < 0.0001f &&
							std::abs(OnCreatePose->Translation.y - 10.0f) < 0.0001f);
		const auto PhysicsPoseFrame = PhysicsPoseRuntime.Advance(PhysicsPoseScene, Timestep(0.1));
		const auto OnUpdatePose = PhysicsPoseEntity->GetTransform();
		PF_CHECK(Tests, PhysicsPoseFrame && PhysicsPoseFrame->PhysicsSteps == 1);
		PF_CHECK(Tests, OnUpdatePose && std::abs(OnUpdatePose->Translation.x - 6.0f) < 0.0001f && OnUpdatePose->Translation.y > 9.8f &&
							OnUpdatePose->Translation.y < 10.0f);
		PhysicsPoseRuntime.Stop();

		const auto PhysicsQueryAsset = CreateScriptAsset("physics_query.lua", "function OnUpdate() end\n");
		PF_CHECK(Tests, PhysicsQueryAsset.has_value());
		if (!PhysicsQueryAsset) return;
		Scene PhysicsQueryScene;
		const UUID PhysicsQueryTargetID{ 0x6600000000000000ull, 1 };
		const auto PhysicsQueryTarget = PhysicsQueryScene.CreateEntityWithUUID(PhysicsQueryTargetID, "Query target");
		const auto PhysicsQueryCaller = PhysicsQueryScene.CreateEntity("Query caller");
		PF_CHECK(Tests, PhysicsQueryTarget && PhysicsQueryCaller);
		if (!PhysicsQueryTarget || !PhysicsQueryCaller) return;
		TransformComponent PhysicsQueryTargetTransform;
		PhysicsQueryTargetTransform.Translation.x = 4.0f;
		PF_CHECK(Tests, PhysicsQueryTarget->SetTransform(PhysicsQueryTargetTransform));
		PF_CHECK(Tests, PhysicsQueryTarget->SetRigidbody({ RigidbodyMotionType::Static }));
		PF_CHECK(Tests, PhysicsQueryTarget->SetCollider(BoxColliderComponent{}));
		PF_CHECK(Tests, PhysicsQueryCaller->SetScript(ScriptComponent{ PhysicsQueryAsset->ID }));
		const std::string PhysicsQuerySource =
			"function OnUpdate()\n"
			"  local hit = physics.raycast(0, 0, 0, 3, 0, 0, 20)\n"
			"  assert(hit ~= nil and hit.uuid == '" + PhysicsQueryTargetID.ToString() + "')\n"
			"  assert(hit.entity:uuid() == hit.uuid and hit.fraction > 0)\n"
			"  local overlap = physics.overlap_sphere(0.75, 4, 0, 0, 8)\n"
			"  assert(overlap ~= nil and #overlap.hits > 0 and overlap.truncated == false)\n"
			"  local cast = physics.cast_box(0.25, 0.25, 0.25, 0, 0, 0, 1, 0, 0, 20)\n"
			"  assert(cast ~= nil and cast.uuid == hit.uuid)\n"
			"  assert(entity:set_translation(42, 0, 0))\n"
			"end\n";
		PF_CHECK(Tests, WriteScriptSource(PhysicsQueryAsset->ID, PhysicsQuerySource));
		SceneRuntimeDesc PhysicsQueryDescription = RuntimeDescription;
		PhysicsQueryDescription.Physics.Gravity = glm::vec3(0.0f);
		SceneRuntime PhysicsQueryRuntime(*ProjectResult, PhysicsQueryDescription);
		PF_CHECK(Tests, PhysicsQueryRuntime.Start(PhysicsQueryScene));
		PF_CHECK(Tests, PhysicsQueryRuntime.Advance(PhysicsQueryScene, Timestep(0.1)));
		const auto PhysicsQueryCallerTransform = PhysicsQueryCaller->GetTransform();
		PF_CHECK(Tests, PhysicsQueryCallerTransform && PhysicsQueryCallerTransform->Translation.x == 42.0f);
		PF_CHECK(Tests, PhysicsQueryRuntime.GetScriptDiagnostics().empty());
		PhysicsQueryRuntime.Stop();

		const UUID CollisionFloorID{ 0x6500000000004000ull, 0x8000000000000001ull };
		const UUID CollisionActorID{ 0x6500000000004000ull, 0x8000000000000002ull };
		const auto CollisionCallbackAsset = CreateScriptAsset("collision_callback.lua", "function OnCollisionBegin(other)\n"
			"  assert(other:uuid() == '" + CollisionFloorID.ToString() + "')\n"
			"  assert(entity:set_translation(11, 0, 0))\n"
			"end\n");
		PF_CHECK(Tests, CollisionCallbackAsset.has_value());
		if (!CollisionCallbackAsset) return;
		Scene CollisionCallbackScene;
		const auto CollisionFloor = CollisionCallbackScene.CreateEntityWithUUID(CollisionFloorID, "Collision floor");
		const auto CollisionActor = CollisionCallbackScene.CreateEntityWithUUID(CollisionActorID, "Collision callback actor");
		PF_CHECK(Tests, CollisionFloor && CollisionActor);
		if (!CollisionFloor || !CollisionActor) return;
		PF_CHECK(Tests, CollisionFloor->SetRigidbody({ RigidbodyMotionType::Static }));
		PF_CHECK(Tests, CollisionFloor->SetCollider(BoxColliderComponent{}));
		PF_CHECK(Tests, CollisionActor->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, false }));
		PF_CHECK(Tests, CollisionActor->SetCollider(BoxColliderComponent{}));
		PF_CHECK(Tests, CollisionActor->SetScript(ScriptComponent{ CollisionCallbackAsset->ID }));
		SceneRuntimeDesc CollisionDescription = PhysicsQueryDescription;
		CollisionDescription.Physics.FixedStepSeconds = 1.0 / 60.0;
		SceneRuntime CollisionRuntime(*ProjectResult, CollisionDescription);
		PF_CHECK(Tests, CollisionRuntime.Start(CollisionCallbackScene));
		PF_CHECK(Tests, CollisionRuntime.Advance(CollisionCallbackScene, Timestep(1.0 / 60.0)));
		const auto CollisionCallbackTransform = CollisionActor->GetTransform();
		PF_CHECK(Tests, CollisionCallbackTransform && CollisionCallbackTransform->Translation.x == 11.0f);
		PF_CHECK(Tests, CollisionRuntime.GetScriptDiagnostics().empty());
		CollisionRuntime.Stop();

		constexpr AssetID DeferredContactSceneAsset{ 0x6800000000004000ull, 0x8000000000000001ull };
		const auto DestroyingContactAsset = CreateScriptAsset("destroy_on_trigger.lua", "function OnTriggerEnter(other)\n"
			"  assert(other:uuid() == '66000000-0000-4000-8000-000000000001')\n"
			"  local x, message = other:get_translation()\n"
			"  assert(x == nil and message ~= nil)\n"
			"  assert(entity:destroy())\n"
			"  assert(scene.request_transition('" + DeferredContactSceneAsset.ToString() + "'))\n"
			"end\n");
		const auto DestroyedOtherContactAsset = CreateScriptAsset("destroy_other_on_trigger.lua", "function OnTriggerEnter(other)\n"
			"  assert(other:uuid() == '67000000-0000-4000-8000-000000000001')\n"
			"  assert(entity:destroy())\n"
			"end\n");
		PF_CHECK(Tests, DestroyingContactAsset && DestroyedOtherContactAsset);
		if (!DestroyingContactAsset || !DestroyedOtherContactAsset) return;
		Scene ContactScene;
		const UUID TriggerID{ 0x6700000000004000ull, 0x8000000000000001ull };
		const UUID ContactBodyID{ 0x6600000000004000ull, 0x8000000000000001ull };
		const auto ContactTrigger = ContactScene.CreateEntityWithUUID(TriggerID, "Trigger with callback");
		const auto ContactBody = ContactScene.CreateEntityWithUUID(ContactBodyID, "Overlapping dynamic body");
		PF_CHECK(Tests, ContactTrigger && ContactBody);
		if (!ContactTrigger || !ContactBody) return;
		BoxColliderComponent TriggerCollider;
		TriggerCollider.Shape = ColliderShape::Sphere;
		TriggerCollider.IsSensor = true;
		PF_CHECK(Tests, ContactTrigger->SetRigidbody({ RigidbodyMotionType::Static }));
		PF_CHECK(Tests, ContactTrigger->SetCollider(TriggerCollider));
		PF_CHECK(Tests, ContactTrigger->SetScript(ScriptComponent{ DestroyingContactAsset->ID }));
		PF_CHECK(Tests, ContactBody->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, false }));
		PF_CHECK(Tests, ContactBody->SetCollider(BoxColliderComponent{}));
		PF_CHECK(Tests, ContactBody->SetScript(ScriptComponent{ DestroyedOtherContactAsset->ID }));
		std::optional<AssetID> QueuedContactTransition;
		struct ContactObservation final
		{
			uint32_t TriggerCallbacks = 0;
			uint32_t ActorCallbacks = 0;
			uint32_t TriggerEntityCallbacks = 0;
			static void Observe(void* Context, const ScriptRuntimeObservation& Observation) noexcept
			{
				auto& State = *static_cast<ContactObservation*>(Context);
				if (Observation.Kind == ScriptRuntimeObservationKind::CallbackEntered &&
					Observation.Callback == ScriptRuntimeCallback::OnTriggerEnter)
				{
					++State.TriggerCallbacks;
					if (Observation.Entity == UUID{ 0x6600000000004000ull, 0x8000000000000001ull }) ++State.ActorCallbacks;
					if (Observation.Entity == UUID{ 0x6700000000004000ull, 0x8000000000000001ull }) ++State.TriggerEntityCallbacks;
				}
			}
		} ContactCallbacks;
		SceneRuntimeServices ContactHostServices;
		ContactHostServices.QueueSceneTransition = [&](const AssetID& Requested)
		{
			QueuedContactTransition = Requested;
			return true;
		};
		ContactHostServices.ScriptObserver = { &ContactCallbacks, ContactObservation::Observe };
		SceneRuntime ContactRuntime(*ProjectResult, PhysicsQueryDescription, ContactHostServices);
		PF_CHECK(Tests, ContactRuntime.Start(ContactScene));
		PF_CHECK(Tests, ContactRuntime.Advance(ContactScene, Timestep(0.1)));
		PF_CHECK(Tests, !ContactTrigger->IsValid() && !ContactBody->IsValid());
		if (ContactCallbacks.TriggerCallbacks != 2 || ContactCallbacks.ActorCallbacks != 1 ||
			ContactCallbacks.TriggerEntityCallbacks != 1 || !QueuedContactTransition || ContactScene.GetEntityCount() != 0)
			std::cerr << "G17_CONTACT_DISPATCH entered=" << ContactCallbacks.TriggerCallbacks << " actor=" <<
				ContactCallbacks.ActorCallbacks << " trigger=" << ContactCallbacks.TriggerEntityCallbacks << " transition=" <<
				QueuedContactTransition.has_value() << " entities=" << ContactScene.GetEntityCount() << std::endl;
		PF_CHECK(Tests, QueuedContactTransition == DeferredContactSceneAsset && ContactRuntime.IsRunning());
		PF_CHECK(Tests, ContactCallbacks.TriggerCallbacks == 2 && ContactCallbacks.ActorCallbacks == 1 &&
			ContactCallbacks.TriggerEntityCallbacks == 1);
		PF_CHECK(Tests, ContactRuntime.GetScriptDiagnostics().empty());
		const auto ContactBoundaryRequest = ContactRuntime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, ContactBoundaryRequest &&
			ContactBoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::SceneTransition &&
			ContactBoundaryRequest->Scene == DeferredContactSceneAsset);
		if (ContactBoundaryRequest)
		{
			auto StaleBoundaryRequest = *ContactBoundaryRequest;
			++StaleBoundaryRequest.SessionGeneration;
			PF_CHECK(Tests, !ContactRuntime.ResolveBoundaryRequest(
				StaleBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
			const uint32_t CallbacksBeforeSuspendedFrame = ContactCallbacks.TriggerCallbacks;
			const auto SuspendedContactFrame = ContactRuntime.Advance(ContactScene, Timestep(0.1));
			PF_CHECK(Tests, SuspendedContactFrame && SuspendedContactFrame->PhysicsSteps == 0);
			PF_CHECK(Tests, ContactCallbacks.TriggerCallbacks == CallbacksBeforeSuspendedFrame);
			PF_CHECK(Tests, ContactRuntime.ResolveBoundaryRequest(
				*ContactBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
			PF_CHECK(Tests, !ContactRuntime.ResolveBoundaryRequest(
				*ContactBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
			PF_CHECK(Tests, !ContactRuntime.GetPendingBoundaryRequest());
			PF_CHECK(Tests, ContactRuntime.Advance(ContactScene, Timestep(1.0 / 60.0)).has_value());
		}
		ContactRuntime.Stop();
		PF_CHECK(Tests, ContactRuntime.Start(ContactScene).has_value());
		if (ContactBoundaryRequest)
			PF_CHECK(Tests, !ContactRuntime.ResolveBoundaryRequest(
				*ContactBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
		ContactRuntime.Stop();

		const auto FirstBoundaryRequestScript = CreateScriptAsset("first_boundary_request.lua",
			"function OnUpdate()\n"
			"  local queued, message = scene.request_transition('" + DeferredContactSceneAsset.ToString() + "')\n"
			"  assert(queued, message)\n"
			"end\n");
		const auto SecondBoundaryRequestScript = CreateScriptAsset("second_boundary_request.lua",
			"function OnUpdate()\n"
			"  local queued, message = scene.request_transition('" + DeferredContactSceneAsset.ToString() + "')\n"
			"  assert(queued, message)\n"
			"end\n");
		PF_CHECK(Tests, FirstBoundaryRequestScript.has_value() && SecondBoundaryRequestScript.has_value());
		if (!FirstBoundaryRequestScript || !SecondBoundaryRequestScript) return;
		Scene StaleAcknowledgementScene;
		const auto StaleAcknowledgementEntity = StaleAcknowledgementScene.CreateEntity("Boundary acknowledgement identity");
		PF_CHECK(Tests, StaleAcknowledgementEntity.has_value());
		if (!StaleAcknowledgementEntity) return;
		PF_CHECK(Tests, StaleAcknowledgementEntity->SetScript(ScriptComponent{ FirstBoundaryRequestScript->ID }).has_value());
		SceneRuntimeServices StaleAcknowledgementServices;
		StaleAcknowledgementServices.QueueSceneTransition = [](const AssetID&) { return true; };
		SceneRuntime StaleAcknowledgementRuntime(*ProjectResult, PhysicsQueryDescription, StaleAcknowledgementServices);
		PF_CHECK(Tests, StaleAcknowledgementRuntime.Start(StaleAcknowledgementScene).has_value());
		const auto FirstRequestFrame = StaleAcknowledgementRuntime.Advance(StaleAcknowledgementScene, Timestep(0.0));
		PF_CHECK(Tests, FirstRequestFrame && FirstRequestFrame->PhysicsSteps == 0);
		const auto FirstBoundaryRequest = StaleAcknowledgementRuntime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, FirstBoundaryRequest.has_value());
		if (!FirstBoundaryRequest) return;
		PF_CHECK(Tests, StaleAcknowledgementRuntime.ResolveBoundaryRequest(
			*FirstBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
		PF_CHECK(Tests, StaleAcknowledgementEntity->SetScript(ScriptComponent{ SecondBoundaryRequestScript->ID }).has_value());
		const auto SecondRequestFrame = StaleAcknowledgementRuntime.Advance(StaleAcknowledgementScene, Timestep(0.0));
		PF_CHECK(Tests, SecondRequestFrame && SecondRequestFrame->PhysicsSteps == 0);
		const auto SecondBoundaryRequest = StaleAcknowledgementRuntime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, SecondBoundaryRequest && FirstBoundaryRequest->SessionGeneration == SecondBoundaryRequest->SessionGeneration &&
			FirstBoundaryRequest->RequestGeneration != SecondBoundaryRequest->RequestGeneration);
		if (SecondBoundaryRequest)
		{
			PF_CHECK(Tests, !StaleAcknowledgementRuntime.ResolveBoundaryRequest(
				*FirstBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
			const auto StillPendingRequest = StaleAcknowledgementRuntime.GetPendingBoundaryRequest();
			PF_CHECK(Tests, StillPendingRequest &&
				StillPendingRequest->SessionGeneration == SecondBoundaryRequest->SessionGeneration &&
				StillPendingRequest->RequestGeneration == SecondBoundaryRequest->RequestGeneration);
			PF_CHECK(Tests, StaleAcknowledgementRuntime.ResolveBoundaryRequest(
				*SecondBoundaryRequest, SceneRuntimeBoundaryResolution::Rejected));
		}
		StaleAcknowledgementRuntime.Stop();

		const auto StopPriorityAsset = CreateScriptAsset("stop_priority.lua",
			"local requested = false\n"
			"function OnUpdate()\n"
			"  if requested then return end\n"
			"  requested = true\n"
			"  assert(scene.request_transition('" + DeferredContactSceneAsset.ToString() + "'))\n"
			"  assert(scene.request_stop())\n"
			"end\n");
		PF_CHECK(Tests, StopPriorityAsset.has_value());
		if (!StopPriorityAsset) return;
		Scene StopPriorityScene;
		const auto StopPriorityEntity = StopPriorityScene.CreateEntity("Stop precedence script");
		PF_CHECK(Tests, StopPriorityEntity && StopPriorityEntity->SetScript(ScriptComponent{ StopPriorityAsset->ID }));
		if (!StopPriorityEntity) return;
		std::optional<AssetID> StopPriorityTransition;
		bool StopPriorityQueued = false;
		SceneRuntimeServices StopPriorityServices;
		StopPriorityServices.QueueSceneTransition = [&StopPriorityTransition](const AssetID& Requested)
		{
			StopPriorityTransition = Requested;
			return true;
		};
		StopPriorityServices.QueueStop = [&StopPriorityQueued]()
		{
			StopPriorityQueued = true;
			return true;
		};
		SceneRuntime StopPriorityRuntime(*ProjectResult, PhysicsQueryDescription, StopPriorityServices);
		PF_CHECK(Tests, StopPriorityRuntime.Start(StopPriorityScene).has_value());
		const auto StopPriorityFrame = StopPriorityRuntime.Advance(StopPriorityScene, Timestep(0.0));
		PF_CHECK(Tests, StopPriorityFrame && StopPriorityFrame->PhysicsSteps == 0);
		const auto StopPriorityRequest = StopPriorityRuntime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, StopPriorityQueued && StopPriorityTransition == DeferredContactSceneAsset && StopPriorityRequest &&
			StopPriorityRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop && !StopPriorityRequest->Scene);
		if (StopPriorityRequest)
		{
			auto SupersededTransition = *StopPriorityRequest;
			SupersededTransition.Kind = SceneRuntimeBoundaryRequestKind::SceneTransition;
			SupersededTransition.Scene = DeferredContactSceneAsset;
			--SupersededTransition.RequestGeneration;
			PF_CHECK(Tests, !StopPriorityRuntime.ResolveBoundaryRequest(
				SupersededTransition, SceneRuntimeBoundaryResolution::Rejected));
			PF_CHECK(Tests, StopPriorityRuntime.ResolveBoundaryRequest(
				*StopPriorityRequest, SceneRuntimeBoundaryResolution::Cancelled));
			PF_CHECK(Tests, StopPriorityRuntime.Advance(StopPriorityScene, Timestep(1.0 / 60.0)).has_value());
		}
		StopPriorityRuntime.Stop();

		constexpr std::string_view ForceScript = R"(
function OnUpdate()
  local applied, message = entity:apply_force(0, 100, 0)
  assert(applied, message)
end
)";
		const auto ForceScriptAsset = CreateScriptAsset("force.lua", ForceScript);
		PF_CHECK(Tests, ForceScriptAsset.has_value());
		if (!ForceScriptAsset) return;
		Scene ScriptedPhysicsScene;
		const auto ScriptedBody = ScriptedPhysicsScene.CreateEntity("Script-driven body");
		PF_CHECK(Tests, ScriptedBody.has_value());
		if (!ScriptedBody) return;
		PF_CHECK(Tests, ScriptedBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetScript(ScriptComponent{ ForceScriptAsset->ID }).has_value());
		SceneRuntime ScriptedPhysicsRuntime(*ProjectResult, RuntimeDescription);
		PF_CHECK(Tests, ScriptedPhysicsRuntime.Start(ScriptedPhysicsScene).has_value());
		const auto ScriptedPhysicsFrame = ScriptedPhysicsRuntime.Advance(ScriptedPhysicsScene, Timestep(0.1));
		const auto ScriptedBodyTransform = ScriptedBody->GetTransform();
		PF_CHECK(Tests, ScriptedPhysicsFrame && ScriptedPhysicsFrame->PhysicsSteps == 1);
		PF_CHECK(Tests, ScriptedBodyTransform && ScriptedBodyTransform->Translation.y > 0.0f);
		ScriptedPhysicsRuntime.Stop();

		const UUID ReusedServiceEntityID{ 0x5500000000000000ull, 0x51 };
		const auto StaleServiceScriptAsset = CreateScriptAsset("stale_services.lua", R"(
function OnDestroy()
  local applied, forceError = entity:apply_force(0, 100, 0)
  local played, playError = entity:play_audio()
  local paused, pauseError = entity:pause_audio()
  local resumed, resumeError = entity:resume_audio()
  local stopped, stopError = entity:stop_audio()
  assert(applied == nil and string.find(forceError, "no longer valid", 1, true) ~= nil)
  assert(played == nil and string.find(playError, "no longer valid", 1, true) ~= nil)
  assert(paused == nil and string.find(pauseError, "no longer valid", 1, true) ~= nil)
  assert(resumed == nil and string.find(resumeError, "no longer valid", 1, true) ~= nil)
  assert(stopped == nil and string.find(stopError, "no longer valid", 1, true) ~= nil)
end
)");
		const auto FreshServiceScriptAsset = CreateScriptAsset("fresh_services.lua", R"(
function OnUpdate()
  assert(entity:apply_force(0, 100, 0))
  assert(entity:play_audio())
end
function OnDestroy()
  assert(entity:apply_force(0, 1, 0))
  assert(entity:play_audio())
end
)");
		PF_CHECK(Tests, StaleServiceScriptAsset && FreshServiceScriptAsset);
		if (!StaleServiceScriptAsset || !FreshServiceScriptAsset) return;

		const std::vector<std::byte> ServiceAudioWave = MakeSilentScriptTestWave();
		const auto ServiceAudioAsset =
			AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(), ProjectResult->GetRootPath(),
				std::as_bytes(std::span(ServiceAudioWave.data(), ServiceAudioWave.size())), "Assets/Scripts/service_test.wav");
		PF_CHECK(Tests, ServiceAudioAsset.has_value());
		if (!ServiceAudioAsset) return;

		auto ServiceAudioEngineResult = AudioEngine::Create({ AudioOutputBackend::Null });
		PF_CHECK(Tests, ServiceAudioEngineResult.has_value());
		if (!ServiceAudioEngineResult) return;
		AudioEngine ServiceAudioEngine = std::move(*ServiceAudioEngineResult);
		AudioAssetCache ServiceAudioCache(ServiceAudioEngine, ProjectResult->GetRootPath(), ProjectResult->GetAssetRegistry());
		AudioSceneRuntime ServiceAudioRuntime(ServiceAudioEngine, ServiceAudioCache);
		PhysicsSceneRuntime ServicePhysicsRuntime(RuntimeDescription.Physics);
		Scene ReusedServiceScene;
		const auto OriginalServiceEntity = ReusedServiceScene.CreateEntityWithUUID(ReusedServiceEntityID, "Original service entity");
		PF_CHECK(Tests, OriginalServiceEntity.has_value());
		if (!OriginalServiceEntity) return;
		PF_CHECK(Tests, OriginalServiceEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, OriginalServiceEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests,
			OriginalServiceEntity->SetAudioSource(AudioSourceComponent{ ServiceAudioAsset->ID, 1.0f, false, false, true }).has_value());
		PF_CHECK(Tests, OriginalServiceEntity->SetScript(ScriptComponent{ StaleServiceScriptAsset->ID }).has_value());
		PF_CHECK(Tests, ServicePhysicsRuntime.Start(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.Start(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 0);
		ScriptRuntimeServices ReusedServiceBindings{ .Physics = &ServicePhysicsRuntime, .Audio = &ServiceAudioRuntime };
		ScriptRuntime ReusedServiceScriptRuntime(*ProjectResult, {}, ReusedServiceBindings);
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Start(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScene.DestroyEntity(*OriginalServiceEntity).has_value());
		const auto ReplacementServiceEntity = ReusedServiceScene.CreateEntityWithUUID(ReusedServiceEntityID, "Replacement service entity");
		PF_CHECK(Tests, ReplacementServiceEntity.has_value());
		if (!ReplacementServiceEntity) return;
		PF_CHECK(Tests, ReplacementServiceEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ReplacementServiceEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests,
			ReplacementServiceEntity->SetAudioSource(AudioSourceComponent{ ServiceAudioAsset->ID, 1.0f, false, false, false }).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, ServiceAudioRuntime.ProcessPendingCommands().has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 0);
		const auto NoStaleForceFrame = ServicePhysicsRuntime.Advance(ReusedServiceScene, Timestep(0.1));
		const auto NoStaleForceTransform = ReplacementServiceEntity->GetTransform();
		PF_CHECK(Tests, NoStaleForceFrame && *NoStaleForceFrame == 1);
		PF_CHECK(Tests, NoStaleForceTransform && NoStaleForceTransform->Translation.y < 0.0f);
		PF_CHECK(Tests, ServiceAudioRuntime.Advance(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 0);

		PF_CHECK(Tests, ReplacementServiceEntity->SetScript(ScriptComponent{ FreshServiceScriptAsset->ID }).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		// The first pass binds the replacement entity incarnation; its first OnUpdate runs on the next advance.
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, ServiceAudioRuntime.ProcessPendingCommands().has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.Advance(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 1);
		const auto FreshServiceForceFrame = ServicePhysicsRuntime.Advance(ReusedServiceScene, Timestep(0.1));
		const auto FreshServiceTransform = ReplacementServiceEntity->GetTransform();
		PF_CHECK(Tests, FreshServiceForceFrame && *FreshServiceForceFrame == 1);
		PF_CHECK(Tests,
			FreshServiceTransform && NoStaleForceTransform && FreshServiceTransform->Translation.y > NoStaleForceTransform->Translation.y);
		ReusedServiceScriptRuntime.Stop();
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		ServiceAudioRuntime.Stop();
		ServicePhysicsRuntime.Stop();

		constexpr std::string_view UnavailableServicesScript = R"(
function OnUpdate()
  local applied, forceMessage = entity:apply_force(0, 1, 0)
  assert(applied == nil and string.find(forceMessage, "unavailable", 1, true) ~= nil)
  local played, audioMessage = entity:play_audio()
  assert(played == nil and string.find(audioMessage, "unavailable", 1, true) ~= nil)
  local pressed, inputMessage = input.is_key_pressed(87)
  assert(pressed == nil and string.find(inputMessage, "unavailable", 1, true) ~= nil)
end
)";
		const auto UnavailableServicesAsset = CreateScriptAsset("services_unavailable.lua", UnavailableServicesScript);
		PF_CHECK(Tests, UnavailableServicesAsset.has_value());
		if (!UnavailableServicesAsset) return;
		Scene UnavailableServicesScene;
		const auto UnavailableServicesEntity = UnavailableServicesScene.CreateEntity("No runtime services");
		PF_CHECK(Tests, UnavailableServicesEntity.has_value());
		if (!UnavailableServicesEntity) return;
		PF_CHECK(Tests, UnavailableServicesEntity->SetScript(ScriptComponent{ UnavailableServicesAsset->ID }).has_value());
		ScriptRuntime UnavailableServicesRuntime(*ProjectResult);
		PF_CHECK(Tests, UnavailableServicesRuntime.Start(UnavailableServicesScene).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.Advance(UnavailableServicesScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.GetDiagnostics().empty());
		UnavailableServicesRuntime.Stop();

		constexpr std::string_view InputScript = R"(
local Updated = false
function OnCreate()
  local invalidKey, keyMessage = input.is_key_pressed("W")
  assert(invalidKey == nil and keyMessage ~= nil)
  local invalidButton, buttonMessage = input.is_mouse_button_pressed("left")
  assert(invalidButton == nil and buttonMessage ~= nil)
end
function OnUpdate()
  local x, y = input.get_mouse_position()
  if not Updated then
    assert(input.is_key_pressed(87))
    assert(not input.is_key_pressed(83))
    assert(input.is_mouse_button_pressed(0))
    assert(not input.is_mouse_button_pressed(1))
    assert(x == 12.5 and y == 9.25)
    Updated = true
  else
    assert(not input.is_key_pressed(87))
    assert(not input.is_mouse_button_pressed(0))
    assert(x == 14.5 and y == 7.25)
  end
  assert(entity:set_translation(x, y, 0))
end
)";
		const auto InputScriptAsset = CreateScriptAsset("input.lua", InputScript);
		PF_CHECK(Tests, InputScriptAsset.has_value());
		if (!InputScriptAsset) return;

		Input TestInput;
		KeyPressedEvent WPressed(87, 0);
		MouseButtonPressedEvent LeftPressed(0);
		MouseMovedEvent InitialMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(WPressed);
		TestInput.OnEvent(LeftPressed);
		TestInput.OnEvent(InitialMousePosition);

		Scene InputTestScene;
		const auto InputTestEntity = InputTestScene.CreateEntity("Input-driven script");
		PF_CHECK(Tests, InputTestEntity && InputTestEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!InputTestEntity) return;
		ScriptRuntimeServices InputServices;
		InputServices.InputState = &TestInput;
		ScriptRuntime InputRuntime(*ProjectResult, {}, InputServices);
		PF_CHECK(Tests, InputRuntime.Start(InputTestScene).has_value());
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		KeyReleasedEvent WReleased(87);
		MouseButtonReleasedEvent LeftReleased(0);
		MouseMovedEvent UpdatedMousePosition(14.5f, 7.25f);
		TestInput.OnEvent(WReleased);
		TestInput.OnEvent(LeftReleased);
		TestInput.OnEvent(UpdatedMousePosition);
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		const auto InputUpdatedTransform = InputTestEntity->GetTransform();
		PF_CHECK(Tests, InputUpdatedTransform && std::abs(InputUpdatedTransform->Translation.x - 14.5f) < 0.0001f &&
							std::abs(InputUpdatedTransform->Translation.y - 7.25f) < 0.0001f);
		InputRuntime.Stop();

		const auto ActionInputScriptAsset = CreateScriptAsset("action_input.lua", R"(
local updates = 0
function OnUpdate()
  updates = updates + 1
  if updates == 1 then
    assert(input.was_action_pressed("Jump"))
    assert(input.is_action_held("Jump"))
    local x, y = input.get_vector("Move")
    assert(x == 1 and y == 0)
    assert(input.get_axis("Throttle") == 1)
    local axis, axisMessage = input.get_axis("Jump")
    assert(axis == nil and axisMessage ~= nil)
    local unknown, unknownMessage = input.is_action_held("Missing")
    assert(unknown == nil and unknownMessage ~= nil)
  else
    assert(not input.is_action_held("Jump"))
    assert(input.was_action_released("Jump"))
  end
end
)" );
		PF_CHECK(Tests, ActionInputScriptAsset.has_value());
		if (!ActionInputScriptAsset) return;
		const auto ActionInputObserverAsset = CreateScriptAsset("action_input_observer.lua", R"(
local updates = 0
function OnUpdate()
  updates = updates + 1
  if updates == 1 then
    assert(input.was_action_pressed("Jump"))
    assert(input.is_action_held("Jump"))
  else
    assert(input.was_action_released("Jump"))
    assert(not input.is_action_held("Jump"))
  end
end
)" );
		PF_CHECK(Tests, ActionInputObserverAsset.has_value());
		if (!ActionInputObserverAsset) return;

		GameplayInputService GameplayInput;
		GameplayActionMap ActionMap;
		GameplayActionDefinition JumpAction;
		JumpAction.Id = "Jump";
		JumpAction.Type = ActionValueType::Digital;
		ActionBinding JumpBinding;
		JumpBinding.Id = "Jump.primary";
		JumpBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) };
		JumpAction.Bindings.push_back(JumpBinding);
		ActionMap.Actions.push_back(JumpAction);
		GameplayActionDefinition MoveAction;
		MoveAction.Id = "Move";
		MoveAction.Type = ActionValueType::Axis2D;
		ActionBinding MoveRight;
		MoveRight.Id = "Move.right";
		MoveRight.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::D) };
		MoveRight.ScaleX = 1.0f;
		ActionBinding MoveUp;
		MoveUp.Id = "Move.up";
		MoveUp.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::W) };
		MoveUp.ScaleY = 1.0f;
		MoveAction.Bindings = { MoveRight, MoveUp };
		ActionMap.Actions.push_back(MoveAction);
		GameplayActionDefinition ThrottleAction;
		ThrottleAction.Id = "Throttle";
		ThrottleAction.Type = ActionValueType::Axis1D;
		ActionBinding ThrottleBinding;
		ThrottleBinding.Id = "Throttle.key";
		ThrottleBinding.Kind = ActionBindingKind::Axis;
		ThrottleBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::E) };
		ThrottleAction.Bindings.push_back(ThrottleBinding);
		ActionMap.Actions.push_back(ThrottleAction);
		PF_CHECK(Tests, GameplayInput.SetActionMap(std::move(ActionMap)).has_value());
		InputRoutingState ActionRouting;
		ActionRouting.FocusedWindow = 17;
		ActionRouting.Contexts[0] = { .Context = InputContext::Gameplay,
			.Consumes = InputDeviceMask::None, .OwnerWindow = 17, .Active = true };
		ActionRouting.ContextCount = 1;
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 17 }));
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::D) }, .Window = 17 }));
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::E) }, .Window = 17 }));
		GameplayInput.PublishFrame(ActionRouting);
		PF_CHECK(Tests, GameplayInput.FindAction("Jump") && GameplayInput.FindAction("Jump")->Held &&
			GameplayInput.FindAction("Jump")->Pressed);
		PF_CHECK(Tests, GameplayInput.FindAction("Move") && GameplayInput.FindAction("Move")->X == 1.0f);
		PF_CHECK(Tests, GameplayInput.FindAction("Throttle") && GameplayInput.FindAction("Throttle")->X == 1.0f);

		Scene ActionInputScene;
		const auto ActionInputEntity = ActionInputScene.CreateEntity("Action input consumer");
		const auto ActionInputObserver = ActionInputScene.CreateEntity("Shared snapshot observer");
		PF_CHECK(Tests, ActionInputEntity && ActionInputObserver);
		if (!ActionInputEntity || !ActionInputObserver) return;
		PF_CHECK(Tests, ActionInputEntity->SetScript(ScriptComponent{ ActionInputScriptAsset->ID }).has_value());
		PF_CHECK(Tests, ActionInputObserver->SetScript(ScriptComponent{ ActionInputObserverAsset->ID }).has_value());
		ScriptRuntimeServices ActionInputServices;
		ActionInputServices.GameplayInput = &GameplayInput;
		ScriptRuntime ActionInputRuntime(*ProjectResult, {}, ActionInputServices);
		PF_CHECK(Tests, ActionInputRuntime.Start(ActionInputScene).has_value());
		PF_CHECK(Tests, ActionInputRuntime.Advance(ActionInputScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ActionInputRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Released,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 17 }));
		GameplayInput.PublishFrame(ActionRouting);
		PF_CHECK(Tests, ActionInputRuntime.Advance(ActionInputScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ActionInputRuntime.GetDiagnostics().empty());
		ActionInputRuntime.Stop();

		KeyPressedEvent CoordinatorWPressed(87, 0);
		MouseButtonPressedEvent CoordinatorLeftPressed(0);
		MouseMovedEvent CoordinatorMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(CoordinatorWPressed);
		TestInput.OnEvent(CoordinatorLeftPressed);
		TestInput.OnEvent(CoordinatorMousePosition);
		Scene CoordinatedInputScene;
		const auto CoordinatedInputEntity = CoordinatedInputScene.CreateEntity("Coordinated input script");
		PF_CHECK(Tests, CoordinatedInputEntity && CoordinatedInputEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!CoordinatedInputEntity) return;
		SceneRuntimeServices HostServices{ .InputState = &TestInput };
		SceneRuntime CoordinatedInputRuntime(*ProjectResult, RuntimeDescription, HostServices);
		PF_CHECK(Tests, CoordinatedInputRuntime.Start(CoordinatedInputScene).has_value());
		PF_CHECK(Tests, CoordinatedInputRuntime.Advance(CoordinatedInputScene, Timestep(1.0 / 60.0)).has_value());
		const auto CoordinatedInputTransform = CoordinatedInputEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedInputTransform && std::abs(CoordinatedInputTransform->Translation.x - 12.5f) < 0.0001f &&
							std::abs(CoordinatedInputTransform->Translation.y - 9.25f) < 0.0001f);
		CoordinatedInputRuntime.Stop();

		Scene RollbackScene;
		const auto RollbackScriptEntity = RollbackScene.CreateEntity("Rollback script");
		const auto RollbackPhysicsEntity = RollbackScene.CreateEntity("Rollback physics entity");
		PF_CHECK(Tests, RollbackScriptEntity && RollbackPhysicsEntity);
		if (!RollbackScriptEntity || !RollbackPhysicsEntity) return;
		PF_CHECK(Tests, RollbackScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		SceneRuntimeDesc InvalidScriptDescription = RuntimeDescription;
		InvalidScriptDescription.Scripting.MaxSourceBytes = 0;
		SceneRuntime RollbackRuntime(*ProjectResult, InvalidScriptDescription);
		const auto FailedRuntimeStart = RollbackRuntime.Start(RollbackScene);
		PF_CHECK(Tests, !FailedRuntimeStart && FailedRuntimeStart.error().Subsystem == SceneRuntimeSubsystem::Scripting);
		PF_CHECK(Tests, !RollbackRuntime.IsRunning());
		const auto RolledBackTransform = RollbackScriptEntity->GetTransform();
		PF_CHECK(Tests, RolledBackTransform && RolledBackTransform->Translation.x == 0.0f);

		constexpr std::string_view SyntaxErrorScript = "function OnUpdate(\n";
		const auto SyntaxErrorAsset = CreateScriptAsset("syntax_error.lua", SyntaxErrorScript);
		PF_CHECK(Tests, SyntaxErrorAsset.has_value());
		if (!SyntaxErrorAsset) return;
		Scene CompileErrorScene;
		const auto CompileErrorEntity = CompileErrorScene.CreateEntity("Compile error");
		PF_CHECK(Tests, CompileErrorEntity && CompileErrorEntity->SetScript(ScriptComponent{ SyntaxErrorAsset->ID }));
		ScriptRuntime CompileErrorRuntime(*ProjectResult);
		PF_CHECK(Tests, CompileErrorRuntime.Start(CompileErrorScene).has_value());
		PF_CHECK(Tests, CompileErrorRuntime.GetScriptCount() == 0 && CompileErrorRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::CompileFailed);
		CompileErrorRuntime.Stop();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().size() == 1);
		CompileErrorRuntime.ClearDiagnostics();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().empty());

		constexpr std::string_view InfiniteScript = "function OnUpdate() while true do end end\n";
		const auto InfiniteScriptAsset = CreateScriptAsset("instruction_limit.lua", InfiniteScript);
		PF_CHECK(Tests, InfiniteScriptAsset.has_value());
		if (!InfiniteScriptAsset) return;
		Scene InfiniteScene;
		const auto InfiniteEntity = InfiniteScene.CreateEntity("Instruction-limited script");
		PF_CHECK(Tests, InfiniteEntity && InfiniteEntity->SetScript(ScriptComponent{ InfiniteScriptAsset->ID }));
		ScriptRuntimeDesc LimitedDescription;
		LimitedDescription.MaxInstructionsPerCallback = 1000;
		ScriptRuntime LimitedRuntime(*ProjectResult, LimitedDescription);
		PF_CHECK(Tests, LimitedRuntime.Start(InfiniteScene).has_value());
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::UpdateFailed);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Message.find("instruction budget") != std::string::npos);
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		LimitedRuntime.Stop();

		constexpr std::string_view MemoryLimitScript = R"(
function OnCreate()
  local values = {}
  while true do
    values[#values + 1] = string.rep("x", 4096)
  end
end
)";
		const auto MemoryScriptAsset = CreateScriptAsset("memory_limit.lua", MemoryLimitScript);
		PF_CHECK(Tests, MemoryScriptAsset.has_value());
		if (!MemoryScriptAsset) return;
		Scene MemoryScene;
		const auto MemoryEntity = MemoryScene.CreateEntity("Memory-limited script");
		PF_CHECK(Tests, MemoryEntity && MemoryEntity->SetScript(ScriptComponent{ MemoryScriptAsset->ID }));
		ScriptRuntimeDesc MemoryDescription;
		MemoryDescription.MaxLuaMemoryBytes = 512u * 1024u;
		MemoryDescription.MaxTotalLuaMemoryBytes = 2u * 1024u * 1024u;
		ScriptRuntime MemoryRuntime(*ProjectResult, MemoryDescription);
		PF_CHECK(Tests, MemoryRuntime.Start(MemoryScene).has_value());
		PF_CHECK(Tests, MemoryRuntime.GetScriptCount() == 0 && MemoryRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, MemoryRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::InitializationFailed);
		MemoryRuntime.Stop();
	}
} // namespace

void RunScriptRuntimeTests(TestRunner& Tests)
{
	TestScriptRuntime(Tests);
}
