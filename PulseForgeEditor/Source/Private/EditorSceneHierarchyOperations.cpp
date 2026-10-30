#include "Editor/EditorSceneHierarchyOperations.h"

#include <limits>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	std::expected<std::vector<PulseForge::Entity>, std::string> EditorSceneHierarchyOperations::GetTopLevelSelection(
		const PulseForge::Scene& SceneValue, std::span<const PulseForge::UUID> SelectedIdentifiers)
	{
		const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Selected(SelectedIdentifiers.begin(), SelectedIdentifiers.end());
		std::vector<PulseForge::Entity> Roots;
		for (const PulseForge::UUID& Identifier : SelectedIdentifiers)
		{
			const auto Candidate = SceneValue.FindEntity(Identifier);
			if (!Candidate) continue;
			bool HasSelectedAncestor = false;
			auto Parent = Candidate->GetParent();
			while (Parent && Parent->has_value())
			{
				if (Selected.contains((**Parent).GetUUID()))
				{
					HasSelectedAncestor = true;
					break;
				}
				Parent = (**Parent).GetParent();
			}
			if (!Parent) return std::unexpected("Could not inspect the selected entity hierarchy: " + Parent.error().Message);
			if (!HasSelectedAncestor) Roots.push_back(*Candidate);
		}
		return Roots;
	}

	std::expected<std::vector<PulseForge::Entity>, std::string> EditorSceneHierarchyOperations::CollectSubtrees(
		std::span<const PulseForge::Entity> Roots)
	{
		std::vector<PulseForge::Entity> Subtree;
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
		const auto Visit = [&Subtree, &Visited](auto&& Self, const PulseForge::Entity& Current) -> std::expected<void, std::string>
		{
			if (!Visited.insert(Current.GetUUID()).second) return {};
			Subtree.push_back(Current);
			auto Children = Current.GetChildren();
			if (!Children) return std::unexpected(Children.error().Message);
			for (const PulseForge::Entity& Child : *Children)
				if (auto Result = Self(Self, Child); !Result) return Result;
			return {};
		};
		for (const PulseForge::Entity& Root : Roots)
			if (auto Result = Visit(Visit, Root); !Result) return std::unexpected(Result.error());
		return Subtree;
	}

	std::string EditorSceneHierarchyOperations::MakeUniqueName(
		const PulseForge::Scene& SceneValue, std::string BaseName, std::optional<PulseForge::UUID> IgnoredEntity)
	{
		std::unordered_set<std::string> ExistingNames;
		for (const PulseForge::Entity& Entity : SceneValue.GetEntities())
			if (!IgnoredEntity || Entity.GetUUID() != *IgnoredEntity)
				if (const auto Tag = Entity.GetTag()) ExistingNames.insert(Tag->Name);
		if (!ExistingNames.contains(BaseName)) return BaseName;
		for (uint32_t Suffix = 2; Suffix < (std::numeric_limits<uint32_t>::max)(); ++Suffix)
		{
			std::string Candidate = BaseName + " " + std::to_string(Suffix);
			if (!ExistingNames.contains(Candidate)) return Candidate;
		}
		return BaseName;
	}

	std::expected<std::vector<PulseForge::UUID>, PulseForge::SceneError> EditorSceneHierarchyOperations::DuplicateSubtrees(
		PulseForge::Scene& SceneValue, std::span<const PulseForge::Entity> Roots)
	{
		std::vector<PulseForge::UUID> DuplicatedRoots;
		std::vector<PulseForge::UUID> TransactionEntities;
		const auto Rollback = [&SceneValue, &TransactionEntities]()
		{
			for (auto Iterator = TransactionEntities.rbegin(); Iterator != TransactionEntities.rend(); ++Iterator)
				if (const auto Entity = SceneValue.FindEntity(*Iterator)) (void)SceneValue.DestroyEntity(*Entity);
		};
		const auto DuplicateSubtree =
			[&SceneValue, &TransactionEntities](auto&& Self, const PulseForge::Entity& Source,
				std::optional<PulseForge::UUID> DuplicatedParent,
				std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash>& IdentityMap,
				bool PreserveNames,
				bool IsRoot)
				-> std::expected<PulseForge::Entity, PulseForge::SceneError>
		{
			const auto SourcePrefab = Source.GetPrefabInstance();
			if (!SourcePrefab) return std::unexpected(SourcePrefab.error());
			if (SourcePrefab->has_value() && !IsRoot)
				return std::unexpected(PulseForge::SceneError{
					PulseForge::SceneErrorCode::InvalidPrefabInstance, "Nested linked prefab instances cannot be duplicated" });
			auto Duplicated = SceneValue.DuplicateEntity(Source, IsRoot);
			if (!Duplicated) return std::unexpected(Duplicated.error());
			TransactionEntities.push_back(Duplicated->GetUUID());
			IdentityMap.emplace(Source.GetUUID(), Duplicated->GetUUID());
			auto Tag = Duplicated->GetTag();
			if (!Tag) return std::unexpected(Tag.error());
			if (!PreserveNames)
			{
				Tag->Name = MakeUniqueName(SceneValue, Tag->Name, Duplicated->GetUUID());
				if (auto Result = Duplicated->SetTag(*Tag); !Result) return std::unexpected(Result.error());
			}
			if (DuplicatedParent)
			{
				const auto Parent = SceneValue.FindEntity(*DuplicatedParent);
				if (!Parent)
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidEntity, "A duplicated parent entity could not be resolved" });
				if (auto Result = Duplicated->SetParent(*Parent); !Result) return std::unexpected(Result.error());
			}

			auto Children = Source.GetChildren();
			if (!Children) return std::unexpected(Children.error());
			for (const PulseForge::Entity& Child : *Children)
				if (auto Result = Self(Self, Child, Duplicated->GetUUID(), IdentityMap, PreserveNames, false); !Result)
					return std::unexpected(Result.error());
			return *Duplicated;
		};

		for (const PulseForge::Entity& Root : Roots)
		{
			std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash> IdentityMap;
			const auto SourcePrefab = Root.GetPrefabInstance();
			if (!SourcePrefab)
			{
				Rollback();
				return std::unexpected(SourcePrefab.error());
			}
			std::optional<PulseForge::UUID> NewInstanceID;
			std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash> LocalKeys;
			if (SourcePrefab->has_value())
			{
				const std::array<PulseForge::Entity, 1> RootSpan{ Root };
				auto Subtree = CollectSubtrees(RootSpan);
				if (!Subtree)
				{
					Rollback();
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidPrefabInstance, Subtree.error() });
				}
				std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> SubtreeIDs;
				SubtreeIDs.reserve(Subtree->size());
				for (const PulseForge::Entity& Current : *Subtree) SubtreeIDs.insert(Current.GetUUID());
				for (const PulseForge::PrefabEntityMapping& Mapping : (**SourcePrefab).SourceEntities)
					if (!SubtreeIDs.contains(Mapping.SceneEntity))
					{
						Rollback();
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance,
							"Cannot duplicate a linked instance with a source mapping outside its subtree" });
					}
				for (const PulseForge::PrefabLocalEntityMapping& Mapping : (**SourcePrefab).LocalEntities)
					if (!SubtreeIDs.contains(Mapping.SceneEntity))
					{
						Rollback();
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance,
							"Cannot duplicate a linked instance with a local mapping outside its subtree" });
					}
				const auto GeneratedInstance = PulseForge::UUID::Generate();
				if (!GeneratedInstance)
				{
					Rollback();
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::UUIDGenerationFailed, GeneratedInstance.error().Message });
				}
				NewInstanceID = *GeneratedInstance;
				for (const PulseForge::PrefabLocalEntityMapping& Mapping : (**SourcePrefab).LocalEntities)
				{
					const auto NewKey = PulseForge::UUID::Generate();
					if (!NewKey)
					{
						Rollback();
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::UUIDGenerationFailed, NewKey.error().Message });
					}
					LocalKeys.emplace(Mapping.LocalKey, *NewKey);
				}
			}

			auto Duplicated = DuplicateSubtree(DuplicateSubtree, Root, std::nullopt, IdentityMap, SourcePrefab->has_value(), true);
			if (!Duplicated)
			{
				Rollback();
				return std::unexpected(Duplicated.error());
			}
			if (SourcePrefab->has_value())
			{
				PulseForge::PrefabInstanceComponent DuplicatedMetadata = **SourcePrefab;
				DuplicatedMetadata.InstanceID = *NewInstanceID;
				for (PulseForge::PrefabEntityMapping& Mapping : DuplicatedMetadata.SourceEntities)
				{
					const auto Found = IdentityMap.find(Mapping.SceneEntity);
					if (Found == IdentityMap.end())
					{
						Rollback();
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, "Duplicated subtree lost a source entity mapping" });
					}
					Mapping.SceneEntity = Found->second;
				}
				for (PulseForge::PrefabLocalEntityMapping& Mapping : DuplicatedMetadata.LocalEntities)
				{
					const auto Found = IdentityMap.find(Mapping.SceneEntity);
					if (Found == IdentityMap.end())
					{
						Rollback();
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, "Duplicated subtree lost a local entity mapping" });
					}
					Mapping.SceneEntity = Found->second;
					Mapping.LocalKey = LocalKeys.at(Mapping.LocalKey);
				}
				const auto RemapLocalAddress = [&LocalKeys](PulseForge::PrefabEntityAddress& Address)
				{
					if (Address.IsLocal)
						if (const auto Found = LocalKeys.find(Address.Key); Found != LocalKeys.end()) Address.Key = Found->second;
				};
				for (PulseForge::PrefabOverrideRecord& Override : DuplicatedMetadata.Overrides)
				{
					RemapLocalAddress(Override.Entity);
					if (Override.Parent) RemapLocalAddress(*Override.Parent);
				}
				for (PulseForge::PrefabConflictRecord& Conflict : DuplicatedMetadata.Conflicts)
					RemapLocalAddress(Conflict.Entity);
				if (auto Result = Duplicated->SetPrefabInstance(DuplicatedMetadata); !Result)
				{
					Rollback();
					return std::unexpected(Result.error());
				}
			}
			DuplicatedRoots.push_back(Duplicated->GetUUID());
		}
		return DuplicatedRoots;
	}
} // namespace PulseForgeEditor
