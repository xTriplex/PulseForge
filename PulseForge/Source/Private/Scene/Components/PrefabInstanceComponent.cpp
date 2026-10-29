#include "Scene/Components/PrefabInstanceComponent.h"

#include <cmath>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>

namespace PulseForge
{
	namespace
	{
		struct OverrideEntityAddress
		{
			UUID Key;
			bool IsLocal;

			bool operator==(const OverrideEntityAddress&) const = default;
		};

		struct OverrideEntityAddressHash
		{
			size_t operator()(const OverrideEntityAddress& Value) const noexcept
			{
				return UUIDHash{}(Value.Key) ^ (Value.IsLocal ? size_t{ 0x9e3779b9 } : size_t{});
			}
		};

		struct OverrideIdentity
		{
			OverrideEntityAddress Entity;
			uint8_t OperationClass;
			std::string_view Component;
			std::string_view Property;

			bool operator==(const OverrideIdentity&) const = default;
		};

		size_t CombineHash(size_t Seed, size_t Value) noexcept
		{
			return Seed ^ (Value + size_t{ 0x9e3779b9 } + (Seed << 6) + (Seed >> 2));
		}

		struct OverrideIdentityHash
		{
			size_t operator()(const OverrideIdentity& Value) const noexcept
			{
				size_t Hash = OverrideEntityAddressHash{}(Value.Entity);
				Hash = CombineHash(Hash, std::hash<uint8_t>{}(Value.OperationClass));
				Hash = CombineHash(Hash, std::hash<std::string_view>{}(Value.Component));
				return CombineHash(Hash, std::hash<std::string_view>{}(Value.Property));
			}
		};

		uint8_t GetOperationClass(PrefabOverrideOperation Operation) noexcept
		{
			switch (Operation)
			{
			case PrefabOverrideOperation::SetProperty:
			case PrefabOverrideOperation::ResetProperty: return 0;
			case PrefabOverrideOperation::AddComponent:
			case PrefabOverrideOperation::RemoveComponent: return 1;
			case PrefabOverrideOperation::AddLocalEntity: return 2;
			case PrefabOverrideOperation::RemoveSourceEntity: return 3;
			case PrefabOverrideOperation::Reparent: return 4;
			case PrefabOverrideOperation::Rename: return 5;
			}
			return std::numeric_limits<uint8_t>::max();
		}
	}

