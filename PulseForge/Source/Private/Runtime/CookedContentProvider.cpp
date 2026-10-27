#include "Runtime/CookedContentProvider.h"
#include "Assets/PrefabSerializer.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <utility>

namespace PulseForge
{
	namespace
	{
		bool ReadU32(std::span<const std::byte> Bytes, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4) return false;
			Value = 0;
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset++])) << Shift;
			return true;
		}

		CookedAssetCatalogError InvalidProduct(const AssetID& Asset, std::string Message)
		{
			return { .Code = CookedAssetCatalogErrorCode::InvalidPayload, .Asset = Asset, .Message = std::move(Message) };
		}
	}

	CookedContentProvider::CookedContentProvider(std::shared_ptr<const CookedAssetCatalog> Catalog)
		: m_Catalog(std::move(Catalog))
	{
	}

	std::expected<std::shared_ptr<const CookedContentProvider>, CookedAssetCatalogError> CookedContentProvider::Open(
		const std::filesystem::path& ContentRoot,
		const CookedAssetCompatibility& ExpectedCompatibility)
	{
		auto Catalog = CookedAssetCatalog::Open(ContentRoot, ExpectedCompatibility);
		if (!Catalog) return std::unexpected(std::move(Catalog.error()));
		return std::shared_ptr<const CookedContentProvider>(new CookedContentProvider(std::move(*Catalog)));
	}

	const UUID& CookedContentProvider::GetProjectID() const noexcept
	{
		return m_Catalog->GetProjectID();
	}

	const AssetID& CookedContentProvider::GetStartupScene() const noexcept
	{
		return m_Catalog->GetStartupScene();
	}

	const std::vector<AssetID>& CookedContentProvider::GetDynamicRoots() const noexcept
	{
		return m_Catalog->GetDynamicRoots();
	}

	const CookedAssetCompatibility& CookedContentProvider::GetCompatibility() const noexcept
	{
		return m_Catalog->GetCompatibility();
	}

	const CookedAssetEntry* CookedContentProvider::Find(
		const AssetID& Asset, CookedAssetProductKind Product, std::string_view Subresource) const noexcept
	{
		if (Asset.IsNil()) return nullptr;
		return m_Catalog->Find(Asset, Product, Subresource);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::Read(
		const AssetID& Asset, CookedAssetProductKind Product, std::string_view Subresource) const
	{
		const CookedAssetEntry* Entry = Find(Asset, Product, Subresource);
		if (!Entry)
		{
			return std::unexpected(CookedAssetCatalogError{
				.Code = CookedAssetCatalogErrorCode::AssetNotFound,
				.Path = m_Catalog->GetContentRoot(),
				.Asset = Asset.IsNil() ? std::nullopt : std::optional<AssetID>(Asset),
				.Message = "Cooked product is not present in the immutable content catalog" });
		}
		return m_Catalog->ReadPayload(*Entry);
	}

	const CookedAssetEntry* CookedContentProvider::FindScene(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Scene);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadScene(const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::Scene);
	}

	std::expected<std::unique_ptr<Scene>, CookedAssetCatalogError> CookedContentProvider::LoadScene(const AssetID& Asset) const
	{
		auto Bytes = ReadScene(Asset);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		if (Bytes->size() > SceneSerializer::MaximumDocumentBytes)
			return std::unexpected(CookedAssetCatalogError{
				.Code = CookedAssetCatalogErrorCode::InvalidPayload,
				.Path = m_Catalog->GetContentRoot(),
				.Asset = Asset,
				.Message = "Cooked scene exceeds the runtime scene document limit" });

		auto Loaded = std::make_unique<Scene>();
		const std::string Text(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
		if (auto Result = SceneSerializer::Deserialize(Text, *Loaded); !Result)
			return std::unexpected(CookedAssetCatalogError{
				.Code = CookedAssetCatalogErrorCode::InvalidPayload,
				.Path = m_Catalog->GetContentRoot(),
				.Asset = Asset,
				.Message = "Cooked scene failed runtime deserialization: " + Result.error().Message });
		return Loaded;
	}

	std::expected<Entity, CookedAssetCatalogError> CookedContentProvider::InstantiatePrefab(
		const AssetID& Asset, Scene& Destination) const
	{
		auto Bytes = ReadPrefab(Asset);
		if (!Bytes) return std::unexpected(Bytes.error());
		const std::string Text(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
		auto Instantiated = PrefabSerializer::Instantiate(Text, Destination);
		if (!Instantiated)
			return std::unexpected(CookedAssetCatalogError{ .Code = CookedAssetCatalogErrorCode::InvalidPayload,
				.Asset = Asset, .Message = Instantiated.error().Message });
		return *Instantiated;
	}

	const CookedAssetEntry* CookedContentProvider::FindPrefab(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Prefab);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadPrefab(const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::Prefab);
	}

	const CookedAssetEntry* CookedContentProvider::FindMaterial(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Material);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadMaterial(const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::Material);
	}

	std::expected<CookedMaterialProduct, CookedAssetCatalogError> CookedContentProvider::LoadMaterialProduct(
		const AssetID& Asset) const
	{
		auto Bytes = ReadMaterial(Asset);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		auto Product = Detail::DecodeMaterialProduct(*Bytes);
		if (!Product) return std::unexpected(InvalidProduct(Asset, "Cooked material product is invalid: " + Product.error()));
		return std::move(*Product);
	}

	const CookedAssetEntry* CookedContentProvider::FindScript(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Script);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadScript(const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::Script);
	}

	const CookedAssetEntry* CookedContentProvider::FindAudio(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Audio);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadAudio(const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::Audio);
	}

	const CookedAssetEntry* CookedContentProvider::FindMeshPrimitive(
		const AssetID& Asset, std::string_view Subresource) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::MeshPrimitive, Subresource);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadMeshPrimitive(
		const AssetID& Asset, std::string_view Subresource) const
	{
		return Read(Asset, CookedAssetProductKind::MeshPrimitive, Subresource);
	}

	std::expected<ImportedGltfMesh, CookedAssetCatalogError> CookedContentProvider::LoadMeshPrimitive(
		const AssetID& Asset, std::string_view Subresource) const
	{
		auto Bytes = ReadMeshPrimitive(Asset, Subresource);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		auto Product = Detail::DecodeMeshProduct(*Bytes);
		if (!Product) return std::unexpected(InvalidProduct(Asset, "Cooked mesh product is invalid: " + Product.error()));
		return std::move(*Product);
	}

	const CookedAssetEntry* CookedContentProvider::FindTexture(const AssetID& Asset, std::string_view Subresource) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Texture, Subresource);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadTexture(
		const AssetID& Asset, std::string_view Subresource) const
	{
		return Read(Asset, CookedAssetProductKind::Texture, Subresource);
	}

	std::expected<Detail::PreparedTextureCpuProduct, CookedAssetCatalogError> CookedContentProvider::LoadTextureProduct(
		const AssetID& Asset, std::string_view Subresource) const
	{
		auto Bytes = ReadTexture(Asset, Subresource);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		auto Product = Detail::DecodeTextureProduct(*Bytes);
		if (!Product) return std::unexpected(InvalidProduct(Asset, "Cooked texture product is invalid: " + Product.error()));
		return std::move(*Product);
	}

	const CookedAssetEntry* CookedContentProvider::FindEnvironment(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::Environment);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadEnvironment(
		const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::Environment);
	}

	std::expected<Detail::DecodedEnvironmentCpuProduct, CookedAssetCatalogError> CookedContentProvider::LoadEnvironmentProduct(
		const AssetID& Asset) const
	{
		auto Bytes = ReadEnvironment(Asset);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		static constexpr std::array<uint8_t, 8> Magic{ 'P', 'F', 'E', 'N', 'V', 0, 0, 1 };
		if (Bytes->size() < 16 || !std::equal(Magic.begin(), Magic.end(), Bytes->begin(),
			[](uint8_t Expected, std::byte Actual) { return Expected == std::to_integer<uint8_t>(Actual); }))
			return std::unexpected(InvalidProduct(Asset, "Cooked environment envelope has invalid magic or is truncated"));
		size_t Offset = Magic.size();
		uint32_t EnvironmentLength = 0, BrdfLength = 0;
		if (!ReadU32(*Bytes, Offset, EnvironmentLength) || !ReadU32(*Bytes, Offset, BrdfLength) ||
			static_cast<uint64_t>(EnvironmentLength) + BrdfLength != Bytes->size() - Offset)
			return std::unexpected(InvalidProduct(Asset, "Cooked environment envelope lengths are inconsistent"));
		const auto EnvironmentBytes = std::span<const std::byte>(*Bytes).subspan(Offset, EnvironmentLength);
		Offset += EnvironmentLength;
		const auto BrdfBytes = std::span<const std::byte>(*Bytes).subspan(Offset, BrdfLength);
		auto Product = Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(EnvironmentBytes);
		if (!Product)
			return std::unexpected(InvalidProduct(Asset, "Cooked environment lighting product is invalid: " + Product.error()));
		auto Brdf = Detail::EnvironmentLightingArtifactCodec::DecodeBrdfLut(
			BrdfBytes, Product->Processing.BrdfLutSize, Product->Processing.BrdfSamples);
		if (!Brdf)
			return std::unexpected(InvalidProduct(Asset, "Cooked environment BRDF product is invalid: " + Brdf.error()));
		Product->Data.BrdfIntegrationLut = std::move(*Brdf);
		return std::move(*Product);
	}

	const CookedAssetEntry* CookedContentProvider::FindGameplayInput(const AssetID& Asset) const noexcept
	{
		return Find(Asset, CookedAssetProductKind::GameplayInput);
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedContentProvider::ReadGameplayInput(
		const AssetID& Asset) const
	{
		return Read(Asset, CookedAssetProductKind::GameplayInput);
	}

	std::expected<GameplayActionMap, CookedAssetCatalogError> CookedContentProvider::LoadGameplayInput(
		const AssetID& Asset) const
	{
		auto Bytes = ReadGameplayInput(Asset);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		const std::string Text(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
		auto Parsed = ParseGameplayActionMap(Text);
		if (!Parsed)
			return std::unexpected(CookedAssetCatalogError{
				.Code = CookedAssetCatalogErrorCode::InvalidPayload,
				.Path = m_Catalog->GetContentRoot(),
				.Asset = Asset,
				.Message = "Cooked gameplay input map failed runtime validation: " + Parsed.error().Message });
		return std::move(*Parsed);
	}
} // namespace PulseForge
