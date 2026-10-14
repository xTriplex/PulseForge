#include "Editor/EditorSceneHierarchyOperations.h"

#include <limits>
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	std::expected<std::vector<PulseForge::Entity>, std::string>
	EditorSceneHierarchyOperations::GetTopLevelSelection(
		const PulseForge::Scene& SceneValue,
		std::span<const PulseForge::UUID> SelectedIdentifiers)
	{
		const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Selected(
			SelectedIdentifiers.begin(), SelectedIdentifiers.end());
		std::vector<PulseForge::Entity> Roots;
		for (const PulseForge::UUID& Identifier : SelectedIdentifiers)
		{
			const auto Candidate = SceneValue.FindEntity(Identifier);
			if (!Candidate)
				continue;
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
			if (!Parent)
				return std::unexpected("Could not inspect the selected entity hierarchy: " + Parent.error().Message);
			if (!HasSelectedAncestor)
				Roots.push_back(*Candidate);
		}
		return Roots;
	}

	std::expected<std::vector<PulseForge::Entity>, std::string>
	EditorSceneHierarchyOperations::CollectSubtrees(std::span<const PulseForge::Entity> Roots)
	{
		std::vector<PulseForge::Entity> Subtree;
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
		const auto Visit = [&Subtree, &Visited](auto&& Self, const PulseForge::Entity& Current)
			-> std::expected<void, std::string>
		{
			if (!Visited.insert(Current.GetUUID()).second)
				return {};
			Subtree.push_back(Current);
			auto Children = Current.GetChildren();
			if (!Children)
				return std::unexpected(Children.error().Message);
			for (const PulseForge::Entity& Child : *Children)
				if (auto Result = Self(Self, Child); !Result)
					return Result;
			return {};
		};
		for (const PulseForge::Entity& Root : Roots)
			if (auto Result = Visit(Visit, Root); !Result)
				return std::unexpected(Result.error());
		return Subtree;
	}

	std::string EditorSceneHierarchyOperations::MakeUniqueName(
		const PulseForge::Scene& SceneValue,
		std::string BaseName,
		std::optional<PulseForge::UUID> IgnoredEntity)
	{
		std::unordered_set<std::string> ExistingNames;
		for (const PulseForge::Entity& Entity : SceneValue.GetEntities())
			if (!IgnoredEntity || Entity.GetUUID() != *IgnoredEntity)
				if (const auto Tag = Entity.GetTag())
					ExistingNames.insert(Tag->Name);
		if (!ExistingNames.contains(BaseName))
			return BaseName;
		for (uint32_t Suffix = 2; Suffix < (std::numeric_limits<uint32_t>::max)(); ++Suffix)
		{
			std::string Candidate = BaseName + " " + std::to_string(Suffix);
			if (!ExistingNames.contains(Candidate))
				return Candidate;
		}
		return BaseName;
	}

	std::expected<std::vector<PulseForge::UUID>, PulseForge::SceneError>
	EditorSceneHierarchyOperations::DuplicateSubtrees(
		PulseForge::Scene& SceneValue,
		std::span<const PulseForge::Entity> Roots)
	{
		std::vector<PulseForge::UUID> DuplicatedRoots;
		const auto DuplicateSubtree = [&SceneValue](auto&& Self,
			const PulseForge::Entity& Source,
			std::optional<PulseForge::UUID> DuplicatedParent)
			-> std::expected<PulseForge::Entity, PulseForge::SceneError>
		{
			auto Duplicated = SceneValue.DuplicateEntity(Source);
			if (!Duplicated)
				return std::unexpected(Duplicated.error());
			auto Tag = Duplicated->GetTag();
			if (!Tag)
				return std::unexpected(Tag.error());
			Tag->Name = MakeUniqueName(SceneValue, Tag->Name, Duplicated->GetUUID());
			if (auto Result = Duplicated->SetTag(*Tag); !Result)
				return std::unexpected(Result.error());
			if (DuplicatedParent)
			{
				const auto Parent = SceneValue.FindEntity(*DuplicatedParent);
				if (!Parent)
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidEntity,
						"A duplicated parent entity could not be resolved" });
				if (auto Result = Duplicated->SetParent(*Parent); !Result)
					return std::unexpected(Result.error());
			}

			auto Children = Source.GetChildren();
			if (!Children)
				return std::unexpected(Children.error());
			for (const PulseForge::Entity& Child : *Children)
				if (auto Result = Self(Self, Child, Duplicated->GetUUID()); !Result)
					return std::unexpected(Result.error());
			return *Duplicated;
		};

		for (const PulseForge::Entity& Root : Roots)
		{
			auto Duplicated = DuplicateSubtree(DuplicateSubtree, Root, std::nullopt);
			if (!Duplicated)
				return std::unexpected(Duplicated.error());
			DuplicatedRoots.push_back(Duplicated->GetUUID());
		}
		return DuplicatedRoots;
	}
}
