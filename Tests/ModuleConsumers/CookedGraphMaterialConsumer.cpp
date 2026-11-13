#include "Assets/CookedAssetCatalog.h"
#include "Runtime/CookedContentProvider.h"

#include <array>
#include <filesystem>
#include <fstream>

int main()
{
	using namespace PulseForge;
	std::error_code Error;
	const auto Unique = UUID::Generate();
	if (!Unique) return 1;
	const auto Temporary = std::filesystem::temp_directory_path(Error) / ("PF-CookedGraphMaterial-" + Unique->ToString());
	if (Error || !std::filesystem::create_directories(Temporary, Error) || Error) return 1;
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
	} CleanupRoot{ Temporary };
	const AssetID Asset(1, 2);
	const AssetID StartupScene(5, 6);
	CookedGraphMaterialProduct Product;
	Product.RecipeFingerprint = "0123456789abcdef0123456789abcdef";
	Product.Parameters.push_back({ UUID(1, 3), "Roughness", 0.4f });
	Product.SpirV.resize(20);
	Product.SpirV[0] = std::byte{ 0x03 };
	Product.SpirV[1] = std::byte{ 0x02 };
	Product.SpirV[2] = std::byte{ 0x23 };
	Product.SpirV[3] = std::byte{ 0x07 };
	auto Payload = Detail::EncodeGraphMaterialProduct(Product);
	if (!Payload) return 2;
	CookedAssetEntry Entry;
	Entry.Asset = Asset;
	Entry.Product = CookedAssetProductKind::GraphMaterial;
	Entry.PayloadVersion = Detail::GraphMaterialProductSchemaVersion;
	Entry.PayloadBytes = Payload->size();
	const auto Digest = CookedAssetCatalog::ComputePayloadDigest(*Payload);
	Entry.DigestAlgorithm = Digest.Algorithm;
	Entry.Digest = Digest.Value;
	Entry.RelativePayloadPath = "g/fixture/graph.pfgm";
	const auto PayloadPath = Temporary / std::filesystem::u8path(Entry.RelativePayloadPath);
	std::filesystem::create_directories(PayloadPath.parent_path(), Error);
	if (Error) return 3;
	{
		std::ofstream Output(PayloadPath, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char*>(Payload->data()), static_cast<std::streamsize>(Payload->size()));
		if (!Output) return 4;
	}
#if defined(_WIN32)
	CookedAssetCompatibility Compatibility{ "Windows", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
#elif defined(__APPLE__)
	CookedAssetCompatibility Compatibility{ "macOS", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
#else
	CookedAssetCompatibility Compatibility{ "Linux", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
#endif
	const std::array<std::byte, 2> ScenePayload{ std::byte{'{'}, std::byte{'}'} };
	CookedAssetEntry SceneEntry;
	SceneEntry.Asset = StartupScene;
	SceneEntry.Product = CookedAssetProductKind::Scene;
	SceneEntry.PayloadVersion = 1;
	SceneEntry.PayloadBytes = ScenePayload.size();
	const auto SceneDigest = CookedAssetCatalog::ComputePayloadDigest(ScenePayload);
	SceneEntry.DigestAlgorithm = SceneDigest.Algorithm;
	SceneEntry.Digest = SceneDigest.Value;
	SceneEntry.RelativePayloadPath = "g/fixture/startup.scene";
	const auto ScenePath = Temporary / std::filesystem::u8path(SceneEntry.RelativePayloadPath);
	std::filesystem::create_directories(ScenePath.parent_path(), Error);
	if (Error) return 9;
	{
		std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char*>(ScenePayload.data()), static_cast<std::streamsize>(ScenePayload.size()));
		if (!Output) return 10;
	}
	const auto Manifest = CookedAssetCatalog::SerializeManifest(UUID(3, 4), StartupScene, Compatibility, {}, { Entry, SceneEntry });
	if (!Manifest) return 5;
	{
		std::ofstream Output(Temporary / CookedAssetCatalog::ManifestFileName, std::ios::binary | std::ios::trunc);
		Output.write(Manifest->data(), static_cast<std::streamsize>(Manifest->size()));
		if (!Output) return 6;
	}
	auto Provider = CookedContentProvider::Open(Temporary, Compatibility);
	if (!Provider) return 7;
	auto Loaded = (*Provider)->LoadGraphMaterialProduct(Asset);
	if (!Loaded || Loaded->RecipeFingerprint != Product.RecipeFingerprint || Loaded->SpirV != Product.SpirV ||
		Loaded->Parameters != Product.Parameters) return 8;
	return 0;
}
