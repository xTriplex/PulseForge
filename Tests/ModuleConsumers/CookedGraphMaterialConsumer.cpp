#include "Assets/CookedAssetCatalog.h"
#include "Runtime/CookedContentProvider.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

int main(int ArgumentCount, char** Arguments)
{
	using namespace PulseForge;
	if (ArgumentCount == 2)
	{
	#if defined(_WIN32)
		CookedAssetCompatibility Compatibility{ "Windows", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
	#elif defined(__APPLE__)
		CookedAssetCompatibility Compatibility{ "macOS", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
	#else
		CookedAssetCompatibility Compatibility{ "Linux", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
	#endif
		auto Provider = CookedContentProvider::Open(std::filesystem::u8path(Arguments[1]), Compatibility);
		if (!Provider) return 20;
		const std::filesystem::path ContentRoot = std::filesystem::u8path(Arguments[1]);
		std::ifstream IdentityFile(ContentRoot / "graph-material-asset-id.txt", std::ios::binary);
		const std::string IdentityText{ std::istreambuf_iterator<char>(IdentityFile), std::istreambuf_iterator<char>() };
		const auto Asset = UUID::Parse(IdentityText);
		if (!IdentityFile.is_open() || !Asset || Asset->IsNil()) return 21;
		auto Product = (*Provider)->LoadGraphMaterialProduct(*Asset);
		if (!Product || Product->RecipeFingerprint.size() != 32 || Product->SpirV.size() < 20 ||
			Product->Parameters.size() != 1 || Product->Parameters.front().Name != "Accent") return 22;
		const auto MaterialState = (*Provider)->LoadMaterialProduct(*Asset);
		if (!MaterialState || MaterialState->BaseColorFactor != std::array<float, 4>{ 1.0f, 1.0f, 1.0f, 1.0f }) return 25;
		if (Product->SpirV[0] != std::byte{ 0x03 } || Product->SpirV[1] != std::byte{ 0x02 } ||
			Product->SpirV[2] != std::byte{ 0x23 } || Product->SpirV[3] != std::byte{ 0x07 }) return 23;
		return 0;
	}
	if (ArgumentCount != 1) return 24;
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
	Product.Material.BaseColorFactor = { 0.2f, 0.4f, 0.6f, 0.8f };
	Product.Material.DoubleSided = true;
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
	auto MaterialState = (*Provider)->LoadMaterialProduct(Asset);
	if (!MaterialState || MaterialState->BaseColorFactor != Product.Material.BaseColorFactor || !MaterialState->DoubleSided) return 26;
	return 0;
}
