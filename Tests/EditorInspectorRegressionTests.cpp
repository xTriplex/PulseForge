#include "TestEditorSupport.h"

#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorAudioPreviewController.h"
#include "Editor/EditorInspectorPanel.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"

#include "Audio/AudioSession.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	std::vector<std::byte> MakePreviewWave(uint32_t Frequency)
	{
		constexpr uint32_t SampleRate = 48000;
		constexpr uint32_t SampleCount = 48000;
		constexpr uint16_t Channels = 1;
		constexpr uint16_t BitsPerSample = 16;
		constexpr uint16_t BlockAlign = Channels * (BitsPerSample / 8);
		constexpr uint32_t DataSize = SampleCount * BlockAlign;
		std::vector<std::byte> Data(44 + DataSize);
		const auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		const auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		const auto Write32 = [&Data](size_t Offset, uint32_t Value)
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
			const int16_t Sample = static_cast<int16_t>(std::lround(std::sin(Phase) * 12000.0));
			Write16(44 + static_cast<size_t>(Frame) * BlockAlign, static_cast<uint16_t>(Sample));
		}
		return Data;
	}

	bool RenderPreviewHasSignal(PulseForge::AudioSession& Session)
	{
		constexpr size_t FrameCount = 4096;
		std::array<float, FrameCount * 2> Samples{};
		const auto Rendered = Session.GetEngine().RenderOfflineFrames(Samples);
		if (!Rendered || *Rendered != FrameCount) return false;
		double SumSquares = 0.0;
		for (size_t Frame = FrameCount / 2; Frame < FrameCount; ++Frame)
		{
			SumSquares += Samples[Frame * 2] * Samples[Frame * 2];
			SumSquares += Samples[Frame * 2 + 1] * Samples[Frame * 2 + 1];
		}
		const double Rms = std::sqrt(SumSquares / FrameCount);
		return Rms > 0.02;
	}

	float RenderPreviewToneMagnitude(PulseForge::AudioSession& Session, uint32_t Frequency)
	{
		constexpr size_t FrameCount = 256;
		std::array<float, FrameCount * 2> Samples{};
		const auto Rendered = Session.GetEngine().RenderOfflineFrames(Samples);
		if (!Rendered || *Rendered != FrameCount) return 0.0f;

		double InPhase = 0.0;
		double Quadrature = 0.0;
		constexpr double TwoPi = 6.28318530717958647692;
		for (size_t Frame = 0; Frame < FrameCount; ++Frame)
		{
			const double Phase = TwoPi * Frequency * Frame / 48000.0;
			const double Sample = Samples[Frame * 2];
			InPhase += Sample * std::cos(Phase);
			Quadrature += Sample * std::sin(Phase);
		}
		return static_cast<float>(2.0 * std::hypot(InPhase, Quadrature) / FrameCount);
	}

	struct ScopedAudioPreviewProject final
	{
		ScopedAudioPreviewProject() = default;
		ScopedAudioPreviewProject(const ScopedAudioPreviewProject&) = delete;
		ScopedAudioPreviewProject& operator=(const ScopedAudioPreviewProject&) = delete;
		ScopedAudioPreviewProject(ScopedAudioPreviewProject&&) noexcept = default;
		ScopedAudioPreviewProject& operator=(ScopedAudioPreviewProject&&) noexcept = default;

		std::filesystem::path Root;
		std::optional<PulseForge::Project> SourceProject;

		~ScopedAudioPreviewProject()
		{
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}
	};

	std::optional<ScopedAudioPreviewProject> MakeAudioPreviewProject(TestRunner& Tests)
	{
		std::error_code Error;
		const auto TemporaryDirectory = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error) return std::nullopt;
		const auto ProjectID = PulseForge::UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID) return std::nullopt;

		ScopedAudioPreviewProject Project;
		Project.Root = TemporaryDirectory / ("PulseForgeEditorAudioPreview-" + ProjectID->ToString());
		std::filesystem::create_directories(Project.Root / "Assets" / "Audio", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return std::nullopt;
		auto CreatedProject = PulseForge::Project::Create(Project.Root / "Preview.pfproj", "Audio preview regression");
		PF_CHECK(Tests, CreatedProject.has_value());
		if (!CreatedProject) return std::nullopt;
		Project.SourceProject.emplace(std::move(*CreatedProject));
		std::filesystem::create_directories(Project.Root / "Assets" / "Audio", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return std::nullopt;
		auto Rebuilt = Project.SourceProject->GetAssetRegistry().Rebuild(Project.Root);
		PF_CHECK(Tests, Rebuilt.has_value());
		if (!Rebuilt) return std::nullopt;
		return std::optional<ScopedAudioPreviewProject>(std::move(Project));
	}

	std::optional<PulseForge::AssetRecord> AddAudioPreviewAsset(
		TestRunner& Tests,
		ScopedAudioPreviewProject& Project,
		std::span<const std::byte> Contents,
		std::string_view Filename)
	{
		const auto Created = PulseForge::AssetOperations::CreateAssetFromBytes(
			Project.SourceProject->GetAssetRegistry(),
			Project.Root,
			Contents,
			std::filesystem::path("Assets") / "Audio" / std::string(Filename));
		PF_CHECK(Tests, Created.has_value());
		if (!Created) return std::nullopt;
		return *Created;
	}

	void TestEditorAudioPreviewController(TestRunner& Tests)
	{
		using namespace PulseForge;
		auto FirstProject = MakeAudioPreviewProject(Tests);
		PF_CHECK(Tests, FirstProject.has_value());
		if (!FirstProject) return;
		auto SecondProject = MakeAudioPreviewProject(Tests);
		PF_CHECK(Tests, SecondProject.has_value());
		if (!SecondProject) return;

		const std::vector<std::byte> FirstWave = MakePreviewWave(750);
		const std::vector<std::byte> ReplacementWave = MakePreviewWave(1500);
		const std::vector<std::byte> NewProjectWave = MakePreviewWave(1125);
		const std::array<std::byte, 8> InvalidWave{
			std::byte{ 0x6e }, std::byte{ 0x6f }, std::byte{ 0x74 }, std::byte{ 0x2d },
			std::byte{ 0x61 }, std::byte{ 0x2d }, std::byte{ 0x77 }, std::byte{ 0x61 }};
		const auto FirstAsset = AddAudioPreviewAsset(Tests, *FirstProject, FirstWave, "first.wav");
		const auto ReplacementAsset = AddAudioPreviewAsset(Tests, *FirstProject, ReplacementWave, "replacement.wav");
		const auto InvalidAsset = AddAudioPreviewAsset(Tests, *FirstProject, InvalidWave, "invalid.wav");
		const auto NewProjectAsset = AddAudioPreviewAsset(Tests, *SecondProject, NewProjectWave, "new-project.wav");
		PF_CHECK(Tests, FirstAsset && ReplacementAsset && InvalidAsset && NewProjectAsset);
		if (!FirstAsset || !ReplacementAsset || !InvalidAsset || !NewProjectAsset) return;

		AudioSessionDesc FirstSessionDescription;
		FirstSessionDescription.Engine.OutputBackend = AudioOutputBackend::Offline;
		FirstSessionDescription.UserDataDirectory = FirstProject->Root / "UserData";
		auto FirstSessionResult = AudioSession::CreateForProject(*FirstProject->SourceProject, std::move(FirstSessionDescription));
		PF_CHECK(Tests, FirstSessionResult.has_value());
		if (!FirstSessionResult) return;
		std::unique_ptr<AudioSession> FirstSession = std::move(*FirstSessionResult);

		AudioSessionDesc SecondSessionDescription;
		SecondSessionDescription.Engine.OutputBackend = AudioOutputBackend::Offline;
		SecondSessionDescription.UserDataDirectory = SecondProject->Root / "UserData";
		auto SecondSessionResult = AudioSession::CreateForProject(*SecondProject->SourceProject, std::move(SecondSessionDescription));
		PF_CHECK(Tests, SecondSessionResult.has_value());
		if (!SecondSessionResult) return;
		std::unique_ptr<AudioSession> SecondSession = std::move(*SecondSessionResult);

		PulseForgeEditor::EditorAudioPreviewController Preview;
		Preview.SetSession(FirstSession.get());

		const auto FirstStarted = Preview.Play(FirstAsset->ID, AudioBus::Music);
		PF_CHECK(Tests, FirstStarted.has_value());
		PF_CHECK(Tests, Preview.GetActiveAsset() == FirstAsset->ID);
		PF_CHECK(Tests, Preview.IsPlaying());
		PF_CHECK(Tests, RenderPreviewHasSignal(*FirstSession));
		const uint64_t FirstGeneration = Preview.GetGeneration();
		PF_CHECK(Tests, Preview.IsCurrentGeneration(FirstGeneration));

		const auto MissingID = UUID::Generate();
		PF_CHECK(Tests, MissingID.has_value());
		if (!MissingID) return;
		const auto MissingAsset = Preview.Play(*MissingID, AudioBus::Effects);
		PF_CHECK(Tests, !MissingAsset);
		PF_CHECK(Tests, Preview.GetGeneration() == FirstGeneration);
		PF_CHECK(Tests, Preview.GetActiveAsset() == FirstAsset->ID);
		PF_CHECK(Tests, Preview.IsPlaying());
		PF_CHECK(Tests, RenderPreviewHasSignal(*FirstSession));

		const auto DecoderFailure = Preview.Play(InvalidAsset->ID, AudioBus::UI);
		PF_CHECK(Tests, !DecoderFailure);
		PF_CHECK(Tests, Preview.GetGeneration() == FirstGeneration);
		PF_CHECK(Tests, Preview.GetActiveAsset() == FirstAsset->ID);
		PF_CHECK(Tests, Preview.IsPlaying());
		PF_CHECK(Tests, RenderPreviewHasSignal(*FirstSession));

		const auto Replaced = Preview.Play(ReplacementAsset->ID, AudioBus::Effects);
		PF_CHECK(Tests, Replaced.has_value());
		PF_CHECK(Tests, Preview.GetActiveAsset() == ReplacementAsset->ID);
		PF_CHECK(Tests, Preview.IsPlaying());
		PF_CHECK(Tests, RenderPreviewToneMagnitude(*FirstSession, 1500) > 0.1f);
		PF_CHECK(Tests, RenderPreviewToneMagnitude(*FirstSession, 750) < 0.03f);
		const uint64_t ReplacementGeneration = Preview.GetGeneration();
		PF_CHECK(Tests, ReplacementGeneration != FirstGeneration);
		PF_CHECK(Tests, !Preview.IsCurrentGeneration(FirstGeneration));
		PF_CHECK(Tests, Preview.IsCurrentGeneration(ReplacementGeneration));

		const uint64_t BeforeDeviceReplacement = Preview.GetGeneration();
		AudioEngineDesc ReplacementEngineDescription;
		ReplacementEngineDescription.OutputBackend = AudioOutputBackend::Offline;
		auto ReplacementEngine = AudioEngine::Create(ReplacementEngineDescription);
		PF_CHECK(Tests, ReplacementEngine.has_value());
		if (!ReplacementEngine) return;
		FirstSession->GetEngine() = std::move(*ReplacementEngine);
		Preview.ReconcileDeviceGeneration();
		PF_CHECK(Tests, Preview.GetGeneration() > BeforeDeviceReplacement);
		PF_CHECK(Tests, !Preview.GetActiveAsset().has_value());
		PF_CHECK(Tests, !Preview.IsPlaying());
		PF_CHECK(Tests, !RenderPreviewHasSignal(*FirstSession));

		Preview.SetSession(SecondSession.get());
		PF_CHECK(Tests, !Preview.GetActiveAsset().has_value());
		PF_CHECK(Tests, !Preview.IsPlaying());
		PF_CHECK(Tests, !Preview.IsCurrentGeneration(ReplacementGeneration));
		PF_CHECK(Tests, !RenderPreviewHasSignal(*FirstSession));
		const uint64_t ProjectGeneration = Preview.GetGeneration();
		const auto OldProjectAsset = Preview.Play(ReplacementAsset->ID, AudioBus::Music);
		PF_CHECK(Tests, !OldProjectAsset);
		PF_CHECK(Tests, Preview.GetGeneration() == ProjectGeneration);
		PF_CHECK(Tests, !Preview.GetActiveAsset().has_value());

		const auto NewProjectStarted = Preview.Play(NewProjectAsset->ID, AudioBus::UI);
		PF_CHECK(Tests, NewProjectStarted.has_value());
		PF_CHECK(Tests, Preview.GetActiveAsset() == NewProjectAsset->ID);
		PF_CHECK(Tests, Preview.IsPlaying());
		const uint64_t NewProjectGeneration = Preview.GetGeneration();
		Preview.Cancel();
		PF_CHECK(Tests, !Preview.GetActiveAsset().has_value());
		PF_CHECK(Tests, !Preview.IsPlaying());
		PF_CHECK(Tests, !RenderPreviewHasSignal(*SecondSession));
		PF_CHECK(Tests, !Preview.IsCurrentGeneration(NewProjectGeneration));
		PF_CHECK(Tests, Preview.GetGeneration() > NewProjectGeneration);
		Preview.ClearSession();
		const uint64_t ClearedGeneration = Preview.GetGeneration();
		const auto ClearedSessionPlay = Preview.Play(NewProjectAsset->ID, AudioBus::UI);
		PF_CHECK(Tests, !ClearedSessionPlay);
		PF_CHECK(Tests, Preview.GetGeneration() == ClearedGeneration);
	}

	class ScopedInspectorImGuiContext final
	{
	public:
		ScopedInspectorImGuiContext()
		{
			m_Previous = ImGui::GetCurrentContext();
			m_Context = ImGui::CreateContext();
			ImGui::SetCurrentContext(m_Context);
			ImGuiIO& IO = ImGui::GetIO();
			IO.IniFilename = nullptr;
			IO.DisplaySize = ImVec2(960.0f, 720.0f);
			IO.DeltaTime = 1.0f / 60.0f;
			unsigned char* Pixels = nullptr;
			int Width = 0;
			int Height = 0;
			IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
			IO.Fonts->SetTexID(static_cast<ImTextureID>(1));
		}

		~ScopedInspectorImGuiContext()
		{
			ImGui::SetCurrentContext(m_Context);
			ImGui::DestroyContext(m_Context);
			ImGui::SetCurrentContext(m_Previous);
		}

		ScopedInspectorImGuiContext(const ScopedInspectorImGuiContext&) = delete;
		ScopedInspectorImGuiContext& operator=(const ScopedInspectorImGuiContext&) = delete;

	private:
		ImGuiContext* m_Previous = nullptr;
		ImGuiContext* m_Context = nullptr;
	};

	class InspectorActions final : public PulseForgeEditor::EditorInspectorPanelActions
	{
	public:
		void FinishInspectorSceneEdit() override { ++FinishCount; }

		std::expected<void, std::string> RunInspectorSceneEdit(
			std::string, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			++CommandCount;
			return {};
		}

		void ReportInspectorError(std::string Message) override { Errors.push_back(std::move(Message)); }

		int FinishCount = 0;
		int CommandCount = 0;
		std::vector<std::string> Errors;
	};

	ImGuiWindow* FindActiveChildWindow(ImGuiContext& Context, std::string_view NamePart)
	{
		for (ImGuiWindow* Window : Context.Windows)
			if (Window->LastFrameActive == Context.FrameCount && std::string_view(Window->Name).find(NamePart) != std::string_view::npos)
				return Window;
		return nullptr;
	}

	void SetScriptFloatProperties(const PulseForge::Entity& Entity, std::initializer_list<std::pair<std::string, float>> Properties)
	{
		auto Result = Entity.GetScript();
		IM_ASSERT(Result && Result->has_value());
		PulseForge::ScriptComponent Script = **Result;
		Script.FloatProperties.clear();
		for (const auto& [Identifier, Value] : Properties)
			Script.FloatProperties.emplace(Identifier, Value);
		const auto Updated = Entity.SetScript(Script);
		IM_ASSERT(Updated.has_value());
	}

	void DrawInspectorFrame(TestRunner& Tests, PulseForgeEditor::EditorInspectorPanel& Panel,
		PulseForgeEditor::EditorSceneWorkspaceState& Workspace, PulseForgeEditor::EditorStyle& Style,
		PulseForgeEditor::EditorAssetSelector& AssetSelector, PulseForge::Scene& Scene, InspectorActions& Actions,
		const std::vector<PulseForge::AssetRecord>& NoAssets, int ExpectedFloatRows)
	{
		ImGui::GetIO().DisplaySize = ImVec2(960.0f, 720.0f);
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(260.0f, 560.0f), ImGuiCond_Always);
		ImGui::Begin("Inspector regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::GetStateStorage()->SetInt(ImGui::GetID("Mesh Renderer"), 1);
		ImGui::GetStateStorage()->SetInt(ImGui::GetID("Script"), 1);

		ImGuiContext& Context = *ImGui::GetCurrentContext();
		const int InitialColorDepth = Context.ColorStack.Size;
		const int InitialStyleVarDepth = Context.StyleVarStack.Size;
		Panel.DrawContents(&Scene, Workspace, Style, AssetSelector, NoAssets, false, false, Actions);
		PF_CHECK(Tests, Context.CurrentTable == nullptr);
		PF_CHECK(Tests, Context.ColorStack.Size == InitialColorDepth);
		PF_CHECK(Tests, Context.StyleVarStack.Size == InitialStyleVarDepth);

		ImGuiWindow* MeshBody = FindActiveChildWindow(Context, "/MeshRendererComponentBody_");
		ImGuiTable* MeshTable = MeshBody ? Context.Tables.GetByKey(MeshBody->GetID("MeshRendererProperties")) : nullptr;
		PF_CHECK(Tests, MeshTable && MeshTable->LastFrameActive == Context.FrameCount && MeshTable->ColumnsCount == 2 &&
							MeshTable->CurrentRow == 1); // Both Mesh Asset and Material rows were submitted by the panel.

		ImGuiWindow* ScriptBody = FindActiveChildWindow(Context, "/ScriptComponentBody_");
		ImGuiTable* FloatTable = ScriptBody ? Context.Tables.GetByKey(ScriptBody->GetID("ScriptFloatProperties")) : nullptr;
		if (ExpectedFloatRows == 0) PF_CHECK(Tests, !FloatTable || FloatTable->LastFrameActive != Context.FrameCount);
		else
			PF_CHECK(Tests, FloatTable && FloatTable->LastFrameActive == Context.FrameCount && FloatTable->ColumnsCount == 2 &&
								FloatTable->CurrentRow == ExpectedFloatRows - 1);

		ImGui::End();
		ImGui::Render();
		const ImDrawData* DrawData = ImGui::GetDrawData();
		PF_CHECK(Tests, DrawData && DrawData->Valid);
		PF_CHECK(Tests, Context.ColorStack.Size == 0);
		PF_CHECK(Tests, Context.StyleVarStack.Size == 0);
		PF_CHECK(Tests, Actions.Errors.empty());
	}
} // namespace

