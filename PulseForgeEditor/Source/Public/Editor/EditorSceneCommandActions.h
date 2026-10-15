#pragma once

#include "Scene/Scene.h"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	class EditorSceneCommandActions
	{
	public:
		virtual ~EditorSceneCommandActions() = default;
		[[nodiscard]] virtual std::expected<void, std::string> RunEditorSceneEdit(
			std::string Description,
			std::function<std::expected<void, PulseForge::SceneError>()> Operation) = 0;
		virtual void ReportEditorSceneCommandError(std::string Message) = 0;
		virtual void SetEditorSceneCommandStatus(std::string Message) = 0;
	};

	class EditorSceneEntityCommandActions : public EditorSceneCommandActions
	{
	public:
		virtual void ReportEditorSceneCommandWarning(std::string Message) = 0;
		virtual void SetEditorSceneSelection(std::vector<PulseForge::UUID> Identifiers) = 0;
		virtual void SelectSingleEditorEntity(std::optional<PulseForge::UUID> Identifier) = 0;
	};
}
