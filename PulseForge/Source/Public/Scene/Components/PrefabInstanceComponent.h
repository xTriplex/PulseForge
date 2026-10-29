#pragma once

#include "Assets/AssetID.h"
#include "Scene/PropertyMetadata.h"
#include "Scene/UUID.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
	struct PrefabEntityAddress
	{
		UUID Key;
		bool IsLocal = false;

		friend bool operator==(const PrefabEntityAddress&, const PrefabEntityAddress&) = default;
	};

	struct PrefabEntityMapping
	{
		UUID SourceKey;
		UUID SceneEntity;

		friend bool operator==(const PrefabEntityMapping&, const PrefabEntityMapping&) = default;
	};

	struct PrefabLocalEntityMapping
	{
		UUID LocalKey;
		UUID SceneEntity;

		friend bool operator==(const PrefabLocalEntityMapping&, const PrefabLocalEntityMapping&) = default;
	};

	enum class PrefabOverrideOperation : uint8_t
	{
		SetProperty,
		ResetProperty,
		AddComponent,
		RemoveComponent,
		AddLocalEntity,
		RemoveSourceEntity,
		Reparent,
		Rename
	};

	struct PrefabOverrideRecord
	{
		PrefabEntityAddress Entity;
		PrefabOverrideOperation Operation = PrefabOverrideOperation::SetProperty;
		std::string ComponentID;
		std::string PropertyID;
		PropertyValue Value = false;
		std::optional<PrefabEntityAddress> Parent;
		std::string Name;

		friend bool operator==(const PrefabOverrideRecord&, const PrefabOverrideRecord&) = default;
	};

	enum class PrefabConflictKind : uint8_t
	{
		MissingSourceAsset,
		RemovedSourceEntity,
		MissingProperty,
		ChangedPropertyType,
		ConflictingReparent,
		RemovedEntityHasLocalChildren,
		StalePlan,
		UnsupportedNestedInstance
	};

	struct PrefabConflictRecord
	{
		PrefabEntityAddress Entity;
		PrefabConflictKind Kind = PrefabConflictKind::MissingSourceAsset;
		std::string ComponentID;
		std::string PropertyID;
		std::string Message;

		friend bool operator==(const PrefabConflictRecord&, const PrefabConflictRecord&) = default;
	};

	// Persisted on a linked instance's root entity. Source UUIDs are stable keys scoped by SourceAsset.
	struct PrefabInstanceComponent
	{
		UUID InstanceID;
		AssetID SourceAsset;
		UUID SourceRootKey;
		std::string AppliedSourceFingerprint;
		std::vector<PrefabEntityMapping> SourceEntities;
		std::vector<PrefabLocalEntityMapping> LocalEntities;
		std::vector<PrefabOverrideRecord> Overrides;
		std::vector<PrefabConflictRecord> Conflicts;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate(UUID LinkedRootEntity) const;

		friend bool operator==(const PrefabInstanceComponent&, const PrefabInstanceComponent&) = default;
	};
} // namespace PulseForge