void RunEditorInspectorRegressionTests(TestRunner& Tests)
{
	ScopedInspectorImGuiContext Context;
	PulseForge::Scene Scene;
	auto Created = Scene.CreateEntity("Inspector regression entity");
	PF_CHECK(Tests, Created.has_value());
	if (!Created) return;
	const PulseForge::Entity Entity = *Created;
	PulseForgeEditor::EditorSceneWorkspaceState Workspace;
	Workspace.Selection().SelectSingle(Entity.GetUUID());
	PulseForgeEditor::EditorInspectorPanel Panel;
	PulseForgeEditor::EditorStyle Style;
	PulseForgeEditor::EditorAssetSelector AssetSelector;
	InspectorActions Actions;
	const std::vector<PulseForge::AssetRecord> NoAssets;
	const PulseForge::AssetID ScriptAsset{ 1, 1 };
	const PulseForge::AssetID MeshAsset{ 2, 2 };
	PF_CHECK(Tests, Entity.SetScript(PulseForge::ScriptComponent{ ScriptAsset }).has_value());
	PF_CHECK(Tests, Entity.SetMeshRenderer(PulseForge::MeshRendererComponent{ MeshAsset, std::nullopt }).has_value());

	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 0);
	SetScriptFloatProperties(Entity, { { "speed", 1.0f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 1);
	SetScriptFloatProperties(Entity, { { "health", 100.0f }, { "speed", 1.0f }, { "weight", 2.5f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 3);
	SetScriptFloatProperties(Entity, { { "health", 100.0f }, { "weight", 2.5f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 2);
	SetScriptFloatProperties(Entity, { { "weight", 2.5f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 1);
	SetScriptFloatProperties(Entity, {});
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 0);
	TestEditorAudioPreviewController(Tests);
}
