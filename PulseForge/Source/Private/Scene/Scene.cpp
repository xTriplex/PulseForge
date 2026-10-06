#include "Core/PulseForgePCH.h"
#include "Scene/Scene.h"
#include "Scene/Components/MeshRendererComponent.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <cmath>
#include <exception>

namespace PulseForge::Detail
{
	struct HierarchyComponent
	{
		std::optional<UUID> Parent;
		std::vector<UUID> Children;
	};

	struct SceneStorage
	{
		entt::registry Registry;
		std::unordered_map<UUID, entt::entity, UUIDHash> Entities;
	};
}

namespace PulseForge
{
	namespace
	{
		SceneError MakeSceneError(SceneErrorCode Code, const char* Message)
		{
			return { Code, Message };
		}

		std::optional<entt::entity> ResolveEntity(const Detail::SceneStorage& Storage, UUID Identifier)
		{
			const auto It = Storage.Entities.find(Identifier);
			if (It == Storage.Entities.end() || !Storage.Registry.valid(It->second))
				return std::nullopt;
			return It->second;
		}

		std::expected<void, SceneError> SetParentInStorage(
			const std::shared_ptr<Detail::SceneStorage>& Storage,
			UUID ChildIdentifier,
			std::optional<UUID> ParentIdentifier)
		{
			const auto ChildNative = ResolveEntity(*Storage, ChildIdentifier);
			if (!ChildNative)
				return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot parent an invalid entity"));

			auto& ChildHierarchy = Storage->Registry.get<Detail::HierarchyComponent>(*ChildNative);
			if (ChildHierarchy.Parent == ParentIdentifier)
				return {};

			std::optional<entt::entity> ParentNative;
			if (ParentIdentifier)
			{
				if (*ParentIdentifier == ChildIdentifier)
					return std::unexpected(MakeSceneError(SceneErrorCode::ParentCycle, "An entity cannot be its own parent"));

				ParentNative = ResolveEntity(*Storage, *ParentIdentifier);
				if (!ParentNative)
					return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot parent to an invalid entity"));

				std::optional<UUID> Ancestor = ParentIdentifier;
				for (size_t Traversal = 0; Ancestor && Traversal <= Storage->Entities.size(); ++Traversal)
				{
					if (*Ancestor == ChildIdentifier)
						return std::unexpected(MakeSceneError(SceneErrorCode::ParentCycle, "Parenting would create a hierarchy cycle"));

					const auto AncestorNative = ResolveEntity(*Storage, *Ancestor);
					if (!AncestorNative)
						return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Hierarchy references an entity that no longer exists"));
					Ancestor = Storage->Registry.get<Detail::HierarchyComponent>(*AncestorNative).Parent;
				}

				if (Ancestor)
					return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Existing hierarchy contains a cycle"));

				const auto& NewChildren = Storage->Registry.get<Detail::HierarchyComponent>(*ParentNative).Children;
				if (std::find(NewChildren.begin(), NewChildren.end(), ChildIdentifier) == NewChildren.end())
				{
					try
					{
						Storage->Registry.get<Detail::HierarchyComponent>(*ParentNative).Children.reserve(NewChildren.size() + 1);
					}
					catch (const std::exception& Exception)
					{
						return std::unexpected(SceneError{
							SceneErrorCode::StorageFailure,
							std::string("Could not grow parent child list: ") + Exception.what()
						});
					}
				}
			}

			if (ChildHierarchy.Parent)
			{
				if (const auto OldParent = ResolveEntity(*Storage, *ChildHierarchy.Parent))
				{
					auto& OldChildren = Storage->Registry.get<Detail::HierarchyComponent>(*OldParent).Children;
					std::erase(OldChildren, ChildIdentifier);
				}
			}

			ChildHierarchy.Parent = ParentIdentifier;
			if (ParentNative)
			{
				auto& NewChildren = Storage->Registry.get<Detail::HierarchyComponent>(*ParentNative).Children;
				if (std::find(NewChildren.begin(), NewChildren.end(), ChildIdentifier) == NewChildren.end())
					NewChildren.push_back(ChildIdentifier);
			}
			return {};
		}
	}

