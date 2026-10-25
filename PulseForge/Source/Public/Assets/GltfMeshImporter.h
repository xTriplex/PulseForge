#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/AssetImportProvenance.h"
#include "Core/Core.h"
#include "Renderer/MeshData.h"

#include <cstdint>
#include <array>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	struct GltfMeshVertex
	{
		float Position[3]{};
		float Color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		float TexCoord[2]{};
		float Normal[3]{ 0.0f, 0.0f, 1.0f };
		std::array<float, 4> Tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
	};

	struct ImportedGltfMesh
	{
		std::string Name;
		std::vector<GltfMeshVertex> Vertices;
		std::vector<uint32_t> Indices;

		// The returned spans borrow from this object's vectors and remain valid until it is modified, moved, or destroyed.
		[[nodiscard]] MeshDesc GetMeshDescription() const;
	};

	enum class MissingNormalsPolicy : uint8_t
	{
		GenerateSmooth,
		Reject
	};

	struct GltfMeshImportSettings
	{
		static constexpr uint32_t CurrentVersion = 1;
		MissingNormalsPolicy NormalsPolicy = MissingNormalsPolicy::GenerateSmooth;

		[[nodiscard]] static std::expected<GltfMeshImportSettings, std::string> Parse(std::string_view ImportSettingsObjectJson);
		[[nodiscard]] std::expected<std::string, std::string> Canonicalize() const;
	};

	struct GltfMeshImportProduct
	{
		ImportedGltfMesh Mesh;
		AssetImportRecipe Recipe;
		AssetContentFingerprint ResultFingerprint;
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
		// Imports one static triangle primitive by stable asset UUID. Missing normals are generated smoothly across shared
		// vertices; degenerate triangles add no contribution, and vertices without a valid contribution use +Z.
		// This first version accepts glTF 2.0 JSON/GLB dense-float vertex attributes and unsigned scalar indices.
		[[nodiscard]] static std::expected<ImportedGltfMesh, GltfMeshImportError> ImportStaticPrimitive(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			uint32_t MeshIndex = 0,
			uint32_t PrimitiveIndex = 0);

		// Performs a CPU import and returns the exact input recipe and output digest used for truthful provenance.
		[[nodiscard]] static std::expected<GltfMeshImportProduct, GltfMeshImportError> ProcessStaticPrimitive(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			uint32_t MeshIndex = 0,
			uint32_t PrimitiveIndex = 0);
	};
}
