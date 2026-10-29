#include "Editor/EditorBuildProfileController.h"

#include "Assets/ExportBuildProfile.h"

#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		class AssetExportServiceRunner final : public EditorBuildProfileExportRunner
		{
		public:
			std::expected<uint64_t, PulseForge::AssetExportError> Start(PulseForge::AssetExportRequest Request) override
			{
				return m_Service.Start(std::move(Request));
			}

			PulseForge::AssetExportSnapshot GetSnapshot(uint64_t RequestID) const override
			{
				return m_Service.GetSnapshot(RequestID);
			}

			bool RequestCancel(uint64_t RequestID) noexcept override
			{
				return m_Service.RequestCancel(RequestID);
			}

			void Shutdown() noexcept override
			{
				m_Service.Shutdown();
			}

		private:
			PulseForge::AssetExportService m_Service;
		};
	} // namespace

	EditorBuildProfileController::EditorBuildProfileController()
		: m_ExportRunner(std::make_unique<AssetExportServiceRunner>())
	{
	}

	EditorBuildProfileController::EditorBuildProfileController(
		std::unique_ptr<EditorBuildProfileExportRunner> ExportRunner)
		: m_ExportRunner(std::move(ExportRunner))
	{
		if (!m_ExportRunner) m_ExportRunner = std::make_unique<AssetExportServiceRunner>();
	}

	EditorBuildProfilePanelState EditorBuildProfileController::GetPanelState(
		const EditorBuildProfileProjectContext& Context) const
	{
		EditorBuildProfilePanelState State;
		State.Profile = m_Profile;
		State.ProfilePath = m_ProfilePath;
		State.TemplateDescriptorPath = m_TemplateDescriptorPath;
		State.ProjectOpen = Context.SavedProjectFile.has_value();
		State.SavedContentReady = State.ProjectOpen && Context.DirtyReason.empty();
		if (m_ExportRequestID) State.Export = m_ExportRunner->GetSnapshot(*m_ExportRequestID);
		State.StatusMessage = m_StatusMessage;
		return State;
	}

	void EditorBuildProfileController::ApplyProfileEdit(std::optional<PulseForge::ExportBuildProfile> EditedProfile)
	{
		if (!EditedProfile || (m_Profile && *EditedProfile == *m_Profile)) return;
		m_Profile = std::move(EditedProfile);
		m_StatusMessage = "Profile has unsaved edits.";
	}

	std::optional<EditorBuildProfileDialogIntent> EditorBuildProfileController::HandleAction(
		const EditorBuildProfileAction& Action,
		const EditorBuildProfileProjectContext& Context)
	{
		const std::filesystem::path InitialDirectory = Context.SavedProjectFile
			? Context.SavedProjectFile->parent_path()
			: std::filesystem::path{};

		switch (Action.Kind)
		{
		case EditorBuildProfileActionKind::None:
			return std::nullopt;
		case EditorBuildProfileActionKind::NewProfile:
		{
			const auto Identifier = PulseForge::UUID::Generate();
			if (!Identifier)
			{
				m_StatusMessage = Identifier.error().Message;
				return std::nullopt;
			}

			PulseForge::ExportBuildProfile Profile;
			Profile.ProfileID = *Identifier;
			Profile.DisplayName = "Build Profile";
			if (Context.ProjectStartupScene) Profile.StartupScene = *Context.ProjectStartupScene;
			else if (Context.CurrentSceneAsset) Profile.StartupScene = *Context.CurrentSceneAsset;
#if defined(__APPLE__)
			Profile.TargetPlatform = PulseForge::ExportTargetPlatform::MacOS;
			Profile.Architecture = PulseForge::ExportTargetArchitecture::Arm64;
#elif defined(_WIN32)
			Profile.TargetPlatform = PulseForge::ExportTargetPlatform::Windows;
			Profile.Architecture = PulseForge::ExportTargetArchitecture::X64;
#else
			Profile.TargetPlatform = PulseForge::ExportTargetPlatform::Linux;
			Profile.Architecture = PulseForge::ExportTargetArchitecture::X64;
#endif
			m_Profile = std::move(Profile);
			m_ProfilePath.clear();
			m_StatusMessage = "New profile created. Save it to persist the profile.";
			return std::nullopt;
		}
		case EditorBuildProfileActionKind::LoadProfile:
			return EditorBuildProfileDialogIntent{
				EditorBuildProfileDialogKind::LoadProfile, InitialDirectory, {}};
		case EditorBuildProfileActionKind::SaveProfile:
			if (!m_Profile)
			{
				m_StatusMessage = "Create or load a profile before saving.";
				return std::nullopt;
			}
			if (m_ProfilePath.empty())
				return EditorBuildProfileDialogIntent{
					EditorBuildProfileDialogKind::SaveProfileAs, InitialDirectory, "Default.pfbuildprofile"};
			SaveProfile();
			return std::nullopt;
		case EditorBuildProfileActionKind::SaveProfileAs:
			if (!m_Profile)
			{
				m_StatusMessage = "Create or load a profile before saving.";
				return std::nullopt;
			}
			return EditorBuildProfileDialogIntent{
				EditorBuildProfileDialogKind::SaveProfileAs, InitialDirectory, "Default.pfbuildprofile"};
		case EditorBuildProfileActionKind::SelectTemplate:
			return EditorBuildProfileDialogIntent{
				EditorBuildProfileDialogKind::SelectTemplate, InitialDirectory, {}};
		case EditorBuildProfileActionKind::StartExport:
			StartExport(Context);
			return std::nullopt;
		case EditorBuildProfileActionKind::CancelExport:
			if (CancelActiveExport("the user requested cancellation"))
				m_StatusMessage = "Export cancellation requested.";
			else
				m_StatusMessage = "Export is no longer cancellable; package publication may be completing.";
			return std::nullopt;
		}
		return std::nullopt;
	}

	void EditorBuildProfileController::ResolveDialogResult(const EditorBuildProfileDialogIntent& Intent,
		const std::optional<std::filesystem::path>& SelectedPath)
	{
		if (!SelectedPath) return;

		switch (Intent.Kind)
		{
		case EditorBuildProfileDialogKind::LoadProfile:
		{
			const auto Loaded = PulseForge::ExportBuildProfileSerializer::Load(*SelectedPath);
			if (!Loaded)
			{
				m_StatusMessage = Loaded.error().Message;
				return;
			}
			m_Profile = *Loaded;
			m_ProfilePath = *SelectedPath;
			m_StatusMessage = "Build profile loaded.";
			return;
		}
		case EditorBuildProfileDialogKind::SaveProfileAs:
		{
			if (!m_Profile)
			{
				m_StatusMessage = "Create or load a profile before saving.";
				return;
			}
			std::filesystem::path Destination = *SelectedPath;
			if (Destination.extension() != ".pfbuildprofile") Destination += ".pfbuildprofile";
			const auto Saved = PulseForge::ExportBuildProfileSerializer::Save(Destination, *m_Profile);
			if (!Saved)
			{
				m_StatusMessage = Saved.error().Message;
				return;
			}
			m_ProfilePath = std::move(Destination);
			m_StatusMessage = "Build profile saved.";
			return;
		}
		case EditorBuildProfileDialogKind::SelectTemplate:
			m_TemplateDescriptorPath = *SelectedPath;
			return;
		}
	}

	void EditorBuildProfileController::ProjectChanged(std::string_view Reason)
	{
		if (!CancelActiveExport(Reason) && m_ExportRequestID && m_ExportRunner->GetSnapshot(*m_ExportRequestID).Running)
			m_StatusMessage = "Export has passed its cancellation boundary and will finish publishing its captured project.";
		m_Profile.reset();
		m_ProfilePath.clear();
		m_TemplateDescriptorPath.clear();
	}

	void EditorBuildProfileController::Shutdown(std::string_view Reason)
	{
		(void)CancelActiveExport(Reason);
		m_ExportRunner->Shutdown();
	}

	void EditorBuildProfileController::SaveProfile()
	{
		if (!m_Profile)
		{
			m_StatusMessage = "Create or load a profile before saving.";
			return;
		}
		const auto Saved = PulseForge::ExportBuildProfileSerializer::Save(m_ProfilePath, *m_Profile);
		if (!Saved)
		{
			m_StatusMessage = Saved.error().Message;
			return;
		}
		m_StatusMessage = "Build profile saved.";
	}

	void EditorBuildProfileController::StartExport(const EditorBuildProfileProjectContext& Context)
	{
		if (!Context.SavedProjectFile || !m_Profile || !Context.DirtyReason.empty())
		{
			m_StatusMessage = !Context.DirtyReason.empty()
				? std::string(Context.DirtyReason)
				: "Open a project and select a valid profile before exporting.";
			return;
		}
		if (m_TemplateDescriptorPath.empty())
		{
			m_StatusMessage = "Select a compatible local player template before exporting.";
			return;
		}

		const auto ValidProfile = PulseForge::ExportBuildProfileSerializer::Serialize(*m_Profile);
		if (!ValidProfile)
		{
			m_StatusMessage = ValidProfile.error().Message;
			return;
		}

		PulseForge::AssetExportRequest Request;
		Request.SourceProjectFile = *Context.SavedProjectFile;
		Request.Profile = *m_Profile;
		Request.TemplateDescriptor = m_TemplateDescriptorPath;

		// The template root is resolved by the shared export policy before the request snapshot is handed to the worker.
		const auto TemplateRoot = PulseForge::ResolveExportTemplateRoot(m_TemplateDescriptorPath, std::nullopt);
		if (!TemplateRoot)
		{
			m_StatusMessage = TemplateRoot.error().Message;
			return;
		}
		Request.TemplateRoot = *TemplateRoot;

		auto Started = m_ExportRunner->Start(std::move(Request));
		if (!Started)
		{
			m_StatusMessage = Started.error().Message;
			return;
		}
		m_ExportRequestID = *Started;
		m_StatusMessage = "Export started using saved project content.";
	}

	bool EditorBuildProfileController::CancelActiveExport(std::string_view Reason)
	{
		if (!m_ExportRequestID) return false;
		const PulseForge::AssetExportSnapshot Snapshot = m_ExportRunner->GetSnapshot(*m_ExportRequestID);
		if (!Snapshot.Running) return false;
		if (!m_ExportRunner->RequestCancel(*m_ExportRequestID)) return false;
		m_StatusMessage = "Export cancellation requested because " + std::string(Reason) + ".";
		return true;
	}
} // namespace PulseForgeEditor
