#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <glm/vec4.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	struct MaterialAssetDesc
	{
		AssetID BaseColorTexture;
		glm::vec4 BaseColorFactor{ 1.0f };
	};

	enum class MaterialAssetErrorCode : uint8_t
	{
		InvalidDescription,
		InvalidDocument,
		UnsupportedFormat,
		UnsupportedVersion,
		FileReadFailed,
		FileWriteFailed,
		FileReplaceFailed,
		AssetNotFound,
		UnsupportedAssetType,
		PathResolutionFailed,
		AssetOperationFailed,
		CacheFailure,
		CommittedWithCleanupFailure
	};

	struct MaterialAssetError
	{
		MaterialAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
		std::optional<AssetRecord> CommittedAsset;
		std::optional<std::filesystem::path> RecoveryPath;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, MaterialAssetError> ValidateMaterialAssetDescription(
		const MaterialAssetDesc& Description);

	class PULSEFORGE_API MaterialAssetSerializer final
	{
	public:
		[[nodiscard]] static std::expected<std::string, MaterialAssetError> Serialize(const MaterialAssetDesc& Material);
		[[nodiscard]] static std::expected<MaterialAssetDesc, MaterialAssetError> Deserialize(std::string_view Data);
		[[nodiscard]] static std::expected<void, MaterialAssetError> SaveToFile(
			const MaterialAssetDesc& Material,
			const std::filesystem::path& Path);
		[[nodiscard]] static std::expected<MaterialAssetDesc, MaterialAssetError> LoadFromFile(
			const std::filesystem::path& Path);
	};
}
