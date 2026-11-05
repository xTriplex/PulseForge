#include "TestEditorSupport.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorMaterialPreviewPublication.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorMaterialDetailsPanel.h"
#include "Editor/EditorMaterialDocumentEditor.h"
#include "Editor/EditorStyle.h"

#include <fstream>
#include <iterator>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
	void TestMaterialAssets(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto TextureAsset = PulseForge::UUID::Generate();
		PF_CHECK(Tests, TextureAsset.has_value());
		if (!TextureAsset) return;

		MaterialAssetDesc Description;
		Description.BaseColorTexture = *TextureAsset;
		Description.BaseColorFactor = { 0.8f, 0.6f, 0.4f, 1.0f };
		Description.MetallicFactor = 0.65f;
		Description.RoughnessFactor = 0.25f;
		const auto Serialized = MaterialAssetSerializer::Serialize(Description);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		const auto Deserialized = MaterialAssetSerializer::Deserialize(*Serialized);
		PF_CHECK(Tests, Deserialized && Deserialized->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Deserialized && glm::all(glm::equal(Deserialized->BaseColorFactor, Description.BaseColorFactor)));
		PF_CHECK(Tests, Deserialized && Deserialized->MetallicFactor == Description.MetallicFactor &&
							Deserialized->RoughnessFactor == Description.RoughnessFactor);
		PF_CHECK(Tests, Serialized && Serialized->find("\"version\": 4") != std::string::npos);
		const std::string Version1Document = "{\"format\":\"PulseForgeMaterial\",\"version\":1,\"baseColorTexture\":\"" +
											 TextureAsset->ToString() + "\",\"baseColorFactor\":[1,1,1,1]}";
		const auto Version1Material = MaterialAssetSerializer::Deserialize(Version1Document);
		PF_CHECK(Tests, Version1Material && Version1Material->MetallicFactor == 0.0f && Version1Material->RoughnessFactor == 1.0f);
		PF_CHECK(Tests, Version1Material && Version1Material->BaseColorTexture == *TextureAsset &&
							Version1Material->NormalTexture.IsNil() && Version1Material->AlphaMode == MaterialAlphaMode::Opaque);
		const std::string Version2Document = "{\"format\":\"PulseForgeMaterial\",\"version\":2,\"baseColorTexture\":\"" +
											 TextureAsset->ToString() +
											 "\",\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0.4,\"roughnessFactor\":0.6}";
		const auto Version2Material = MaterialAssetSerializer::Deserialize(Version2Document);
		PF_CHECK(Tests, Version2Material && Version2Material->MetallicFactor == 0.4f && Version2Material->RoughnessFactor == 0.6f &&
							Version2Material->NormalTexture.IsNil());
		const std::string Version3Document = "{\"format\":\"PulseForgeMaterial\",\"version\":3,\"baseColorFactor\":[1,1,1,1],"
											 "\"baseColorTexture\":null,\"metallicFactor\":0.4,\"roughnessFactor\":0.6}";
		const auto Version3Material = MaterialAssetSerializer::Deserialize(Version3Document);
		PF_CHECK(Tests, Version3Material && Version3Material->AlphaMode == MaterialAlphaMode::Opaque &&
							Version3Material->AlphaCutoff == 0.5f && !Version3Material->DoubleSided);
		PF_CHECK(Tests, Version3Material && Version3Material->MetallicFactor == 0.4f && Version3Material->RoughnessFactor == 0.6f &&
							Version3Material->NormalScale == 1.0f && Version3Material->OcclusionStrength == 1.0f);
		const auto CheckLegacyMaterialUpgrade = [&](const MaterialAssetDesc& LegacyMaterial)
		{
			const auto Upgraded = MaterialAssetSerializer::Serialize(LegacyMaterial);
			PF_CHECK(Tests, Upgraded && Upgraded->find("\"version\": 4") != std::string::npos);
			if (!Upgraded) return;
			const auto RoundTripped = MaterialAssetSerializer::Deserialize(*Upgraded);
			PF_CHECK(Tests, RoundTripped && RoundTripped->BaseColorTexture == LegacyMaterial.BaseColorTexture &&
								glm::all(glm::equal(RoundTripped->BaseColorFactor, LegacyMaterial.BaseColorFactor)) &&
								RoundTripped->MetallicFactor == LegacyMaterial.MetallicFactor &&
								RoundTripped->RoughnessFactor == LegacyMaterial.RoughnessFactor);
		};
		if (Version1Material) CheckLegacyMaterialUpgrade(*Version1Material);
		if (Version2Material) CheckLegacyMaterialUpgrade(*Version2Material);
		if (Version3Material) CheckLegacyMaterialUpgrade(*Version3Material);
		MaterialAssetDesc TexturedDescription = Description;
		TexturedDescription.BaseColorTexture = {};
		TexturedDescription.MetallicRoughnessTexture = *TextureAsset;
		TexturedDescription.NormalTexture = *TextureAsset;
		TexturedDescription.OcclusionTexture = *TextureAsset;
		TexturedDescription.EmissiveTexture = *TextureAsset;
		TexturedDescription.NormalScale = 0.75f;
		TexturedDescription.OcclusionStrength = 0.35f;
		TexturedDescription.EmissiveFactor = { 0.5f, 1.25f, 2.0f };
		TexturedDescription.AlphaMode = MaterialAlphaMode::Mask;
		TexturedDescription.AlphaCutoff = 0.42f;
		TexturedDescription.DoubleSided = true;
		const auto TexturedSerialized = MaterialAssetSerializer::Serialize(TexturedDescription);
		PF_CHECK(Tests, TexturedSerialized.has_value());
		if (!TexturedSerialized) return;
		const auto TexturedDeserialized = MaterialAssetSerializer::Deserialize(*TexturedSerialized);
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->BaseColorTexture.IsNil() &&
							TexturedDeserialized->MetallicRoughnessTexture == TexturedDescription.MetallicRoughnessTexture &&
							TexturedDeserialized->NormalTexture == TexturedDescription.NormalTexture &&
							TexturedDeserialized->OcclusionTexture == TexturedDescription.OcclusionTexture &&
							TexturedDeserialized->EmissiveTexture == TexturedDescription.EmissiveTexture);
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->NormalScale == 0.75f &&
							TexturedDeserialized->OcclusionStrength == 0.35f &&
							glm::all(glm::equal(TexturedDeserialized->EmissiveFactor, TexturedDescription.EmissiveFactor)));
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->AlphaMode == MaterialAlphaMode::Mask &&
							TexturedDeserialized->AlphaCutoff == 0.42f && TexturedDeserialized->DoubleSided);
		MaterialAssetDesc BlendDescription = Description;
		BlendDescription.AlphaMode = MaterialAlphaMode::Blend;
		const auto BlendSerialized = MaterialAssetSerializer::Serialize(BlendDescription);
		PF_CHECK(Tests, BlendSerialized.has_value());
		if (BlendSerialized)
		{
			const auto BlendDeserialized = MaterialAssetSerializer::Deserialize(*BlendSerialized);
			PF_CHECK(Tests, BlendDeserialized && BlendDeserialized->AlphaMode == MaterialAlphaMode::Blend);
		}
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(
							"{\"format\":\"PulseForgeMaterial\",\"version\":4,\"baseColorFactor\":[1,1,1,1],\"alphaMode\":\"HASH\"}"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(
							"{\"format\":\"PulseForgeMaterial\",\"version\":3,\"baseColorFactor\":[1,1,1,1],\"normalTexture\":7}"));
		std::string InvalidMetallicDocument = *Serialized;
		const size_t MetallicField = InvalidMetallicDocument.find("\"metallicFactor\":");
		const size_t MetallicValue = MetallicField == std::string::npos ? MetallicField
																		: InvalidMetallicDocument.find_first_not_of(" :",
																			  MetallicField + std::string("\"metallicFactor\"").size());
		const size_t MetallicEnd =
			MetallicValue == std::string::npos ? MetallicValue : InvalidMetallicDocument.find_first_of(",\n", MetallicValue);
		PF_CHECK(Tests, MetallicValue != std::string::npos && MetallicEnd != std::string::npos);
		if (MetallicEnd != std::string::npos) InvalidMetallicDocument.replace(MetallicValue, MetallicEnd - MetallicValue, "-0.1");
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(InvalidMetallicDocument));

		MaterialAssetDesc MissingTexture = Description;
		MissingTexture.BaseColorTexture = {};
		PF_CHECK(Tests, ValidateMaterialAssetDescription(MissingTexture).has_value());
		MaterialAssetDesc InvalidFactor = Description;
		InvalidFactor.BaseColorFactor.g = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.BaseColorFactor = Description.BaseColorFactor;
		InvalidFactor.BaseColorFactor.r = 1.1f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.MetallicFactor = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.MetallicFactor = 1.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.RoughnessFactor = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.NormalScale = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.NormalScale = 4.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.OcclusionStrength = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.AlphaCutoff = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.AlphaCutoff = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.AlphaMode = static_cast<MaterialAlphaMode>(0xff);
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.EmissiveFactor.g = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.EmissiveFactor = { 10000.1f, 0.0f, 0.0f };
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{invalid"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{\"format\":\"OtherMaterial\",\"version\":1}"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{\"format\":\"PulseForgeMaterial\",\"version\":99}"));
		for (const std::string_view UnsupportedVersion : { "0", "-1", "5" })
		{
			const std::string UnsupportedDocument =
				"{\"format\":\"PulseForgeMaterial\",\"version\":" + std::string(UnsupportedVersion) + ",\"baseColorFactor\":[1,1,1,1]}";
			const auto Result = MaterialAssetSerializer::Deserialize(UnsupportedDocument);
			PF_CHECK(Tests, !Result && Result.error().Code == MaterialAssetErrorCode::UnsupportedVersion);
		}
		for (const std::string_view InvalidVersion : { "1.5", "\"1\"", "null" })
		{
			const std::string InvalidDocument =
				"{\"format\":\"PulseForgeMaterial\",\"version\":" + std::string(InvalidVersion) + ",\"baseColorFactor\":[1,1,1,1]}";
			const auto Result = MaterialAssetSerializer::Deserialize(InvalidDocument);
			PF_CHECK(Tests, !Result && Result.error().Code == MaterialAssetErrorCode::InvalidDocument);
		}
		const auto MissingMaterialVersion =
			MaterialAssetSerializer::Deserialize("{\"format\":\"PulseForgeMaterial\",\"baseColorFactor\":[1,1,1,1]}");
		PF_CHECK(Tests, !MissingMaterialVersion && MissingMaterialVersion.error().Code == MaterialAssetErrorCode::InvalidDocument);
		const std::string MaterialWithUnknownField = "{\"format\":\"PulseForgeMaterial\",\"version\":4,"
													 "\"baseColorFactor\":[1,1,1,1],\"futureMaterialData\":{\"keep\":true}}";
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(MaterialWithUnknownField));

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		const auto ProjectIdentifier = PulseForge::UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeMaterials-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };
		const std::filesystem::path MaterialsDirectory = ProjectRoot / "Assets" / "Materials";
		const std::filesystem::path TexturesDirectory = ProjectRoot / "Assets" / "Textures";
		std::filesystem::create_directories(MaterialsDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::create_directories(TexturesDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const std::filesystem::path TexturePath = TexturesDirectory / "base-color.png";
		{
			std::ofstream Output(TexturePath, std::ios::binary | std::ios::trunc);
			Output << "texture placeholder";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto TextureMetadata = AssetMetadataSerializer::CreateForNewAsset(TexturePath);
		PF_CHECK(Tests, TextureMetadata.has_value());
		if (!TextureMetadata) return;
		Description.BaseColorTexture = TextureMetadata->ID;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Created = MaterialAssetService::Create(Registry, ProjectRoot, "Assets/Materials/validation.MATERIAL", Description);
		PF_CHECK(Tests, Created.has_value());
		PF_CHECK(Tests, Created && Created->ID != TextureMetadata->ID);
		if (!Created) return;

		const std::filesystem::path MaterialPath = ProjectRoot / "Assets" / "Materials" / "validation.MATERIAL";
		const auto Metadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(MaterialPath));
		PF_CHECK(Tests, Metadata && Metadata->ID == Created->ID);
		PF_CHECK(Tests, Registry.Find(Created->ID).has_value());
		const auto Loaded = MaterialAssetService::Load(Created->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Loaded && Loaded->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Loaded && glm::all(glm::equal(Loaded->BaseColorFactor, Description.BaseColorFactor)));
		PF_CHECK(Tests, Loaded && std::abs(Loaded->MetallicFactor - Description.MetallicFactor) < 1.0e-5f);
		PF_CHECK(Tests, Loaded && std::abs(Loaded->RoughnessFactor - Description.RoughnessFactor) < 1.0e-5f);

		MaterialAssetCache Cache(ProjectRoot, Registry);
		const auto Cached = Cache.GetOrLoad(Created->ID);
		const auto CachedAgain = Cache.GetOrLoad(Created->ID);
		PF_CHECK(Tests, Cached && CachedAgain && &Cached->get() == &CachedAgain->get());
		PF_CHECK(Tests, Cache.GetLoadedCount() == 1);

		MaterialAssetDesc Updated = Description;
		Updated.BaseColorFactor = { 0.3f, 0.5f, 0.7f, 1.0f };
		PF_CHECK(Tests, MaterialAssetService::Save(Created->ID, ProjectRoot, Registry, Updated).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(MaterialPath));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == Created->ID);
		PF_CHECK(Tests, Cached && glm::all(glm::equal(Cached->get().BaseColorFactor, Description.BaseColorFactor)));
		Cache.Clear();
		const auto Reloaded = Cache.GetOrLoad(Created->ID);
		PF_CHECK(Tests, Reloaded && glm::all(glm::equal(Reloaded->get().BaseColorFactor, Updated.BaseColorFactor)));

		const auto Moved = AssetOperations::Move(Registry, ProjectRoot, Created->ID, "Assets/Materials/renamed.material");
		PF_CHECK(Tests, Moved && Moved->ID == Created->ID);
		const auto LoadedAfterMove = MaterialAssetService::Load(Created->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, LoadedAfterMove && glm::all(glm::equal(LoadedAfterMove->BaseColorFactor, Updated.BaseColorFactor)));

		const auto InvalidExtension = MaterialAssetService::Create(Registry, ProjectRoot, "Assets/Materials/invalid.json", Description);
		PF_CHECK(Tests, !InvalidExtension && InvalidExtension.error().Code == MaterialAssetErrorCode::UnsupportedAssetType);
		const auto Duplicate = MaterialAssetService::Create(Registry, ProjectRoot, "Assets/Materials/renamed.material", Description);
		PF_CHECK(Tests, !Duplicate && Duplicate.error().Code == MaterialAssetErrorCode::AssetOperationFailed);

		const std::filesystem::path WrongTypePath = MaterialsDirectory / "not-material.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "not a material";
		}
		const auto WrongTypeMetadata = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeMetadata.has_value());
		const std::filesystem::path BrokenPath = MaterialsDirectory / "broken.material";
		{
			std::ofstream Output(BrokenPath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
		}
		const auto BrokenMetadata = AssetMetadataSerializer::CreateForNewAsset(BrokenPath);
		PF_CHECK(Tests, BrokenMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (WrongTypeMetadata)
		{
			const auto WrongType = MaterialAssetService::Load(WrongTypeMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == MaterialAssetErrorCode::UnsupportedAssetType);
		}
		if (BrokenMetadata)
		{
			const auto Broken = MaterialAssetService::Load(BrokenMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !Broken && Broken.error().Code == MaterialAssetErrorCode::InvalidDocument);
		}
	}

	void TestMaterialDraftController(TestRunner& Tests)
	{
		using namespace PulseForge;
		{
			PulseForgeEditor::EditorMaterialPreviewPublication Publication;
			MaterialAssetDesc DraftA;
			DraftA.BaseColorFactor = { 0.25f, 0.4f, 0.7f, 1.0f };
			MaterialAssetDesc DraftB = DraftA;
			DraftB.BaseColorFactor.r = 0.75f;
			const auto TextureA = PulseForge::UUID::Generate();
			const auto TextureB = PulseForge::UUID::Generate();
			PF_CHECK(Tests, TextureA && TextureB);
			if (!TextureA || !TextureB) return;
			DraftA.BaseColorTexture = *TextureA;
			DraftB.BaseColorTexture = *TextureA;
			MaterialAssetDesc DraftC = DraftA;
			DraftC.BaseColorTexture = *TextureB;
			const auto BytesA = MaterialAssetSerializer::Serialize(DraftA);
			const auto BytesB = MaterialAssetSerializer::Serialize(DraftB);
			const auto BytesC = MaterialAssetSerializer::Serialize(DraftC);
			PF_CHECK(Tests, BytesA && BytesB && BytesC);
			if (!BytesA || !BytesB || !BytesC) return;
			const uint64_t FingerprintA = PulseForgeEditor::ComputeEditorMaterialPreviewFingerprint(*BytesA);
			const uint64_t FingerprintB = PulseForgeEditor::ComputeEditorMaterialPreviewFingerprint(*BytesB);
			const uint64_t FingerprintC = PulseForgeEditor::ComputeEditorMaterialPreviewFingerprint(*BytesC);
			PF_CHECK(Tests, FingerprintA != FingerprintB && FingerprintA != FingerprintC);
			PF_CHECK(Tests, !Publication.HasPublishedFrame() && Publication.GetCandidateSlot() == 1);
			PF_CHECK(Tests, Publication.PublishCandidate(FingerprintA, 1, 1, 1, 1, true, true, 1));
			const size_t DisplayedA = Publication.GetDisplayedSlot();
			PF_CHECK(Tests, Publication.HasPublishedFrame() && Publication.IsCurrent(FingerprintA, 1) && DisplayedA == 1);

			// The production asset-refresh path invalidates freshness while retaining the displayed image.
			Publication.Invalidate(2);
			PF_CHECK(Tests, Publication.HasPublishedFrame() && Publication.GetDisplayedSlot() == DisplayedA);
			PF_CHECK(Tests, !Publication.IsCurrent(FingerprintA, 2));
			const size_t CandidateB = Publication.GetCandidateSlot();
			PF_CHECK(Tests, CandidateB != DisplayedA);
			// Pending preparation, failed rendering, and a partial render cannot replace A.
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintB, 2, 2, 2, 2, false, false, 0));
			PF_CHECK(Tests, Publication.GetDisplayedSlot() == DisplayedA && Publication.HasPublishedFrame());
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintB, 2, 2, 2, 2, true, false, 0));
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintB, 2, 2, 2, 2, true, true, 0));
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintB, 2, 2, 4, 5, true, true, 1));
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintB, 2, 3, 5, 5, true, true, 1));
			PF_CHECK(Tests, Publication.GetDisplayedSlot() == DisplayedA && Publication.HasPublishedFrame());
			PF_CHECK(Tests, Publication.PublishCandidate(FingerprintC, 2, 2, 5, 5, true, true, 1));
			PF_CHECK(Tests, Publication.GetDisplayedSlot() == CandidateB && Publication.IsCurrent(FingerprintC, 2));
			PF_CHECK(Tests, !Publication.IsCurrent(FingerprintA, 2) && !Publication.IsCurrent(FingerprintB, 2));
			const size_t DisplayedC = Publication.GetDisplayedSlot();
			PF_CHECK(Tests, Publication.IsCurrent(FingerprintC, 2) && Publication.GetDisplayedSlot() == DisplayedC);

			// Identical material bytes still require a fresh preview after referenced texture content changes.
			Publication.Invalidate(3);
			PF_CHECK(Tests, Publication.HasPublishedFrame() && Publication.GetDisplayedSlot() == DisplayedC);
			PF_CHECK(Tests, !Publication.IsCurrent(FingerprintC, 3));

			PulseForgeEditor::EditorMaterialPreviewPublication OtherMaterial;
			PF_CHECK(Tests, OtherMaterial.PublishCandidate(FingerprintA, 1, 1, 1, 1, true, true, 1));
			OtherMaterial.Invalidate(2);
			PF_CHECK(Tests, OtherMaterial.HasPublishedFrame() && OtherMaterial.GetDisplayedSlot() == 1 &&
				!OtherMaterial.IsCurrent(FingerprintA, 2));

			PulseForgeEditor::EditorMaterialPreviewPublication NewDocument;
			NewDocument.Invalidate(2);
			PF_CHECK(Tests, !NewDocument.HasPublishedFrame());
		}
		{
			using namespace PulseForge;
			PulseForgeEditor::EditorMaterialDraftStore Drafts;
			PulseForgeEditor::EditorMaterialPreviewPublication Publication;
			constexpr std::string_view Key = "project:material-preview-transaction";
			MaterialAssetDesc DraftA;
			DraftA.BaseColorFactor = { 0.2f, 0.3f, 0.4f, 1.0f };
			Drafts.PutDraft(std::string(Key), DraftA);
			const uint64_t RevisionA = Drafts.GetContentRevision(Key);
			const auto BytesA = MaterialAssetSerializer::Serialize(DraftA);
			PF_CHECK(Tests, BytesA && RevisionA != 0);
			if (!BytesA) return;
			const uint64_t FingerprintA = PulseForgeEditor::ComputeEditorMaterialPreviewFingerprint(*BytesA);
			PF_CHECK(Tests, Publication.PublishCandidate(FingerprintA, 1, 1, RevisionA, RevisionA, true, true, 1));
			const size_t DisplayedA = Publication.GetDisplayedSlot();

			// Model the production property panel: each changed value calls BeginEdit, while only its first return
			// advances the document transaction revision through MaterialDetailsDraftEdited.
			uint64_t DocumentRevision = 6;
			const MaterialAssetDesc EditStart = *Drafts.FindDraft(Key);
			Drafts.FindDraft(Key)->BaseColorFactor.r = 0.7f;
			const bool FirstChangeStartedEdit = Drafts.BeginEdit(Key, EditStart);
			if (FirstChangeStartedEdit) ++DocumentRevision;
			PF_CHECK(Tests, FirstChangeStartedEdit && DocumentRevision == 7);
			const uint64_t RevisionB = Drafts.GetContentRevision(Key);
			const auto BytesB = MaterialAssetSerializer::Serialize(*Drafts.FindDraft(Key));
			PF_CHECK(Tests, BytesB);
			if (!BytesB) return;
			const uint64_t FingerprintB = PulseForgeEditor::ComputeEditorMaterialPreviewFingerprint(*BytesB);
			PF_CHECK(Tests, FingerprintA != FingerprintB && RevisionB != RevisionA);

			// Preparation captures B. A later slider update in the same drag keeps the transaction revision at 7.
			Drafts.FindDraft(Key)->BaseColorFactor.r = 0.85f;
			const bool LaterChangeStartedEdit = Drafts.BeginEdit(Key, EditStart);
			if (LaterChangeStartedEdit) ++DocumentRevision;
			const uint64_t RevisionC = Drafts.GetContentRevision(Key);
			const auto BytesC = MaterialAssetSerializer::Serialize(*Drafts.FindDraft(Key));
			PF_CHECK(Tests, BytesC && !LaterChangeStartedEdit && DocumentRevision == 7);
			if (!BytesC) return;
			const uint64_t FingerprintC = PulseForgeEditor::ComputeEditorMaterialPreviewFingerprint(*BytesC);
			PF_CHECK(Tests, FingerprintC != FingerprintB && RevisionC != RevisionB);
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintB, 1, 1, RevisionB, RevisionC, true, true, 1));
			PF_CHECK(Tests, Publication.GetDisplayedSlot() == DisplayedA && Publication.HasPublishedFrame());
			PF_CHECK(Tests, Publication.PublishCandidate(FingerprintC, 1, 1, RevisionC, RevisionC, true, true, 1));
			const size_t DisplayedC = Publication.GetDisplayedSlot();
			PF_CHECK(Tests, DisplayedC != DisplayedA && Publication.IsCurrent(FingerprintC, 1));
			PF_CHECK(Tests, !Publication.PublishCandidate(FingerprintC, 1, 1, RevisionC, RevisionC + 1, true, true, 1));
			PF_CHECK(Tests, Publication.GetDisplayedSlot() == DisplayedC);

			// One completed drag is still one history step, and history replacement invalidates a prepared frame.
			Drafts.FinalizePendingEdits(false);
			const auto Undo = Drafts.ApplyHistoryStep(Key, false);
			PF_CHECK(Tests, Undo && *Undo && Drafts.FindDraft(Key)->BaseColorFactor.r == DraftA.BaseColorFactor.r);
			const uint64_t UndoRevision = Drafts.GetContentRevision(Key);
			PF_CHECK(Tests, UndoRevision != RevisionC &&
				!Publication.PublishCandidate(FingerprintC, 1, 1, RevisionC, UndoRevision, true, true, 1));
			const auto Redo = Drafts.ApplyHistoryStep(Key, true);
			PF_CHECK(Tests, Redo && *Redo && Drafts.FindDraft(Key)->BaseColorFactor.r == 0.85f);
			const uint64_t RedoRevision = Drafts.GetContentRevision(Key);
			PF_CHECK(Tests, RedoRevision != UndoRevision &&
				!Publication.PublishCandidate(FingerprintC, 1, 1, RevisionC, RedoRevision, true, true, 1));
			Drafts.ReplaceDraft(std::string(Key), DraftA);
			const uint64_t ReloadRevision = Drafts.GetContentRevision(Key);
			PF_CHECK(Tests, ReloadRevision != RedoRevision &&
				!Publication.PublishCandidate(FingerprintC, 1, 1, RedoRevision, ReloadRevision, true, true, 1));
		}
		const auto Identifier = PulseForge::UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;

		std::error_code Error;
		const std::filesystem::path Root =
			std::filesystem::temp_directory_path(Error) / ("PulseForgeMaterialDraft-" + Identifier->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} RemoveProject{ Root };

		auto ProjectResult = Project::Create(Root / "Drafts.pfproj", "Material Draft Test");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		Project& ProjectValue = *ProjectResult;
		std::filesystem::create_directories(Root / "Assets" / "Materials", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		MaterialAssetDesc Initial;
		Initial.BaseColorFactor = { 0.2f, 0.3f, 0.4f, 1.0f };
		auto Material = MaterialAssetService::Create(
			ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(), "Assets/Materials/draft.material", Initial);
		PF_CHECK(Tests, Material.has_value());
		if (!Material) return;

		PulseForgeEditor::EditorDocumentManager Documents;
		const PulseForgeEditor::EditorDocumentIdentity Identity{ ProjectValue.GetDescription().ID, Material->ID,
			PulseForgeEditor::EditorDocumentKind::Material };
		auto Opened = Documents.Open(
			Identity, Material->ProjectRelativePath, "draft.material", PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, Opened.has_value());
		if (!Opened) return;

		PulseForgeEditor::EditorMaterialDraftController Drafts;
		auto Loaded = Drafts.LoadDraft(ProjectValue, Material->ID);
		PF_CHECK(Tests, Loaded.has_value());
		if (!Loaded) return;
		PF_CHECK(Tests, Drafts.GetContentRevision(ProjectValue, Material->ID) != 0);
		struct MaterialActions final : PulseForgeEditor::EditorMaterialDetailsActionSink
		{
			Project& ProjectValue;
			PulseForgeEditor::EditorMaterialDraftController& Drafts;
			PulseForgeEditor::EditorDocumentManager& Documents;
			MaterialActions(Project& ProjectInput, PulseForgeEditor::EditorMaterialDraftController& DraftsInput,
				PulseForgeEditor::EditorDocumentManager& DocumentsInput)
				: ProjectValue(ProjectInput), Drafts(DraftsInput), Documents(DocumentsInput) {}
			bool SaveMaterialFromDetails(const AssetID& Asset) override
			{
				return Drafts.Save(ProjectValue, Asset, Documents).has_value();
			}
			bool RevertMaterialFromDetails(const AssetID& Asset) override
			{
				return Drafts.Reload(ProjectValue, Asset, Documents).has_value();
			}
			void MaterialDetailsDraftEdited(const PulseForge::UUID& ProjectID, const AssetID& Asset) override
			{
				const PulseForgeEditor::EditorDocumentIdentity Document{ ProjectID, Asset,
					PulseForgeEditor::EditorDocumentKind::Material };
				if (Documents.Find(Document)) (void)Documents.MarkEdited(Document);
			}
		} Actions{ ProjectValue, Drafts, Documents };
		PulseForgeEditor::EditorDocumentEditorRegistry EditorRegistry;
		PulseForgeEditor::EditorMaterialDetailsPanel MaterialPanel;
		PulseForgeEditor::EditorAssetSelector AssetSelector;
		PulseForgeEditor::EditorStyle Style;
		PulseForgeEditor::EditorMaterialDocumentEditor MaterialEditor(Documents, EditorRegistry, Drafts, MaterialPanel,
			AssetSelector, Style, Actions, [&ProjectValue] { return &ProjectValue; },
			[&ProjectValue] { return ProjectValue.GetAssetRegistry().GetAssets(); });
		PF_CHECK(Tests, MaterialEditor.Register().has_value());
		auto* MaterialSession = Documents.Find(Identity);
		const PulseForgeEditor::EditorDocumentEditorContext MaterialContext{ Identity, ProjectValue.GetRootPath(),
			Material->ProjectRelativePath, "draft.material", &ProjectValue.GetAssetRegistry() };
		PF_CHECK(Tests, MaterialSession && MaterialEditor.Open(*MaterialSession, MaterialContext));
		PF_CHECK(Tests, MaterialSession && MaterialSession->EditorState != nullptr);
		PF_CHECK(Tests, Drafts.FindDraft(ProjectValue, Material->ID) &&
			std::abs(Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r - Initial.BaseColorFactor.r) < 1.0e-5f);
		const auto UnchangedSave = Drafts.Save(ProjectValue, Material->ID, Documents);
		PF_CHECK(Tests, UnchangedSave.has_value());
		PF_CHECK(Tests, Drafts.FindDraft(ProjectValue, Material->ID) != nullptr);
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID));
		auto SecondMaterial = MaterialAssetService::Create(ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(),
			"Assets/Materials/Independent.material", Initial);
		PF_CHECK(Tests, SecondMaterial.has_value());
		if (!SecondMaterial) return;
		const PulseForgeEditor::EditorDocumentIdentity SecondIdentity{ ProjectValue.GetDescription().ID, SecondMaterial->ID,
			PulseForgeEditor::EditorDocumentKind::Material };
		auto SecondSessionResult = Documents.Open(SecondIdentity, SecondMaterial->ProjectRelativePath, "Independent.material",
			PulseForgeEditor::EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, SecondSessionResult.has_value());
		if (!SecondSessionResult) return;
		auto* SecondSession = Documents.Find(SecondIdentity);
		PF_CHECK(Tests, SecondSession != nullptr);
		if (!SecondSession) return;
		const PulseForgeEditor::EditorDocumentEditorContext SecondContext{ SecondIdentity, ProjectValue.GetRootPath(),
			SecondMaterial->ProjectRelativePath, "Independent.material", &ProjectValue.GetAssetRegistry() };
		PF_CHECK(Tests, MaterialEditor.Open(*SecondSession, SecondContext));
		const auto FirstStart = *Drafts.FindDraft(ProjectValue, Material->ID);
		const auto SecondStart = *Drafts.FindDraft(ProjectValue, SecondMaterial->ID);
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, FirstStart));
		Drafts.FindDraft(ProjectValue, Material->ID)->MetallicFactor = 0.35f;
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, SecondMaterial->ID, SecondStart));
		Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->RoughnessFactor = 0.25f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Documents.MarkEdited(Identity));
		PF_CHECK(Tests, Documents.MarkEdited(SecondIdentity));
		const uint64_t SecondGeneration = SecondSession->SessionGeneration;
		const uint64_t SecondRevision = SecondSession->Revision;
		const std::string NativeHostID = SecondSession->HostID;
		const auto Transferred = Documents.Transfer(SecondIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID, 1);
		PF_CHECK(Tests, Transferred && Transferred->Changed);
		PF_CHECK(Tests, SecondSession->SessionGeneration == SecondGeneration && SecondSession->HostID ==
			PulseForgeEditor::EditorDocumentManager::MainHostID && SecondSession->Revision == SecondRevision);
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, SecondMaterial->ID) &&
			std::abs(Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->RoughnessFactor - 0.25f) < 1.0e-5f &&
			Drafts.CanUndo(ProjectValue, SecondMaterial->ID));
		PF_CHECK(Tests, EditorRegistry.DispatchFocusedCommand(*SecondSession, PulseForgeEditor::EditorDocumentCommand::Undo));
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->RoughnessFactor - SecondStart.RoughnessFactor) < 1.0e-5f);
		PF_CHECK(Tests, EditorRegistry.DispatchFocusedCommand(*SecondSession, PulseForgeEditor::EditorDocumentCommand::Redo));
		PF_CHECK(Tests, SecondSession->SessionGeneration == SecondGeneration && SecondSession->HostID != NativeHostID &&
			std::abs(Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->RoughnessFactor - 0.25f) < 1.0e-5f);
		PF_CHECK(Tests, EditorRegistry.Save(*MaterialSession));
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, SecondMaterial->ID) && SecondSession->IsDirty());
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->RoughnessFactor - 0.25f) < 1.0e-5f);
		PF_CHECK(Tests, EditorRegistry.Save(*SecondSession));
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, SecondMaterial->ID) && !SecondSession->IsDirty());

		auto UnknownAsset = PulseForge::UUID::Generate();
		PF_CHECK(Tests, UnknownAsset.has_value());
		if (UnknownAsset) PF_CHECK(Tests, !Drafts.Save(ProjectValue, *UnknownAsset, Documents));

		const std::filesystem::path WrongTypePath = Root / "Assets" / "Materials" / "not-a-material.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "not a material";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto WrongTypeRecord = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeRecord.has_value());
		PF_CHECK(Tests, ProjectValue.GetAssetRegistry().Rebuild(Root).has_value());
		if (WrongTypeRecord) PF_CHECK(Tests, !Drafts.Save(ProjectValue, WrongTypeRecord->ID, Documents));

		auto RemovedAsset = MaterialAssetService::Create(
			ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(), "Assets/Materials/removed.material", Initial);
		PF_CHECK(Tests, RemovedAsset.has_value());
		if (!RemovedAsset) return;
		auto RemovedDraft = Drafts.LoadDraft(ProjectValue, RemovedAsset->ID);
		PF_CHECK(Tests, RemovedDraft.has_value());
		const PulseForgeEditor::EditorDocumentIdentity RemovedIdentity{ ProjectValue.GetDescription().ID, RemovedAsset->ID,
			PulseForgeEditor::EditorDocumentKind::Material };
		PF_CHECK(Tests, Documents
							.Open(RemovedIdentity, RemovedAsset->ProjectRelativePath, "removed.material",
								PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip)
							.has_value());
		const std::filesystem::path RemovedPath = Root / RemovedAsset->ProjectRelativePath;
		std::filesystem::remove(RemovedPath, Error);
		PF_CHECK(Tests, !Error);
		const std::filesystem::path RemovedSidecar = AssetMetadataSerializer::GetSidecarPath(RemovedPath);
		std::filesystem::remove(RemovedSidecar, Error);
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, ProjectValue.GetAssetRegistry().Rebuild(Root).has_value());
		PF_CHECK(Tests, !Drafts.Save(ProjectValue, RemovedAsset->ID, Documents));
		PF_CHECK(Tests, Drafts.FindDraft(ProjectValue, RemovedAsset->ID) != nullptr);
		PF_CHECK(Tests, Documents.Find(RemovedIdentity) != nullptr);

		const MaterialAssetDesc EditStart = Loaded->get();
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, EditStart));
		Loaded->get().BaseColorFactor.r = 0.85f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, Documents.MarkEdited(Identity));
		PF_CHECK(Tests, Drafts.CanUndo(ProjectValue, Material->ID));

		const auto Undone = Drafts.ApplyHistoryStep(ProjectValue, Material->ID, false, Documents);
		PF_CHECK(Tests, Undone.has_value() && *Undone);
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, !Documents.Find(Identity)->IsDirty());
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r - 0.2f) < 1.0e-5f);
		const uint64_t UnchangedSourceRevision = Drafts.GetContentRevision(ProjectValue, Material->ID);
		Drafts.ReconcileProjectSources(ProjectValue);
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID) && Drafts.CanRedo(ProjectValue, Material->ID));
		PF_CHECK(Tests, Drafts.GetContentRevision(ProjectValue, Material->ID) == UnchangedSourceRevision);

		const auto Redone = Drafts.ApplyHistoryStep(ProjectValue, Material->ID, true, Documents);
		PF_CHECK(Tests, Redone.has_value() && *Redone);
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, Documents.Find(Identity)->IsDirty());
		auto RenamedMaterial = AssetOperations::Move(ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(), Material->ID,
			"Assets/Materials/Renamed-draft.material");
		PF_CHECK(Tests, RenamedMaterial && RenamedMaterial->ID == Material->ID);
		if (RenamedMaterial)
		{
			const uint64_t DirtyContentRevision = Drafts.GetContentRevision(ProjectValue, Material->ID);
			*Material = *RenamedMaterial;
			Drafts.ReconcileProjectSources(ProjectValue);
			PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID) && Drafts.CanUndo(ProjectValue, Material->ID));
			PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r - 0.85f) < 1.0e-5f);
			PF_CHECK(Tests, Drafts.GetContentRevision(ProjectValue, Material->ID) == DirtyContentRevision);
		}
		const uint64_t DirtyMaterialRevision = Drafts.GetContentRevision(ProjectValue, Material->ID);
		const uint64_t CleanMaterialRevision = Drafts.GetContentRevision(ProjectValue, SecondMaterial->ID);
		MaterialAssetDesc ExternalEdit = *Drafts.FindDraft(ProjectValue, Material->ID);
		ExternalEdit.BaseColorFactor = { 0.1f, 0.2f, 0.9f, 1.0f };
		PF_CHECK(Tests, MaterialAssetSerializer::SaveToFile(ExternalEdit, Root / Material->ProjectRelativePath).has_value());
		const auto ExternalBytes = MaterialAssetSerializer::Serialize(ExternalEdit);
		PF_CHECK(Tests, ExternalBytes.has_value());
		MaterialAssetDesc CleanExternalEdit = *Drafts.FindDraft(ProjectValue, SecondMaterial->ID);
		CleanExternalEdit.BaseColorFactor = { 0.65f, 0.55f, 0.45f, 1.0f };
		PF_CHECK(Tests, MaterialAssetSerializer::SaveToFile(CleanExternalEdit, Root / SecondMaterial->ProjectRelativePath).has_value());
		PF_CHECK(Tests, ProjectValue.GetAssetRegistry().Rebuild(Root).has_value());
		Drafts.ReconcileProjectSources(ProjectValue);
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, SecondMaterial->ID));
		PF_CHECK(Tests, Drafts.GetContentRevision(ProjectValue, SecondMaterial->ID) != CleanMaterialRevision);
		PF_CHECK(Tests, Drafts.GetContentRevision(ProjectValue, Material->ID) == DirtyMaterialRevision);
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->BaseColorFactor.r - 0.65f) < 1.0e-5f);
		PF_CHECK(Tests, !Drafts.FindError(ProjectValue, SecondMaterial->ID));
		const auto RefreshedCleanSave = Drafts.Save(ProjectValue, SecondMaterial->ID, Documents);
		PF_CHECK(Tests, RefreshedCleanSave.has_value());
		const MaterialAssetDesc CleanEditStart = *Drafts.FindDraft(ProjectValue, SecondMaterial->ID);
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, SecondMaterial->ID, CleanEditStart));
		Drafts.FindDraft(ProjectValue, SecondMaterial->ID)->MetallicFactor = 0.42f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Documents.MarkEdited(SecondIdentity));
		const auto CleanAfterRefreshSave = Drafts.Save(ProjectValue, SecondMaterial->ID, Documents);
		PF_CHECK(Tests, CleanAfterRefreshSave.has_value() && !Drafts.IsDirty(ProjectValue, SecondMaterial->ID));
		PF_CHECK(Tests, Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r == 0.85f);
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, Documents.Find(Identity)->IsDirty());
		PF_CHECK(Tests, Drafts.FindError(ProjectValue, Material->ID) != nullptr);
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r - 0.85f) < 1.0e-5f);
		const bool ConflictedSave = MaterialSession && !EditorRegistry.Save(*MaterialSession);
		PF_CHECK(Tests, ConflictedSave);
		PF_CHECK(Tests, MaterialSession && MaterialSession->IsDirty());
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, Documents.Find(Identity)->IsDirty());
		{
			std::ifstream Input(Root / Material->ProjectRelativePath, std::ios::binary);
			const std::string OnDisk((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
			PF_CHECK(Tests, ExternalBytes && OnDisk == *ExternalBytes);
		}
		auto ConflictSource = MaterialAssetService::Load(Material->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, ConflictSource && std::abs(ConflictSource->BaseColorFactor.r - 0.1f) < 1.0e-5f);
		PF_CHECK(Tests, MaterialSession && EditorRegistry.Discard(*MaterialSession));
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r - 0.1f) < 1.0e-5f);
		const auto DiscardBaseline = MaterialAssetService::LoadSourceSnapshot(
			Material->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, DiscardBaseline && Drafts.Save(ProjectValue, Material->ID, Documents));
		const MaterialAssetDesc RetryStart = *Drafts.FindDraft(ProjectValue, Material->ID);
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, RetryStart));
		Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.r = 0.85f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Documents.MarkEdited(Identity));
		const bool Saved = MaterialSession && EditorRegistry.Save(*MaterialSession);
		PF_CHECK(Tests, Saved);
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, !Documents.Find(Identity)->IsDirty());
		auto Persisted = MaterialAssetService::Load(Material->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, Persisted && std::abs(Persisted->BaseColorFactor.r - 0.85f) < 1.0e-5f);

		const MaterialAssetDesc SavedState = *Drafts.FindDraft(ProjectValue, Material->ID);
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, SavedState));
		Drafts.FindDraft(ProjectValue, Material->ID)->MetallicFactor = 0.7f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Documents.MarkEdited(Identity));
		PF_CHECK(Tests, Drafts.Reload(ProjectValue, Material->ID, Documents).has_value());
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, !Documents.Find(Identity)->IsDirty());
		PF_CHECK(Tests, Drafts.FindDraft(ProjectValue, Material->ID)->MetallicFactor == SavedState.MetallicFactor);

		const MaterialAssetDesc RevertStart = *Drafts.FindDraft(ProjectValue, Material->ID);
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, RevertStart));
		Drafts.FindDraft(ProjectValue, Material->ID)->RoughnessFactor = 0.15f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Documents.MarkEdited(Identity));
		const auto MaterialPath = Root / Material->ProjectRelativePath;
