#include "TestAssetSupport.h"

#include "Assets/AssetImportProvenance.h"
#include "Assets/GltfMeshImportCoordinator.h"
#include "Assets/GltfMeshImporter.h"

#include <bit>
#include <cmath>

namespace
{
	bool WriteTextFile(const std::filesystem::path& Path, std::string_view Text)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) return false;
		Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		Output.close();
		return static_cast<bool>(Output);
	}

	std::optional<std::string> ReadTextFile(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::nullopt;
		std::string Text{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
		if (Input.bad()) return std::nullopt;
		return Text;
	}

	std::string Base64Encode(std::span<const std::byte> Bytes)
	{
		constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Result;
		for (size_t Index = 0; Index < Bytes.size(); Index += 3)
		{
			const uint32_t A = std::to_integer<uint8_t>(Bytes[Index]);
			const uint32_t B = Index + 1 < Bytes.size() ? std::to_integer<uint8_t>(Bytes[Index + 1]) : 0;
			const uint32_t C = Index + 2 < Bytes.size() ? std::to_integer<uint8_t>(Bytes[Index + 2]) : 0;
			const uint32_t Value = (A << 16) | (B << 8) | C;
			Result.push_back(Alphabet[(Value >> 18) & 63]);
			Result.push_back(Alphabet[(Value >> 12) & 63]);
			Result.push_back(Index + 1 < Bytes.size() ? Alphabet[(Value >> 6) & 63] : '=');
			Result.push_back(Index + 2 < Bytes.size() ? Alphabet[Value & 63] : '=');
		}
		return Result;
	}

	std::vector<std::byte> MakeTriangleBytes()
	{
		const std::array<float, 9> Positions{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		std::vector<std::byte> Bytes;
		for (float Position : Positions)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Position);
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Bytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		}
		return Bytes;
	}

	std::string TriangleDocument()
	{
		const std::vector<std::byte> Bytes = MakeTriangleBytes();
		return "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
			Base64Encode(Bytes) + "\",\"byteLength\":36}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}]," +
			"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}]," +
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";
	}

	PulseForge::AssetImportRecipe MakeRecipe(const PulseForge::AssetID& SourceAsset, std::string EffectiveSettings)
	{
		using namespace PulseForge;
		AssetFingerprintBuilder Bytes("test-input");
		Bytes.UpdateText("bytes", "same input");
		AssetImportRecipe Recipe;
		Recipe.SourceAsset = SourceAsset;
		Recipe.SubresourceKey = "mesh/0/primitive/0";
		Recipe.ImporterID = "PulseForge.StaticGltfMeshImporter";
		Recipe.ImporterVersion = "1";
		Recipe.OutputSchemaVersion = 1;
		Recipe.EffectiveSettings = std::move(EffectiveSettings);
		Recipe.TargetID = "cpu-generic-v1";
		Recipe.Inputs.push_back({ "Assets/mesh.gltf", Bytes.Finish() });
		Recipe.InputFingerprint = ComputeAssetImportInputFingerprint(Recipe).value();
		return Recipe;
	}
}