	std::expected<void, std::string> PrefabInstanceComponent::Validate(UUID LinkedRootEntity) const
	{
		constexpr size_t MaximumMappings = 250'000;
		constexpr size_t MaximumOverrides = 1'000'000;
		if (InstanceID.IsNil() || SourceAsset.IsNil() || SourceRootKey.IsNil() || LinkedRootEntity.IsNil())
			return std::unexpected("Linked prefab identity fields must be non-nil");
		if (AppliedSourceFingerprint.empty() || AppliedSourceFingerprint.size() > 160)
			return std::unexpected("Linked prefab source fingerprint is missing or exceeds its limit");
		if (SourceEntities.empty() || SourceEntities.size() > MaximumMappings || LocalEntities.size() > MaximumMappings ||
			Overrides.size() > MaximumOverrides || Conflicts.size() > MaximumOverrides)
			return std::unexpected("Linked prefab mappings or overrides exceed their supported limits");

		std::unordered_set<UUID, UUIDHash> SourceKeys;
		std::unordered_set<UUID, UUIDHash> SceneEntities;
		SourceKeys.reserve(SourceEntities.size());
		SceneEntities.reserve(SourceEntities.size() + LocalEntities.size());
		bool FoundRoot = false;
		for (const PrefabEntityMapping& Mapping : SourceEntities)
		{
			if (Mapping.SourceKey.IsNil() || Mapping.SceneEntity.IsNil() || !SourceKeys.insert(Mapping.SourceKey).second ||
				!SceneEntities.insert(Mapping.SceneEntity).second)
				return std::unexpected("Linked prefab source mapping contains a nil or duplicate identity");
			if (Mapping.SourceKey == SourceRootKey)
			{
				if (Mapping.SceneEntity != LinkedRootEntity)
					return std::unexpected("Linked prefab root mapping does not point to its owning scene entity");
				FoundRoot = true;
			}
		}
		if (!FoundRoot) return std::unexpected("Linked prefab mapping does not contain its source root");

		std::unordered_set<UUID, UUIDHash> LocalKeys;
		LocalKeys.reserve(LocalEntities.size());
		for (const PrefabLocalEntityMapping& Mapping : LocalEntities)
		{
			if (Mapping.LocalKey.IsNil() || Mapping.SceneEntity.IsNil() || !LocalKeys.insert(Mapping.LocalKey).second ||
				!SceneEntities.insert(Mapping.SceneEntity).second)
				return std::unexpected("Linked prefab local mapping contains a nil or duplicate identity");
		}
		std::unordered_set<OverrideIdentity, OverrideIdentityHash> OverrideIdentities;
		OverrideIdentities.reserve(Overrides.size());
		std::unordered_map<OverrideEntityAddress, bool, OverrideEntityAddressHash> EntityRemovalOverrides;
		EntityRemovalOverrides.reserve(Overrides.size());
		for (const PrefabOverrideRecord& Override : Overrides)
		{
			if (static_cast<uint8_t>(Override.Operation) > static_cast<uint8_t>(PrefabOverrideOperation::Rename))
				return std::unexpected("Linked prefab override contains an unsupported operation");
			if (Override.Entity.Key.IsNil() || Override.ComponentID.size() > 128 || Override.PropertyID.size() > 128 ||
				Override.Name.size() > 4096 || (Override.Parent && Override.Parent->Key.IsNil()))
				return std::unexpected("Linked prefab override contains an invalid or oversized address");
			if (Override.Entity.IsLocal && !LocalKeys.contains(Override.Entity.Key))
				return std::unexpected("Linked prefab override refers to an unknown local entity key");
			if (!Override.Entity.IsLocal && !SourceKeys.contains(Override.Entity.Key))
				return std::unexpected("Linked prefab override refers to an unknown source entity key");
			if ((Override.Operation == PrefabOverrideOperation::SetProperty ||
				 Override.Operation == PrefabOverrideOperation::ResetProperty) &&
				 (Override.ComponentID.empty() || Override.PropertyID.empty()))
				return std::unexpected("Linked prefab property overrides require component and property IDs");
			if ((Override.Operation == PrefabOverrideOperation::AddComponent ||
				 Override.Operation == PrefabOverrideOperation::RemoveComponent) && Override.ComponentID.empty())
				return std::unexpected("Linked prefab component overrides require a component ID");
			if ((Override.Operation == PrefabOverrideOperation::AddComponent ||
				 Override.Operation == PrefabOverrideOperation::RemoveComponent) && !Override.PropertyID.empty())
				return std::unexpected("Linked prefab component overrides cannot contain a property ID");
			if (Override.Operation == PrefabOverrideOperation::AddLocalEntity && !Override.Entity.IsLocal)
				return std::unexpected("Linked prefab local-entity operations require a local entity address");
			if (Override.Operation == PrefabOverrideOperation::RemoveSourceEntity &&
				(Override.Entity.IsLocal || Override.Entity.Key == SourceRootKey))
				return std::unexpected("Linked prefab removal operations require a non-root source entity address");
			if (Override.Operation == PrefabOverrideOperation::Rename && Override.Name.empty())
				return std::unexpected("Linked prefab rename overrides require a non-empty name");
			if (Override.Operation == PrefabOverrideOperation::Reparent && !Override.Entity.IsLocal &&
				Override.Entity.Key == SourceRootKey)
				return std::unexpected("Linked prefab root parenting is destination placement, not an override");
			if (Override.Operation == PrefabOverrideOperation::Reparent && !Override.Parent)
				return std::unexpected("Linked prefab child reparent overrides require a parent inside the linked instance");
			if ((Override.Operation == PrefabOverrideOperation::Reparent) != Override.Parent.has_value())
				return std::unexpected("Linked prefab override contains a parent payload that does not match its operation");
			if ((Override.Operation == PrefabOverrideOperation::Rename) != !Override.Name.empty())
				return std::unexpected("Linked prefab override contains a name payload that does not match its operation");
			const bool PropertyOperation = Override.Operation == PrefabOverrideOperation::SetProperty ||
				Override.Operation == PrefabOverrideOperation::ResetProperty;
			const bool ComponentOperation = Override.Operation == PrefabOverrideOperation::AddComponent ||
				Override.Operation == PrefabOverrideOperation::RemoveComponent;
			if (PropertyOperation != (!Override.ComponentID.empty() && !Override.PropertyID.empty()) ||
				(ComponentOperation && (Override.ComponentID.empty() || !Override.PropertyID.empty())) ||
				(!PropertyOperation && !ComponentOperation && (!Override.ComponentID.empty() || !Override.PropertyID.empty())))
				return std::unexpected("Linked prefab override contains component/property fields that do not match its operation");
			const OverrideIdentity Identity{
				{ Override.Entity.Key, Override.Entity.IsLocal }, GetOperationClass(Override.Operation),
				PropertyOperation || ComponentOperation ? std::string_view(Override.ComponentID) : std::string_view{},
				PropertyOperation ? std::string_view(Override.PropertyID) : std::string_view{} };
			if (!OverrideIdentities.insert(Identity).second)
				return std::unexpected("Linked prefab metadata contains duplicate or conflicting operations for one stable address");
			const OverrideEntityAddress EntityAddress{ Override.Entity.Key, Override.Entity.IsLocal };
			const bool IsRemoval = Override.Operation == PrefabOverrideOperation::RemoveSourceEntity;
			const auto [ExistingRemoval, InsertedAddress] = EntityRemovalOverrides.try_emplace(EntityAddress, IsRemoval);
			if (!InsertedAddress && (IsRemoval || ExistingRemoval->second))
				return std::unexpected("A source-entity removal cannot coexist with another override for the same entity");
			if (Override.Operation == PrefabOverrideOperation::Reparent && Override.Parent)
			{
				const bool ParentKnown = Override.Parent->IsLocal
					? LocalKeys.contains(Override.Parent->Key)
					: SourceKeys.contains(Override.Parent->Key);
				if (!ParentKnown) return std::unexpected("Linked prefab reparent override refers to an unknown parent key");
			}
			if (Override.Operation == PrefabOverrideOperation::SetProperty)
			{
				const bool ValidValue = std::visit([](const auto& Value)
				{
					using T = std::decay_t<decltype(Value)>;
					if constexpr (std::is_same_v<T, float>) return std::isfinite(Value);
					else if constexpr (std::is_same_v<T, glm::vec3>)
						return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
					else if constexpr (std::is_same_v<T, glm::quat>)
					{
						const float Length = glm::length(Value);
						return std::isfinite(Value.w) && std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) &&
							std::isfinite(Length) && Length > 0.000001f;
					}
					else if constexpr (std::is_same_v<T, std::string>) return Value.size() <= 4096;
					else return true;
				}, Override.Value);
				if (!ValidValue) return std::unexpected("Linked prefab property override contains an invalid typed value");
			}
		}
		for (const PrefabConflictRecord& Conflict : Conflicts)
		{
			if (static_cast<uint8_t>(Conflict.Kind) > static_cast<uint8_t>(PrefabConflictKind::UnsupportedNestedInstance))
				return std::unexpected("Linked prefab conflict contains an unsupported kind");
			if (Conflict.Entity.Key.IsNil() || Conflict.ComponentID.size() > 128 || Conflict.PropertyID.size() > 128 ||
				Conflict.Message.size() > 4096)
				return std::unexpected("Linked prefab conflict record is invalid or oversized");
			if (Conflict.Entity.IsLocal ? !LocalKeys.contains(Conflict.Entity.Key) : !SourceKeys.contains(Conflict.Entity.Key))
				return std::unexpected("Linked prefab conflict refers to an unknown entity key");
		}
		return {};
	}
} // namespace PulseForge