	Scene::Scene()
		: m_Storage(std::make_shared<Detail::SceneStorage>())
	{
	}

	Scene::~Scene() = default;

	std::expected<Entity, SceneError> Scene::CreateEntity(std::string Name)
	{
		for (uint32_t Attempt = 0; Attempt < 16; ++Attempt)
		{
			auto Generated = UUID::Generate();
			if (!Generated)
				return std::unexpected(SceneError{ SceneErrorCode::UUIDGenerationFailed, Generated.error().Message });
			if (!m_Storage->Entities.contains(Generated.value()))
				return CreateEntityWithUUID(Generated.value(), std::move(Name));
		}

		return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Could not generate a unique entity UUID after 16 attempts"));
	}

	std::expected<Entity, SceneError> Scene::CreateEntityWithUUID(UUID Identifier, std::string Name)
	{
		if (Identifier.IsNil())
			return std::unexpected(MakeSceneError(SceneErrorCode::NilUUID, "Entity UUID must not be nil"));
		if (m_Storage->Entities.contains(Identifier))
			return std::unexpected(MakeSceneError(SceneErrorCode::DuplicateUUID, "An entity with this UUID already exists in the scene"));

		entt::entity Native = entt::null;
		try
		{
			Native = m_Storage->Registry.create();
			m_Storage->Registry.emplace<TagComponent>(Native, TagComponent{ std::move(Name) });
			m_Storage->Registry.emplace<TransformComponent>(Native);
			m_Storage->Registry.emplace<Detail::HierarchyComponent>(Native);

			const auto [Iterator, Inserted] = m_Storage->Entities.emplace(Identifier, Native);
			(void)Iterator;
			if (!Inserted)
			{
				m_Storage->Registry.destroy(Native);
				return std::unexpected(MakeSceneError(SceneErrorCode::DuplicateUUID, "An entity with this UUID already exists in the scene"));
			}
			return Entity{ m_Storage, Identifier };
		}
		catch (const std::exception& Exception)
		{
			if (Native != entt::null && m_Storage->Registry.valid(Native))
				m_Storage->Registry.destroy(Native);
			return std::unexpected(SceneError{ SceneErrorCode::StorageFailure, std::string("Could not create entity: ") + Exception.what() });
		}
	}

	std::expected<Entity, SceneError> Scene::DuplicateEntity(const Entity& Source)
	{
		const auto SourceStorage = Source.m_Storage.lock();
		if (!SourceStorage || !Source.IsValid())
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot duplicate an invalid entity"));
		if (SourceStorage.get() != m_Storage.get())
			return std::unexpected(MakeSceneError(SceneErrorCode::ForeignEntity, "Cannot duplicate an entity from another scene"));

		const auto Tag = Source.GetTag();
		const auto Transform = Source.GetTransform();
		const auto Camera = Source.GetCamera();
		const auto MeshRenderer = Source.GetMeshRenderer();
		const auto Rigidbody = Source.GetRigidbody();
		const auto BoxCollider = Source.GetBoxCollider();
		if (!Tag || !Transform || !Camera || !MeshRenderer || !Rigidbody || !BoxCollider)
			return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Could not read source entity components for duplication"));

		auto Duplicated = CreateEntity(Tag->Name + " Copy");
		if (!Duplicated)
			return std::unexpected(Duplicated.error());

		if (auto TransformResult = Duplicated->SetTransform(*Transform); !TransformResult)
		{
			(void)DestroyEntity(*Duplicated);
			return std::unexpected(TransformResult.error());
		}
		if (Camera->has_value())
		{
			if (auto CameraResult = Duplicated->SetCamera(Camera->value()); !CameraResult)
			{
				(void)DestroyEntity(*Duplicated);
				return std::unexpected(CameraResult.error());
			}
		}
		if (MeshRenderer->has_value())
		{
			if (auto MeshRendererResult = Duplicated->SetMeshRenderer(MeshRenderer->value()); !MeshRendererResult)
			{
				(void)DestroyEntity(*Duplicated);
				return std::unexpected(MeshRendererResult.error());
			}
		}
		if (Rigidbody->has_value())
		{
			if (auto RigidbodyResult = Duplicated->SetRigidbody(Rigidbody->value()); !RigidbodyResult)
			{
				(void)DestroyEntity(*Duplicated);
				return std::unexpected(RigidbodyResult.error());
			}
		}
		if (BoxCollider->has_value())
		{
			if (auto ColliderResult = Duplicated->SetBoxCollider(BoxCollider->value()); !ColliderResult)
			{
				(void)DestroyEntity(*Duplicated);
				return std::unexpected(ColliderResult.error());
			}
		}

		const auto Parent = Source.GetParent();
		if (!Parent)
		{
			(void)DestroyEntity(*Duplicated);
			return std::unexpected(Parent.error());
		}
		if (Parent->has_value())
		{
			if (auto ParentResult = Duplicated->SetParent(**Parent); !ParentResult)
			{
				(void)DestroyEntity(*Duplicated);
				return std::unexpected(ParentResult.error());
			}
		}
		return Duplicated;
	}