void RunAssetImportProvenanceTests(TestRunner& Tests)
{
	using namespace PulseForge;
	std::error_code Error;
	const auto TemporaryRoot = std::filesystem::temp_directory_path(Error);
	PF_CHECK(Tests, !Error);
	if (Error) return;
	const auto Identifier = UUID::Generate();
	PF_CHECK(Tests, Identifier.has_value());
	if (!Identifier) return;
	const std::filesystem::path ProjectRoot = TemporaryRoot / ("PulseForgeImportProvenance-" + Identifier->ToString());
	struct Cleanup
	{
		std::filesystem::path Path;
		~Cleanup() { std::error_code RemoveError; std::filesystem::remove_all(Path, RemoveError); }
	} CleanupDirectory{ ProjectRoot };
	const std::filesystem::path ModelDirectory = ProjectRoot / "Assets" / "Models";
	std::filesystem::create_directories(ModelDirectory, Error);
	PF_CHECK(Tests, !Error);
	if (Error) return;

	const std::string LegacyUUID = Identifier->ToString();
	const std::filesystem::path LegacyPath = ProjectRoot / "legacy.gltf.meta";
	const std::string LegacySidecar = "{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" + LegacyUUID +
		"\",\"importSettings\":{}}";
	PF_CHECK(Tests, WriteTextFile(LegacyPath, LegacySidecar));
	const auto LegacyMetadata = AssetMetadataSerializer::Deserialize(LegacySidecar, LegacyPath);
	PF_CHECK(Tests, LegacyMetadata && LegacyMetadata->ID == *Identifier && LegacyMetadata->ImportSettingsJson == "{}");
	PF_CHECK(Tests, !AssetMetadataSerializer::Deserialize(
		"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" + LegacyUUID +
		"\",\"importSettings\":{},\"importSettings\":{}}", LegacyPath));
	PF_CHECK(Tests, !AssetImportProvenanceSerializer::Deserialize(
		"{\"format\":\"PulseForgeImportResult\",\"format\":\"PulseForgeImportResult\"}"));
	PF_CHECK(Tests, ReadTextFile(LegacyPath) == LegacySidecar);
	const auto DefaultSettings = GltfMeshImportSettings::Parse(LegacyMetadata ? LegacyMetadata->ImportSettingsJson : "{}");
	PF_CHECK(Tests, DefaultSettings && DefaultSettings->NormalsPolicy == MissingNormalsPolicy::GenerateSmooth);
	const auto DefaultCanonical = DefaultSettings ? DefaultSettings->Canonicalize() : std::expected<std::string, std::string>{};
	PF_CHECK(Tests, DefaultCanonical && *DefaultCanonical == "{\"version\":1,\"missingNormalsPolicy\":\"GenerateSmooth\"}");
	const auto ExplicitDefault = GltfMeshImportSettings::Parse(
		"{\"gltfStaticMesh\":{\"version\":1,\"missingNormalsPolicy\":\"GenerateSmooth\"}}");
	const auto ExplicitDefaultCanonical = ExplicitDefault ? ExplicitDefault->Canonicalize()
		: std::expected<std::string, std::string>{};
	PF_CHECK(Tests, DefaultCanonical && ExplicitDefaultCanonical && *DefaultCanonical == *ExplicitDefaultCanonical);
	PF_CHECK(Tests, !GltfMeshImportSettings::Parse("{\"gltfStaticMesh\":{\"version\":2,\"missingNormalsPolicy\":\"GenerateSmooth\"}}"));
	PF_CHECK(Tests, !GltfMeshImportSettings::Parse("{\"gltfStaticMesh\":{\"version\":1,\"missingNormalsPolicy\":\"future\"}}"));

	PF_CHECK(Tests, !GltfMeshImportSettings::Parse("{\"ignoredOption\":true}"));
	PF_CHECK(Tests, !GltfMeshImportSettings::Parse("{\"gltfStaticMesh\":{},\"gltfStaticMesh\":{}}"));
	PF_CHECK(Tests, !GltfMeshImportSettings::Parse("{\"gltfStaticMesh\":{\"version\":1,\"version\":1}}"));
	const auto RecipeAsset = UUID::Generate();
	PF_CHECK(Tests, RecipeAsset.has_value());
	if (!RecipeAsset) return;
	const auto DefaultRecipe = MakeRecipe(*RecipeAsset, *DefaultCanonical);
	const auto EquivalentRecipe = MakeRecipe(*RecipeAsset, *DefaultCanonical);
	const auto DifferentRecipe = MakeRecipe(*RecipeAsset, "{\"version\":1,\"missingNormalsPolicy\":\"Reject\"}");
	PF_CHECK(Tests, DefaultRecipe.InputFingerprint == EquivalentRecipe.InputFingerprint);
	PF_CHECK(Tests, DefaultRecipe.InputFingerprint != DifferentRecipe.InputFingerprint);

	const std::filesystem::path SourcePath = ModelDirectory / "triangle.gltf";
	PF_CHECK(Tests, WriteTextFile(SourcePath, TriangleDocument()));
	const auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
	PF_CHECK(Tests, SourceMetadata.has_value());
	if (!SourceMetadata) return;
	PF_CHECK(Tests, SourceMetadata->ImportSettingsJson.find("GenerateSmooth") != std::string::npos);
	AssetRegistry Registry;
	PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
	const auto Imported = GltfMeshImporter::ProcessStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
	PF_CHECK(Tests, Imported.has_value());
	if (!Imported) return;
	PF_CHECK(Tests, Imported->Recipe.SubresourceKey == "mesh/0/primitive/0");
	PF_CHECK(Tests, Imported->Recipe.Inputs.size() == 1);
	PF_CHECK(Tests, Imported->Recipe.Inputs.front().ProjectRelativePath == "Assets/Models/triangle.gltf");
	PF_CHECK(Tests, Imported->Mesh.Vertices.size() == 3);
	PF_CHECK(Tests, std::abs(Imported->Mesh.Vertices[0].Normal[2] - 1.0f) < 1.0e-6f);

	const auto Coordinated = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
	PF_CHECK(Tests, Coordinated.has_value());
	const std::filesystem::path ReceiptPath = AssetImportProvenanceSerializer::GetSidecarPath(SourcePath);
	const auto PreviousReceipt = AssetImportProvenanceSerializer::Load(ReceiptPath);
	PF_CHECK(Tests, PreviousReceipt.has_value());
	PF_CHECK(Tests, PreviousReceipt && PreviousReceipt->Recipe.InputFingerprint == Imported->Recipe.InputFingerprint);
	PF_CHECK(Tests, PreviousReceipt && PreviousReceipt->Recipe.SubresourceKey == Imported->Recipe.SubresourceKey);
	PF_CHECK(Tests, PreviousReceipt && PreviousReceipt->ResultIdentity == "mesh/0/primitive/0");
	PF_CHECK(Tests, PreviousReceipt && PreviousReceipt->ResultFingerprint == Imported->ResultFingerprint);

	const std::string RejectSettings = "{\"gltfStaticMesh\":{\"version\":1,\"missingNormalsPolicy\":\"Reject\"}}";
	PF_CHECK(Tests, AssetMetadataSerializer::SaveImportSettings(
		AssetMetadataSerializer::GetSidecarPath(SourcePath), *SourceMetadata, RejectSettings).has_value());
	const auto MetadataAfterSettingsWrite = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(SourcePath));
	PF_CHECK(Tests, MetadataAfterSettingsWrite && MetadataAfterSettingsWrite->ID == SourceMetadata->ID);
	const auto Rejected = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
	PF_CHECK(Tests, !Rejected && Rejected.error().Message.find("no authored normals") != std::string::npos);
	const auto FailedCoordinator = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
	PF_CHECK(Tests, !FailedCoordinator && FailedCoordinator.error().Code == GltfMeshImportCoordinatorErrorCode::ImportFailed);
	const auto ReceiptAfterFailure = AssetImportProvenanceSerializer::Load(ReceiptPath);
	PF_CHECK(Tests, ReceiptAfterFailure && PreviousReceipt &&
		ReceiptAfterFailure->ResultFingerprint == PreviousReceipt->ResultFingerprint &&
		ReceiptAfterFailure->Recipe.InputFingerprint == PreviousReceipt->Recipe.InputFingerprint);

	const std::filesystem::path DuplicatePath = ModelDirectory / "triangle-copy.gltf";
	PF_CHECK(Tests, WriteTextFile(DuplicatePath, TriangleDocument()));
	const auto DuplicateMetadata = AssetMetadataSerializer::CreateDuplicateForNewAsset(SourcePath, DuplicatePath);
	PF_CHECK(Tests, DuplicateMetadata && DuplicateMetadata->ID != SourceMetadata->ID);
	if (DuplicateMetadata)
	{
		const auto LoadedDuplicate = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(DuplicatePath));
		PF_CHECK(Tests, LoadedDuplicate && LoadedDuplicate->ID == DuplicateMetadata->ID);
		const auto DuplicateSettings = LoadedDuplicate
			? GltfMeshImportSettings::Parse(LoadedDuplicate->ImportSettingsJson)
			: std::expected<GltfMeshImportSettings, std::string>{};
		PF_CHECK(Tests, DuplicateSettings && DuplicateSettings->NormalsPolicy == MissingNormalsPolicy::Reject);
		PF_CHECK(Tests, !std::filesystem::exists(AssetImportProvenanceSerializer::GetSidecarPath(DuplicatePath)));
	}

	const std::filesystem::path WriteFailureSource = ModelDirectory / "receipt-write-failure.gltf";
	PF_CHECK(Tests, WriteTextFile(WriteFailureSource, TriangleDocument()));
	const auto WriteFailureMetadata = AssetMetadataSerializer::CreateForNewAsset(WriteFailureSource);
	PF_CHECK(Tests, WriteFailureMetadata.has_value());
	if (!WriteFailureMetadata) return;
	AssetRegistry WriteFailureRegistry;
	PF_CHECK(Tests, WriteFailureRegistry.Rebuild(ProjectRoot).has_value());
	std::filesystem::create_directory(AssetImportProvenanceSerializer::GetSidecarPath(WriteFailureSource), Error);
	PF_CHECK(Tests, !Error);
	const auto WriteFailure = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		WriteFailureMetadata->ID, ProjectRoot, WriteFailureRegistry);
	PF_CHECK(Tests, !WriteFailure && WriteFailure.error().Code == GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed);
	PF_CHECK(Tests, std::filesystem::is_directory(AssetImportProvenanceSerializer::GetSidecarPath(WriteFailureSource)));
}
