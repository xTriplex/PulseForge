#pragma once

#include "Assets/AssetRegistry.h"
#include "Scene/PropertyMetadata.h"
#include "Scene/Scene.h"

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
	};

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
		void DrawScriptFloatProperties(Context& Inspector, const PulseForge::Entity& Entity, PulseForge::ScriptComponent& Component);

		bool m_FocusEntityNameNextFrame = false;
		std::string m_NewScriptPropertyIdentifier;
		float m_NewScriptPropertyValue = 0.0f;
	};
} // namespace PulseForgeEditor
