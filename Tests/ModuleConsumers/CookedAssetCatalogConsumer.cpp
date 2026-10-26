#include "Runtime/CookedContentProvider.h"
#include "Scene/SceneSerializer.h"
#include "Scene/UUID.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main()
{
	using namespace PulseForge;
	const auto Project = UUID::Generate();
	const auto SceneID = UUID::Generate();
	if (!Project || !SceneID) return 1;
	std::error_code Error;
	const auto Temporary = std::filesystem::canonical(std::filesystem::temp_directory_path(Error), Error);
	if (Error) return 2;
	const auto Root = Temporary / ("PulseForgeCookedCatalogConsumer-" + Project->ToString());
	struct Cleanup final
	{
		std::filesystem::path Root;
		~Cleanup()
		{
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
	} CleanupRoot{ Root };
	std::filesystem::create_directories(Root / "Payloads", Error);
	if (Error) return 3;
	PulseForge::Scene SourceScene;
	const auto SerializedScene = SceneSerializer::Serialize(SourceScene);
	if (!SerializedScene) return 3;
	const std::vector<std::byte> Payload(
		reinterpret_cast<const std::byte*>(SerializedScene->data()),
		reinterpret_cast<const std::byte*>(SerializedScene->data() + SerializedScene->size()));
	{
		std::ofstream Output(Root / "Payloads" / "Startup.scene.bin", std::ios::binary);
		Output.write(reinterpret_cast<const char*>(Payload.data()), static_cast<std::streamsize>(Payload.size()));
		if (!Output) return 4;
	}
	const CookedAssetDigest Digest = CookedAssetCatalog::ComputePayloadDigest(Payload);
	CookedAssetEntry Entry{ *SceneID, CookedAssetProductKind::Scene, {}, "Payloads/Startup.scene.bin", 1,
		Payload.size(), Digest.Algorithm, Digest.Value, {} };
	const AssetID MeshID{ 0x100, 0x200 };
	ImportedGltfMesh Mesh;
	Mesh.Name = "Cooked triangle";
	Mesh.Vertices.resize(3);
	Mesh.Vertices[0].Position[0] = -1.0f;
	Mesh.Vertices[1].Position[0] = 1.0f;
	Mesh.Vertices[2].Position[1] = 1.0f;
	Mesh.Indices = { 0, 1, 2 };
	const auto MeshBytes = Detail::EncodeMeshProduct(Mesh);
	if (!MeshBytes) return 11;
	{
		std::ofstream Output(Root / "Payloads" / "Triangle.mesh.bin", std::ios::binary);
		Output.write(reinterpret_cast<const char*>(MeshBytes->data()), static_cast<std::streamsize>(MeshBytes->size()));
		if (!Output) return 12;
	}
	const CookedAssetDigest MeshDigest = CookedAssetCatalog::ComputePayloadDigest(*MeshBytes);
	CookedAssetEntry MeshEntry{ MeshID, CookedAssetProductKind::MeshPrimitive, "mesh/0/primitive/0",
		"Payloads/Triangle.mesh.bin", 1, MeshBytes->size(), MeshDigest.Algorithm, MeshDigest.Value, {} };
	const AssetID TextureID{ 0x300, 0x400 };
	Detail::PreparedTextureCpuProduct Texture{ 1, 1, Detail::ImageColorSpace::Srgb, { { 1, 1, { 20, 40, 60, 255 } } } };
	const auto TextureBytes = Detail::EncodeTextureProduct(Texture);
	if (!TextureBytes) return 13;
	{
		std::ofstream Output(Root / "Payloads" / "Texture.texture.bin", std::ios::binary);
		Output.write(reinterpret_cast<const char*>(TextureBytes->data()), static_cast<std::streamsize>(TextureBytes->size()));
		if (!Output) return 14;
	}
	const CookedAssetDigest TextureDigest = CookedAssetCatalog::ComputePayloadDigest(*TextureBytes);
	CookedAssetEntry TextureEntry{ TextureID, CookedAssetProductKind::Texture, "srgb", "Payloads/Texture.texture.bin",
		1, TextureBytes->size(), TextureDigest.Algorithm, TextureDigest.Value, {} };
	const AssetID MaterialID{ 0x500, 0x600 };
	CookedMaterialProduct Material;
	Material.BaseColorTexture = TextureID;
	Material.BaseColorFactor = { 0.2f, 0.4f, 0.6f, 1.0f };
	const auto MaterialBytes = Detail::EncodeMaterialProduct(Material);
	if (!MaterialBytes) return 17;
	{
		std::ofstream Output(Root / "Payloads" / "Material.material.bin", std::ios::binary);
		Output.write(reinterpret_cast<const char*>(MaterialBytes->data()), static_cast<std::streamsize>(MaterialBytes->size()));
		if (!Output) return 18;
	}
	const CookedAssetDigest MaterialDigest = CookedAssetCatalog::ComputePayloadDigest(*MaterialBytes);
	CookedAssetEntry MaterialEntry{ MaterialID, CookedAssetProductKind::Material, {}, "Payloads/Material.material.bin",
		Detail::MaterialProductSchemaVersion, MaterialBytes->size(), MaterialDigest.Algorithm, MaterialDigest.Value,
		{ { TextureID, CookedAssetProductKind::Texture, "srgb" } } };
	const CookedAssetCompatibility Compatibility{ "test-target", "test-renderer", "test-runtime-abi" };
	const auto Manifest = CookedAssetCatalog::SerializeManifest(*Project, *SceneID, Compatibility, {},
		{ std::move(Entry), std::move(MeshEntry), std::move(TextureEntry), std::move(MaterialEntry) });
	if (!Manifest) return 5;
	{
		std::ofstream Output(Root / CookedAssetCatalog::ManifestFileName, std::ios::binary);
		Output.write(Manifest->data(), static_cast<std::streamsize>(Manifest->size()));
		if (!Output) return 6;
	}
	const auto Provider = CookedContentProvider::Open(Root, Compatibility);
	if (!Provider || (*Provider)->GetStartupScene() != *SceneID) return 7;
	const CookedAssetEntry* Found = (*Provider)->FindScene(*SceneID);
	if (!Found) return 8;
	const auto Read = (*Provider)->ReadScene(*SceneID);
	if (!Read || *Read != Payload) return 9;
	const auto Loaded = (*Provider)->LoadScene(*SceneID);
	if (!Loaded || !(*Loaded)->GetEntities().empty()) return 10;
	const auto LoadedMesh = (*Provider)->LoadMeshPrimitive(MeshID, "mesh/0/primitive/0");
	if (!LoadedMesh || LoadedMesh->Vertices.size() != 3 || LoadedMesh->Indices != std::vector<uint32_t>{ 0, 1, 2 }) return 15;
	const auto LoadedTexture = (*Provider)->LoadTextureProduct(TextureID, "srgb");
	if (!LoadedTexture || LoadedTexture->Width != 1 || LoadedTexture->MipLevels.size() != 1 ||
		LoadedTexture->MipLevels.front().Pixels != std::vector<uint8_t>{ 20, 40, 60, 255 }) return 16;
	const auto LoadedMaterial = (*Provider)->LoadMaterialProduct(MaterialID);
	if (!LoadedMaterial || LoadedMaterial->BaseColorTexture != TextureID ||
		LoadedMaterial->BaseColorFactor != std::array<float, 4>{ 0.2f, 0.4f, 0.6f, 1.0f }) return 19;
	return 0;
}
