#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Renderer/Mesh.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace PulseForge
{
	struct GltfMeshVertex
	{
		float Position[3]{};
		float Color[3]{ 1.0f, 1.0f, 1.0f };
		float TexCoord[2]{};
	};

	struct ImportedGltfMesh
	{
		std::string Name;
		std::vector<GltfMeshVertex> Vertices;
		std::vector<uint32_t> Indices;

		// The returned spans borrow from this object's vectors and remain valid until it is modified, moved, or destroyed.
		[[nodiscard]] MeshDesc GetMeshDescription() const;
	};

	enum class GltfMeshImportErrorCode : uint8_t
	{
		AssetNotFound,
		InvalidProjectPath,
		UnsupportedFileType,
		FileReadFailed,
		ResourceLimitExceeded,
		InvalidDocument,
		InvalidBuffer,
		InvalidAccessor,
		InvalidMesh,
		UnsupportedFeature
	};

	struct GltfMeshImportError
	{
		GltfMeshImportErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API GltfMeshImporter final
	{
	public:
		// Imports one static triangle primitive by stable asset UUID. This first version accepts glTF 2.0 JSON/GLB,
		// dense float POSITION/TEXCOORD_0/COLOR_0 accessors, and unsigned scalar indices; it does not import nodes/materials.
		[[nodiscard]] static std::expected<ImportedGltfMesh, GltfMeshImportError> ImportStaticPrimitive(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			uint32_t MeshIndex = 0,
			uint32_t PrimitiveIndex = 0);
	};
}