	std::expected<void, SceneError> Scene::DestroyEntity(const Entity& Target)
	{
		const auto TargetStorage = Target.m_Storage.lock();
		if (!TargetStorage || !Target.IsValid())
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot destroy an invalid entity"));
		if (TargetStorage.get() != m_Storage.get())
			return std::unexpected(MakeSceneError(SceneErrorCode::ForeignEntity, "Cannot destroy an entity owned by another scene"));

		const auto Native = ResolveEntity(*m_Storage, Target.GetUUID());
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Entity no longer exists in this scene"));

		auto& Hierarchy = m_Storage->Registry.get<Detail::HierarchyComponent>(*Native);
		if (Hierarchy.Parent)
		{
			if (const auto ParentNative = ResolveEntity(*m_Storage, *Hierarchy.Parent))
			{
				auto& Siblings = m_Storage->Registry.get<Detail::HierarchyComponent>(*ParentNative).Children;
				std::erase(Siblings, Target.GetUUID());
			}
		}

		for (const UUID ChildIdentifier : Hierarchy.Children)
		{
			if (const auto ChildNative = ResolveEntity(*m_Storage, ChildIdentifier))
				m_Storage->Registry.get<Detail::HierarchyComponent>(*ChildNative).Parent.reset();
		}

		m_Storage->Entities.erase(Target.GetUUID());
		m_Storage->Registry.destroy(*Native);
		return {};
	}

	std::optional<Entity> Scene::FindEntity(UUID Identifier) const
	{
		if (!ResolveEntity(*m_Storage, Identifier))
			return std::nullopt;
		return Entity{ m_Storage, Identifier };
	}

	std::vector<Entity> Scene::GetEntities() const
	{
		std::vector<Entity> Entities;
		Entities.reserve(m_Storage->Entities.size());
		for (const auto& [Identifier, Native] : m_Storage->Entities)
		{
			if (m_Storage->Registry.valid(Native))
				Entities.push_back(Entity{ m_Storage, Identifier });
		}
		std::sort(Entities.begin(), Entities.end(), [](const Entity& First, const Entity& Second)
		{
			return First.GetUUID() < Second.GetUUID();
		});
		return Entities;
	}

	size_t Scene::GetEntityCount() const noexcept
	{
		return m_Storage->Entities.size();
	}

	Entity::Entity(std::weak_ptr<Detail::SceneStorage> Storage, UUID Identifier)
		: m_Storage(std::move(Storage)), m_UUID(Identifier)
	{
	}

