#include "TestEditorSupport.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetMetadata.h"
#include "Assets/MaterialAsset.h"
#include "Editor/EditorResourceInventory.h"
#include "Editor/EditorResourceInventoryService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
	using namespace PulseForge;
using namespace PulseForgeEditor;

	class InventoryFixture final
	{
	public:
		InventoryFixture()
		{
			const auto ID = PulseForge::UUID::Generate();
			if (!ID) return;
			ProjectID = *ID;
			Root = std::filesystem::temp_directory_path() / ("PulseForgeResourceInventory-" + ProjectID.ToString());
			std::error_code Error;
			std::filesystem::create_directories(Root / "Assets" / "Materials", Error);
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets" / "Textures", Error);
			if (Error) return;
			std::filesystem::create_directories(Root / ".pulseforge" / "DerivedData", Error);
			if (Error) return;
			const std::string Ppm = "P3\n1 1\n255\n32 64 96\n";
			if (!Write(Root / "Assets" / "Textures" / "Surface.ppm", Ppm)) return;
			const auto TextureMetadata = AssetMetadataSerializer::CreateForNewAsset(Root / "Assets" / "Textures" / "Surface.ppm");
			if (!TextureMetadata) return;
			MaterialAssetDesc Material;
			Material.BaseColorTexture = TextureMetadata->ID;
			const auto Serialized = MaterialAssetSerializer::Serialize(Material);
			if (!Serialized || !Write(Root / "Assets" / "Materials" / "Surface.material", *Serialized)) return;
			ExpectedSourceBytes = Serialized->size() + Ppm.size();
			const auto MaterialMetadata = AssetMetadataSerializer::CreateForNewAsset(Root / "Assets" / "Materials" / "Surface.material");
			if (!MaterialMetadata) return;
			if (!Write(Root / ".pulseforge" / "DerivedData" / "a.bin", "1234567")) return;
			if (!Write(Root / ".pulseforge" / "DerivedData" / "b.bin", "abcdefghijkl")) return;
			Valid = true;
		}

		~InventoryFixture()
		{
			std::error_code Error;
			if (!Root.empty()) std::filesystem::remove_all(Root, Error);
		}

		static bool Write(const std::filesystem::path& Path, std::string_view Bytes)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
			return static_cast<bool>(Output);
		}

		static bool CreateDirectoryLink(
			const std::filesystem::path& Target,
			const std::filesystem::path& Link,
			std::error_code& Error)
		{
#ifdef _WIN32
			DWORD Flags = SYMBOLIC_LINK_FLAG_DIRECTORY;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
			Flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
			if (CreateSymbolicLinkW(Link.c_str(), Target.c_str(), Flags)) return true;
			Error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			return false;
#else
			std::filesystem::create_directory_symlink(Target, Link, Error);
			return !Error;
#endif
		}

		static bool CreateFileLink(
			const std::filesystem::path& Target,
			const std::filesystem::path& Link,
			std::error_code& Error)
		{
#ifdef _WIN32
			DWORD Flags = 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
			Flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
			if (CreateSymbolicLinkW(Link.c_str(), Target.c_str(), Flags)) return true;
			Error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			return false;
#else
			std::filesystem::create_symlink(Target, Link, Error);
			return !Error;
#endif
		}

		static bool IsSymlinkUnavailable(const std::error_code& Error)
		{
#ifdef _WIN32
			return Error.value() == ERROR_PRIVILEGE_NOT_HELD || Error.value() == ERROR_ACCESS_DENIED ||
				Error.value() == ERROR_NOT_SUPPORTED;
#else
			(void)Error;
			return false;
#endif
		}

		PulseForge::UUID ProjectID;
		std::filesystem::path Root;
		uint64_t ExpectedSourceBytes = 0;
		bool Valid = false;
	};
}

void RunEditorResourceInventoryTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;
	InventoryFixture Fixture;
	PF_CHECK(Tests, Fixture.Valid);
	if (!Fixture.Valid) return;
	const auto Catalog = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 17);
	PF_CHECK(Tests, Catalog.has_value());
	if (!Catalog) return;
	const auto AllAssets = (*Catalog)->GetRegistry().GetAssets();
	const auto BoundedAssets = (*Catalog)->GetRegistry().GetAssets(1);
	PF_CHECK(Tests, BoundedAssets.size() == 1 && !AllAssets.empty() &&
		BoundedAssets.front().ProjectRelativePath == AllAssets.front().ProjectRelativePath);
	PF_CHECK(Tests, (*Catalog)->GetRegistry().GetAssets(0).empty());

	const EditorResourceInventorySnapshot Snapshot = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData");
	PF_CHECK(Tests, !Snapshot.Cancelled);
	PF_CHECK(Tests, Snapshot.ProjectIdentity == GetEditorResourceInventoryProjectIdentity(Fixture.Root, Fixture.ProjectID));
	const std::filesystem::path CopiedProjectRoot = Fixture.Root.parent_path() / (Fixture.Root.filename().string() + "-copy");
	PF_CHECK(Tests, GetEditorResourceInventoryProjectIdentity(CopiedProjectRoot, Fixture.ProjectID) != Snapshot.ProjectIdentity);
	PF_CHECK(Tests, Snapshot.Generation == 17);
	PF_CHECK(Tests, Snapshot.OmittedSourceAssets == 0);
	PF_CHECK(Tests, Snapshot.OmittedDerivedArtifacts == 0);
	PF_CHECK(Tests, Snapshot.Entries.size() == 4);
	PF_CHECK(Tests, Snapshot.KnownUniquePathBytes == 19 + Fixture.ExpectedSourceBytes);

	const auto Material = std::find_if(Snapshot.Entries.begin(), Snapshot.Entries.end(), [](const auto& Entry)
		{ return Entry.Owner == "AssetRegistry" && Entry.StorageIdentity.ends_with("Surface.material"); });
	PF_CHECK(Tests, Material != Snapshot.Entries.end());
	if (Material != Snapshot.Entries.end())
	{
		PF_CHECK(Tests, Material->Availability == EditorResourceInventoryState::Available);
		PF_CHECK(Tests, Material->Dependencies.size() == 1);
	}
	PF_CHECK(Tests, InventoryFixture::Write(Fixture.Root / ".pulseforge" / "DerivedData" / "b.bin", "abcdefghijklmn"));
	PF_CHECK(Tests, Snapshot.KnownUniquePathBytes == 19 + Fixture.ExpectedSourceBytes);
	const EditorResourceInventorySnapshot Refreshed = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData");
	PF_CHECK(Tests, Refreshed.KnownUniquePathBytes == 21 + Fixture.ExpectedSourceBytes);

	EditorResourceInventoryLimits Limits;
	Limits.MaximumSourceAssets = 1;
	Limits.MaximumDerivedArtifacts = 1;
	const EditorResourceInventorySnapshot Bounded = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData", Limits);
	PF_CHECK(Tests, Bounded.OmittedSourceAssets == 1);
	PF_CHECK(Tests, Bounded.OmittedDerivedArtifacts == 1);
	PF_CHECK(Tests, Bounded.DerivedArtifactsTruncated);
	PF_CHECK(Tests, Bounded.Entries.size() == 2);
	PF_CHECK(Tests, Bounded.UnavailableStorageCount == 0);
	EditorResourceInventoryLimits DependencyLimits;
	DependencyLimits.MaximumDependenciesPerEntry = 0;
	const EditorResourceInventorySnapshot DependencyBounded = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData", DependencyLimits);
	const auto BoundedMaterial = std::find_if(DependencyBounded.Entries.begin(), DependencyBounded.Entries.end(),
		[](const auto& Entry) { return Entry.Owner == "AssetRegistry" && Entry.StorageIdentity.ends_with("Surface.material"); });
	PF_CHECK(Tests, BoundedMaterial != DependencyBounded.Entries.end());
	if (BoundedMaterial != DependencyBounded.Entries.end())
		PF_CHECK(Tests, BoundedMaterial->Dependencies.empty() && BoundedMaterial->OmittedDependencies == 1);
	EditorResourceInventoryLimits EdgeLimits;
	EdgeLimits.MaximumDependencyEdgesVisited = 0;
	const EditorResourceInventorySnapshot EdgeBounded = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData", EdgeLimits);
	PF_CHECK(Tests, EdgeBounded.DependencyEdgesTruncated && EdgeBounded.OmittedDependencyEdges == 1);
	const auto EdgeBoundedMaterial = std::find_if(EdgeBounded.Entries.begin(), EdgeBounded.Entries.end(),
		[](const auto& Entry) { return Entry.Owner == "AssetRegistry" && Entry.StorageIdentity.ends_with("Surface.material"); });
	PF_CHECK(Tests, EdgeBoundedMaterial != EdgeBounded.Entries.end());
	if (EdgeBoundedMaterial != EdgeBounded.Entries.end()) PF_CHECK(Tests, EdgeBoundedMaterial->Dependencies.empty());

	std::stop_source Cancellation;
	Cancellation.request_stop();
	const EditorResourceInventorySnapshot Cancelled = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData", {}, Cancellation.get_token());
	PF_CHECK(Tests, Cancelled.Cancelled);
	PF_CHECK(Tests, Cancelled.Entries.empty());
	PF_CHECK(Tests, Cancelled.KnownUniquePathBytes == 0);

	// The immutable catalog may outlive a directory replacement. Inventory must not follow a new symlink
	// introduced between catalog creation and the read-only file-size scan.
	const std::filesystem::path LinkedAssetDirectory = Fixture.Root / "Assets" / "Linked";
	const std::filesystem::path ExternalAssetDirectory = Fixture.Root.parent_path() /
		("PulseForgeResourceInventoryExternal-" + Fixture.ProjectID.ToString());
	std::error_code Error;
	std::filesystem::create_directories(LinkedAssetDirectory, Error);
	PF_CHECK(Tests, !Error);
	std::filesystem::create_directories(ExternalAssetDirectory, Error);
	PF_CHECK(Tests, !Error);
	const std::filesystem::path LinkedAssetPath = LinkedAssetDirectory / "Linked.material";
	if (!Error)
	{
		MaterialAssetDesc LinkedMaterial;
		const auto LinkedBytes = MaterialAssetSerializer::Serialize(LinkedMaterial);
		PF_CHECK(Tests, LinkedBytes.has_value());
		if (LinkedBytes)
		{
			PF_CHECK(Tests, InventoryFixture::Write(LinkedAssetPath, *LinkedBytes));
			PF_CHECK(Tests, InventoryFixture::Write(ExternalAssetDirectory / "Linked.material", *LinkedBytes + std::string(4096, 'x')));
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(LinkedAssetPath);
			PF_CHECK(Tests, Metadata.has_value());
			const auto LinkedCatalog = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 18);
			PF_CHECK(Tests, LinkedCatalog.has_value());
			if (LinkedCatalog)
			{
				std::filesystem::remove_all(LinkedAssetDirectory, Error);
				PF_CHECK(Tests, !Error);
				const bool LinkCreated = InventoryFixture::CreateDirectoryLink(
					ExternalAssetDirectory, LinkedAssetDirectory, Error);
				#ifndef _WIN32
				PF_CHECK(Tests, LinkCreated);
				#else
				if (!LinkCreated) PF_CHECK(Tests, InventoryFixture::IsSymlinkUnavailable(Error));
				#endif
				if (LinkCreated)
				{
					const auto LinkSnapshot = BuildEditorResourceInventory(
						**LinkedCatalog, Fixture.Root / ".pulseforge" / "DerivedData");
					const auto LinkedEntry = std::find_if(LinkSnapshot.Entries.begin(), LinkSnapshot.Entries.end(),
						[](const auto& Entry) { return Entry.StorageIdentity.ends_with("Linked.material"); });
					PF_CHECK(Tests, LinkedEntry != LinkSnapshot.Entries.end());
					if (LinkedEntry != LinkSnapshot.Entries.end())
					{
						PF_CHECK(Tests, LinkedEntry->Availability == EditorResourceInventoryState::Unavailable);
						PF_CHECK(Tests, LinkedEntry->State == "unavailable-source");
						PF_CHECK(Tests, !LinkedEntry->StorageBytes);
					}
					PF_CHECK(Tests, LinkSnapshot.KnownUniquePathBytes < 4096);
				}
			}
		}
	}
	std::filesystem::remove_all(LinkedAssetDirectory, Error);
	std::filesystem::remove_all(ExternalAssetDirectory, Error);
	Error.clear();
	const std::filesystem::path ExternalMaterialPath = Fixture.Root.parent_path() /
		("PulseForgeResourceInventoryExternal-" + Fixture.ProjectID.ToString() + ".material");
	const std::filesystem::path SurfaceMaterialPath = Fixture.Root / "Assets" / "Materials" / "Surface.material";
	PF_CHECK(Tests, InventoryFixture::Write(ExternalMaterialPath, std::string(8192, 'x')));
	std::filesystem::remove(SurfaceMaterialPath, Error);
	PF_CHECK(Tests, !Error);
	Error.clear();
	const bool FileLinkCreated = InventoryFixture::CreateFileLink(ExternalMaterialPath, SurfaceMaterialPath, Error);
	if (!FileLinkCreated && !Error)
		Error = std::make_error_code(std::errc::operation_not_permitted);
	#ifndef _WIN32
	PF_CHECK(Tests, FileLinkCreated);
	#else
	if (!FileLinkCreated) PF_CHECK(Tests, InventoryFixture::IsSymlinkUnavailable(Error));
	#endif
	if (FileLinkCreated)
	{
		const EditorResourceInventorySnapshot FileLinkSnapshot = BuildEditorResourceInventory(
			**Catalog, Fixture.Root / ".pulseforge" / "DerivedData");
		const auto FileLinkedMaterial = std::find_if(FileLinkSnapshot.Entries.begin(), FileLinkSnapshot.Entries.end(),
			[](const auto& Entry) { return Entry.Owner == "AssetRegistry" && Entry.StorageIdentity.ends_with("Surface.material"); });
		PF_CHECK(Tests, FileLinkedMaterial != FileLinkSnapshot.Entries.end());
		if (FileLinkedMaterial != FileLinkSnapshot.Entries.end())
		{
			PF_CHECK(Tests, FileLinkedMaterial->Availability == EditorResourceInventoryState::Unavailable);
			PF_CHECK(Tests, FileLinkedMaterial->State == "unavailable-source");
			PF_CHECK(Tests, !FileLinkedMaterial->StorageBytes);
		}
		PF_CHECK(Tests, FileLinkSnapshot.KnownUniquePathBytes < 8192);
	}
	std::filesystem::remove(SurfaceMaterialPath, Error);
	std::filesystem::remove(ExternalMaterialPath, Error);
	Error.clear();
	std::filesystem::remove(Fixture.Root / "Assets" / "Materials" / "Surface.material", Error);
	PF_CHECK(Tests, !Error);
	const EditorResourceInventorySnapshot MissingSource = BuildEditorResourceInventory(
		**Catalog, Fixture.Root / ".pulseforge" / "DerivedData");
	const auto MissingMaterial = std::find_if(MissingSource.Entries.begin(), MissingSource.Entries.end(),
		[](const auto& Entry) { return Entry.Owner == "AssetRegistry" && Entry.StorageIdentity.ends_with("Surface.material"); });
	PF_CHECK(Tests, MissingMaterial != MissingSource.Entries.end());
	if (MissingMaterial != MissingSource.Entries.end())
	{
		PF_CHECK(Tests, MissingMaterial->Availability == EditorResourceInventoryState::Missing);
		PF_CHECK(Tests, MissingMaterial->State == "missing-source");
		PF_CHECK(Tests, !MissingMaterial->StorageBytes);
	}
}

