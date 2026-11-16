#include "Editor/EditorResourceInventory.h"

#include "Assets/AssetDependencyCatalog.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace PulseForgeEditor
{
	std::string GetEditorResourceInventoryProjectIdentity(
		const std::filesystem::path& ProjectRoot,
		const PulseForge::UUID& ProjectID)
	{
		std::error_code Error;
		const std::filesystem::path AbsoluteRoot = std::filesystem::absolute(ProjectRoot, Error);
		const std::filesystem::path IdentityRoot = (Error ? ProjectRoot : AbsoluteRoot).lexically_normal();
		return ProjectID.ToString() + "@" + IdentityRoot.generic_string();
	}

	namespace
	{
		[[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path& Path)
		{
			if (Path.empty() || Path.is_absolute()) return false;
			for (const auto& Component : Path)
				if (Component == "..") return false;
			return true;
		}

		enum class DirectoryRootSafety : uint8_t
		{
			Safe,
			Missing,
			Unsafe
		};

		[[nodiscard]] DirectoryRootSafety InspectProjectSourcePath(
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& RelativePath,
			std::string& FailureReason)
		{
			if (!IsSafeRelativePath(RelativePath))
			{
				FailureReason = "Registry path is not a safe project-relative path";
				return DirectoryRootSafety::Unsafe;
			}

			std::filesystem::path Current = ProjectRoot;
			for (auto Component = RelativePath.begin(); Component != RelativePath.end(); ++Component)
			{
				if (*Component == ".") continue;
				Current /= *Component;
				std::error_code Error;
				const std::filesystem::file_status Status = std::filesystem::symlink_status(Current, Error);
				if (Error == std::errc::no_such_file_or_directory ||
					(!Error && Status.type() == std::filesystem::file_type::not_found))
					return DirectoryRootSafety::Missing;
				if (Error)
				{
					FailureReason = Error.message();
					return DirectoryRootSafety::Unsafe;
				}
				if (std::filesystem::is_symlink(Status))
				{
					FailureReason = "Source path contains a symlink component";
					return DirectoryRootSafety::Unsafe;
				}
				const bool IsFinalComponent = std::next(Component) == RelativePath.end();
				if ((!IsFinalComponent && !std::filesystem::is_directory(Status)) ||
					(IsFinalComponent && !std::filesystem::is_regular_file(Status)))
				{
					FailureReason = IsFinalComponent ? "Source is not a regular file" : "Source path contains a non-directory component";
					return DirectoryRootSafety::Unsafe;
				}
			}
			return DirectoryRootSafety::Safe;
		}

		[[nodiscard]] DirectoryRootSafety InspectDerivedDataRoot(
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& DerivedDataRoot,
			std::string& FailureReason)
		{
			const std::filesystem::path Relative = DerivedDataRoot.lexically_normal().lexically_relative(ProjectRoot.lexically_normal());
			if (!IsSafeRelativePath(Relative))
			{
				FailureReason = "Derived-data root is outside the active project";
				return DirectoryRootSafety::Unsafe;
			}
			std::filesystem::path Current = ProjectRoot;
			for (const auto& Component : Relative)
			{
				if (Component == ".") continue;
				Current /= Component;
				std::error_code Error;
				const auto Status = std::filesystem::symlink_status(Current, Error);
				if (Error == std::errc::no_such_file_or_directory) return DirectoryRootSafety::Missing;
				if (Error)
				{
					FailureReason = Error.message();
					return DirectoryRootSafety::Unsafe;
				}
				if (std::filesystem::is_symlink(Status) || !std::filesystem::is_directory(Status))
				{
					FailureReason = "Derived-data path contains a symlink or non-directory component";
					return DirectoryRootSafety::Unsafe;
				}
			}
			return DirectoryRootSafety::Safe;
		}

		[[nodiscard]] std::string DependencyIdentity(const PulseForge::AssetDependencyNode& Node)
		{
			if (Node.IsManagedAsset())
				return Node.Asset.ToString() + (Node.SubresourceKey.empty() ? std::string{} : ":" + Node.SubresourceKey);
			return Node.SourceFileKey + (Node.SubresourceKey.empty() ? std::string{} : ":" + Node.SubresourceKey);
		}

		void AddKnownBytes(EditorResourceInventorySnapshot& Snapshot, uint64_t Bytes) noexcept
		{
			if (Bytes > std::numeric_limits<uint64_t>::max() - Snapshot.KnownUniquePathBytes)
				Snapshot.KnownUniquePathBytes = std::numeric_limits<uint64_t>::max();
			else
				Snapshot.KnownUniquePathBytes += Bytes;
		}

		void Cancel(EditorResourceInventorySnapshot& Snapshot)
		{
			Snapshot.Cancelled = true;
			Snapshot.Entries.clear();
			Snapshot.KnownUniquePathBytes = 0;
			Snapshot.UnavailableStorageCount = 0;
			Snapshot.OmittedSourceAssets = 0;
			Snapshot.OmittedDerivedArtifacts = 0;
			Snapshot.OmittedDependencyEdges = 0;
			Snapshot.DerivedArtifactsTruncated = false;
			Snapshot.DependencyEdgesTruncated = false;
		}
	}

	EditorResourceInventorySnapshot BuildEditorResourceInventory(
		const PulseForge::AssetDependencyCatalogSnapshot& Catalog,
		const std::filesystem::path& DerivedDataRoot,
		const EditorResourceInventoryLimits& Limits,
		std::stop_token CancellationToken)
	{
		EditorResourceInventorySnapshot Snapshot;
		Snapshot.ProjectIdentity = GetEditorResourceInventoryProjectIdentity(
			Catalog.GetProjectRootPath(), Catalog.GetProjectID());
		Snapshot.Generation = Catalog.GetGeneration();
		if (CancellationToken.stop_requested())
		{
			Cancel(Snapshot);
			return Snapshot;
		}

		std::unordered_map<PulseForge::AssetID, std::vector<std::string>, PulseForge::UUIDHash> DependenciesByAsset;
		size_t VisitedDependencyEdges = 0;
		for (const PulseForge::AssetDependencyEdge& Edge : Catalog.GetEdges())
		{
			if (CancellationToken.stop_requested())
			{
				Cancel(Snapshot);
				return Snapshot;
			}
			if (VisitedDependencyEdges >= Limits.MaximumDependencyEdgesVisited)
			{
				Snapshot.DependencyEdgesTruncated = true;
				Snapshot.OmittedDependencyEdges = Catalog.GetEdges().size() - VisitedDependencyEdges;
				break;
			}
			++VisitedDependencyEdges;
			if (Edge.Referrer.Asset.IsNil()) continue;
			DependenciesByAsset[Edge.Referrer.Asset].push_back(DependencyIdentity(Edge.Target));
		}
		for (auto& [Asset, Dependencies] : DependenciesByAsset)
		{
			(void)Asset;
			std::ranges::sort(Dependencies);
			Dependencies.erase(std::unique(Dependencies.begin(), Dependencies.end()), Dependencies.end());
		}

		const auto& Registry = Catalog.GetRegistry();
		const std::vector<PulseForge::AssetRecord> Assets = Registry.GetAssets(Limits.MaximumSourceAssets);
		const size_t SourceCount = Assets.size();
		Snapshot.OmittedSourceAssets = Registry.GetAssetCount() - SourceCount;
		Snapshot.Entries.reserve(SourceCount + (std::min)(Limits.MaximumDerivedArtifacts, size_t{ 256 }));
		std::unordered_set<std::string> CountedStorage;
		for (size_t Index = 0; Index < SourceCount; ++Index)
		{
			if (CancellationToken.stop_requested())
			{
				Cancel(Snapshot);
				return Snapshot;
			}
			const PulseForge::AssetRecord& Asset = Assets[Index];
			EditorResourceInventoryEntry Entry;
			Entry.Identity = Asset.ID.ToString();
			Entry.Owner = "AssetRegistry";
			Entry.State = "registered-source";
			Entry.StorageIdentity = "source:" + Asset.ProjectRelativePath.generic_string();
			if (const auto Found = DependenciesByAsset.find(Asset.ID); Found != DependenciesByAsset.end())
			{
				const size_t DependencyCount = (std::min)(Found->second.size(), Limits.MaximumDependenciesPerEntry);
				Entry.Dependencies.assign(Found->second.begin(), Found->second.begin() + DependencyCount);
				Entry.OmittedDependencies = Found->second.size() - DependencyCount;
			}

			std::string SourceFailure;
			const DirectoryRootSafety SourceSafety = InspectProjectSourcePath(
				Catalog.GetProjectRootPath(), Asset.ProjectRelativePath, SourceFailure);
			if (SourceSafety == DirectoryRootSafety::Missing)
			{
				Entry.Availability = EditorResourceInventoryState::Missing;
				Entry.State = "missing-source";
				Entry.AvailabilityReason = "Source file is missing from the project";
			}
			else if (SourceSafety == DirectoryRootSafety::Unsafe)
			{
				Entry.Availability = EditorResourceInventoryState::Unavailable;
				Entry.State = "unavailable-source";
				Entry.AvailabilityReason = std::move(SourceFailure);
			}
			else
			{
				const std::filesystem::path Path = Catalog.GetProjectRootPath() / Asset.ProjectRelativePath;
				std::error_code Error;
				const uintmax_t Size = std::filesystem::file_size(Path, Error);
				if (Error || Size > std::numeric_limits<uint64_t>::max())
				{
					Entry.Availability = EditorResourceInventoryState::Unavailable;
					Entry.AvailabilityReason = Error ? Error.message() : "Source size exceeds the supported byte counter";
				}
				else Entry.StorageBytes = static_cast<uint64_t>(Size);
			}
			if (!Entry.StorageBytes) ++Snapshot.UnavailableStorageCount;
			else if (CountedStorage.insert(Entry.StorageIdentity).second) AddKnownBytes(Snapshot, *Entry.StorageBytes);
			Snapshot.Entries.push_back(std::move(Entry));
		}

		if (!DerivedDataRoot.empty() && Limits.MaximumDerivedArtifacts > 0)
		{
			std::string RootFailure;
			const DirectoryRootSafety RootSafety = InspectDerivedDataRoot(
				Catalog.GetProjectRootPath(), DerivedDataRoot, RootFailure);
			if (RootSafety == DirectoryRootSafety::Unsafe)
			{
				Snapshot.Entries.push_back({ "derived-root", "DerivedDataArtifactStore", "unavailable", {}, {}, 0, std::nullopt,
					EditorResourceInventoryState::Unavailable, std::move(RootFailure) });
				++Snapshot.UnavailableStorageCount;
			}
			else if (RootSafety == DirectoryRootSafety::Safe)
			{
				std::error_code Error;
				const auto RootStatus = std::filesystem::symlink_status(DerivedDataRoot, Error);
				if (Error || !std::filesystem::is_directory(RootStatus) || std::filesystem::is_symlink(RootStatus))
				{
					Snapshot.Entries.push_back({ "derived-root", "DerivedDataArtifactStore", "unavailable", {}, {}, 0, std::nullopt,
						EditorResourceInventoryState::Unavailable, Error ? Error.message() : "Derived-data root is not a safe directory" });
					++Snapshot.UnavailableStorageCount;
				}
				else
				{
					std::filesystem::recursive_directory_iterator Iterator(
						DerivedDataRoot, std::filesystem::directory_options::skip_permission_denied, Error);
					const std::filesystem::recursive_directory_iterator End;
					size_t AddedArtifacts = 0;
					size_t VisitedEntries = 0;
					while (!Error && Iterator != End)
					{
						if (CancellationToken.stop_requested())
						{
							Cancel(Snapshot);
							return Snapshot;
						}
						if (VisitedEntries++ >= Limits.MaximumVisitedDerivedEntries)
						{
							Snapshot.DerivedArtifactsTruncated = true;
							Snapshot.OmittedDerivedArtifacts = 1;
							break;
						}
						const std::filesystem::directory_entry& Item = *Iterator;
						const auto Status = Item.symlink_status(Error);
						if (Error) break;
						if (std::filesystem::is_symlink(Status))
						{
							if (std::filesystem::is_directory(Item.status(Error))) Iterator.disable_recursion_pending();
							if (Error) break;
						}
						else if (std::filesystem::is_regular_file(Status))
						{
							if (AddedArtifacts >= Limits.MaximumDerivedArtifacts)
							{
								Snapshot.DerivedArtifactsTruncated = true;
								Snapshot.OmittedDerivedArtifacts = 1;
								break;
							}
							EditorResourceInventoryEntry Entry;
							const std::filesystem::path Relative = std::filesystem::relative(Item.path(), DerivedDataRoot, Error);
							if (Error || !IsSafeRelativePath(Relative))
							{
								Error.clear();
								continue;
							}
							Entry.Identity = Relative.generic_string();
							Entry.Owner = "DerivedDataArtifactStore";
							Entry.State = "stored-artifact";
							Entry.StorageIdentity = "derived:" + Entry.Identity;
							const uintmax_t Size = Item.file_size(Error);
							if (Error || Size > std::numeric_limits<uint64_t>::max())
							{
								Entry.Availability = EditorResourceInventoryState::Unavailable;
								Entry.AvailabilityReason = Error ? Error.message() : "Artifact size exceeds the supported byte counter";
								Error.clear();
							}
							else
								Entry.StorageBytes = static_cast<uint64_t>(Size);
							if (!Entry.StorageBytes) ++Snapshot.UnavailableStorageCount;
							else if (CountedStorage.insert(Entry.StorageIdentity).second) AddKnownBytes(Snapshot, *Entry.StorageBytes);
							Snapshot.Entries.push_back(std::move(Entry));
							++AddedArtifacts;
						}
						Iterator.increment(Error);
					}
					if (Error)
					{
						Snapshot.Entries.push_back({ "derived-scan", "DerivedDataArtifactStore", "unavailable", {}, {}, 0, std::nullopt,
							EditorResourceInventoryState::Unavailable, Error.message() });
						++Snapshot.UnavailableStorageCount;
					}
				}
			}
		}
		if (CancellationToken.stop_requested())
		{
			Cancel(Snapshot);
			return Snapshot;
		}
		std::ranges::sort(Snapshot.Entries, {}, &EditorResourceInventoryEntry::Identity);
		return Snapshot;
	}
} // namespace PulseForgeEditor
