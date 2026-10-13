#pragma once

#include "Editor/EditorIcons.h"

namespace PulseForgeEditor
{
	struct EditorEntityIconComponents
	{
		bool HasCamera = false;
		bool HasMesh = false;
		bool HasSpotLight = false;
		bool HasPointLight = false;
		bool HasDirectionalLight = false;
		bool HasEnvironmentLight = false;
		bool HasAudioSource = false;
		bool HasAudioListener = false;
	};

	[[nodiscard]] constexpr EditorIcon GetEditorEntityIcon(EditorEntityIconComponents Components) noexcept
	{
		if (Components.HasCamera)
			return EditorIcon::Camera;
		if (Components.HasMesh)
			return EditorIcon::Cube;
		if (Components.HasSpotLight)
			return EditorIcon::SpotLight;
		if (Components.HasPointLight)
			return EditorIcon::PointLight;
		if (Components.HasDirectionalLight)
			return EditorIcon::DirectionalLight;
		if (Components.HasEnvironmentLight)
			return EditorIcon::Environment;
		if (Components.HasAudioSource)
			return EditorIcon::AudioSource;
		if (Components.HasAudioListener)
			return EditorIcon::AudioListener;
		return EditorIcon::Entity;
	}
}
