#include "Assets/GltfMeshImportCoordinator.h"

#include "Assets/AssetMetadata.h"
#include "Assets/AssetPathResolver.h"

#include <system_error>

namespace PulseForge
{
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

		const std::filesystem::path ReceiptPath = AssetImportProvenanceSerializer::GetSidecarPath(*Source);
		if (auto Saved = AssetImportProvenanceSerializer::Save(ReceiptPath, Receipt); !Saved)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				ReceiptPath,
				"CPU import succeeded, but successful provenance was not persisted: " + Saved.error().Message,
				std::nullopt,
				std::move(Saved.error())
			});

		return GltfMeshImportWithReceipt{ std::move(Product->Mesh), Receipt };
	}
}
