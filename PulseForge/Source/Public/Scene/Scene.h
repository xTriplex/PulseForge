#pragma once

#include "Core/Core.h"
#include "Scene/Entity.h"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
	class SceneSerializer;
	using RuntimeSkeletalLocalTransform = std::array<float, 16>;
	inline constexpr size_t MaximumRuntimeSkeletalPoseNodesPerEntity = 4096;
	inline constexpr size_t MaximumRuntimeSkeletalPoseNodesTotal = 65536;

	// Scene is not internally synchronized; callers must serialize access across threads.
	class PULSEFORGE_API Scene final
	{
	public:
		Scene();
		~Scene();

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;
		Scene(Scene&&) = delete;
		Scene& operator=(Scene&&) = delete;

		[[nodiscard]] std::expected<Entity, SceneError> CreateEntity(std::string Name = "Entity");
		[[nodiscard]] std::expected<Entity, SceneError> CreateEntityWithUUID(UUID Identifier, std::string Name = "Entity");
		// Duplicates supported components and optionally the parent; child entities are not recursively copied.
		// Primary camera/listener roles are cleared on the duplicate.
		[[nodiscard]] std::expected<Entity, SceneError> DuplicateEntity(const Entity& Source, bool PreserveParent = true);
		// Existing children survive as roots when a parent entity is destroyed.
		[[nodiscard]] std::expected<void, SceneError> DestroyEntity(const Entity& Target);
		[[nodiscard]] std::optional<Entity> FindEntity(UUID Identifier) const;
		[[nodiscard]] std::vector<Entity> GetEntities() const;
		[[nodiscard]] size_t GetEntityCount() const noexcept;

		// Transient Play/runtime state. Local transforms are column-major and never serialized.
		[[nodiscard]] std::expected<void, SceneError> SetRuntimeSkeletalPose(
			UUID EntityIdentifier,
			std::vector<RuntimeSkeletalLocalTransform> LocalNodeTransforms);
		[[nodiscard]] std::optional<std::vector<RuntimeSkeletalLocalTransform>> GetRuntimeSkeletalPose(UUID EntityIdentifier) const;
		void ClearRuntimeSkeletalPose(UUID EntityIdentifier) noexcept;
		void ClearRuntimeSkeletalPoses() noexcept;

	private:
		friend class SceneSerializer;
		std::shared_ptr<Detail::SceneStorage> m_Storage;
	};
}
