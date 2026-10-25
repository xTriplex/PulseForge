#pragma once

#include "Assets/AssetImportProvenance.h"
#include "Assets/GltfMeshImporter.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace PulseForge
{
	enum class GltfMeshImportCoordinatorErrorCode : uint8_t
	{
		ImportFailed,
		ReceiptPersistenceFailed
	};

	struct GltfMeshImportCoordinatorError
	{
		GltfMeshImportCoordinatorErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
		std::optional<GltfMeshImportError> ImportError;
		std::optional<AssetImportProvenanceError> ReceiptError;
	};

	struct GltfMeshImportWithReceipt
	{
		ImportedGltfMesh Mesh;
		AssetImportSuccessReceipt Receipt;
	};

	// Explicit CPU processing operation. Ordinary importer and renderer-cache calls do not write provenance.
	class PULSEFORGE_API GltfMeshImportCoordinator final
	{
	public:
		[[nodiscard]] static std::expected<GltfMeshImportWithReceipt, GltfMeshImportCoordinatorError>
			ProcessAndRecordStaticPrimitive(
				const AssetID& Asset,
				const std::filesystem::path& ProjectRoot,
				const AssetRegistry& Registry,
				uint32_t MeshIndex = 0,
				uint32_t PrimitiveIndex = 0);
	};
}
