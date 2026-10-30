#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	enum class PrefabErrorCode : uint8_t
	{
		InvalidRootEntity,
		InvalidDocument,
		UnsupportedFormat,
		UnsupportedVersion,
		SceneSerializationFailed,
		SceneOperationFailed,
		InstantiationFailed,
		FileOpenFailed,
		FileReadFailed,
		FileWriteFailed,
		FileReplaceFailed
	};

	struct PrefabError
	{
		PrefabErrorCode Code;
		std::string Message;
	};

	// An owned editable prefab source scene. Source entity UUIDs are stable within the asset.
	class PULSEFORGE_API PrefabSourceDocument final
	{
	public:
		PrefabSourceDocument(PrefabSourceDocument&&) noexcept = default;
		PrefabSourceDocument& operator=(PrefabSourceDocument&&) noexcept = default;
		PrefabSourceDocument(const PrefabSourceDocument&) = delete;
		PrefabSourceDocument& operator=(const PrefabSourceDocument&) = delete;

		[[nodiscard]] Scene& GetScene() noexcept { return *m_Scene; }
		[[nodiscard]] const Scene& GetScene() const noexcept { return *m_Scene; }
		[[nodiscard]] UUID GetRootKey() const noexcept { return m_RootKey; }
		[[nodiscard]] AssetID GetSourceAsset() const noexcept { return m_SourceAsset; }
		[[nodiscard]] std::string_view GetFingerprint() const noexcept { return m_Fingerprint; }

	private:
		friend class PrefabSerializer;
		friend class PrefabAssetService;
		friend class PrefabInstanceReconciler;

		PrefabSourceDocument(
			std::unique_ptr<Scene> SourceScene,
			UUID RootKey,
			AssetID SourceAsset,
			std::string Fingerprint,
			std::string OriginalData);

		std::unique_ptr<Scene> m_Scene;
		UUID m_RootKey;
		AssetID m_SourceAsset;
		std::string m_Fingerprint;
		std::string m_OriginalData;
	};

	struct PrefabInstantiationResult
	{
		Entity Root;
		std::vector<PrefabEntityMapping> SourceEntities;
	};

	class PULSEFORGE_API PrefabSerializer final
	{
	public:
		// Captures the root and its descendants. A parent outside the captured subtree is omitted.
		[[nodiscard]] static std::expected<std::string, PrefabError> Serialize(const Scene& Source, const Entity& Root);
		[[nodiscard]] static std::expected<std::string, PrefabError> Serialize(const PrefabSourceDocument& Source);
		[[nodiscard]] static std::expected<PrefabSourceDocument, PrefabError> Parse(
			std::string_view Data,
			AssetID SourceAsset = {},
			std::string Fingerprint = {});
		// Creates a new subtree with fresh entity UUIDs; asset UUID references are retained.
		[[nodiscard]] static std::expected<Entity, PrefabError> Instantiate(std::string_view Data, Scene& Destination);
		[[nodiscard]] static std::expected<PrefabInstantiationResult, PrefabError> Instantiate(
			const PrefabSourceDocument& Source,
			Scene& Destination,
			bool ClearPrimaryRoles = true);
		[[nodiscard]] static std::expected<void, PrefabError> SaveToFile(
			const Scene& Source,
			const Entity& Root,
			const std::filesystem::path& Path);
		[[nodiscard]] static std::expected<Entity, PrefabError> InstantiateFromFile(
			const std::filesystem::path& Path,
			Scene& Destination);
	};
}
