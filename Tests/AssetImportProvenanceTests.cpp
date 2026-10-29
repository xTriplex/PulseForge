#include "TestAssetSupport.h"

#include "Assets/AssetImportProvenance.h"
#include "Assets/AssetDependencyCatalog.h"
#include "Assets/GltfMeshImportCoordinator.h"
#include "Assets/GltfMeshImporter.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>

namespace PulseForge
{
	struct AssetImportProvenanceTestAccess
	{
		using Replacement = std::function<std::expected<void, std::string>(
			const std::filesystem::path&, const std::filesystem::path&)>;

		static std::expected<void, AssetImportProvenanceError> SaveAllWithReplacement(
			const std::filesystem::path& Path,
			std::span<const AssetImportSuccessReceipt> Receipts,
			const Replacement& Replace)
		{
			return AssetImportProvenanceSerializer::SaveAllWithReplacement(Path, Receipts, Replace);
		}
	};
}

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

	std::string TwoPrimitiveTriangleDocument()
	{
		const std::vector<std::byte> Bytes = MakeTriangleBytes();
		return "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
			Base64Encode(Bytes) + "\",\"byteLength\":36}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}]," +
			"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}]," +
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}},{\"attributes\":{\"POSITION\":0}}]}]}";
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
	const std::vector<std::byte> FingerprintedBytes = MakeTriangleBytes();
	const AssetContentFingerprint OrdinaryFingerprint = ComputeAssetContentFingerprint(FingerprintedBytes);
	const auto CancellableFingerprint = ComputeAssetContentFingerprintCancellable(FingerprintedBytes, {});
	PF_CHECK(Tests, CancellableFingerprint && *CancellableFingerprint == OrdinaryFingerprint);
	std::stop_source FingerprintStop;
	FingerprintStop.request_stop();
	const auto CancelledFingerprint = ComputeAssetContentFingerprintCancellable(FingerprintedBytes, FingerprintStop.get_token());
	PF_CHECK(Tests, !CancelledFingerprint && CancelledFingerprint.error().find("cancelled") != std::string::npos);
	AssetFingerprintBuilder IdenticalContent("test-identical-input");
	IdenticalContent.UpdateText("value", "same");
	const AssetContentFingerprint IdenticalFingerprint = IdenticalContent.Finish();
	AssetFingerprintBuilder ConflictingContent("test-conflicting-input");
	ConflictingContent.UpdateText("value", "different");
	const AssetContentFingerprint ConflictingFingerprint = ConflictingContent.Finish();
	std::vector<AssetImportInputFingerprint> DuplicateInputs{
		{ "Assets/shared.bin", IdenticalFingerprint }, { "Assets/shared.bin", IdenticalFingerprint }
	};
	PF_CHECK(Tests, NormalizeAssetImportRecipeInputs(DuplicateInputs).has_value());
	PF_CHECK(Tests, DuplicateInputs.size() == 1);
	std::vector<AssetImportInputFingerprint> ConflictingInputs{
		{ "Assets/shared.bin", ConflictingFingerprint }, { "Assets/shared.bin", IdenticalFingerprint }
	};
	const auto ConflictingNormalization = NormalizeAssetImportRecipeInputs(ConflictingInputs);
	PF_CHECK(Tests, !ConflictingNormalization &&
		ConflictingNormalization.error().find("conflicting content") != std::string::npos);
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

	// Each supported primitive is an independent logical processing output for provenance purposes.
	const std::filesystem::path MultiOutputPath = ModelDirectory / "two-primitives.gltf";
	PF_CHECK(Tests, WriteTextFile(MultiOutputPath, TwoPrimitiveTriangleDocument()));
	const auto MultiOutputMetadata = AssetMetadataSerializer::CreateForNewAsset(MultiOutputPath);
	PF_CHECK(Tests, MultiOutputMetadata.has_value());
	if (!MultiOutputMetadata) return;
	AssetRegistry MultiOutputRegistry;
	PF_CHECK(Tests, MultiOutputRegistry.Rebuild(ProjectRoot).has_value());
	const auto PrimitiveA = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		MultiOutputMetadata->ID, ProjectRoot, MultiOutputRegistry, 0, 0);
	const auto PrimitiveB = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		MultiOutputMetadata->ID, ProjectRoot, MultiOutputRegistry, 0, 1);
	PF_CHECK(Tests, PrimitiveA.has_value());
	PF_CHECK(Tests, PrimitiveB.has_value());
	if (!PrimitiveA || !PrimitiveB) return;
	PF_CHECK(Tests, PrimitiveA->Receipt.ResultIdentity == "mesh/0/primitive/0");
	PF_CHECK(Tests, PrimitiveB->Receipt.ResultIdentity == "mesh/0/primitive/1");
	const std::filesystem::path MultiOutputReceiptPath =
		AssetImportProvenanceSerializer::GetSidecarPath(MultiOutputPath);
	auto ReceiptsAfterBoth = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, ReceiptsAfterBoth && ReceiptsAfterBoth->size() == 2);
	if (!ReceiptsAfterBoth || ReceiptsAfterBoth->size() != 2) return;
	PF_CHECK(Tests, (*ReceiptsAfterBoth)[0].Recipe.SubresourceKey == "mesh/0/primitive/0");
	PF_CHECK(Tests, (*ReceiptsAfterBoth)[1].Recipe.SubresourceKey == "mesh/0/primitive/1");
	PF_CHECK(Tests, (*ReceiptsAfterBoth)[0].Recipe.SourceAsset == MultiOutputMetadata->ID &&
		(*ReceiptsAfterBoth)[1].Recipe.SourceAsset == MultiOutputMetadata->ID);
	const auto MultiOutputProjectID = UUID::Generate();
	PF_CHECK(Tests, MultiOutputProjectID.has_value());
	if (!MultiOutputProjectID) return;
	const auto CatalogAfterTwoImports = AssetDependencyCatalogBuilder::Build(ProjectRoot, *MultiOutputProjectID, 1);
	PF_CHECK(Tests, CatalogAfterTwoImports.has_value());
	if (!CatalogAfterTwoImports) return;
	const auto FindMultiOutputObservation = [](const AssetDependencyCatalogSnapshot& Snapshot,
		const AssetID& Asset, std::string_view Key)
	{
		return std::find_if(Snapshot.GetImportObservations().begin(), Snapshot.GetImportObservations().end(),
			[&](const AssetImportObservation& Observation)
			{ return Observation.Asset == Asset && Observation.SubresourceKey == Key; });
	};
	const auto& TwoImportSnapshot = **CatalogAfterTwoImports;
	const auto PrimitiveAObservation = FindMultiOutputObservation(
		TwoImportSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/0");
	const auto PrimitiveBObservation = FindMultiOutputObservation(
		TwoImportSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/1");
	PF_CHECK(Tests, PrimitiveAObservation != TwoImportSnapshot.GetImportObservations().end());
	PF_CHECK(Tests, PrimitiveBObservation != TwoImportSnapshot.GetImportObservations().end());
	if (PrimitiveAObservation != TwoImportSnapshot.GetImportObservations().end() &&
		PrimitiveBObservation != TwoImportSnapshot.GetImportObservations().end())
	{
		PF_CHECK(Tests, PrimitiveAObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
		PF_CHECK(Tests, PrimitiveAObservation->LastSuccessfulResult.has_value());
		PF_CHECK(Tests, PrimitiveAObservation->LastSuccessfulResult &&
			PrimitiveAObservation->LastSuccessfulResult->Recipe.InputFingerprint == PrimitiveA->Receipt.Recipe.InputFingerprint);
		PF_CHECK(Tests, PrimitiveBObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
		PF_CHECK(Tests, PrimitiveBObservation->LastSuccessfulResult.has_value());
		PF_CHECK(Tests, PrimitiveBObservation->LastSuccessfulResult &&
			PrimitiveBObservation->LastSuccessfulResult->Recipe.InputFingerprint == PrimitiveB->Receipt.Recipe.InputFingerprint);
	}
	const auto ReimportedPrimitiveA = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		MultiOutputMetadata->ID, ProjectRoot, MultiOutputRegistry, 0, 0);
	PF_CHECK(Tests, ReimportedPrimitiveA.has_value());
	if (!ReimportedPrimitiveA) return;
	const auto CatalogAfterAReimport = AssetDependencyCatalogBuilder::Build(ProjectRoot, *MultiOutputProjectID, 2);
	PF_CHECK(Tests, CatalogAfterAReimport.has_value());
	if (!CatalogAfterAReimport) return;
	const auto& ReimportSnapshot = **CatalogAfterAReimport;
	const auto ReimportedAObservation = FindMultiOutputObservation(
		ReimportSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/0");
	const auto RetainedBObservation = FindMultiOutputObservation(
		ReimportSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/1");
	PF_CHECK(Tests, ReimportedAObservation != ReimportSnapshot.GetImportObservations().end());
	PF_CHECK(Tests, RetainedBObservation != ReimportSnapshot.GetImportObservations().end());
	if (ReimportedAObservation != ReimportSnapshot.GetImportObservations().end() &&
		RetainedBObservation != ReimportSnapshot.GetImportObservations().end())
	{
		PF_CHECK(Tests, ReimportedAObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
		PF_CHECK(Tests, RetainedBObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
		PF_CHECK(Tests, RetainedBObservation->LastSuccessfulResult.has_value());
	}
	auto ReceiptsAfterAReimport = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, ReceiptsAfterAReimport && ReceiptsAfterAReimport->size() == 2);
	if (!ReceiptsAfterAReimport || ReceiptsAfterAReimport->size() != 2) return;
	PF_CHECK(Tests, (*ReceiptsAfterAReimport)[0].ResultFingerprint == ReimportedPrimitiveA->Receipt.ResultFingerprint);
	PF_CHECK(Tests, (*ReceiptsAfterAReimport)[1].ResultFingerprint == (*ReceiptsAfterBoth)[1].ResultFingerprint);

	// A legacy v1 single-record receipt remains readable without a read-time rewrite, then upgrades on a successful update.
	const auto MultiOutputReceiptBeforeLegacyRead = ReadTextFile(MultiOutputReceiptPath);
	PF_CHECK(Tests, MultiOutputReceiptBeforeLegacyRead.has_value());
	if (!MultiOutputReceiptBeforeLegacyRead) return;
	using Json = nlohmann::ordered_json;
	Json LegacyVersionOne = Json::object({ { "format", "PulseForgeImportResult" }, { "version", 1 } });
	const Json CurrentVersionTwo = Json::parse(*MultiOutputReceiptBeforeLegacyRead);
	for (auto It = CurrentVersionTwo.at("records").front().begin(); It != CurrentVersionTwo.at("records").front().end(); ++It)
		LegacyVersionOne[It.key()] = It.value();
	const std::string LegacyVersionOneBytes = LegacyVersionOne.dump(2) + "\n";
	PF_CHECK(Tests, WriteTextFile(MultiOutputReceiptPath, LegacyVersionOneBytes));
	const auto ReadLegacyReceipt = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, ReadLegacyReceipt && ReadLegacyReceipt->size() == 1 &&
		ReadLegacyReceipt->front().Recipe.SubresourceKey == "mesh/0/primitive/0");
	PF_CHECK(Tests, ReadTextFile(MultiOutputReceiptPath) == LegacyVersionOneBytes);
	const auto UpgradeByImportingB = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		MultiOutputMetadata->ID, ProjectRoot, MultiOutputRegistry, 0, 1);
	PF_CHECK(Tests, UpgradeByImportingB.has_value());
	if (!UpgradeByImportingB) return;
	auto UpgradedReceiptRecords = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, UpgradedReceiptRecords && UpgradedReceiptRecords->size() == 2);
	if (!UpgradedReceiptRecords || UpgradedReceiptRecords->size() != 2) return;
	PF_CHECK(Tests, (*UpgradedReceiptRecords)[0].Recipe.SubresourceKey == "mesh/0/primitive/0");
	PF_CHECK(Tests, (*UpgradedReceiptRecords)[1].Recipe.SubresourceKey == "mesh/0/primitive/1");
	const auto CatalogAfterLegacyUpgrade = AssetDependencyCatalogBuilder::Build(ProjectRoot, *MultiOutputProjectID, 3);
	PF_CHECK(Tests, CatalogAfterLegacyUpgrade.has_value());
	if (!CatalogAfterLegacyUpgrade) return;
	const auto& LegacyUpgradeSnapshot = **CatalogAfterLegacyUpgrade;
	const auto LegacyAObservation = FindMultiOutputObservation(
		LegacyUpgradeSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/0");
	const auto LegacyBObservation = FindMultiOutputObservation(
		LegacyUpgradeSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/1");
	PF_CHECK(Tests, LegacyAObservation != LegacyUpgradeSnapshot.GetImportObservations().end());
	PF_CHECK(Tests, LegacyBObservation != LegacyUpgradeSnapshot.GetImportObservations().end());
	if (LegacyAObservation != LegacyUpgradeSnapshot.GetImportObservations().end() &&
		LegacyBObservation != LegacyUpgradeSnapshot.GetImportObservations().end())
	{
		PF_CHECK(Tests, LegacyAObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
		PF_CHECK(Tests, LegacyBObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
		PF_CHECK(Tests, LegacyAObservation->LastSuccessfulResult.has_value());
		PF_CHECK(Tests, LegacyBObservation->LastSuccessfulResult.has_value());
	}
	const auto VersionTwoBytesBeforeInjectedFailure = ReadTextFile(MultiOutputReceiptPath);
	PF_CHECK(Tests, VersionTwoBytesBeforeInjectedFailure.has_value());
	if (!VersionTwoBytesBeforeInjectedFailure) return;
	std::vector<AssetImportSuccessReceipt> AlteredRecords = *UpgradedReceiptRecords;
	AlteredRecords[0].ResultFingerprint.DigestHex[0] = AlteredRecords[0].ResultFingerprint.DigestHex[0] == '0' ? '1' : '0';
	bool ReplacementInvoked = false;
	const auto InjectedPersistenceFailure = AssetImportProvenanceTestAccess::SaveAllWithReplacement(
		MultiOutputReceiptPath, AlteredRecords,
		[&ReplacementInvoked](const std::filesystem::path&, const std::filesystem::path&) -> std::expected<void, std::string>
		{
			ReplacementInvoked = true;
			return std::unexpected("deterministic replacement failure");
		});
	PF_CHECK(Tests, !InjectedPersistenceFailure &&
		InjectedPersistenceFailure.error().Code == AssetImportProvenanceErrorCode::WriteFailed);
	PF_CHECK(Tests, ReplacementInvoked);
	PF_CHECK(Tests, ReadTextFile(MultiOutputReceiptPath) == VersionTwoBytesBeforeInjectedFailure);
	const auto RecordsAfterInjectedFailure = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, RecordsAfterInjectedFailure && RecordsAfterInjectedFailure->size() == 2);
	if (RecordsAfterInjectedFailure && RecordsAfterInjectedFailure->size() == 2)
		PF_CHECK(Tests, (*RecordsAfterInjectedFailure)[0].ResultFingerprint == (*UpgradedReceiptRecords)[0].ResultFingerprint);

	Json MalformedDuplicateOutput = Json::parse(*VersionTwoBytesBeforeInjectedFailure);
	MalformedDuplicateOutput["records"].push_back(MalformedDuplicateOutput["records"].front());
	const auto DuplicateOutputParse = AssetImportProvenanceSerializer::DeserializeAll(MalformedDuplicateOutput.dump());
	PF_CHECK(Tests, !DuplicateOutputParse &&
		DuplicateOutputParse.error().Code == AssetImportProvenanceErrorCode::InvalidDocument);

	const std::string RejectMultiOutputSettings =
		"{\"gltfStaticMesh\":{\"version\":1,\"missingNormalsPolicy\":\"Reject\"}}";
	PF_CHECK(Tests, AssetMetadataSerializer::SaveImportSettings(
		AssetMetadataSerializer::GetSidecarPath(MultiOutputPath), *MultiOutputMetadata, RejectMultiOutputSettings).has_value());
	const auto FailedMultiOutputReimport = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		MultiOutputMetadata->ID, ProjectRoot, MultiOutputRegistry, 0, 0);
	PF_CHECK(Tests, !FailedMultiOutputReimport &&
		FailedMultiOutputReimport.error().Code == GltfMeshImportCoordinatorErrorCode::ImportFailed);
	const auto RecordsAfterMultiOutputFailure = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, RecordsAfterMultiOutputFailure && RecordsAfterMultiOutputFailure->size() == 2);
	if (!RecordsAfterMultiOutputFailure || RecordsAfterMultiOutputFailure->size() != 2) return;
	PF_CHECK(Tests, (*RecordsAfterMultiOutputFailure)[0].ResultFingerprint == (*UpgradedReceiptRecords)[0].ResultFingerprint);
	PF_CHECK(Tests, (*RecordsAfterMultiOutputFailure)[1].ResultFingerprint == (*UpgradedReceiptRecords)[1].ResultFingerprint);
	const auto CatalogAfterSettingsChange = AssetDependencyCatalogBuilder::Build(ProjectRoot, *MultiOutputProjectID, 4);
	PF_CHECK(Tests, CatalogAfterSettingsChange.has_value());
	if (!CatalogAfterSettingsChange) return;
	const auto& SettingsChangeSnapshot = **CatalogAfterSettingsChange;
	const auto HistoricalAObservation = FindMultiOutputObservation(
		SettingsChangeSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/0");
	const auto HistoricalBObservation = FindMultiOutputObservation(
		SettingsChangeSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/1");
	PF_CHECK(Tests, HistoricalAObservation != SettingsChangeSnapshot.GetImportObservations().end());
	PF_CHECK(Tests, HistoricalBObservation != SettingsChangeSnapshot.GetImportObservations().end());
	if (HistoricalAObservation != SettingsChangeSnapshot.GetImportObservations().end() &&
		HistoricalBObservation != SettingsChangeSnapshot.GetImportObservations().end())
	{
		PF_CHECK(Tests, HistoricalAObservation->ResultState == AssetImportResultState::SuccessfulInputsDiffer);
		PF_CHECK(Tests, HistoricalBObservation->ResultState == AssetImportResultState::SuccessfulInputsDiffer);
		PF_CHECK(Tests, HistoricalAObservation->LastSuccessfulResult.has_value());
		PF_CHECK(Tests, HistoricalBObservation->LastSuccessfulResult.has_value());
	}

	const std::filesystem::path ForeignOwnerPath = ModelDirectory / "foreign-owner.gltf";
	PF_CHECK(Tests, WriteTextFile(ForeignOwnerPath, TwoPrimitiveTriangleDocument()));
	const auto ForeignOwnerMetadata = AssetMetadataSerializer::CreateForNewAsset(ForeignOwnerPath);
	PF_CHECK(Tests, ForeignOwnerMetadata.has_value());
	if (!ForeignOwnerMetadata) return;
	const std::filesystem::path ForeignReceiptPath = AssetImportProvenanceSerializer::GetSidecarPath(ForeignOwnerPath);
	std::filesystem::copy_file(MultiOutputReceiptPath, ForeignReceiptPath,
		std::filesystem::copy_options::overwrite_existing, Error);
	PF_CHECK(Tests, !Error);
	const auto ForeignReceiptBytesBeforeProcess = ReadTextFile(ForeignReceiptPath);
	PF_CHECK(Tests, ForeignReceiptBytesBeforeProcess.has_value());
	AssetRegistry ForeignOwnerRegistry;
	PF_CHECK(Tests, ForeignOwnerRegistry.Rebuild(ProjectRoot).has_value());
	const auto ForeignReceiptImport = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
		ForeignOwnerMetadata->ID, ProjectRoot, ForeignOwnerRegistry, 0, 0);
	PF_CHECK(Tests, !ForeignReceiptImport &&
		ForeignReceiptImport.error().Code == GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed);
	PF_CHECK(Tests, ReadTextFile(ForeignReceiptPath) == ForeignReceiptBytesBeforeProcess);

	// Stale records for a removed primitive remain historical data and do not create a live output observation.
	PF_CHECK(Tests, WriteTextFile(MultiOutputPath, TriangleDocument()));
	const auto CatalogAfterOutputRemoval = AssetDependencyCatalogBuilder::Build(ProjectRoot, *MultiOutputProjectID, 5);
	PF_CHECK(Tests, CatalogAfterOutputRemoval.has_value());
	if (!CatalogAfterOutputRemoval) return;
	const auto& OutputRemovalSnapshot = **CatalogAfterOutputRemoval;
	const auto RemovedPrimitiveObservation = FindMultiOutputObservation(
		OutputRemovalSnapshot, MultiOutputMetadata->ID, "mesh/0/primitive/1");
	PF_CHECK(Tests, RemovedPrimitiveObservation == OutputRemovalSnapshot.GetImportObservations().end());
	const auto HistoricalRecordsAfterOutputRemoval = AssetImportProvenanceSerializer::LoadAll(MultiOutputReceiptPath);
	PF_CHECK(Tests, HistoricalRecordsAfterOutputRemoval && HistoricalRecordsAfterOutputRemoval->size() == 2);

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
