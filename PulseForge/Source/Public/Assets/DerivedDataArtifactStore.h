#pragma once

#include "Assets/AssetImportProvenance.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	struct DerivedDataArtifactKey
	{
		std::string ProductKind;
		AssetContentFingerprint RecipeDigest;
		uint32_t ProductSchemaVersion = 0;
	};

	struct DerivedDataArtifactStoreOptions
	{
		std::filesystem::path Root;
		bool Enabled = true;
		uint64_t MaxTotalBytes = 2ull * 1024ull * 1024ull * 1024ull;
		uint64_t MaxArtifactBytes = 256ull * 1024ull * 1024ull;
		size_t MaxEntries = 8192;
	};

	enum class DerivedDataArtifactReadStatus : uint8_t
	{
		Hit,
		Miss,
		// A well-formed envelope names another recipe/product or an unsupported envelope version.
		Incompatible,
		// The owned file is malformed, structurally invalid or fails its payload digest.
		Corrupt,
		// The cache is disabled, inaccessible or could not allocate/read the bounded result.
		Unavailable
	};

	struct DerivedDataArtifactReadResult
	{
		DerivedDataArtifactReadStatus Status = DerivedDataArtifactReadStatus::Miss;
		std::vector<std::byte> Payload;
		std::string Message;
		// Checksum from the validated envelope, used to conditionally replace a product-codec rejection.
		AssetContentFingerprint EntryChecksum;
	};

	enum class DerivedDataArtifactWriteStatus : uint8_t
	{
		Written,
		ExistingValid,
		Rejected,
		Unavailable
	};

	struct DerivedDataArtifactWriteResult
	{
		DerivedDataArtifactWriteStatus Status = DerivedDataArtifactWriteStatus::Unavailable;
		std::string Message;
	};

	// CPU-only opaque payload storage. Product codecs remain responsible for validating decoded payload structure.
	// Callers provide a dedicated cache root outside authored Assets.
	// Bounds apply to retained owned artifacts; one in-progress temporary file is additionally bounded by
	// MaxArtifactBytes plus the small fixed envelope.
	// Operations are serialized within this instance. No cross-process coordination is promised.
	class PULSEFORGE_API DerivedDataArtifactStore final
	{
	public:
		explicit DerivedDataArtifactStore(DerivedDataArtifactStoreOptions Options);
		DerivedDataArtifactStore(const DerivedDataArtifactStore&) = delete;
		DerivedDataArtifactStore& operator=(const DerivedDataArtifactStore&) = delete;

		[[nodiscard]] DerivedDataArtifactReadResult Read(const DerivedDataArtifactKey& Key) const;
		[[nodiscard]] DerivedDataArtifactWriteResult Write(
			const DerivedDataArtifactKey& Key,
			std::span<const std::byte> Payload);
		// Replaces an envelope only if it is still the exact validated entry observed by Read().
		// This coordinates requests using this store instance; cross-process writers are not coordinated.
		[[nodiscard]] DerivedDataArtifactWriteResult ReplaceRejectedProduct(
			const DerivedDataArtifactKey& Key,
			const AssetContentFingerprint& ObservedEntryChecksum,
			std::span<const std::byte> ReplacementPayload);

		[[nodiscard]] const DerivedDataArtifactStoreOptions& GetOptions() const noexcept { return m_Options; }

	private:
		friend struct DerivedDataArtifactStoreTestAccess;
		using ReplaceFunction = std::function<std::expected<void, std::string>(
			const std::filesystem::path&, const std::filesystem::path&)>;

		[[nodiscard]] DerivedDataArtifactReadResult ReadUnlocked(const DerivedDataArtifactKey& Key) const;
		[[nodiscard]] DerivedDataArtifactWriteResult WriteWithReplacement(
			const DerivedDataArtifactKey& Key,
			std::span<const std::byte> Payload,
			const ReplaceFunction& Replace,
			const AssetContentFingerprint* ExpectedExistingChecksum = nullptr);

		DerivedDataArtifactStoreOptions m_Options;
		mutable std::mutex m_Mutex;
	};
} // namespace PulseForge
