#include "Assets/GltfMeshImportCoordinator.h"

#include "Assets/AssetMetadata.h"
#include "Assets/AssetPathResolver.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		std::shared_ptr<std::mutex> GetReceiptPathMutex(const std::filesystem::path& Path)
		{
			static std::mutex RegistryMutex;
			static std::map<std::filesystem::path, std::weak_ptr<std::mutex>> PathMutexes;
			std::lock_guard RegistryLock(RegistryMutex);
			for (auto It = PathMutexes.begin(); It != PathMutexes.end();)
				if (It->second.expired()) It = PathMutexes.erase(It);
				else ++It;

			auto& WeakMutex = PathMutexes[Path];
			if (auto Existing = WeakMutex.lock()) return Existing;
			auto Created = std::make_shared<std::mutex>();
			WeakMutex = Created;
			return Created;
		}
	}

	std::expected<GltfMeshImportWithReceipt, GltfMeshImportCoordinatorError>
		GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			uint32_t MeshIndex,
			uint32_t PrimitiveIndex)
	{
		auto Product = GltfMeshImporter::ProcessStaticPrimitive(Asset, ProjectRoot, Registry, MeshIndex, PrimitiveIndex);
		if (!Product)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ImportFailed,
				Product.error().Path,
				"CPU mesh import failed: " + Product.error().Message,
				std::move(Product.error()),
				std::nullopt
			});

		const AssetImportSuccessReceipt Receipt{
			Product->Recipe,
			Product->ResultFingerprint,
			Product->Recipe.SubresourceKey
		};
		const auto Record = Registry.Find(Asset);
		if (!Record)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ImportFailed, ProjectRoot,
				"Asset disappeared from the registry after CPU processing", std::nullopt, std::nullopt
			});
		std::error_code AbsoluteError;
		const std::filesystem::path AbsoluteRoot = std::filesystem::absolute(ProjectRoot, AbsoluteError).lexically_normal();
		if (AbsoluteError)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				ProjectRoot,
				"CPU import succeeded, but the project root could not be resolved for receipt persistence: " + AbsoluteError.message(),
				std::nullopt,
				std::nullopt
			});
		auto Source = AssetPathResolver::ResolveManagedSourcePath(AbsoluteRoot, Record->ProjectRelativePath);
		if (!Source)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				Source.error().Path,
				"CPU import succeeded, but its receipt source path could not be resolved: " + Source.error().Message,
				std::nullopt,
				std::nullopt
			});

		const std::filesystem::path ReceiptPath = AssetImportProvenanceSerializer::GetSidecarPath(*Source).lexically_normal();
		std::error_code ReceiptPathError;
		const std::filesystem::path AbsoluteReceiptPath = std::filesystem::absolute(ReceiptPath, ReceiptPathError).lexically_normal();
		if (ReceiptPathError)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				ReceiptPath,
				"CPU import succeeded, but its receipt path could not be normalized: " + ReceiptPathError.message(),
				std::nullopt,
				std::nullopt
			});

		// CPU work remains concurrent; only read-modify-write of this source's sidecar is serialized.
		const std::shared_ptr<std::mutex> PathMutex = GetReceiptPathMutex(AbsoluteReceiptPath);
		std::lock_guard ReceiptLock(*PathMutex);
		auto Receipts = AssetImportProvenanceSerializer::LoadAll(AbsoluteReceiptPath);
		if (!Receipts)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				AbsoluteReceiptPath,
				"CPU import succeeded, but existing provenance could not be read safely: " + Receipts.error().Message,
				std::nullopt,
				std::move(Receipts.error())
			});
		if (std::any_of(Receipts->begin(), Receipts->end(), [&Asset](const AssetImportSuccessReceipt& Existing)
			{ return Existing.Recipe.SourceAsset != Asset; }))
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				AbsoluteReceiptPath,
				"CPU import succeeded, but the existing receipt belongs to a different asset UUID",
				std::nullopt,
				std::nullopt
			});
		const auto ExistingOutput = std::lower_bound(Receipts->begin(), Receipts->end(), Receipt.Recipe.SubresourceKey,
			[](const AssetImportSuccessReceipt& Existing, std::string_view Key)
			{ return Existing.Recipe.SubresourceKey < Key; });
		if (ExistingOutput != Receipts->end() && ExistingOutput->Recipe.SubresourceKey == Receipt.Recipe.SubresourceKey)
			*ExistingOutput = Receipt;
		else Receipts->insert(ExistingOutput, Receipt);
		if (auto Saved = AssetImportProvenanceSerializer::SaveAll(AbsoluteReceiptPath, *Receipts); !Saved)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				AbsoluteReceiptPath,
				"CPU import succeeded, but successful provenance was not persisted: " + Saved.error().Message,
				std::nullopt,
				std::move(Saved.error())
			});

		return GltfMeshImportWithReceipt{ std::move(Product->Mesh), Receipt };
	}
}