	bool Entity::IsValid() const noexcept
	{
		const auto Storage = m_Storage.lock();
		return Storage && !m_UUID.IsNil() && ResolveEntity(*Storage, m_UUID).has_value();
	}

	std::expected<TagComponent, SceneError> Entity::GetTag() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the tag of an invalid entity"));
		return Storage->Registry.get<TagComponent>(*Native);
	}

	std::expected<void, SceneError> Entity::SetTag(TagComponent Tag) const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot set the tag of an invalid entity"));

		try
		{
			Storage->Registry.get<TagComponent>(*Native) = std::move(Tag);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneError{ SceneErrorCode::StorageFailure, std::string("Could not update entity tag: ") + Exception.what() });
		}
	}

	std::expected<TransformComponent, SceneError> Entity::GetTransform() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the transform of an invalid entity"));
		return Storage->Registry.get<TransformComponent>(*Native);
	}

	std::expected<void, SceneError> Entity::SetTransform(const TransformComponent& Transform) const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot set the transform of an invalid entity"));

		const glm::quat& Rotation = Transform.Rotation;
		const bool FiniteTransform =
			std::isfinite(Transform.Translation.x) && std::isfinite(Transform.Translation.y) && std::isfinite(Transform.Translation.z) &&
			std::isfinite(Transform.Scale.x) && std::isfinite(Transform.Scale.y) && std::isfinite(Transform.Scale.z) &&
			std::isfinite(Rotation.w) && std::isfinite(Rotation.x) && std::isfinite(Rotation.y) && std::isfinite(Rotation.z);
		const float RotationLength = glm::length(Rotation);
		if (!FiniteTransform || !std::isfinite(RotationLength) || RotationLength < 0.000001f)
		{
			return std::unexpected(MakeSceneError(
				SceneErrorCode::InvalidTransform,
				"Transform values must be finite and rotation must be non-zero"));
		}

		TransformComponent NormalizedTransform = Transform;
		NormalizedTransform.Rotation = glm::normalize(Rotation);
		Storage->Registry.get<TransformComponent>(*Native) = NormalizedTransform;
		return {};
	}

	std::expected<std::optional<CameraComponent>, SceneError> Entity::GetCamera() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the camera component of an invalid entity"));
		if (!Storage->Registry.all_of<CameraComponent>(*Native))
			return std::optional<CameraComponent>{};
		return std::optional<CameraComponent>{ Storage->Registry.get<CameraComponent>(*Native) };
	}

	std::expected<void, SceneError> Entity::SetCamera(const CameraComponent& Camera) const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot add a camera component to an invalid entity"));

		if (auto ValidationResult = Camera.Validate(); !ValidationResult)
		{
			return std::unexpected(SceneError{
				SceneErrorCode::InvalidCamera,
				ValidationResult.error().Message });
		}

		try
		{
			Storage->Registry.emplace_or_replace<CameraComponent>(*Native, Camera);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneError{
				SceneErrorCode::StorageFailure,
				std::string("Could not set camera component: ") + Exception.what() });
		}
	}

	std::expected<void, SceneError> Entity::RemoveCamera() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot remove a camera component from an invalid entity"));
		if (!Storage->Registry.all_of<CameraComponent>(*Native))
			return std::unexpected(MakeSceneError(SceneErrorCode::MissingComponent, "Entity does not have a camera component"));

		Storage->Registry.remove<CameraComponent>(*Native);
		return {};
	}

	std::expected<std::optional<MeshRendererComponent>, SceneError> Entity::GetMeshRenderer() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the mesh renderer of an invalid entity"));
		if (!Storage->Registry.all_of<MeshRendererComponent>(*Native))
			return std::optional<MeshRendererComponent>{};
		return std::optional<MeshRendererComponent>{ Storage->Registry.get<MeshRendererComponent>(*Native) };
	}

	std::expected<void, SceneError> Entity::SetMeshRenderer(const MeshRendererComponent& MeshRenderer) const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot add a mesh renderer to an invalid entity"));
		if (MeshRenderer.MeshAsset.IsNil())
			return std::unexpected(MakeSceneError(
				SceneErrorCode::InvalidAssetReference,
				"Mesh renderer requires a non-nil mesh asset UUID"));
		if (MeshRenderer.MaterialAsset && MeshRenderer.MaterialAsset->IsNil())
			return std::unexpected(MakeSceneError(
				SceneErrorCode::InvalidAssetReference,
				"Mesh renderer material reference must be a non-nil asset UUID when present"));

		try
		{
			Storage->Registry.emplace_or_replace<MeshRendererComponent>(*Native, MeshRenderer);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneError{
				SceneErrorCode::StorageFailure,
				std::string("Could not set mesh renderer component: ") + Exception.what() });
		}
	}

	std::expected<void, SceneError> Entity::RemoveMeshRenderer() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(
				SceneErrorCode::InvalidEntity,
				"Cannot remove a mesh renderer from an invalid entity"));
		if (!Storage->Registry.all_of<MeshRendererComponent>(*Native))
			return std::unexpected(MakeSceneError(SceneErrorCode::MissingComponent, "Entity does not have a mesh renderer component"));

		Storage->Registry.remove<MeshRendererComponent>(*Native);
		return {};
	}

	std::expected<std::optional<RigidbodyComponent>, SceneError> Entity::GetRigidbody() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the rigidbody of an invalid entity"));
		if (!Storage->Registry.all_of<RigidbodyComponent>(*Native))
			return std::optional<RigidbodyComponent>{};
		return std::optional<RigidbodyComponent>{ Storage->Registry.get<RigidbodyComponent>(*Native) };
	}

	std::expected<void, SceneError> Entity::SetRigidbody(const RigidbodyComponent& Rigidbody) const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot add a rigidbody to an invalid entity"));
		if (auto Validation = Rigidbody.Validate(); !Validation)
			return std::unexpected(SceneError{ SceneErrorCode::InvalidPhysicsComponent, Validation.error().Message });

		try
		{
			Storage->Registry.emplace_or_replace<RigidbodyComponent>(*Native, Rigidbody);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneError{
				SceneErrorCode::StorageFailure,
				std::string("Could not set rigidbody component: ") + Exception.what() });
		}
	}

	std::expected<void, SceneError> Entity::RemoveRigidbody() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot remove a rigidbody from an invalid entity"));
		if (!Storage->Registry.all_of<RigidbodyComponent>(*Native))
			return std::unexpected(MakeSceneError(SceneErrorCode::MissingComponent, "Entity does not have a rigidbody component"));

		Storage->Registry.remove<RigidbodyComponent>(*Native);
		return {};
	}

	std::expected<std::optional<BoxColliderComponent>, SceneError> Entity::GetBoxCollider() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the box collider of an invalid entity"));
		if (!Storage->Registry.all_of<BoxColliderComponent>(*Native))
			return std::optional<BoxColliderComponent>{};
		return std::optional<BoxColliderComponent>{ Storage->Registry.get<BoxColliderComponent>(*Native) };
	}

	std::expected<void, SceneError> Entity::SetBoxCollider(const BoxColliderComponent& Collider) const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot add a box collider to an invalid entity"));
		if (auto Validation = Collider.Validate(); !Validation)
			return std::unexpected(SceneError{ SceneErrorCode::InvalidPhysicsComponent, Validation.error().Message });

		try
		{
			Storage->Registry.emplace_or_replace<BoxColliderComponent>(*Native, Collider);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneError{
				SceneErrorCode::StorageFailure,
				std::string("Could not set box collider component: ") + Exception.what() });
		}
	}

	std::expected<void, SceneError> Entity::RemoveBoxCollider() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot remove a box collider from an invalid entity"));
		if (!Storage->Registry.all_of<BoxColliderComponent>(*Native))
			return std::unexpected(MakeSceneError(SceneErrorCode::MissingComponent, "Entity does not have a box collider component"));

		Storage->Registry.remove<BoxColliderComponent>(*Native);
		return {};
	}

	std::expected<glm::mat4, SceneError> Entity::GetWorldMatrix() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot compute the transform of an invalid entity"));

		glm::mat4 WorldMatrix = Storage->Registry.get<TransformComponent>(*Native).GetLocalMatrix();
		std::optional<UUID> Ancestor = Storage->Registry.get<Detail::HierarchyComponent>(*Native).Parent;
		for (size_t Traversal = 0; Ancestor && Traversal <= Storage->Entities.size(); ++Traversal)
		{
			const auto AncestorNative = ResolveEntity(*Storage, *Ancestor);
			if (!AncestorNative)
				return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Hierarchy references an entity that no longer exists"));

			WorldMatrix = Storage->Registry.get<TransformComponent>(*AncestorNative).GetLocalMatrix() * WorldMatrix;
			Ancestor = Storage->Registry.get<Detail::HierarchyComponent>(*AncestorNative).Parent;
		}

		if (Ancestor)
			return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Existing hierarchy contains a cycle"));
		return WorldMatrix;
	}

	std::expected<std::optional<Entity>, SceneError> Entity::GetParent() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the parent of an invalid entity"));

		const std::optional<UUID> ParentIdentifier = Storage->Registry.get<Detail::HierarchyComponent>(*Native).Parent;
		if (!ParentIdentifier)
			return std::optional<Entity>{};
		if (!ResolveEntity(*Storage, *ParentIdentifier))
			return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Hierarchy references a parent that no longer exists"));

		return std::optional<Entity>{ Entity{ Storage, *ParentIdentifier } };
	}

	std::expected<std::vector<Entity>, SceneError> Entity::GetChildren() const
	{
		const auto Storage = m_Storage.lock();
		const auto Native = Storage ? ResolveEntity(*Storage, m_UUID) : std::nullopt;
		if (!Native)
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot read the children of an invalid entity"));

		std::vector<Entity> Children;
		const auto& ChildIdentifiers = Storage->Registry.get<Detail::HierarchyComponent>(*Native).Children;
		Children.reserve(ChildIdentifiers.size());
		for (const UUID ChildIdentifier : ChildIdentifiers)
		{
			if (!ResolveEntity(*Storage, ChildIdentifier))
				return std::unexpected(MakeSceneError(SceneErrorCode::StorageFailure, "Hierarchy references a child that no longer exists"));
			Children.push_back(Entity{ Storage, ChildIdentifier });
		}
		return Children;
	}

	std::expected<void, SceneError> Entity::SetParent(const Entity& Parent) const
	{
		const auto Storage = m_Storage.lock();
		const auto ParentStorage = Parent.m_Storage.lock();
		if (!Storage || !ParentStorage || !IsValid() || !Parent.IsValid())
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Both entities must be valid before parenting"));
		if (Storage.get() != ParentStorage.get())
			return std::unexpected(MakeSceneError(SceneErrorCode::ForeignEntity, "Entities must belong to the same scene to form a hierarchy"));

		return SetParentInStorage(Storage, m_UUID, Parent.m_UUID);
	}

	std::expected<void, SceneError> Entity::ClearParent() const
	{
		const auto Storage = m_Storage.lock();
		if (!Storage || !IsValid())
			return std::unexpected(MakeSceneError(SceneErrorCode::InvalidEntity, "Cannot clear the parent of an invalid entity"));
		return SetParentInStorage(Storage, m_UUID, std::nullopt);
	}

	bool operator==(const Entity& First, const Entity& Second) noexcept
	{
		return First.m_UUID == Second.m_UUID &&
			!First.m_Storage.owner_before(Second.m_Storage) &&
			!Second.m_Storage.owner_before(First.m_Storage);
	}
}
