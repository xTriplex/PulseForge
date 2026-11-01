#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/ScriptSourceMetadata.h"
#include "Scene/PropertyMetadata.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptRuntime.h"

#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	class EditorAssetSelector;
	class EditorSceneWorkspaceState;
	class EditorStyle;

	class EditorInspectorPanelActions
	{
	public:
		virtual ~EditorInspectorPanelActions() = default;
		virtual void FinishInspectorSceneEdit() = 0;
		[[nodiscard]] virtual std::expected<void, std::string> RunInspectorSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) = 0;
		virtual void ReportInspectorError(std::string Message) = 0;
		[[nodiscard]] virtual std::expected<PulseForge::ScriptSourceMetadata, std::string> ReadScriptSourceMetadata(
			const PulseForge::AssetID& Asset)
		{
			(void)Asset;
			return std::unexpected("Script source metadata is unavailable in this Inspector context");
		}
		[[nodiscard]] virtual PulseForge::UUID GetScriptProjectIdentity() const { return {}; }
		[[nodiscard]] virtual std::expected<void, std::string> OpenScriptAsset(const PulseForge::AssetID& Asset)
		{
			(void)Asset;
			return std::unexpected("Opening script source is unavailable in this Inspector context");
		}
		[[nodiscard]] virtual std::expected<void, std::string> OpenScriptDiagnostic(
			const PulseForge::ScriptDiagnostic& Diagnostic)
		{
			(void)Diagnostic;
			return std::unexpected("Opening a diagnostic source is unavailable in this Inspector context");
		}
		[[nodiscard]] virtual std::expected<void, std::string> RequestScriptReload(const PulseForge::Entity& Entity)
		{
			(void)Entity;
			return std::unexpected("Script reload is unavailable in this Inspector context");
		}
		[[nodiscard]] virtual std::vector<PulseForge::ScriptDiagnostic> GetScriptDiagnostics(
			const PulseForge::UUID& Entity, const PulseForge::AssetID& Asset) const
		{
			(void)Entity;
			(void)Asset;
			return {};
		}
		[[nodiscard]] virtual bool IsLinkedPrefabEntity(const PulseForge::Entity& Entity) const { (void)Entity; return false; }
		[[nodiscard]] virtual std::expected<void, std::string> RecordPrefabPropertyOverride(
			const PulseForge::Entity& Entity, std::string_view ComponentID, std::string_view PropertyID)
		{
			(void)Entity; (void)ComponentID; (void)PropertyID; return {};
		}
		[[nodiscard]] virtual std::expected<void, std::string> RecordPrefabRenameOverride(
			const PulseForge::Entity& Entity, std::string_view Name)
		{
			(void)Entity; (void)Name; return {};
		}
		[[nodiscard]] virtual std::expected<void, std::string> ResetPrefabPropertyOverride(
			const PulseForge::Entity& Entity, std::string_view ComponentID, std::string_view PropertyID)
		{
			(void)Entity; (void)ComponentID; (void)PropertyID;
			return std::unexpected("Prefab property reset is unavailable in this editor context");
		}
	};

	// Routes the diagnostic's structured source identity through the active editor action owner.
	// The human-readable message is never used to select a file.
	void DispatchScriptDiagnosticOpen(EditorInspectorPanelActions& Actions, const PulseForge::ScriptDiagnostic& Diagnostic);

	class EditorInspectorPanel final
	{
	public:
		void FocusEntityNameNextFrame() noexcept { m_FocusEntityNameNextFrame = true; }
		void DrawContents(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, EditorStyle& Style,
			EditorAssetSelector& AssetSelector, const std::vector<PulseForge::AssetRecord>& Assets, bool ProjectAvailable,
			bool FinishCurrentEdit, EditorInspectorPanelActions& Actions);

	private:
		struct Context;

		template <typename Action> void RecordComponentOperation(Context& Inspector, Action&& Operation, std::string_view Description);
		void DrawProperty(Context& Inspector, const PulseForge::Entity& Entity, std::string_view ComponentId, std::string_view PropertyId,
			const std::function<bool(PulseForge::PropertyValue&)>& DrawWidget);
		void SetCameraComponent(Context& Inspector, const PulseForge::Entity& Target, const PulseForge::CameraComponent& Camera);
		void SetAudioListenerComponent(
			Context& Inspector, const PulseForge::Entity& Target, const PulseForge::AudioListenerComponent& Listener);
		[[nodiscard]] bool DrawAssetSelector(Context& Inspector, const char* Label, std::optional<PulseForge::AssetID>& Current,
			std::initializer_list<std::string_view> Extensions, bool AllowNone);
		[[nodiscard]] bool IsAssetExtension(const PulseForge::AssetRecord& Asset, std::initializer_list<std::string_view> Extensions) const;
		void DrawAddComponentControl(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawExistingComponents(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawEnvironmentLightComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawDirectionalLightComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawPointLightComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawSpotLightComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawCameraComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawMeshRendererComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawRigidbodyComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawBoxColliderComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawAudioSourceComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawAudioListenerComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawScriptComponent(Context& Inspector, const PulseForge::Entity& Entity);
		void DrawScriptFloatProperties(Context& Inspector, const PulseForge::Entity& Entity);

		bool m_FocusEntityNameNextFrame = false;
		std::optional<PulseForge::UUID> m_CachedScriptProject;
		std::optional<PulseForge::AssetID> m_CachedScriptAsset;
		std::optional<std::expected<PulseForge::ScriptSourceMetadata, std::string>> m_CachedScriptMetadata;
	};
} // namespace PulseForgeEditor
