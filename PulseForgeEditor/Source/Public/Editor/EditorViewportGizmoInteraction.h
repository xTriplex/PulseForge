#pragma once

#include "Editor/EditorViewportToolState.h"
#include "Editor/ViewportGizmoGeometry.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/UUID.h"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	class Scene;
}

namespace PulseForgeEditor
{
	class EditorSceneEditController;
	class EditorSceneWorkspaceState;

	struct EditorViewportGizmoBeginInput
	{
		ViewportGizmoHandle Handle;
		ViewportGizmoGeometry Geometry;
		glm::vec2 MousePosition{ 0.0f };
		ViewportImageRect ImageRect{};
		glm::mat4 ViewProjection{ 1.0f };
		glm::vec3 CameraPosition{ 0.0f };
		bool ViewProjectionValid = false;
		bool RuntimeActive = false;
		bool CameraNavigationActive = false;
		bool DuplicateOnDrag = false;
	};

	struct EditorViewportGizmoUpdateInput
	{
		glm::vec2 MousePosition{ 0.0f };
		ViewportImageRect ImageRect{};
		glm::mat4 ViewProjection{ 1.0f };
		EditorViewportToolValues ToolValues{};
		bool RuntimeActive = false;
		bool ControlDown = false;
		bool AltDown = false;
	};

	class EditorViewportGizmoInteraction final
	{
	public:
		using RestoreCallback = std::function<std::expected<void, std::string>(std::string_view)>;

		[[nodiscard]] bool IsActive() const noexcept { return m_Drag.has_value(); }
		void DiscardActiveState() noexcept { m_Drag.reset(); }

		[[nodiscard]] std::expected<bool, std::string> Begin(PulseForge::Scene& SceneValue,
			EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController,
			const EditorViewportGizmoBeginInput& Input, const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<void, std::string> Update(PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController,
			const EditorViewportGizmoUpdateInput& Input, const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<void, std::string> Cancel(PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController,
			const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<void, std::string> Complete(PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController,
			const RestoreCallback& RestoreState);

	private:
		struct EntityStart
		{
			PulseForge::UUID Identifier;
			PulseForge::TransformComponent LocalTransform;
			glm::mat4 WorldTransform{ 1.0f };
			glm::mat4 ParentWorldTransform{ 1.0f };
		};

		struct DragState
		{
			PulseForge::UUID Entity;
			const PulseForge::Scene* SceneIdentity = nullptr;
			PulseForge::TransformComponent InitialTransform;
			std::vector<EntityStart> Targets;
			std::vector<PulseForge::UUID> SourceRoots;
			TransformGizmoOperation Operation = TransformGizmoOperation::Translate;
			uint32_t Axis = 0;
			glm::vec3 PivotWorld{ 0.0f };
			glm::vec3 WorldAxis{ 0.0f };
			glm::vec3 PlaneNormal{ 0.0f };
			glm::vec3 StartPoint{ 0.0f };
			glm::vec2 StartMouse{ 0.0f };
			float WorldUnitsPerLocalUnit = 0.0f;
			bool DuplicateOnDrag = false;
			bool HasDuplicated = false;
		};

		[[nodiscard]] static std::optional<std::vector<EntityStart>> CaptureTargets(
			const PulseForge::Scene& SceneValue, const std::vector<PulseForge::UUID>& Identifiers);
		[[nodiscard]] static std::optional<PulseForge::TransformComponent> DecomposeLocalTransform(const glm::mat4& LocalMatrix);
		[[nodiscard]] static bool TransformsDiffer(
			const PulseForge::TransformComponent& First, const PulseForge::TransformComponent& Second);
		[[nodiscard]] std::expected<void, std::string> CancelAfterFailure(PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController,
			const RestoreCallback& RestoreState, std::string Error);

		std::optional<DragState> m_Drag;
	};
} // namespace PulseForgeEditor
