#pragma once

#include "Core/Core.h"
#include "Scene/Entity.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
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
		// Duplicates one entity's tag/transform and parent; child entities are not recursively copied.
		[[nodiscard]] std::expected<Entity, SceneError> DuplicateEntity(const Entity& Source);
		// Existing children survive as roots when a parent entity is destroyed.
		[[nodiscard]] std::expected<void, SceneError> DestroyEntity(const Entity& Target);
		[[nodiscard]] std::optional<Entity> FindEntity(UUID Identifier) const;
		[[nodiscard]] std::vector<Entity> GetEntities() const;
		[[nodiscard]] size_t GetEntityCount() const noexcept;

	private:
		std::shared_ptr<Detail::SceneStorage> m_Storage;
	};
}