#if defined(_WIN32)
		const auto LastSavedBytes = MaterialAssetSerializer::Serialize(SavedState);
		HANDLE LockedMaterial = CreateFileW(MaterialPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		PF_CHECK(Tests, LockedMaterial != INVALID_HANDLE_VALUE);
		if (LockedMaterial != INVALID_HANDLE_VALUE)
		{
			const auto FailedReplacement = Drafts.Save(ProjectValue, Material->ID, Documents);
			PF_CHECK(Tests, !FailedReplacement);
			PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID) && Documents.Find(Identity)->IsDirty());
			{
				std::ifstream Input(MaterialPath, std::ios::binary);
				const std::string UnchangedBytes((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
				PF_CHECK(Tests, LastSavedBytes && UnchangedBytes == *LastSavedBytes);
			}
			CloseHandle(LockedMaterial);
			const auto RetriedReplacement = Drafts.Save(ProjectValue, Material->ID, Documents);
			PF_CHECK(Tests, RetriedReplacement.has_value());
			PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID) && !Documents.Find(Identity)->IsDirty());
		}
#endif
		const MaterialAssetDesc MissingSourceEditStart = *Drafts.FindDraft(ProjectValue, Material->ID);
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, MissingSourceEditStart));
		Drafts.FindDraft(ProjectValue, Material->ID)->RoughnessFactor = 0.35f;
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Documents.MarkEdited(Identity));
		const auto BaselineBeforeMissingSource = MaterialAssetService::LoadSourceSnapshot(
			Material->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, BaselineBeforeMissingSource.has_value());
		std::filesystem::remove(MaterialPath, Error);
		PF_CHECK(Tests, !Error);
		std::filesystem::create_directory(MaterialPath, Error);
		PF_CHECK(Tests, !Error);
		const auto FailedSave = Drafts.Save(ProjectValue, Material->ID, Documents);
		PF_CHECK(Tests, !FailedSave);
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID));
		PF_CHECK(Tests, Documents.Find(Identity)->IsDirty());
		PF_CHECK(Tests, !Drafts.Reload(ProjectValue, Material->ID, Documents));
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID));
		const auto DirtyAssets = Drafts.GetDirtyAssets(ProjectValue);
		PF_CHECK(Tests, DirtyAssets.size() == 1 && DirtyAssets.front() == Material->ID);
		std::filesystem::remove_all(MaterialPath, Error);
		PF_CHECK(Tests, !Error);
		if (BaselineBeforeMissingSource)
		{
			std::ofstream Output(MaterialPath, std::ios::binary | std::ios::trunc);
			Output.write(BaselineBeforeMissingSource->SourceBytes.data(),
				static_cast<std::streamsize>(BaselineBeforeMissingSource->SourceBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto MissingSourceRetry = Drafts.Save(ProjectValue, Material->ID, Documents);
		PF_CHECK(Tests, MissingSourceRetry.has_value());
		PF_CHECK(Tests, !Drafts.IsDirty(ProjectValue, Material->ID) && !Documents.Find(Identity)->IsDirty());
	}
	void TestProjectFiles(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = PulseForge::UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const std::filesystem::path Root = TemporaryDirectory / ("PulseForgeProject-" + ProjectIdentifier->ToString());
		const std::filesystem::path RelocatedRoot = Root.parent_path() / (Root.filename().string() + "-relocated");
		struct ProjectCleanup
		{
			std::filesystem::path First;
			std::filesystem::path Second;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(First, Error);
				Error.clear();
				std::filesystem::remove_all(Second, Error);
			}
		} Cleanup{ Root, RelocatedRoot };

		const std::filesystem::path ProjectFile = Root / "Example.pfproj";
		auto Created = Project::Create(ProjectFile, "Example Project");
		PF_CHECK(Tests, Created.has_value());
		if (!Created) return;
		std::stop_source CancelledOpen;
		CancelledOpen.request_stop();
		const auto CancelledProjectOpen = Project::Open(ProjectFile, nullptr, CancelledOpen.get_token());
		PF_CHECK(Tests, !CancelledProjectOpen && CancelledProjectOpen.error().Code == ProjectErrorCode::Cancelled);
		PF_CHECK(Tests, Created->GetDescription().Name == "Example Project");
		PF_CHECK(Tests, !Created->GetDescription().StartScene.has_value());
		PF_CHECK(Tests, std::filesystem::is_directory(Root / "Assets"));
		PF_CHECK(Tests, !Project::Create(ProjectFile, "Duplicate Project"));
		PF_CHECK(Tests, !ProjectSerializer::Deserialize("{broken"));
		for (const std::string_view UnsupportedVersion : { "0", "3", "99" })
		{
			const std::string UnsupportedProject = "{\"format\":\"PulseForgeProject\",\"version\":" + std::string(UnsupportedVersion) + "}";
			const auto Result = ProjectSerializer::Deserialize(UnsupportedProject);
			PF_CHECK(Tests, !Result && Result.error().Code == ProjectErrorCode::UnsupportedVersion);
		}
		for (const std::string_view InvalidVersion : { "1.5", "\"1\"", "null" })
		{
			const std::string InvalidProject = "{\"format\":\"PulseForgeProject\",\"version\":" + std::string(InvalidVersion) + "}";
			const auto Result = ProjectSerializer::Deserialize(InvalidProject);
			PF_CHECK(Tests, !Result && Result.error().Code == ProjectErrorCode::InvalidDocument);
		}
		const auto MissingProjectVersion = ProjectSerializer::Deserialize("{\"format\":\"PulseForgeProject\"}");
		PF_CHECK(Tests, !MissingProjectVersion && MissingProjectVersion.error().Code == ProjectErrorCode::InvalidDocument);
		const auto SerializedDescription = ProjectSerializer::Serialize(Created->GetDescription());
		PF_CHECK(Tests, SerializedDescription.has_value());
		if (SerializedDescription)
		{
			const auto ProjectV1 = ProjectSerializer::Deserialize(*SerializedDescription);
			PF_CHECK(Tests, ProjectV1 && ProjectV1->ID == Created->GetDescription().ID &&
								ProjectV1->Name == Created->GetDescription().Name && !ProjectV1->StartScene);
			auto ProjectWithUnknownField = nlohmann::json::parse(*SerializedDescription);
			ProjectWithUnknownField["futureProjectData"] = nlohmann::json::object({ { "keep", true } });
			PF_CHECK(Tests, !ProjectSerializer::Deserialize(ProjectWithUnknownField.dump()));
			std::string ExactBoundaryProject = *SerializedDescription;
			ExactBoundaryProject.resize(ProjectSerializer::MaximumDocumentBytes, ' ');
			PF_CHECK(Tests, ProjectSerializer::Deserialize(ExactBoundaryProject).has_value());
			ExactBoundaryProject.push_back(' ');
			const auto OversizedProject = ProjectSerializer::Deserialize(ExactBoundaryProject);
			PF_CHECK(Tests, !OversizedProject && OversizedProject.error().Code == ProjectErrorCode::DocumentTooLarge);
		}

		const std::filesystem::path OversizedProjectFile = Root / "Oversized.pfproj";
		std::ofstream(OversizedProjectFile, std::ios::binary | std::ios::trunc).close();
		std::filesystem::resize_file(OversizedProjectFile, ProjectSerializer::MaximumDocumentBytes + 1, FileError);
		PF_CHECK(Tests, !FileError);
		const auto OversizedProjectLoad = Project::Open(OversizedProjectFile);
		PF_CHECK(Tests, !OversizedProjectLoad && OversizedProjectLoad.error().Code == ProjectErrorCode::FileReadFailed);
		PF_CHECK(Tests, OversizedProjectLoad.error().Message.find("1 MiB") != std::string::npos);

		const std::filesystem::path ScenesDirectory = Root / "Assets" / "Scenes";
		std::filesystem::create_directories(ScenesDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		const std::filesystem::path ScenePath = ScenesDirectory / "start.SCENE";
		{
			std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
			Output << "{}";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto SceneMetadata = AssetMetadataSerializer::CreateForNewAsset(ScenePath);
		PF_CHECK(Tests, SceneMetadata.has_value());
		if (!SceneMetadata) return;
		PF_CHECK(Tests, Created->GetAssetRegistry().Rebuild(Root).has_value());
		PF_CHECK(Tests, Created->SetStartScene(SceneMetadata->ID).has_value());
		PF_CHECK(Tests, Created->GetDescription().StartScene == SceneMetadata->ID);

		const std::filesystem::path WrongTypePath = Root / "Assets" / "not-a-scene.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "asset";
		}
		const auto WrongTypeMetadata = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeMetadata.has_value());
		PF_CHECK(Tests, Created->GetAssetRegistry().Rebuild(Root).has_value());
		if (WrongTypeMetadata)
		{
			const auto WrongType = Created->SetStartScene(WrongTypeMetadata->ID);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == ProjectErrorCode::StartSceneWrongType);
			PF_CHECK(Tests, Created->GetDescription().StartScene == SceneMetadata->ID);
		}

		Created = Project::Open(ProjectFile);
		PF_CHECK(Tests, Created && Created->GetDescription().StartScene == SceneMetadata->ID);
		if (!Created) return;
		std::filesystem::rename(Root, RelocatedRoot, FileError);
		PF_CHECK(Tests, !FileError);
		const auto RelocatedProject = Project::Open(RelocatedRoot / "Example.pfproj");
		PF_CHECK(Tests, RelocatedProject.has_value());
		PF_CHECK(Tests, RelocatedProject && RelocatedProject->GetAssetRegistry().Find(SceneMetadata->ID).has_value());
		PF_CHECK(Tests, RelocatedProject && RelocatedProject->GetRootPath() == RelocatedRoot);
	}
} // namespace

void RunEditorMaterialProjectTests(TestRunner& Tests)
{
	TestMaterialAssets(Tests);
	TestMaterialDraftController(Tests);
	TestProjectFiles(Tests);
}
