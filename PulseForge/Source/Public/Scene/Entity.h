#pragma once

#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/EnvironmentLightComponent.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"
#include "Scene/Components/AudioListenerComponent.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/RigidbodyComponent.h"
#include "Scene/Components/BoxColliderComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/Components/TagComponent.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/SceneError.h"
#include "Scene/UUID.h"

#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace PulseForge
{
	namespace Detail
	{
		struct SceneStorage;
		struct EntityIncarnation;
	}

	class Scene;

	// A non-owning reference to one entity incarnation. It becomes invalid when that entity or its Scene is destroyed.
	class PULSEFORGE_API Entity final
	{
	public:
		Entity() = default;

		[[nodiscard]] bool IsValid() const noexcept;
		explicit operator bool() const noexcept { return IsValid(); }
		[[nodiscard]] UUID GetUUID() const noexcept { return m_UUID; }
		[[nodiscard]] std::expected<TagComponent, SceneError> GetTag() const;
		[[nodiscard]] std::expected<void, SceneError> SetTag(TagComponent Tag) const;
		[[nodiscard]] std::expected<TransformComponent, SceneError> GetTransform() const;
		[[nodiscard]] std::expected<void, SceneError> SetTransform(const TransformComponent& Transform) const;
		[[nodiscard]] std::expected<std::optional<CameraComponent>, SceneError> GetCamera() const;
		[[nodiscard]] std::expected<void, SceneError> SetCamera(const CameraComponent& Camera) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveCamera() const;
		[[nodiscard]] std::expected<std::optional<DirectionalLightComponent>, SceneError> GetDirectionalLight() const;
		[[nodiscard]] std::expected<void, SceneError> SetDirectionalLight(const DirectionalLightComponent& Light) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveDirectionalLight() const;
		[[nodiscard]] std::expected<std::optional<EnvironmentLightComponent>, SceneError> GetEnvironmentLight() const;
		[[nodiscard]] std::expected<void, SceneError> SetEnvironmentLight(const EnvironmentLightComponent& Environment) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveEnvironmentLight() const;
		[[nodiscard]] std::expected<std::optional<PointLightComponent>, SceneError> GetPointLight() const;
		[[nodiscard]] std::expected<void, SceneError> SetPointLight(const PointLightComponent& Light) const;
		[[nodiscard]] std::expected<void, SceneError> RemovePointLight() const;
		[[nodiscard]] std::expected<std::optional<SpotLightComponent>, SceneError> GetSpotLight() const;
		[[nodiscard]] std::expected<void, SceneError> SetSpotLight(const SpotLightComponent& Light) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveSpotLight() const;
		[[nodiscard]] std::expected<std::optional<MeshRendererComponent>, SceneError> GetMeshRenderer() const;
		[[nodiscard]] std::expected<void, SceneError> SetMeshRenderer(const MeshRendererComponent& MeshRenderer) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveMeshRenderer() const;
		[[nodiscard]] std::expected<std::optional<RigidbodyComponent>, SceneError> GetRigidbody() const;
		[[nodiscard]] std::expected<void, SceneError> SetRigidbody(const RigidbodyComponent& Rigidbody) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveRigidbody() const;
		[[nodiscard]] std::expected<std::optional<BoxColliderComponent>, SceneError> GetBoxCollider() const;
		[[nodiscard]] std::expected<void, SceneError> SetBoxCollider(const BoxColliderComponent& Collider) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveBoxCollider() const;
		[[nodiscard]] std::expected<std::optional<AudioSourceComponent>, SceneError> GetAudioSource() const;
		[[nodiscard]] std::expected<void, SceneError> SetAudioSource(const AudioSourceComponent& AudioSource) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveAudioSource() const;
		[[nodiscard]] std::expected<std::optional<AudioListenerComponent>, SceneError> GetAudioListener() const;
		[[nodiscard]] std::expected<void, SceneError> SetAudioListener(const AudioListenerComponent& AudioListener) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveAudioListener() const;
		[[nodiscard]] std::expected<std::optional<ScriptComponent>, SceneError> GetScript() const;
		[[nodiscard]] std::expected<void, SceneError> SetScript(const ScriptComponent& Script) const;
		[[nodiscard]] std::expected<void, SceneError> RemoveScript() const;
		[[nodiscard]] std::expected<glm::mat4, SceneError> GetWorldMatrix() const;
		[[nodiscard]] std::expected<std::optional<Entity>, SceneError> GetParent() const;
		[[nodiscard]] std::expected<std::vector<Entity>, SceneError> GetChildren() const;
		// Reparenting preserves local transform values, not the current world-space transform.
		[[nodiscard]] std::expected<void, SceneError> SetParent(const Entity& Parent) const;
		[[nodiscard]] std::expected<void, SceneError> ClearParent() const;

		friend bool operator==(const Entity& First, const Entity& Second) noexcept;

	private:
		friend class Scene;
		Entity(
			std::weak_ptr<Detail::SceneStorage> Storage,
			UUID Identifier,
			std::weak_ptr<Detail::EntityIncarnation> Incarnation);

		std::weak_ptr<Detail::SceneStorage> m_Storage;
		UUID m_UUID;
		std::weak_ptr<Detail::EntityIncarnation> m_Incarnation;
	};
}
