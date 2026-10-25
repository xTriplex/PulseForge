#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	struct AssetContentFingerprint
	{
		std::string Algorithm;
		std::string DigestHex;
		friend bool operator==(const AssetContentFingerprint&, const AssetContentFingerprint&) = default;
	};

	struct AssetImportInputFingerprint
	{
		std::string ProjectRelativePath;
		AssetContentFingerprint Content;
		friend bool operator==(const AssetImportInputFingerprint&, const AssetImportInputFingerprint&) = default;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> NormalizeAssetImportRecipeInputs(
		std::vector<AssetImportInputFingerprint>& Inputs);

	struct AssetImportRecipe
	{
		AssetID SourceAsset;
		std::string SubresourceKey;
		std::string ImporterID;
		std::string ImporterVersion;
		uint32_t OutputSchemaVersion = 0;
		std::string EffectiveSettings;
		std::string TargetID;
		std::vector<AssetImportInputFingerprint> Inputs;
		AssetContentFingerprint InputFingerprint;
	};

	struct AssetImportSuccessReceipt
	{
		AssetImportRecipe Recipe;
		AssetContentFingerprint ResultFingerprint;
		std::string ResultIdentity;
	};

	class PULSEFORGE_API AssetFingerprintBuilder final
	{
	public:
		explicit AssetFingerprintBuilder(std::string_view Domain);
		~AssetFingerprintBuilder();
		AssetFingerprintBuilder(const AssetFingerprintBuilder&) = delete;
		AssetFingerprintBuilder& operator=(const AssetFingerprintBuilder&) = delete;
		AssetFingerprintBuilder(AssetFingerprintBuilder&&) noexcept;
		AssetFingerprintBuilder& operator=(AssetFingerprintBuilder&&) noexcept;

		void UpdateBytes(std::string_view Label, std::span<const std::byte> Bytes);
		void UpdateText(std::string_view Label, std::string_view Text);
		[[nodiscard]] AssetContentFingerprint Finish();

	private:
		struct State;
		std::unique_ptr<State> m_State;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<AssetContentFingerprint, std::string> ComputeAssetImportInputFingerprint(
		const AssetImportRecipe& Recipe);
	[[nodiscard]] PULSEFORGE_API AssetContentFingerprint ComputeAssetContentFingerprint(std::span<const std::byte> Bytes);

	enum class AssetImportProvenanceErrorCode : uint8_t
	{
		InvalidPath,
		FileOpenFailed,
		FileReadFailed,
		InvalidDocument,
		UnsupportedVersion,
		WriteFailed
	};

	struct AssetImportProvenanceError
	{
		AssetImportProvenanceErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API AssetImportProvenanceSerializer final
	{
	public:
		static constexpr uint32_t LegacySingleRecordVersion = 1;
		static constexpr uint32_t CurrentVersion = 2;

		[[nodiscard]] static std::filesystem::path GetSidecarPath(const std::filesystem::path& SourceAssetPath);
		[[nodiscard]] static std::expected<std::vector<AssetImportSuccessReceipt>, AssetImportProvenanceError> DeserializeAll(
			std::string_view Data,
			const std::filesystem::path& Path = {});
		[[nodiscard]] static std::expected<AssetImportSuccessReceipt, AssetImportProvenanceError> Deserialize(
			std::string_view Data,
			const std::filesystem::path& Path = {});
		// An absent optional receipt sidecar is represented by an empty record list.
		[[nodiscard]] static std::expected<std::vector<AssetImportSuccessReceipt>, AssetImportProvenanceError> LoadAll(
			const std::filesystem::path& Path);
		[[nodiscard]] static std::expected<AssetImportSuccessReceipt, AssetImportProvenanceError> Load(
			const std::filesystem::path& Path);

	private:
		friend class GltfMeshImportCoordinator;
		friend struct AssetImportProvenanceTestAccess;
		using ReplacementFunction = std::function<std::expected<void, std::string>(
			const std::filesystem::path&, const std::filesystem::path&)>;
		[[nodiscard]] static std::expected<void, AssetImportProvenanceError> SaveAll(
			const std::filesystem::path& Path,
			std::span<const AssetImportSuccessReceipt> Receipts);
		[[nodiscard]] static std::expected<void, AssetImportProvenanceError> SaveAllWithReplacement(
			const std::filesystem::path& Path,
			std::span<const AssetImportSuccessReceipt> Receipts,
			const ReplacementFunction& Replace);
	};
}