void RunEditorResourceInventoryServiceTests(TestRunner& Tests)
{
	using namespace PulseForgeEditor;
	InventoryFixture Fixture;
	PF_CHECK(Tests, Fixture.Valid);
	if (!Fixture.Valid) return;

	std::mutex GateMutex;
	std::condition_variable_any Gate;
	bool FirstBuildStarted = false;
	bool FirstBuildCancelled = false;
	size_t BuildCount = 0;
	EditorResourceInventoryService Service([&](const std::filesystem::path& Root, const PulseForge::UUID& ProjectID,
		uint64_t Generation, const EditorResourceInventoryLimits&, std::stop_token CancellationToken)
	{
		EditorResourceInventoryResult Built;
		Built.ProjectIdentity = GetEditorResourceInventoryProjectIdentity(Root, ProjectID);
		Built.Generation = Generation;
		{
			std::unique_lock Lock(GateMutex);
			++BuildCount;
			if (BuildCount == 1)
			{
				FirstBuildStarted = true;
				Gate.notify_all();
				(void)Gate.wait(Lock, CancellationToken, [] { return false; });
				FirstBuildCancelled = CancellationToken.stop_requested();
				Built.Cancelled = FirstBuildCancelled;
				Gate.notify_all();
				return Built;
			}
		}
		EditorResourceInventorySnapshot Snapshot;
		Snapshot.ProjectIdentity = Built.ProjectIdentity;
		Snapshot.Generation = Generation;
		Built.Snapshot = std::move(Snapshot);
		return Built;
	});
	Service.RequestRefresh(Fixture.Root, Fixture.ProjectID, 20);
	{
		std::unique_lock Lock(GateMutex);
		PF_CHECK(Tests, Gate.wait_for(Lock, std::chrono::seconds(10), [&] { return FirstBuildStarted; }));
	}
	Service.RequestRefresh(Fixture.Root, Fixture.ProjectID, 21);
	{
		std::unique_lock Lock(GateMutex);
		PF_CHECK(Tests, Gate.wait_for(Lock, std::chrono::seconds(10), [&] { return FirstBuildCancelled; }));
	}
	std::optional<EditorResourceInventoryResult> Result;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < Deadline && !Result)
	{
		Result = Service.Poll();
		if (!Result) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	PF_CHECK(Tests, Result.has_value());
	if (Result)
	{
		PF_CHECK(Tests, Result->Error.empty());
		PF_CHECK(Tests, !Result->Cancelled);
		PF_CHECK(Tests, Result->Generation == 21);
		PF_CHECK(Tests, Result->Snapshot.has_value());
		if (Result->Snapshot)
			PF_CHECK(Tests, Result->Snapshot->Generation == 21 && Result->Snapshot->Entries.empty());
	}

	bool CancelBuildStarted = false;
	bool CancelBuildFinished = false;
	EditorResourceInventoryService CancelledService([&](const std::filesystem::path& Root, const PulseForge::UUID& ProjectID,
		uint64_t Generation, const EditorResourceInventoryLimits&, std::stop_token CancellationToken)
	{
		EditorResourceInventoryResult Built;
		Built.ProjectIdentity = GetEditorResourceInventoryProjectIdentity(Root, ProjectID);
		Built.Generation = Generation;
		std::unique_lock Lock(GateMutex);
		CancelBuildStarted = true;
		Gate.notify_all();
		(void)Gate.wait(Lock, CancellationToken, [] { return false; });
		CancelBuildFinished = CancellationToken.stop_requested();
		Built.Cancelled = CancelBuildFinished;
		Gate.notify_all();
		return Built;
	});
	CancelledService.RequestRefresh(Fixture.Root, Fixture.ProjectID, 22);
	{
		std::unique_lock Lock(GateMutex);
		PF_CHECK(Tests, Gate.wait_for(Lock, std::chrono::seconds(10), [&] { return CancelBuildStarted; }));
	}
	CancelledService.Cancel();
	{
		std::unique_lock Lock(GateMutex);
		PF_CHECK(Tests, Gate.wait_for(Lock, std::chrono::seconds(10), [&] { return CancelBuildFinished; }));
	}
	PF_CHECK(Tests, !CancelledService.Poll().has_value());

	EditorResourceInventoryService ThrowingService([](const std::filesystem::path&, const PulseForge::UUID&, uint64_t,
		const EditorResourceInventoryLimits&, std::stop_token) -> EditorResourceInventoryResult
	{
		throw std::runtime_error("injected inventory builder failure");
	});
	ThrowingService.RequestRefresh(Fixture.Root, Fixture.ProjectID, 23);
	std::optional<EditorResourceInventoryResult> ThrowingResult;
	const auto ThrowingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < ThrowingDeadline && !ThrowingResult)
	{
		ThrowingResult = ThrowingService.Poll();
		if (!ThrowingResult) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	PF_CHECK(Tests, ThrowingResult.has_value());
	if (ThrowingResult)
	{
		PF_CHECK(Tests, ThrowingResult->ProjectIdentity == GetEditorResourceInventoryProjectIdentity(Fixture.Root, Fixture.ProjectID));
		PF_CHECK(Tests, ThrowingResult->Generation == 23);
		PF_CHECK(Tests, ThrowingResult->Error == "injected inventory builder failure");
		PF_CHECK(Tests, !ThrowingResult->Snapshot);
	}

	EditorResourceInventoryService MismatchedService([](const std::filesystem::path&, const PulseForge::UUID&, uint64_t,
		const EditorResourceInventoryLimits&, std::stop_token)
	{
		EditorResourceInventoryResult Built;
		Built.ProjectIdentity = "stale-project";
		Built.Generation = 999;
		EditorResourceInventorySnapshot Snapshot;
		Snapshot.ProjectIdentity = Built.ProjectIdentity;
		Snapshot.Generation = Built.Generation;
		Built.Snapshot = std::move(Snapshot);
		return Built;
	});
	MismatchedService.RequestRefresh(Fixture.Root, Fixture.ProjectID, 24);
	std::optional<EditorResourceInventoryResult> MismatchedResult;
	const auto MismatchedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < MismatchedDeadline && !MismatchedResult)
	{
		MismatchedResult = MismatchedService.Poll();
		if (!MismatchedResult) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	PF_CHECK(Tests, MismatchedResult.has_value());
	if (MismatchedResult)
	{
		PF_CHECK(Tests, MismatchedResult->ProjectIdentity == GetEditorResourceInventoryProjectIdentity(Fixture.Root, Fixture.ProjectID));
		PF_CHECK(Tests, MismatchedResult->Generation == 24);
		PF_CHECK(Tests, MismatchedResult->Error.find("different project or generation") != std::string::npos);
		PF_CHECK(Tests, !MismatchedResult->Snapshot);
	}

	bool StaleBuildStarted = false;
	bool AllowStaleBuildReturn = false;
	bool CurrentBuildStarted = false;
	bool AllowCurrentBuildReturn = false;
	EditorResourceInventoryService StaleService([&](const std::filesystem::path& Root, const PulseForge::UUID& ProjectID,
		uint64_t Generation, const EditorResourceInventoryLimits&, std::stop_token)
	{
		EditorResourceInventoryResult Built;
		Built.ProjectIdentity = GetEditorResourceInventoryProjectIdentity(Root, ProjectID);
		Built.Generation = Generation;
		std::unique_lock Lock(GateMutex);
		if (Generation == 25)
		{
			StaleBuildStarted = true;
			Gate.notify_all();
			Gate.wait(Lock, [&] { return AllowStaleBuildReturn; }); // Deliberately ignore cancellation and return stale work.
		}
		else
		{
			CurrentBuildStarted = true;
			Gate.notify_all();
			Gate.wait(Lock, [&] { return AllowCurrentBuildReturn; });
		}
		Lock.unlock();
		EditorResourceInventorySnapshot Snapshot;
		Snapshot.ProjectIdentity = Built.ProjectIdentity;
		Snapshot.Generation = Generation;
		Built.Snapshot = std::move(Snapshot);
		return Built;
	});
	StaleService.RequestRefresh(Fixture.Root, Fixture.ProjectID, 25);
	{
		std::unique_lock Lock(GateMutex);
		PF_CHECK(Tests, Gate.wait_for(Lock, std::chrono::seconds(10), [&] { return StaleBuildStarted; }));
	}
	StaleService.RequestRefresh(Fixture.Root, Fixture.ProjectID, 26);
	{
		std::unique_lock Lock(GateMutex);
		AllowStaleBuildReturn = true;
		Gate.notify_all();
		PF_CHECK(Tests, Gate.wait_for(Lock, std::chrono::seconds(10), [&] { return CurrentBuildStarted; }));
	}
	PF_CHECK(Tests, !StaleService.Poll().has_value());
	{
		std::scoped_lock Lock(GateMutex);
		AllowCurrentBuildReturn = true;
		Gate.notify_all();
	}
	std::optional<EditorResourceInventoryResult> CurrentResult;
	const auto CurrentDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < CurrentDeadline && !CurrentResult)
	{
		CurrentResult = StaleService.Poll();
		if (!CurrentResult) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	PF_CHECK(Tests, CurrentResult.has_value());
	if (CurrentResult)
		PF_CHECK(Tests, CurrentResult->Generation == 26 && CurrentResult->Snapshot && CurrentResult->Snapshot->Generation == 26);

	bool DestructionBuildStarted = false;
	bool DestructionBuildExited = false;
	std::mutex DestructionMutex;
	std::condition_variable_any DestructionGate;
	auto DestructionService = std::make_unique<EditorResourceInventoryService>(
		[&](const std::filesystem::path& Root, const PulseForge::UUID& ProjectID, uint64_t Generation,
			const EditorResourceInventoryLimits&, std::stop_token CancellationToken)
		{
			EditorResourceInventoryResult Built;
			Built.ProjectIdentity = GetEditorResourceInventoryProjectIdentity(Root, ProjectID);
			Built.Generation = Generation;
			{
				std::unique_lock Lock(DestructionMutex);
				DestructionBuildStarted = true;
				DestructionGate.notify_all();
				(void)DestructionGate.wait(Lock, CancellationToken, [] { return false; });
				DestructionBuildExited = true;
				Built.Cancelled = CancellationToken.stop_requested();
				DestructionGate.notify_all();
			}
			return Built;
		});
	DestructionService->RequestRefresh(Fixture.Root, Fixture.ProjectID, 27);
	{
		std::unique_lock Lock(DestructionMutex);
		PF_CHECK(Tests, DestructionGate.wait_for(Lock, std::chrono::seconds(10), [&] { return DestructionBuildStarted; }));
	}
	DestructionService.reset();
	{
		std::scoped_lock Lock(DestructionMutex);
		PF_CHECK(Tests, DestructionBuildExited);
	}
}
