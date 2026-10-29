#include "TestAssetSupport.h"

#include "Assets/PrefabInstanceReconciler.h"
#include "Assets/AssetImportProvenance.h"

#include <span>
#include <utility>

namespace
{
	using namespace PulseForge;

	constexpr AssetID FirstPrefabAsset{ 0x8100000000000000ull, 81 };
	constexpr AssetID SecondPrefabAsset{ 0x8200000000000000ull, 82 };
	constexpr UUID SourceRootKey{ 0x8300000000000000ull, 83 };
	constexpr UUID SourceChildKey{ 0x8400000000000000ull, 84 };
	constexpr UUID LocalEntityKey{ 0x8500000000000000ull, 85 };
	constexpr UUID AddedSourceEntityKey{ 0x8600000000000000ull, 86 };
	constexpr UUID NestedSourceRootKey{ 0x8700000000000000ull, 87 };
	constexpr AssetID SourceMeshAsset{ 0x8800000000000000ull, 88 };
	constexpr AssetID SourceMaterialAsset{ 0x8900000000000000ull, 89 };
	constexpr AssetID FirstOverrideMaterial{ 0x8a00000000000000ull, 90 };
	constexpr AssetID SecondOverrideMaterial{ 0x8b00000000000000ull, 91 };

	std::string CanonicalPrefabFingerprint(const PrefabSourceDocument& Source)
	{
		auto Serialized = PrefabSerializer::Serialize(Source);
		if (!Serialized) return {};
		const auto Bytes = std::as_bytes(std::span(Serialized->data(), Serialized->size()));
		const AssetContentFingerprint Fingerprint = ComputeAssetContentFingerprint(Bytes);
		return Fingerprint.Algorithm + ":" + Fingerprint.DigestHex;
	}

	class LinkedPrefabFixture final
	{
	public:
		LinkedPrefabFixture()
		{
			std::error_code Error;
			const std::filesystem::path TemporaryRoot = std::filesystem::temp_directory_path(Error);
			if (Error) return;
			const auto Identifier = UUID::Generate();
			if (!Identifier) return;
			ProjectRoot = TemporaryRoot / ("PulseForge-LinkedPrefab-" + Identifier->ToString());
			std::filesystem::create_directories(ProjectRoot / "Assets" / "Prefabs", Error);
			if (Error) return;
			if (!Registry.Rebuild(ProjectRoot)) return;

			Scene SourceScene;
			const auto Root = SourceScene.CreateEntityWithUUID(SourceRootKey, "Source root");
			const auto Child = SourceScene.CreateEntityWithUUID(SourceChildKey, "Source child");
			if (!Root || !Child) return;
			TransformComponent RootTransform;
			RootTransform.Translation = { 2.0f, 3.0f, 4.0f };
			if (!Root->SetTransform(RootTransform)) return;
			TransformComponent ChildTransform;
			ChildTransform.Translation = { 1.0f, 0.0f, 0.0f };
			if (!Child->SetTransform(ChildTransform) || !Child->SetParent(*Root)) return;
			if (!Child->SetMeshRenderer(MeshRendererComponent{ SourceMeshAsset, SourceMaterialAsset })) return;

			const auto Created = PrefabAssetService::Create(
				Registry, ProjectRoot, "Assets/Prefabs/linked.prefab", SourceScene, *Root);
			if (!Created) return;
			Asset = Created->ID;
			Valid = true;
		}

		~LinkedPrefabFixture()
		{
			std::error_code Error;
			if (!ProjectRoot.empty()) std::filesystem::remove_all(ProjectRoot, Error);
		}

		std::filesystem::path ProjectRoot;
		AssetRegistry Registry;
		AssetID Asset;
		bool Valid = false;
	};

	[[nodiscard]] std::optional<UUID> FindMappedEntity(const Entity& LinkedRoot, const UUID& SourceKey)
	{
		const auto Metadata = LinkedRoot.GetPrefabInstance();
		if (!Metadata || !Metadata->has_value()) return std::nullopt;
		const auto Mapping = std::find_if((**Metadata).SourceEntities.begin(), (**Metadata).SourceEntities.end(),
			[&](const PrefabEntityMapping& Entry) { return Entry.SourceKey == SourceKey; });
		if (Mapping == (**Metadata).SourceEntities.end()) return std::nullopt;
		return Mapping->SceneEntity;
	}

	[[nodiscard]] bool HasOverride(const Entity& LinkedRoot, const PrefabEntityAddress& Address,
		std::string_view ComponentID, std::string_view PropertyID)
	{
		const auto Metadata = LinkedRoot.GetPrefabInstance();
		if (!Metadata || !Metadata->has_value()) return false;
		return std::any_of((**Metadata).Overrides.begin(), (**Metadata).Overrides.end(),
			[&](const PrefabOverrideRecord& Override)
			{
				return Override.Entity == Address && Override.ComponentID == ComponentID && Override.PropertyID == PropertyID;
			});
	}

	[[nodiscard]] PrefabInstanceComponent MakeInstanceMetadata(
		const UUID& InstanceID,
		const AssetID& SourceAsset,
		const UUID& RootEntity,
		const UUID& ChildEntity,
		const std::optional<UUID>& LocalEntity = {})
	{
		PrefabInstanceComponent Instance;
		Instance.InstanceID = InstanceID;
		Instance.SourceAsset = SourceAsset;
		Instance.SourceRootKey = SourceRootKey;
		Instance.AppliedSourceFingerprint = "source-fingerprint-17";
		Instance.SourceEntities = { { SourceRootKey, RootEntity }, { SourceChildKey, ChildEntity } };
		if (LocalEntity) Instance.LocalEntities.push_back({ LocalEntityKey, *LocalEntity });
		return Instance;
	}

	[[nodiscard]] nlohmann::json* FindSerializedEntity(nlohmann::json& Document, const UUID& Identifier)
	{
		if (!Document.contains("entities") || !Document["entities"].is_array()) return nullptr;
		for (nlohmann::json& Entity : Document["entities"])
			if (Entity.is_object() && Entity.value("uuid", std::string{}) == Identifier.ToString()) return &Entity;
		return nullptr;
	}

	void TestPrefabInstanceMetadataRoundTripAndValidation(TestRunner& Tests)
	{
		Scene AuthoredScene;
		constexpr UUID RootEntityID{ 0x9100000000000000ull, 91 };
		constexpr UUID ChildEntityID{ 0x9200000000000000ull, 92 };
		constexpr UUID LocalEntityID{ 0x9300000000000000ull, 93 };
		const auto Root = AuthoredScene.CreateEntityWithUUID(RootEntityID, "Placed root");
		const auto Child = AuthoredScene.CreateEntityWithUUID(ChildEntityID, "Renamed source child");
		const auto Local = AuthoredScene.CreateEntityWithUUID(LocalEntityID, "Instance local child");
		PF_CHECK(Tests, Root.has_value() && Child.has_value() && Local.has_value());
		if (!Root || !Child || !Local) return;

		TransformComponent RootTransform;
		RootTransform.Translation = { 12.0f, 3.0f, -7.0f };
		PF_CHECK(Tests, Root->SetTransform(RootTransform).has_value());
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Local->SetParent(*Root).has_value());

		PrefabInstanceComponent Instance = MakeInstanceMetadata(
			UUID{ 0x9400000000000000ull, 94 }, FirstPrefabAsset, RootEntityID, ChildEntityID, LocalEntityID);
		PrefabOverrideRecord RenameOverride;
		RenameOverride.Entity = { SourceChildKey, false };
		RenameOverride.Operation = PrefabOverrideOperation::SetProperty;
		RenameOverride.ComponentID = "tag";
		RenameOverride.PropertyID = "name";
		RenameOverride.Value = std::string("Local child label");
		Instance.Overrides.push_back(RenameOverride);
		Instance.Conflicts.push_back({
			{ SourceChildKey, false }, PrefabConflictKind::MissingProperty, "transform", "scale", "Source property removed" });
		PrefabInstanceComponent DuplicateMapping = Instance;
		DuplicateMapping.SourceEntities.push_back({ SourceChildKey, UUID{ 0x9600000000000000ull, 96 } });
		const auto RejectedMapping = Root->SetPrefabInstance(DuplicateMapping);
		PF_CHECK(Tests, !RejectedMapping && RejectedMapping.error().Code == SceneErrorCode::InvalidPrefabInstance);

		PrefabInstanceComponent DuplicatePropertyOverride = Instance;
		PrefabOverrideRecord ConflictingPropertyOverride = RenameOverride;
		ConflictingPropertyOverride.Value = std::string("Last writer wins must be rejected");
		DuplicatePropertyOverride.Overrides.push_back(ConflictingPropertyOverride);
		PF_CHECK(Tests, !DuplicatePropertyOverride.Validate(RootEntityID));
		PF_CHECK(Tests, !Root->SetPrefabInstance(DuplicatePropertyOverride));

		PrefabOverrideRecord FirstRenameOverride;
		FirstRenameOverride.Entity = { SourceChildKey, false };
		FirstRenameOverride.Operation = PrefabOverrideOperation::Rename;
		FirstRenameOverride.Name = "First rename";
		PrefabOverrideRecord SecondRenameOverride = FirstRenameOverride;
		SecondRenameOverride.Name = "Second rename";
		PrefabInstanceComponent DuplicateRenameOverride = Instance;
		DuplicateRenameOverride.Overrides = { FirstRenameOverride, SecondRenameOverride };
		PF_CHECK(Tests, !DuplicateRenameOverride.Validate(RootEntityID));
		PF_CHECK(Tests, !Root->SetPrefabInstance(DuplicateRenameOverride));

		PrefabInstanceComponent MalformedSetPropertyPayload = Instance;
		MalformedSetPropertyPayload.Overrides.front().Name = "Unexpected rename payload";
		PF_CHECK(Tests, !MalformedSetPropertyPayload.Validate(RootEntityID));
		PF_CHECK(Tests, !Root->SetPrefabInstance(MalformedSetPropertyPayload));
		PrefabInstanceComponent MalformedSetPropertyParent = Instance;
		MalformedSetPropertyParent.Overrides.front().Parent = PrefabEntityAddress{ SourceRootKey, false };
		PF_CHECK(Tests, !MalformedSetPropertyParent.Validate(RootEntityID));
		PF_CHECK(Tests, !Root->SetPrefabInstance(MalformedSetPropertyParent));
		PF_CHECK(Tests, Root->SetPrefabInstance(Instance).has_value());

		const auto Serialized = SceneSerializer::Serialize(AuthoredScene);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;

		nlohmann::json Document;
		try
		{
			Document = nlohmann::json::parse(*Serialized);
		}
		catch (const nlohmann::json::exception&)
		{
			PF_CHECK(Tests, false);
			return;
		}
		PF_CHECK(Tests, Document.value("version", 0) == 9);
		const nlohmann::json* RootRecord = FindSerializedEntity(Document, RootEntityID);
		PF_CHECK(Tests, RootRecord != nullptr && RootRecord->contains("prefabInstance"));
		if (!RootRecord || !RootRecord->contains("prefabInstance")) return;
		PF_CHECK(Tests, (*RootRecord)["prefabInstance"].value("asset", std::string{}) == FirstPrefabAsset.ToString());
		PF_CHECK(Tests, (*RootRecord)["prefabInstance"].value("root", std::string{}) == SourceRootKey.ToString());

		Scene ReopenedScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, ReopenedScene).has_value());
		const auto ReopenedRoot = ReopenedScene.FindEntity(RootEntityID);
		const auto ReopenedChild = ReopenedScene.FindEntity(ChildEntityID);
		PF_CHECK(Tests, ReopenedRoot.has_value() && ReopenedChild.has_value());
		if (!ReopenedRoot || !ReopenedChild) return;
		const auto ReopenedInstance = ReopenedRoot->GetPrefabInstance();
		PF_CHECK(Tests, ReopenedInstance && ReopenedInstance->has_value());
		if (!ReopenedInstance || !ReopenedInstance->has_value()) return;

		const PrefabInstanceComponent& Reopened = **ReopenedInstance;
		PF_CHECK(Tests, Reopened.InstanceID == Instance.InstanceID);
		PF_CHECK(Tests, Reopened.SourceAsset == FirstPrefabAsset);
		PF_CHECK(Tests, Reopened.SourceRootKey == SourceRootKey);
		PF_CHECK(Tests, Reopened.AppliedSourceFingerprint == Instance.AppliedSourceFingerprint);
		PF_CHECK(Tests, Reopened.SourceEntities == Instance.SourceEntities);
		PF_CHECK(Tests, Reopened.LocalEntities == Instance.LocalEntities);
		PF_CHECK(Tests, Reopened.Overrides.size() == 1);
		if (Reopened.Overrides.size() == 1)
		{
			const PrefabOverrideRecord& Override = Reopened.Overrides.front();
			PF_CHECK(Tests, Override.Entity == RenameOverride.Entity);
			PF_CHECK(Tests, Override.Operation == PrefabOverrideOperation::SetProperty);
			PF_CHECK(Tests, Override.ComponentID == RenameOverride.ComponentID && Override.PropertyID == RenameOverride.PropertyID);
			PF_CHECK(Tests, std::holds_alternative<std::string>(Override.Value));
			if (std::holds_alternative<std::string>(Override.Value))
				PF_CHECK(Tests, std::get<std::string>(Override.Value) == "Local child label");
		}
		PF_CHECK(Tests, Reopened.Conflicts.size() == Instance.Conflicts.size());
		if (Reopened.Conflicts.size() == 1 && Instance.Conflicts.size() == 1)
		{
			const PrefabConflictRecord& ReopenedConflict = Reopened.Conflicts.front();
			const PrefabConflictRecord& ExpectedConflict = Instance.Conflicts.front();
			PF_CHECK(Tests, ReopenedConflict.Entity == ExpectedConflict.Entity);
			PF_CHECK(Tests, ReopenedConflict.Kind == ExpectedConflict.Kind);
			PF_CHECK(Tests, ReopenedConflict.ComponentID == ExpectedConflict.ComponentID);
			PF_CHECK(Tests, ReopenedConflict.PropertyID == ExpectedConflict.PropertyID);
			PF_CHECK(Tests, ReopenedConflict.Message == ExpectedConflict.Message);
		}
		const auto ReopenedTransform = ReopenedRoot->GetTransform();
		PF_CHECK(Tests, ReopenedTransform && glm::all(glm::equal(ReopenedTransform->Translation, RootTransform.Translation)));
		const auto ReopenedChildParent = ReopenedChild->GetParent();
		PF_CHECK(Tests, ReopenedChildParent && ReopenedChildParent->has_value() && **ReopenedChildParent == *ReopenedRoot);

		// Duplicate source keys are rejected before replacing the destination scene.
		nlohmann::json DuplicateSourceKey = Document;
		nlohmann::json* DuplicateRootRecord = FindSerializedEntity(DuplicateSourceKey, RootEntityID);
		PF_CHECK(Tests, DuplicateRootRecord != nullptr);
		if (!DuplicateRootRecord) return;
		auto& Mappings = (*DuplicateRootRecord)["prefabInstance"]["sourceEntities"];
		PF_CHECK(Tests, Mappings.is_array() && Mappings.size() == 2);
		if (!Mappings.is_array() || Mappings.size() != 2) return;
		Mappings[1]["source"] = Mappings[0]["source"];

		Scene PreservedDestination;
		constexpr UUID SentinelID{ 0x9500000000000000ull, 95 };
		PF_CHECK(Tests, PreservedDestination.CreateEntityWithUUID(SentinelID, "Preserve on invalid load").has_value());
		const auto InvalidDuplicate = SceneSerializer::Deserialize(DuplicateSourceKey.dump(), PreservedDestination);
		PF_CHECK(Tests, !InvalidDuplicate && InvalidDuplicate.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 && PreservedDestination.FindEntity(SentinelID).has_value());

		// Conflicting persisted property addresses must be rejected, not resolved by array order.
		nlohmann::json DuplicatePropertyDocument = Document;
		nlohmann::json* DuplicatePropertyRoot = FindSerializedEntity(DuplicatePropertyDocument, RootEntityID);
		PF_CHECK(Tests, DuplicatePropertyRoot != nullptr);
		if (!DuplicatePropertyRoot) return;
		auto& DuplicatePropertyRecords = (*DuplicatePropertyRoot)["prefabInstance"]["overrides"];
		PF_CHECK(Tests, DuplicatePropertyRecords.is_array() && DuplicatePropertyRecords.size() == 1);
		if (!DuplicatePropertyRecords.is_array() || DuplicatePropertyRecords.size() != 1) return;
		nlohmann::json ConflictingPropertyRecord = DuplicatePropertyRecords.front();
		ConflictingPropertyRecord["value"]["value"] = "Conflicting persisted value";
		DuplicatePropertyRecords.push_back(std::move(ConflictingPropertyRecord));
		const auto InvalidDuplicateProperty = SceneSerializer::Deserialize(DuplicatePropertyDocument.dump(), PreservedDestination);
		PF_CHECK(Tests, !InvalidDuplicateProperty &&
				InvalidDuplicateProperty.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 && PreservedDestination.FindEntity(SentinelID).has_value());

		// Two distinct names for the same mapped entity are also an invalid duplicate override address.
		nlohmann::json DuplicateRenameDocument = Document;
		nlohmann::json* DuplicateRenameRoot = FindSerializedEntity(DuplicateRenameDocument, RootEntityID);
		PF_CHECK(Tests, DuplicateRenameRoot != nullptr);
		if (!DuplicateRenameRoot) return;
		auto& DuplicateRenameRecords = (*DuplicateRenameRoot)["prefabInstance"]["overrides"];
		PF_CHECK(Tests, DuplicateRenameRecords.is_array() && DuplicateRenameRecords.size() == 1);
		if (!DuplicateRenameRecords.is_array() || DuplicateRenameRecords.size() != 1) return;
		nlohmann::json FirstRenameRecord = DuplicateRenameRecords.front();
		FirstRenameRecord["operation"] = static_cast<int>(PrefabOverrideOperation::Rename);
		FirstRenameRecord["component"] = "";
		FirstRenameRecord["property"] = "";
		FirstRenameRecord["parent"] = nullptr;
		FirstRenameRecord["name"] = "First persisted rename";
		FirstRenameRecord.erase("value");
		DuplicateRenameRecords[0] = FirstRenameRecord;
		nlohmann::json SecondRenameRecord = FirstRenameRecord;
		SecondRenameRecord["name"] = "Second persisted rename";
		DuplicateRenameRecords.push_back(std::move(SecondRenameRecord));
		const auto InvalidDuplicateRename = SceneSerializer::Deserialize(DuplicateRenameDocument.dump(), PreservedDestination);
		PF_CHECK(Tests, !InvalidDuplicateRename &&
				InvalidDuplicateRename.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 && PreservedDestination.FindEntity(SentinelID).has_value());

		// A property operation cannot carry a separate rename payload.
		nlohmann::json MalformedSetPropertyDocument = Document;
		nlohmann::json* MalformedSetPropertyRoot = FindSerializedEntity(MalformedSetPropertyDocument, RootEntityID);
		PF_CHECK(Tests, MalformedSetPropertyRoot != nullptr);
		if (!MalformedSetPropertyRoot) return;
		(*MalformedSetPropertyRoot)["prefabInstance"]["overrides"][0]["name"] = "Unexpected rename payload";
		const auto InvalidSetPropertyPayload = SceneSerializer::Deserialize(MalformedSetPropertyDocument.dump(), PreservedDestination);
		PF_CHECK(Tests, !InvalidSetPropertyPayload &&
				InvalidSetPropertyPayload.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 && PreservedDestination.FindEntity(SentinelID).has_value());
		nlohmann::json MalformedSetPropertyParentDocument = Document;
		nlohmann::json* MalformedSetPropertyParentRoot = FindSerializedEntity(MalformedSetPropertyParentDocument, RootEntityID);
		PF_CHECK(Tests, MalformedSetPropertyParentRoot != nullptr);
		if (!MalformedSetPropertyParentRoot) return;
		(*MalformedSetPropertyParentRoot)["prefabInstance"]["overrides"][0]["parent"] = {
			{ "key", SourceRootKey.ToString() }, { "local", false } };
		const auto InvalidSetPropertyParent = SceneSerializer::Deserialize(
			MalformedSetPropertyParentDocument.dump(), PreservedDestination);
		PF_CHECK(Tests, !InvalidSetPropertyParent &&
				InvalidSetPropertyParent.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 && PreservedDestination.FindEntity(SentinelID).has_value());

		// Linked metadata is invalid in a legacy scene document, even when its record is otherwise valid.
		nlohmann::json LegacyVersionWithLink = Document;
		LegacyVersionWithLink["version"] = 8;
		const auto LegacyLinkResult = SceneSerializer::Deserialize(LegacyVersionWithLink.dump(), PreservedDestination);
		PF_CHECK(Tests, !LegacyLinkResult && LegacyLinkResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 && PreservedDestination.FindEntity(SentinelID).has_value());
	}

	void TestPrefabInstanceMappingsRemainStableAndDisjoint(TestRunner& Tests)
	{
		Scene AuthoredScene;
		const auto AddLinkedPair = [&](
			const UUID& RootID,
			const UUID& ChildID,
			const UUID& InstanceID,
			const AssetID& SourceAsset,
			std::string_view RootName,
			std::string_view ChildName)
		{
			auto Root = AuthoredScene.CreateEntityWithUUID(RootID, std::string(RootName));
			auto Child = AuthoredScene.CreateEntityWithUUID(ChildID, std::string(ChildName));
			PF_CHECK(Tests, Root.has_value() && Child.has_value());
			if (!Root || !Child) return;
			PF_CHECK(Tests, Child->SetParent(*Root).has_value());
			const auto Metadata = MakeInstanceMetadata(InstanceID, SourceAsset, RootID, ChildID);
			PF_CHECK(Tests, Root->SetPrefabInstance(Metadata).has_value());
		};

		constexpr UUID FirstRoot{ 0xa100000000000000ull, 101 };
		constexpr UUID FirstChild{ 0xa200000000000000ull, 102 };
		constexpr UUID SecondRoot{ 0xa300000000000000ull, 103 };
		constexpr UUID SecondChild{ 0xa400000000000000ull, 104 };
		constexpr UUID OtherAssetRoot{ 0xa500000000000000ull, 105 };
		constexpr UUID OtherAssetChild{ 0xa600000000000000ull, 106 };
		constexpr UUID FirstInstanceID{ 0xa700000000000000ull, 107 };
		constexpr UUID SecondInstanceID{ 0xa800000000000000ull, 108 };
		constexpr UUID OtherAssetInstanceID{ 0xa900000000000000ull, 109 };
		AddLinkedPair(FirstRoot, FirstChild, FirstInstanceID, FirstPrefabAsset, "Changed root name A", "Same child label");
		AddLinkedPair(SecondRoot, SecondChild, SecondInstanceID, FirstPrefabAsset, "Changed root name B", "Same child label");
		AddLinkedPair(OtherAssetRoot, OtherAssetChild, OtherAssetInstanceID, SecondPrefabAsset, "Changed root name C", "Same child label");
		PF_CHECK(Tests, AuthoredScene.GetEntityCount() == 6);
		if (AuthoredScene.GetEntityCount() != 6) return;

		const auto Serialized = SceneSerializer::Serialize(AuthoredScene);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		Scene ReopenedScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, ReopenedScene).has_value());

		const auto VerifyMapping = [&](const UUID& RootID, const UUID& ChildID, const UUID& InstanceID, const AssetID& SourceAsset)
		{
			const auto Root = ReopenedScene.FindEntity(RootID);
			const auto Child = ReopenedScene.FindEntity(ChildID);
			PF_CHECK(Tests, Root.has_value() && Child.has_value());
			if (!Root || !Child) return;
			const auto Metadata = Root->GetPrefabInstance();
			PF_CHECK(Tests, Metadata && Metadata->has_value());
			if (!Metadata || !Metadata->has_value()) return;
			const auto& Instance = **Metadata;
			PF_CHECK(Tests, Instance.InstanceID == InstanceID && Instance.SourceAsset == SourceAsset);
			PF_CHECK(Tests, Instance.SourceRootKey == SourceRootKey);
			PF_CHECK(Tests, Instance.SourceEntities.size() == 2);
			if (Instance.SourceEntities.size() != 2) return;
			PF_CHECK(Tests, Instance.SourceEntities[0] == (PrefabEntityMapping{ SourceRootKey, RootID }));
			PF_CHECK(Tests, Instance.SourceEntities[1] == (PrefabEntityMapping{ SourceChildKey, ChildID }));
			const auto Parent = Child->GetParent();
			PF_CHECK(Tests, Parent && Parent->has_value() && **Parent == *Root);
			const auto ChildTag = Child->GetTag();
			PF_CHECK(Tests, ChildTag && ChildTag->Name == "Same child label");
		};
		VerifyMapping(FirstRoot, FirstChild, FirstInstanceID, FirstPrefabAsset);
		VerifyMapping(SecondRoot, SecondChild, SecondInstanceID, FirstPrefabAsset);
		VerifyMapping(OtherAssetRoot, OtherAssetChild, OtherAssetInstanceID, SecondPrefabAsset);

		PF_CHECK(Tests, FirstRoot != SecondRoot && FirstRoot != OtherAssetRoot && SecondRoot != OtherAssetRoot);
		PF_CHECK(Tests, FirstChild != SecondChild && FirstChild != OtherAssetChild && SecondChild != OtherAssetChild);
		PF_CHECK(Tests, FirstInstanceID != SecondInstanceID && FirstInstanceID != OtherAssetInstanceID &&
					SecondInstanceID != OtherAssetInstanceID);
		const std::array SceneEntityIDs = { FirstRoot, FirstChild, SecondRoot, SecondChild, OtherAssetRoot, OtherAssetChild };
		for (size_t First = 0; First < SceneEntityIDs.size(); ++First)
			for (size_t Second = First + 1; Second < SceneEntityIDs.size(); ++Second)
				PF_CHECK(Tests, SceneEntityIDs[First] != SceneEntityIDs[Second]);
	}

	void TestLegacyScenesLoadWithoutLinkedPrefabMetadata(TestRunner& Tests)
	{
		Scene AuthoredScene;
		constexpr UUID EntityID{ 0xb100000000000000ull, 111 };
		const auto Entity = AuthoredScene.CreateEntityWithUUID(EntityID, "Legacy entity");
		PF_CHECK(Tests, Entity.has_value());
		if (!Entity) return;
		const auto CurrentFormat = SceneSerializer::Serialize(AuthoredScene);
		PF_CHECK(Tests, CurrentFormat.has_value());
		if (!CurrentFormat) return;

		for (const int LegacyVersion : { 1, 8 })
		{
			nlohmann::json LegacyDocument;
			try
			{
				LegacyDocument = nlohmann::json::parse(*CurrentFormat);
			}
			catch (const nlohmann::json::exception&)
			{
				PF_CHECK(Tests, false);
				return;
			}
			LegacyDocument["version"] = LegacyVersion;
			Scene LoadedScene;
			const auto Loaded = SceneSerializer::Deserialize(LegacyDocument.dump(), LoadedScene);
			PF_CHECK(Tests, Loaded.has_value());
			if (!Loaded) continue;
			const auto LoadedEntity = LoadedScene.FindEntity(EntityID);
			PF_CHECK(Tests, LoadedEntity.has_value());
			if (!LoadedEntity) continue;
			const auto Metadata = LoadedEntity->GetPrefabInstance();
			PF_CHECK(Tests, Metadata && !Metadata->has_value());
		}
	}

	void TestLinkedInstantiationReconcileOverridesAndStalePlans(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		auto SourceResult = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, SourceResult.has_value());
		if (!SourceResult) return;
		PrefabSourceDocument& Source = *SourceResult;
		PF_CHECK(Tests, Source.GetSourceAsset() == Fixture.Asset && Source.GetRootKey() == SourceRootKey);
		PF_CHECK(Tests, !Source.GetFingerprint().empty());
		const auto SourceChild = Source.GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceChild.has_value());
		if (!SourceChild) return;

		Scene Destination;
		const auto Unrelated = Destination.CreateEntity("Unrelated scene entity");
		PF_CHECK(Tests, Unrelated.has_value());
		if (!Unrelated) return;
		const Entity UnrelatedHandle = *Unrelated;
		PrefabPlacement FirstPlacement;
		TransformComponent FirstPlacementTransform;
		FirstPlacementTransform.Translation = { 100.0f, 10.0f, 0.0f };
		FirstPlacement.RootTransform = FirstPlacementTransform;
		PrefabPlacement SecondPlacement;
		TransformComponent SecondPlacementTransform;
		SecondPlacementTransform.Translation = { 200.0f, 20.0f, 0.0f };
		SecondPlacement.RootTransform = SecondPlacementTransform;
		const auto FirstRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, FirstPlacement);
		const auto SecondRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, SecondPlacement);
		PF_CHECK(Tests, FirstRootResult.has_value() && SecondRootResult.has_value());
		if (!FirstRootResult || !SecondRootResult) return;
		const Entity FirstRoot = *FirstRootResult;
		const Entity SecondRoot = *SecondRootResult;
		PF_CHECK(Tests, FirstRoot.GetUUID() != SecondRoot.GetUUID());

		const auto FirstChildID = FindMappedEntity(FirstRoot, SourceChildKey);
		const auto SecondChildID = FindMappedEntity(SecondRoot, SourceChildKey);
		PF_CHECK(Tests, FirstChildID.has_value() && SecondChildID.has_value());
		if (!FirstChildID || !SecondChildID) return;
		PF_CHECK(Tests, *FirstChildID != *SecondChildID);
		const auto FirstChildResult = Destination.FindEntity(*FirstChildID);
		const auto SecondChildResult = Destination.FindEntity(*SecondChildID);
		PF_CHECK(Tests, FirstChildResult.has_value() && SecondChildResult.has_value());
		if (!FirstChildResult || !SecondChildResult) return;
		const Entity FirstChild = *FirstChildResult;
		const Entity SecondChild = *SecondChildResult;
		const auto FirstMetadata = FirstRoot.GetPrefabInstance();
		const auto SecondMetadata = SecondRoot.GetPrefabInstance();
		PF_CHECK(Tests, FirstMetadata && FirstMetadata->has_value() && SecondMetadata && SecondMetadata->has_value());
		if (!FirstMetadata || !FirstMetadata->has_value() || !SecondMetadata || !SecondMetadata->has_value()) return;
		PF_CHECK(Tests, (**FirstMetadata).SourceAsset == Fixture.Asset && (**SecondMetadata).SourceAsset == Fixture.Asset);
		PF_CHECK(Tests, (**FirstMetadata).InstanceID != (**SecondMetadata).InstanceID);
		PF_CHECK(Tests, (**FirstMetadata).SourceEntities[1].SourceKey == (**SecondMetadata).SourceEntities[1].SourceKey);
		PF_CHECK(Tests, (**FirstMetadata).SourceEntities[1].SceneEntity != (**SecondMetadata).SourceEntities[1].SceneEntity);
		const auto FirstRootTransform = FirstRoot.GetTransform();
		const auto SecondRootTransform = SecondRoot.GetTransform();
		PF_CHECK(Tests, FirstRootTransform && glm::all(glm::equal(FirstRootTransform->Translation, FirstPlacementTransform.Translation)));
		PF_CHECK(Tests, SecondRootTransform && glm::all(glm::equal(SecondRootTransform->Translation, SecondPlacementTransform.Translation)));

		const PrefabEntityAddress ChildAddress{ SourceChildKey, false };
		PrefabOverrideRecord FirstOverride;
		FirstOverride.Entity = ChildAddress;
		FirstOverride.Operation = PrefabOverrideOperation::SetProperty;
		FirstOverride.ComponentID = std::string(PropertyComponentIds::MeshRenderer);
		FirstOverride.PropertyID = std::string(PropertyIds::MaterialAsset);
		FirstOverride.Value = std::optional<AssetID>{ FirstOverrideMaterial };
		PF_CHECK(Tests, WriteProperty(FirstChild, FirstOverride.ComponentID, FirstOverride.PropertyID, FirstOverride.Value).has_value());
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordOverride(FirstRoot, FirstOverride).has_value());
		PF_CHECK(Tests, HasOverride(FirstRoot, ChildAddress, FirstOverride.ComponentID, FirstOverride.PropertyID));
		const auto OverrideMetadata = FirstRoot.GetPrefabInstance();
		PF_CHECK(Tests, OverrideMetadata && OverrideMetadata->has_value() && (**OverrideMetadata).Overrides.size() == 1);

		// ResetOverride removes the recorded intent; reconcile then reapplies the current source value.
		PrefabOverrideRecord SecondOverride = FirstOverride;
		SecondOverride.Value = std::optional<AssetID>{ SecondOverrideMaterial };
		PF_CHECK(Tests, WriteProperty(SecondChild, SecondOverride.ComponentID, SecondOverride.PropertyID, SecondOverride.Value).has_value());
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordOverride(SecondRoot, SecondOverride).has_value());
		PF_CHECK(Tests, HasOverride(SecondRoot, ChildAddress, SecondOverride.ComponentID, SecondOverride.PropertyID));
		PF_CHECK(Tests, PrefabInstanceReconciler::ResetOverride(
			SecondRoot, Source, ChildAddress, SecondOverride.ComponentID, SecondOverride.PropertyID).has_value());
		PF_CHECK(Tests, !HasOverride(SecondRoot, ChildAddress, SecondOverride.ComponentID, SecondOverride.PropertyID));
		const auto PendingResetMaterial = SecondChild.GetMeshRenderer();
		PF_CHECK(Tests, PendingResetMaterial && PendingResetMaterial->has_value() &&
					PendingResetMaterial->value().MaterialAsset == std::optional<AssetID>{ SourceMaterialAsset });
		const auto ResetPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, SecondRoot);
		PF_CHECK(Tests, ResetPlan.has_value());
		if (!ResetPlan) return;
		const auto ResetApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *ResetPlan);
		PF_CHECK(Tests, ResetApply.has_value());
		if (!ResetApply) return;
		const auto ResetMaterial = SecondChild.GetMeshRenderer();
		PF_CHECK(Tests, ResetMaterial && ResetMaterial->has_value() &&
					ResetMaterial->value().MaterialAsset == std::optional<AssetID>{ SourceMaterialAsset });

		// Update the source root, an unoverridden child property, and add a source entity.
		const auto SourceRoot = Source.GetScene().FindEntity(SourceRootKey);
		PF_CHECK(Tests, SourceRoot.has_value());
		if (!SourceRoot) return;
		auto SourceRootTag = SourceRoot->GetTag();
		PF_CHECK(Tests, SourceRootTag.has_value());
		if (!SourceRootTag) return;
		SourceRootTag->Name = "Source root revision two";
		PF_CHECK(Tests, SourceRoot->SetTag(*SourceRootTag).has_value());
		auto SourceChildTag = SourceChild->GetTag();
		PF_CHECK(Tests, SourceChildTag.has_value());
		if (!SourceChildTag) return;
		SourceChildTag->Name = "Renamed source child revision two";
		PF_CHECK(Tests, SourceChild->SetTag(*SourceChildTag).has_value());
		TransformComponent UpdatedRootTransform = *SourceRoot->GetTransform();
		UpdatedRootTransform.Translation = { 500.0f, 30.0f, 40.0f };
		PF_CHECK(Tests, SourceRoot->SetTransform(UpdatedRootTransform).has_value());
		TransformComponent UpdatedChildTransform = *SourceChild->GetTransform();
		UpdatedChildTransform.Scale = { 2.0f, 2.0f, 2.0f };
		PF_CHECK(Tests, SourceChild->SetTransform(UpdatedChildTransform).has_value());
		const auto AddedSourceEntity = Source.GetScene().CreateEntityWithUUID(AddedSourceEntityKey, "Added source entity");
		PF_CHECK(Tests, AddedSourceEntity.has_value());
		if (!AddedSourceEntity) return;
		PF_CHECK(Tests, AddedSourceEntity->SetParent(*SourceRoot).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Source).has_value());

		const auto FirstPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, FirstRoot);
		PF_CHECK(Tests, FirstPlan.has_value());
		if (!FirstPlan) return;
		PF_CHECK(Tests, FirstPlan->CanApply() && FirstPlan->GetConflicts().empty());
		const auto FirstApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *FirstPlan);
		PF_CHECK(Tests, FirstApply.has_value());
		if (!FirstApply) return;
		PF_CHECK(Tests, FirstApply->Added >= 1);
		PF_CHECK(Tests, FirstRoot.IsValid() && FirstChild.IsValid() && UnrelatedHandle.IsValid());
		PF_CHECK(Tests, FirstChild.GetUUID() == *FirstChildID);
		const auto AppliedFirstChildTag = FirstChild.GetTag();
		PF_CHECK(Tests, AppliedFirstChildTag && AppliedFirstChildTag->Name == "Renamed source child revision two");
		const auto AppliedFirstChildTransform = FirstChild.GetTransform();
		PF_CHECK(Tests, AppliedFirstChildTransform &&
					glm::all(glm::equal(AppliedFirstChildTransform->Scale, glm::vec3{ 2.0f, 2.0f, 2.0f })));
		const auto AppliedFirstChildRenderer = FirstChild.GetMeshRenderer();
		PF_CHECK(Tests, AppliedFirstChildRenderer && AppliedFirstChildRenderer->has_value() &&
					AppliedFirstChildRenderer->value().MaterialAsset == std::optional<AssetID>{ FirstOverrideMaterial });
		const auto AppliedFirstRootTag = FirstRoot.GetTag();
		PF_CHECK(Tests, AppliedFirstRootTag && AppliedFirstRootTag->Name == "Source root revision two");
		const auto AppliedFirstRootTransform = FirstRoot.GetTransform();
		PF_CHECK(Tests, AppliedFirstRootTransform &&
					glm::all(glm::equal(AppliedFirstRootTransform->Translation, FirstPlacementTransform.Translation)));
		PF_CHECK(Tests, FindMappedEntity(FirstRoot, AddedSourceEntityKey).has_value());

		const auto SecondChildBeforeReconcile = SecondChild.GetTransform();
		PF_CHECK(Tests, SecondChildBeforeReconcile &&
					glm::all(glm::equal(SecondChildBeforeReconcile->Scale, glm::vec3{ 1.0f, 1.0f, 1.0f })));
		const auto SecondPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, SecondRoot);
		PF_CHECK(Tests, SecondPlan.has_value());
		if (!SecondPlan) return;
		PF_CHECK(Tests, SecondPlan->CanApply());
		const auto SecondApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *SecondPlan);
		PF_CHECK(Tests, SecondApply.has_value());
		if (!SecondApply) return;
		PF_CHECK(Tests, SecondRoot.IsValid() && SecondChild.IsValid() && UnrelatedHandle.IsValid());
		const auto AppliedSecondChildTransform = SecondChild.GetTransform();
		PF_CHECK(Tests, AppliedSecondChildTransform &&
					glm::all(glm::equal(AppliedSecondChildTransform->Scale, glm::vec3{ 2.0f, 2.0f, 2.0f })));
		const auto AddedFirstID = FindMappedEntity(FirstRoot, AddedSourceEntityKey);
		const auto AddedSecondID = FindMappedEntity(SecondRoot, AddedSourceEntityKey);
		PF_CHECK(Tests, AddedFirstID.has_value() && AddedSecondID.has_value() && *AddedFirstID != *AddedSecondID);

		// Scene save/reopen must preserve the concrete linked maps created by the service.
		const auto ReconciledSceneData = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, ReconciledSceneData.has_value());
		if (!ReconciledSceneData) return;
		Scene ReopenedScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*ReconciledSceneData, ReopenedScene).has_value());
		const auto ReopenedFirstRoot = ReopenedScene.FindEntity(FirstRoot.GetUUID());
		PF_CHECK(Tests, ReopenedFirstRoot.has_value());
		if (!ReopenedFirstRoot) return;
		const auto ReopenedFirstMap = FindMappedEntity(*ReopenedFirstRoot, SourceChildKey);
		PF_CHECK(Tests, ReopenedFirstMap == FirstChildID);
		const auto ReopenedFirstMetadata = ReopenedFirstRoot->GetPrefabInstance();
		PF_CHECK(Tests, ReopenedFirstMetadata && ReopenedFirstMetadata->has_value() &&
					(**ReopenedFirstMetadata).InstanceID == (**FirstMetadata).InstanceID);

		// A plan is tied to both its source bytes and the linked scene draft snapshot.
		const auto StaleSourcePlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, SecondRoot);
		PF_CHECK(Tests, StaleSourcePlan.has_value());
		if (!StaleSourcePlan) return;
		UpdatedChildTransform = *SourceChild->GetTransform();
		UpdatedChildTransform.Scale = { 3.0f, 3.0f, 3.0f };
		PF_CHECK(Tests, SourceChild->SetTransform(UpdatedChildTransform).has_value());
		PF_CHECK(Tests, SourceChild->RemoveMeshRenderer().has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Source).has_value());
		const auto StaleSourceApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *StaleSourcePlan);
		PF_CHECK(Tests, !StaleSourceApply && StaleSourceApply.error().Code == PrefabAssetErrorCode::StaleReconcilePlan);
		const auto SecondAfterStaleSource = SecondChild.GetTransform();
		const auto SecondRendererAfterStaleSource = SecondChild.GetMeshRenderer();
		PF_CHECK(Tests, SecondAfterStaleSource &&
					glm::all(glm::equal(SecondAfterStaleSource->Scale, glm::vec3{ 2.0f, 2.0f, 2.0f })));
		PF_CHECK(Tests, SecondRendererAfterStaleSource && SecondRendererAfterStaleSource->has_value());

		const auto StaleDraftPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, SecondRoot);
		PF_CHECK(Tests, StaleDraftPlan.has_value());
		if (!StaleDraftPlan) return;
		TransformComponent ChangedPlacement = *SecondRoot.GetTransform();
		ChangedPlacement.Translation.x += 5.0f;
		PF_CHECK(Tests, SecondRoot.SetTransform(ChangedPlacement).has_value());
		const auto StaleDraftApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *StaleDraftPlan);
		PF_CHECK(Tests, !StaleDraftApply && StaleDraftApply.error().Code == PrefabAssetErrorCode::StaleReconcilePlan);
		const auto ChangedPlacementAfterStale = SecondRoot.GetTransform();
		PF_CHECK(Tests, ChangedPlacementAfterStale && ChangedPlacementAfterStale->Translation.x == ChangedPlacement.Translation.x);
		PF_CHECK(Tests, SecondChild.IsValid() && UnrelatedHandle.IsValid());

		const auto FreshSecondPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, SecondRoot);
		PF_CHECK(Tests, FreshSecondPlan.has_value());
		if (!FreshSecondPlan) return;
		const auto FreshSecondApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *FreshSecondPlan);
		PF_CHECK(Tests, FreshSecondApply.has_value());
		if (!FreshSecondApply) return;
		const auto SecondAfterFresh = SecondChild.GetTransform();
		const auto SecondRendererAfterFresh = SecondChild.GetMeshRenderer();
		PF_CHECK(Tests, SecondAfterFresh && glm::all(glm::equal(SecondAfterFresh->Scale, glm::vec3{ 3.0f, 3.0f, 3.0f })));
		PF_CHECK(Tests, SecondRendererAfterFresh && !SecondRendererAfterFresh->has_value());
		PF_CHECK(Tests, SecondChild.IsValid() && SecondRoot.IsValid() && UnrelatedHandle.IsValid());

		// Removing a source property with a local override creates a conflict and applies no other planned changes.
		const size_t CountBeforeConflict = Destination.GetEntityCount();
		const auto ConflictPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, FirstRoot);
		PF_CHECK(Tests, ConflictPlan.has_value());
		if (!ConflictPlan) return;
		PF_CHECK(Tests, !ConflictPlan->CanApply() && !ConflictPlan->GetConflicts().empty());
		PF_CHECK(Tests, ConflictPlan->GetSummary().ConflictCount > 0);
		PF_CHECK(Tests, std::any_of(ConflictPlan->GetConflicts().begin(), ConflictPlan->GetConflicts().end(),
			[&](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == ChildAddress && Conflict.Kind == PrefabConflictKind::MissingProperty &&
					Conflict.ComponentID == FirstOverride.ComponentID && Conflict.PropertyID == FirstOverride.PropertyID;
			}));
		const auto ConflictApply = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *ConflictPlan);
		PF_CHECK(Tests, ConflictApply.has_value() && ConflictApply->ConflictCount > 0);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeConflict);
		const auto PersistedConflictMetadata = FirstRoot.GetPrefabInstance();
		PF_CHECK(Tests, PersistedConflictMetadata && PersistedConflictMetadata->has_value() &&
					!(*PersistedConflictMetadata)->Conflicts.empty());
		const auto ConflictSceneText = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, ConflictSceneText.has_value());
		if (ConflictSceneText)
		{
			Scene ConflictReopened;
			PF_CHECK(Tests, SceneSerializer::Deserialize(*ConflictSceneText, ConflictReopened).has_value());
			const auto ReopenedConflictRoot = ConflictReopened.FindEntity(FirstRoot.GetUUID());
			PF_CHECK(Tests, ReopenedConflictRoot.has_value());
			if (ReopenedConflictRoot)
			{
				const auto ReopenedConflict = ReopenedConflictRoot->GetPrefabInstance();
				PF_CHECK(Tests, ReopenedConflict && ReopenedConflict->has_value() &&
							(**ReopenedConflict).Conflicts == (**PersistedConflictMetadata).Conflicts);
			}
		}
		const auto FirstAfterConflictTransform = FirstChild.GetTransform();
		const auto FirstAfterConflictRenderer = FirstChild.GetMeshRenderer();
		PF_CHECK(Tests, FirstAfterConflictTransform &&
					glm::all(glm::equal(FirstAfterConflictTransform->Scale, glm::vec3{ 2.0f, 2.0f, 2.0f })));
		PF_CHECK(Tests, FirstAfterConflictRenderer && FirstAfterConflictRenderer->has_value() &&
					FirstAfterConflictRenderer->value().MaterialAsset == std::optional<AssetID>{ FirstOverrideMaterial });
		PF_CHECK(Tests, FirstRoot.IsValid() && FirstChild.IsValid() && UnrelatedHandle.IsValid());
	}

	void TestPrefabSourceSaveRejectsDetachedEntitiesWithoutDroppingThem(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		auto SourceResult = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, SourceResult.has_value());
		if (!SourceResult) return;
		PrefabSourceDocument& Source = *SourceResult;
		const std::string OriginalFingerprint(Source.GetFingerprint());
		const auto SourceChild = Source.GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceChild.has_value());
		if (!SourceChild) return;
		PF_CHECK(Tests, Source.GetScene().GetEntityCount() == 2);
		PF_CHECK(Tests, SourceChild->ClearParent().has_value());

		const auto RejectedSerialization = PrefabSerializer::Serialize(Source);
		PF_CHECK(Tests, !RejectedSerialization);
		const auto RejectedSave = PrefabAssetService::SaveSource(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Source);
		PF_CHECK(Tests, !RejectedSave);
		PF_CHECK(Tests, Source.GetScene().GetEntityCount() == 2);
		PF_CHECK(Tests, Source.GetScene().FindEntity(SourceChildKey).has_value());

		const auto PersistedSource = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, PersistedSource.has_value());
		if (!PersistedSource) return;
		PF_CHECK(Tests, PersistedSource->GetFingerprint() == std::string_view(OriginalFingerprint));
		PF_CHECK(Tests, PersistedSource->GetScene().GetEntityCount() == 2);
		const auto PersistedRoot = PersistedSource->GetScene().FindEntity(SourceRootKey);
		const auto PersistedChild = PersistedSource->GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, PersistedRoot.has_value() && PersistedChild.has_value());
		if (!PersistedRoot || !PersistedChild) return;
		const auto Parent = PersistedChild->GetParent();
		PF_CHECK(Tests, Parent && Parent->has_value() && **Parent == *PersistedRoot);
	}

	void TestLinkedMappedChildCannotBeDetached(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		Scene Destination;
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, LinkedRootResult.has_value());
		if (!LinkedRootResult) return;
		const Entity LinkedRoot = *LinkedRootResult;
		const auto ChildID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, ChildID.has_value());
		if (!ChildID) return;
		const auto ChildResult = Destination.FindEntity(*ChildID);
		PF_CHECK(Tests, ChildResult.has_value());
		if (!ChildResult) return;
		const Entity Child = *ChildResult;

		const auto Detached = Child.ClearParent();
		PF_CHECK(Tests, !Detached);
		const auto ParentAfterRejectedDetach = Child.GetParent();
		PF_CHECK(Tests, ParentAfterRejectedDetach && ParentAfterRejectedDetach->has_value() &&
				**ParentAfterRejectedDetach == LinkedRoot);
		PF_CHECK(Tests, Child.IsValid() && LinkedRoot.IsValid());
		PF_CHECK(Tests, SceneSerializer::Serialize(Destination).has_value());
	}

	void TestSourceDeleteAndSameNameRecreateUsesFreshLiveEntity(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		auto SourceResult = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, SourceResult.has_value());
		if (!SourceResult) return;
		PrefabSourceDocument& Source = *SourceResult;
		const auto SourceRoot = Source.GetScene().FindEntity(SourceRootKey);
		const auto OldSourceChild = Source.GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceRoot.has_value() && OldSourceChild.has_value());
		if (!SourceRoot || !OldSourceChild) return;
		const auto OldSourceTag = OldSourceChild->GetTag();
		PF_CHECK(Tests, OldSourceTag.has_value());
		if (!OldSourceTag) return;

		Scene Destination;
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, LinkedRootResult.has_value());
		if (!LinkedRootResult) return;
		const Entity LinkedRoot = *LinkedRootResult;
		const auto OldLiveID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, OldLiveID.has_value());
		if (!OldLiveID) return;
		const auto OldLiveChildResult = Destination.FindEntity(*OldLiveID);
		PF_CHECK(Tests, OldLiveChildResult.has_value());
		if (!OldLiveChildResult) return;
		const Entity OldLiveChild = *OldLiveChildResult;

		PF_CHECK(Tests, Source.GetScene().DestroyEntity(*OldSourceChild).has_value());
		const auto ReplacementKey = UUID::Generate();
		PF_CHECK(Tests, ReplacementKey.has_value());
		if (!ReplacementKey) return;
		PF_CHECK(Tests, *ReplacementKey != SourceChildKey);
		const auto Replacement = Source.GetScene().CreateEntityWithUUID(*ReplacementKey, OldSourceTag->Name);
		PF_CHECK(Tests, Replacement.has_value());
		if (!Replacement) return;
		PF_CHECK(Tests, Replacement->SetParent(*SourceRoot).has_value());

		// Before reconciliation the old source key remains a tombstone for its original live identity.
		PF_CHECK(Tests, FindMappedEntity(LinkedRoot, SourceChildKey) == OldLiveID);
		PF_CHECK(Tests, PrefabAssetService::SaveSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Source).has_value());
		const auto Plan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, LinkedRoot);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, Plan->CanApply());
		PF_CHECK(Tests, Plan->GetSummary().Added == 1 && Plan->GetSummary().Removed == 1);
		const auto Applied = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *Plan);
		PF_CHECK(Tests, Applied.has_value());
		if (!Applied) return;

		PF_CHECK(Tests, !OldLiveChild.IsValid());
		PF_CHECK(Tests, !Destination.FindEntity(*OldLiveID).has_value());
		PF_CHECK(Tests, !FindMappedEntity(LinkedRoot, SourceChildKey).has_value());
		const auto NewLiveID = FindMappedEntity(LinkedRoot, *ReplacementKey);
		PF_CHECK(Tests, NewLiveID.has_value() && *NewLiveID != *OldLiveID);
		if (!NewLiveID) return;
		const auto NewLiveChild = Destination.FindEntity(*NewLiveID);
		PF_CHECK(Tests, NewLiveChild.has_value());
		if (!NewLiveChild) return;
		const auto NewLiveTag = NewLiveChild->GetTag();
		PF_CHECK(Tests, NewLiveTag && NewLiveTag->Name == OldSourceTag->Name);
	}

	void TestDetachedRemovedSourceConflictRetainsMappingAfterSourceDeletion(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		auto Source = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, Source.has_value());
		if (!Source) return;
		const auto SourceChild = Source->GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceChild.has_value());
		if (!SourceChild) return;

		Scene Destination;
		const auto Unrelated = Destination.CreateEntity("Outside the linked instance");
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, Unrelated.has_value() && LinkedRootResult.has_value());
		if (!Unrelated || !LinkedRootResult) return;
		const Entity UnrelatedEntity = *Unrelated;
		const Entity LinkedRoot = *LinkedRootResult;
		const UUID RootID = LinkedRoot.GetUUID();
		const UUID UnrelatedID = UnrelatedEntity.GetUUID();
		const auto LinkedChildID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, LinkedChildID.has_value());
		if (!LinkedChildID) return;
		const auto InitialMetadata = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, InitialMetadata && InitialMetadata->has_value());
		if (!InitialMetadata || !InitialMetadata->has_value()) return;
		PrefabInstanceComponent DetachedMetadata = **InitialMetadata;
		DetachedMetadata.Conflicts.push_back({
			{ SourceChildKey, false }, PrefabConflictKind::RemovedSourceEntity, {}, {},
			"The mapped entity was detached and retained for recovery" });
		PF_CHECK(Tests, LinkedRoot.SetPrefabInstance(DetachedMetadata).has_value());
		const std::string AppliedFingerprint = DetachedMetadata.AppliedSourceFingerprint;
		const size_t OriginalEntityCount = Destination.GetEntityCount();

		// Construct the valid serialized recovery state without bypassing Scene::SetParent at runtime.
		const auto AuthoredSceneData = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, AuthoredSceneData.has_value());
		if (!AuthoredSceneData) return;
		nlohmann::json DetachedSceneDocument;
		try
		{
			DetachedSceneDocument = nlohmann::json::parse(*AuthoredSceneData);
		}
		catch (const nlohmann::json::exception&)
		{
			PF_CHECK(Tests, false);
			return;
		}
		nlohmann::json* DetachedChildRecord = FindSerializedEntity(DetachedSceneDocument, *LinkedChildID);
		PF_CHECK(Tests, DetachedChildRecord != nullptr);
		if (!DetachedChildRecord) return;
		(*DetachedChildRecord)["parent"] = UnrelatedID.ToString();
		nlohmann::json DetachedWithoutRecoveryConflict = DetachedSceneDocument;
		nlohmann::json* DetachedRootWithoutConflict = FindSerializedEntity(DetachedWithoutRecoveryConflict, RootID);
		PF_CHECK(Tests, DetachedRootWithoutConflict != nullptr && DetachedRootWithoutConflict->contains("prefabInstance"));
		if (!DetachedRootWithoutConflict || !DetachedRootWithoutConflict->contains("prefabInstance")) return;
		(*DetachedRootWithoutConflict)["prefabInstance"]["conflicts"] = nlohmann::json::array();
		Scene PreservedDestination;
		constexpr UUID PreservedDestinationEntityID{ 0x8f00000000000000ull, 95 };
		PF_CHECK(Tests, PreservedDestination.CreateEntityWithUUID(PreservedDestinationEntityID, "Preserve invalid recovery load").has_value());
		const auto MissingRecoveryConflict = SceneSerializer::Deserialize(
			DetachedWithoutRecoveryConflict.dump(), PreservedDestination);
		PF_CHECK(Tests, !MissingRecoveryConflict &&
				MissingRecoveryConflict.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, PreservedDestination.GetEntityCount() == 1 &&
				PreservedDestination.FindEntity(PreservedDestinationEntityID).has_value());
		Scene DetachedScene;
		const auto DetachedLoad = SceneSerializer::Deserialize(DetachedSceneDocument.dump(), DetachedScene);
		PF_CHECK(Tests, DetachedLoad.has_value());
		if (!DetachedLoad) return;
		const auto DetachedRoot = DetachedScene.FindEntity(RootID);
		const auto DetachedChild = DetachedScene.FindEntity(*LinkedChildID);
		const auto DetachedUnrelated = DetachedScene.FindEntity(UnrelatedID);
		PF_CHECK(Tests, DetachedRoot.has_value() && DetachedChild.has_value() && DetachedUnrelated.has_value());
		if (!DetachedRoot || !DetachedChild || !DetachedUnrelated) return;
		const auto DetachedParent = DetachedChild->GetParent();
		PF_CHECK(Tests, DetachedParent && DetachedParent->has_value() && **DetachedParent == *DetachedUnrelated);
		PF_CHECK(Tests, FindMappedEntity(*DetachedRoot, SourceChildKey) == LinkedChildID);

		// Removing the source key later must not erase the already-conflicted recovery mapping or live entity.
		PF_CHECK(Tests, Source->GetScene().DestroyEntity(*SourceChild).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, *Source).has_value());
		const auto DeletedSourcePlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, DetachedScene, *DetachedRoot);
		PF_CHECK(Tests, DeletedSourcePlan.has_value());
		if (!DeletedSourcePlan) return;
		PF_CHECK(Tests, !DeletedSourcePlan->CanApply());
		PF_CHECK(Tests, std::any_of(DeletedSourcePlan->GetConflicts().begin(), DeletedSourcePlan->GetConflicts().end(),
			[&](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
					Conflict.Kind == PrefabConflictKind::RemovedSourceEntity;
			}));
		const auto DeletedSourceApplied = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, DetachedScene, *DeletedSourcePlan);
		PF_CHECK(Tests, DeletedSourceApplied.has_value() && DeletedSourceApplied->ConflictCount > 0);
		if (!DeletedSourceApplied) return;

		PF_CHECK(Tests, DetachedRoot->IsValid() && DetachedChild->IsValid() && DetachedUnrelated->IsValid());
		PF_CHECK(Tests, DetachedScene.GetEntityCount() == OriginalEntityCount);
		PF_CHECK(Tests, FindMappedEntity(*DetachedRoot, SourceChildKey) == LinkedChildID);
		const auto Metadata = DetachedRoot->GetPrefabInstance();
		PF_CHECK(Tests, Metadata && Metadata->has_value());
		if (!Metadata || !Metadata->has_value()) return;
		PF_CHECK(Tests, (**Metadata).AppliedSourceFingerprint == AppliedFingerprint);
		PF_CHECK(Tests, std::any_of((**Metadata).Conflicts.begin(), (**Metadata).Conflicts.end(),
			[](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
					Conflict.Kind == PrefabConflictKind::RemovedSourceEntity;
			}));

		const auto Serialized = SceneSerializer::Serialize(DetachedScene);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		Scene Reopened;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, Reopened).has_value());
		const auto ReopenedRoot = Reopened.FindEntity(RootID);
		const auto ReopenedChild = Reopened.FindEntity(*LinkedChildID);
		const auto ReopenedUnrelated = Reopened.FindEntity(UnrelatedID);
		PF_CHECK(Tests, ReopenedRoot.has_value() && ReopenedChild.has_value() && ReopenedUnrelated.has_value());
		if (!ReopenedRoot || !ReopenedChild || !ReopenedUnrelated) return;
		PF_CHECK(Tests, FindMappedEntity(*ReopenedRoot, SourceChildKey) == LinkedChildID);
		const auto ReopenedParent = ReopenedChild->GetParent();
		PF_CHECK(Tests, ReopenedParent && ReopenedParent->has_value() && **ReopenedParent == *ReopenedUnrelated);
		const auto ReopenedMetadata = ReopenedRoot->GetPrefabInstance();
		PF_CHECK(Tests, ReopenedMetadata && ReopenedMetadata->has_value() &&
				std::any_of((**ReopenedMetadata).Conflicts.begin(), (**ReopenedMetadata).Conflicts.end(),
					[](const PrefabConflictRecord& Conflict)
					{
						return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
							Conflict.Kind == PrefabConflictKind::RemovedSourceEntity;
					}));
	}

	void TestDeletedLocalOverridePrunesBeforePersistingOtherConflicts(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		auto SourceResult = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, SourceResult.has_value());
		if (!SourceResult) return;
		PrefabSourceDocument& Source = *SourceResult;
		const auto SourceChild = Source.GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceChild.has_value());
		if (!SourceChild) return;

		Scene Destination;
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, LinkedRootResult.has_value());
		if (!LinkedRootResult) return;
		const Entity LinkedRoot = *LinkedRootResult;
		const auto ChildID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, ChildID.has_value());
		if (!ChildID) return;
		const auto LinkedChild = Destination.FindEntity(*ChildID);
		PF_CHECK(Tests, LinkedChild.has_value());
		if (!LinkedChild) return;

		const auto Local = Destination.CreateEntity("Local child with recorded override");
		PF_CHECK(Tests, Local.has_value());
		if (!Local) return;
		PF_CHECK(Tests, Local->SetParent(LinkedRoot).has_value());
		const std::string TagComponent(PropertyComponentIds::Tag);
		const std::string NameProperty(PropertyIds::Name);
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordPropertyOverride(
			LinkedRoot, *Local, TagComponent, NameProperty, std::string("Local name override")).has_value());
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordPropertyOverride(
			LinkedRoot, *LinkedChild, std::string(PropertyComponentIds::MeshRenderer),
			std::string(PropertyIds::MaterialAsset), std::optional<AssetID>{ FirstOverrideMaterial }).has_value());
		const auto BeforeDelete = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, BeforeDelete && BeforeDelete->has_value() && (**BeforeDelete).LocalEntities.size() == 1 &&
				(**BeforeDelete).Overrides.size() == 1);
		PF_CHECK(Tests, Destination.DestroyEntity(*Local).has_value());

		PF_CHECK(Tests, SourceChild->RemoveMeshRenderer().has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Source).has_value());
		const auto Plan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, LinkedRoot);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, !Plan->CanApply() && Plan->GetSummary().ConflictCount > 0);
		PF_CHECK(Tests, std::any_of(Plan->GetConflicts().begin(), Plan->GetConflicts().end(),
			[&](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
					Conflict.Kind == PrefabConflictKind::MissingProperty;
			}));
		const auto Applied = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *Plan);
		PF_CHECK(Tests, Applied.has_value() && Applied->ConflictCount > 0);
		if (!Applied) return;

		const auto MetadataResult = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, MetadataResult && MetadataResult->has_value());
		if (!MetadataResult || !MetadataResult->has_value()) return;
		const PrefabInstanceComponent& Metadata = **MetadataResult;
		PF_CHECK(Tests, Metadata.LocalEntities.empty());
		PF_CHECK(Tests, std::none_of(Metadata.Overrides.begin(), Metadata.Overrides.end(),
			[](const PrefabOverrideRecord& Override) { return Override.Entity.IsLocal; }));
		PF_CHECK(Tests, std::any_of(Metadata.Conflicts.begin(), Metadata.Conflicts.end(),
			[&](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
					Conflict.Kind == PrefabConflictKind::MissingProperty;
			}));

		const auto Serialized = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		Scene Reopened;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, Reopened).has_value());
		const auto ReopenedRoot = Reopened.FindEntity(LinkedRoot.GetUUID());
		PF_CHECK(Tests, ReopenedRoot.has_value());
		if (!ReopenedRoot) return;
		const auto ReopenedMetadata = ReopenedRoot->GetPrefabInstance();
		PF_CHECK(Tests, ReopenedMetadata && ReopenedMetadata->has_value() &&
				(**ReopenedMetadata).Conflicts == Metadata.Conflicts);
		PF_CHECK(Tests, ReopenedMetadata && ReopenedMetadata->has_value() &&
				(**ReopenedMetadata).LocalEntities.empty() &&
				std::none_of((**ReopenedMetadata).Overrides.begin(), (**ReopenedMetadata).Overrides.end(),
					[](const PrefabOverrideRecord& Override) { return Override.Entity.IsLocal; }));
	}

	void TestLinkedPrefabRejectsNestedSourceBeforeDestinationMutation(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		auto SourceResult = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, SourceResult.has_value());
		if (!SourceResult) return;
		PrefabSourceDocument& Source = *SourceResult;
		const auto NestedRoot = Source.GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, NestedRoot.has_value());
		if (!NestedRoot) return;

		PrefabInstanceComponent NestedInstance;
		NestedInstance.InstanceID = UUID{ 0xac00000000000000ull, 112 };
		NestedInstance.SourceAsset = SecondPrefabAsset;
		NestedInstance.SourceRootKey = NestedSourceRootKey;
		NestedInstance.AppliedSourceFingerprint = "nested-source-fingerprint";
		NestedInstance.SourceEntities = { { NestedSourceRootKey, SourceChildKey } };
		PF_CHECK(Tests, NestedRoot->SetPrefabInstance(NestedInstance).has_value());
		const auto SerializedNestedSource = PrefabSerializer::Serialize(Source);
		PF_CHECK(Tests, !SerializedNestedSource);

		Scene Destination;
		const auto Sentinel = Destination.CreateEntity("Must survive rejected nested prefab");
		PF_CHECK(Tests, Sentinel.has_value());
		if (!Sentinel) return;
		const Entity SentinelHandle = *Sentinel;
		const size_t InitialCount = Destination.GetEntityCount();
		const auto Instantiated = PrefabInstanceReconciler::InstantiateLinked(Source, Destination);
		PF_CHECK(Tests, !Instantiated);
		PF_CHECK(Tests, Destination.GetEntityCount() == InitialCount);
		PF_CHECK(Tests, SentinelHandle.IsValid() && Destination.FindEntity(SentinelHandle.GetUUID()).has_value());
	}

	void TestApplyOverridesToSourceDraftAndRevertInstanceOperations(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		auto SourceResult = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, SourceResult.has_value());
		if (!SourceResult) return;
		PrefabSourceDocument& Source = *SourceResult;

		Scene Destination;
		const auto Unrelated = Destination.CreateEntity("Unrelated authoring entity");
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, Unrelated.has_value() && LinkedRootResult.has_value());
		if (!Unrelated || !LinkedRootResult) return;
		const Entity UnrelatedHandle = *Unrelated;
		const Entity LinkedRoot = *LinkedRootResult;
		TransformComponent Placement;
		Placement.Translation = { 23.0f, 5.0f, -4.0f };
		PF_CHECK(Tests, LinkedRoot.SetTransform(Placement).has_value());
		const auto ChildID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, ChildID.has_value());
		if (!ChildID) return;
		const auto ChildResult = Destination.FindEntity(*ChildID);
		PF_CHECK(Tests, ChildResult.has_value());
		if (!ChildResult) return;
		const Entity Child = *ChildResult;

		const std::string MaterialComponent(PropertyComponentIds::MeshRenderer);
		const std::string MaterialProperty(PropertyIds::MaterialAsset);
		const PropertyValue OverrideMaterial = std::optional<AssetID>{ FirstOverrideMaterial };
		PF_CHECK(Tests, WriteProperty(Child, MaterialComponent, MaterialProperty, OverrideMaterial).has_value());
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordPropertyOverride(
			LinkedRoot, Child, MaterialComponent, MaterialProperty, OverrideMaterial).has_value());
		const auto InstanceName = Child.GetTag();
		PF_CHECK(Tests, InstanceName.has_value());
		if (!InstanceName) return;
		auto RenamedInstance = *InstanceName;
		RenamedInstance.Name = "Instance-only label";
		PF_CHECK(Tests, Child.SetTag(RenamedInstance).has_value());
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordRenameOverride(LinkedRoot, Child, RenamedInstance.Name).has_value());
		const auto MetadataBeforeApply = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, MetadataBeforeApply && MetadataBeforeApply->has_value() && (**MetadataBeforeApply).Overrides.size() == 2);
		if (!MetadataBeforeApply || !MetadataBeforeApply->has_value()) return;

		const auto Applied = PrefabInstanceReconciler::ApplyOverridesToSource(Source, **MetadataBeforeApply);
		PF_CHECK(Tests, Applied.has_value() && Applied->Applied == 2);
		if (!Applied) return;
		const auto SourceChild = Source.GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceChild.has_value());
		if (!SourceChild) return;
		const auto AppliedSourceMaterial = ReadProperty(*SourceChild, MaterialComponent, MaterialProperty);
		const auto AppliedSourceName = SourceChild->GetTag();
		PF_CHECK(Tests, AppliedSourceMaterial && std::get<std::optional<AssetID>>(*AppliedSourceMaterial) ==
				std::optional<AssetID>{ FirstOverrideMaterial });
		PF_CHECK(Tests, AppliedSourceName && AppliedSourceName->Name == "Instance-only label");
		const auto MetadataAfterApply = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, MetadataAfterApply && MetadataAfterApply->has_value() && (**MetadataAfterApply).Overrides.size() == 2);

		// The source draft is the revert baseline; staging these operations does not save the unrelated scene.
		const PropertyValue LatestSourceMaterial = std::optional<AssetID>{ SecondOverrideMaterial };
		PF_CHECK(Tests, WriteProperty(*SourceChild, MaterialComponent, MaterialProperty, LatestSourceMaterial).has_value());
		auto LatestSourceName = SourceChild->GetTag();
		PF_CHECK(Tests, LatestSourceName.has_value());
		if (!LatestSourceName) return;
		LatestSourceName->Name = "Later source draft label";
		PF_CHECK(Tests, SourceChild->SetTag(*LatestSourceName).has_value());
		const auto Reverted = PrefabInstanceReconciler::RevertOverrides(
			Destination, LinkedRoot, Source, PrefabEntityAddress{ SourceChildKey, false });
		PF_CHECK(Tests, Reverted.has_value() && Reverted->Applied == 2);
		if (!Reverted) return;
		const auto RevertedMaterial = ReadProperty(Child, MaterialComponent, MaterialProperty);
		const auto RevertedName = Child.GetTag();
		PF_CHECK(Tests, RevertedMaterial && std::get<std::optional<AssetID>>(*RevertedMaterial) ==
				std::optional<AssetID>{ SecondOverrideMaterial });
		PF_CHECK(Tests, RevertedName && RevertedName->Name == "Later source draft label");
		const auto RevertedMetadata = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, RevertedMetadata && RevertedMetadata->has_value() && (**RevertedMetadata).Overrides.empty());
		const auto KeptPlacement = LinkedRoot.GetTransform();
		PF_CHECK(Tests, KeptPlacement && glm::all(glm::equal(KeptPlacement->Translation, Placement.Translation)));
		PF_CHECK(Tests, Child.IsValid() && UnrelatedHandle.IsValid());

		const auto LocalChild = Destination.CreateEntity("Instance-local child");
		PF_CHECK(Tests, LocalChild.has_value());
		if (!LocalChild) return;
		PF_CHECK(Tests, LocalChild->SetParent(LinkedRoot).has_value());
		const auto LocalAddress = PrefabInstanceReconciler::ResolveOrRegisterEntityAddress(LinkedRoot, *LocalChild);
		PF_CHECK(Tests, LocalAddress && LocalAddress->IsLocal);
		PrefabOverrideRecord Removal;
		Removal.Entity = { SourceChildKey, false };
		Removal.Operation = PrefabOverrideOperation::RemoveSourceEntity;
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordOverride(LinkedRoot, Removal).has_value());
		PF_CHECK(Tests, Destination.DestroyEntity(Child).has_value());
		PF_CHECK(Tests, !Child.IsValid());
		const auto Restored = PrefabInstanceReconciler::RevertOverrides(Destination, LinkedRoot, Source);
		PF_CHECK(Tests, Restored.has_value() && Restored->Applied == 1);
		if (!Restored) return;
		const auto RestoredID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, RestoredID.has_value() && *RestoredID != *ChildID);
		if (!RestoredID) return;
		const auto RestoredChild = Destination.FindEntity(*RestoredID);
		PF_CHECK(Tests, RestoredChild.has_value());
		if (RestoredChild)
		{
			const auto RestoredMaterial = ReadProperty(*RestoredChild, MaterialComponent, MaterialProperty);
			PF_CHECK(Tests, RestoredMaterial && std::get<std::optional<AssetID>>(*RestoredMaterial) ==
					std::optional<AssetID>{ SecondOverrideMaterial });
		}
		PF_CHECK(Tests, LocalChild->IsValid() && UnrelatedHandle.IsValid() && LinkedRoot.IsValid());
	}

	void TestDuplicateMissingPrefabMappingTargetsRejected(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		Scene Destination;
		const auto FirstRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		const auto SecondRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, FirstRootResult.has_value() && SecondRootResult.has_value());
		if (!FirstRootResult || !SecondRootResult) return;
		const Entity FirstRoot = *FirstRootResult;
		const Entity SecondRoot = *SecondRootResult;
		const auto FirstChildID = FindMappedEntity(FirstRoot, SourceChildKey);
		const auto SecondChildID = FindMappedEntity(SecondRoot, SourceChildKey);
		PF_CHECK(Tests, FirstChildID.has_value() && SecondChildID.has_value() && FirstChildID != SecondChildID);
		if (!FirstChildID || !SecondChildID) return;

		const auto SecondMetadataResult = SecondRoot.GetPrefabInstance();
		PF_CHECK(Tests, SecondMetadataResult && SecondMetadataResult->has_value());
		if (!SecondMetadataResult || !SecondMetadataResult->has_value()) return;
		PrefabInstanceComponent SecondMetadata = **SecondMetadataResult;
		auto SecondMapping = std::find_if(SecondMetadata.SourceEntities.begin(), SecondMetadata.SourceEntities.end(),
			[](const PrefabEntityMapping& Mapping) { return Mapping.SourceKey == SourceChildKey; });
		PF_CHECK(Tests, SecondMapping != SecondMetadata.SourceEntities.end());
		if (SecondMapping == SecondMetadata.SourceEntities.end()) return;
		SecondMapping->SceneEntity = *FirstChildID;
		PF_CHECK(Tests, SecondRoot.SetPrefabInstance(SecondMetadata).has_value());

		const auto FirstChild = Destination.FindEntity(*FirstChildID);
		const auto SecondChild = Destination.FindEntity(*SecondChildID);
		PF_CHECK(Tests, FirstChild.has_value() && SecondChild.has_value());
		if (!FirstChild || !SecondChild) return;
		PF_CHECK(Tests, Destination.DestroyEntity(*FirstChild).has_value());
		PF_CHECK(Tests, Destination.DestroyEntity(*SecondChild).has_value());
		PF_CHECK(Tests, !Destination.FindEntity(*FirstChildID).has_value());
		PF_CHECK(Tests, !Destination.FindEntity(*SecondChildID).has_value());

		const auto Serialized = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, !Serialized && Serialized.error().Code == SceneSerializationErrorCode::InvalidEntityData);
	}

	void TestChangedPrefabOverrideTypePreservesMaterializedValue(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		Scene Destination;
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, LinkedRootResult.has_value());
		if (!LinkedRootResult) return;
		const Entity LinkedRoot = *LinkedRootResult;
		const auto ChildID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, ChildID.has_value());
		if (!ChildID) return;
		const auto Child = Destination.FindEntity(*ChildID);
		PF_CHECK(Tests, Child.has_value());
		if (!Child) return;

		const glm::vec3 MaterializedTranslation{ 14.0f, -3.0f, 6.0f };
		PF_CHECK(Tests, WriteProperty(*Child, PropertyComponentIds::Transform, PropertyIds::Translation,
			PropertyValue{ MaterializedTranslation }).has_value());
		const auto MetadataResult = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, MetadataResult && MetadataResult->has_value());
		if (!MetadataResult || !MetadataResult->has_value()) return;
		PrefabInstanceComponent Metadata = **MetadataResult;
		PrefabOverrideRecord MismatchedOverride;
		MismatchedOverride.Entity = { SourceChildKey, false };
		MismatchedOverride.Operation = PrefabOverrideOperation::SetProperty;
		MismatchedOverride.ComponentID = std::string(PropertyComponentIds::Transform);
		MismatchedOverride.PropertyID = std::string(PropertyIds::Translation);
		MismatchedOverride.Value = std::string("The authored property is now a string");
		Metadata.Overrides.push_back(MismatchedOverride);
		PF_CHECK(Tests, LinkedRoot.SetPrefabInstance(Metadata).has_value());

		const auto Plan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, LinkedRoot);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, !Plan->CanApply());
		PF_CHECK(Tests, std::any_of(Plan->GetConflicts().begin(), Plan->GetConflicts().end(),
			[](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
					Conflict.Kind == PrefabConflictKind::ChangedPropertyType &&
					Conflict.ComponentID == PropertyComponentIds::Transform &&
					Conflict.PropertyID == PropertyIds::Translation;
			}));
		PF_CHECK(Tests, PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *Plan).has_value());
		const auto PreservedTransform = Child->GetTransform();
		PF_CHECK(Tests, PreservedTransform && glm::all(glm::equal(PreservedTransform->Translation, MaterializedTranslation)));
		const auto PreservedMetadata = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->has_value() && (**PreservedMetadata).Overrides.size() == 1 &&
				(**PreservedMetadata).Overrides.front() == MismatchedOverride &&
				std::any_of((**PreservedMetadata).Conflicts.begin(), (**PreservedMetadata).Conflicts.end(),
					[](const PrefabConflictRecord& Conflict) { return Conflict.Kind == PrefabConflictKind::ChangedPropertyType; }));
	}

	void TestPrefabSourcePrimaryCameraAndListenerPolicy(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		auto Source = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, Source.has_value());
		if (!Source) return;
		const auto SourceRoot = Source->GetScene().FindEntity(SourceRootKey);
		PF_CHECK(Tests, SourceRoot.has_value());
		if (!SourceRoot) return;
		CameraComponent SourceCamera;
		SourceCamera.IsPrimary = true;
		SourceCamera.VerticalFieldOfViewRadians = 1.0f;
		PF_CHECK(Tests, SourceRoot->SetCamera(SourceCamera).has_value());
		PF_CHECK(Tests, SourceRoot->SetAudioListener(AudioListenerComponent{ true }).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, *Source).has_value());

		auto ReloadedSource = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, ReloadedSource.has_value());
		if (!ReloadedSource) return;
		const auto ReloadedRoot = ReloadedSource->GetScene().FindEntity(SourceRootKey);
		PF_CHECK(Tests, ReloadedRoot.has_value());
		if (!ReloadedRoot) return;
		const auto AuthoredCamera = ReloadedRoot->GetCamera();
		const auto AuthoredListener = ReloadedRoot->GetAudioListener();
		PF_CHECK(Tests, AuthoredCamera && AuthoredCamera->has_value() && (**AuthoredCamera).IsPrimary &&
				(**AuthoredCamera).VerticalFieldOfViewRadians == SourceCamera.VerticalFieldOfViewRadians);
		PF_CHECK(Tests, AuthoredListener && AuthoredListener->has_value() && (**AuthoredListener).IsPrimary);

		Scene Destination;
		auto LinkedRoot = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, LinkedRoot.has_value());
		if (!LinkedRoot) return;
		const auto InstanceCamera = LinkedRoot->GetCamera();
		const auto InstanceListener = LinkedRoot->GetAudioListener();
		PF_CHECK(Tests, InstanceCamera && InstanceCamera->has_value() && !(**InstanceCamera).IsPrimary);
		PF_CHECK(Tests, InstanceListener && InstanceListener->has_value() && !(**InstanceListener).IsPrimary);

		CameraComponent UpdatedSourceCamera = SourceCamera;
		UpdatedSourceCamera.VerticalFieldOfViewRadians = 1.2f;
		PF_CHECK(Tests, ReloadedRoot->SetCamera(UpdatedSourceCamera).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, *ReloadedSource).has_value());
		const auto Plan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *LinkedRoot);
		PF_CHECK(Tests, Plan.has_value() && Plan->CanApply());
		if (!Plan) return;
		PF_CHECK(Tests, PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *Plan).has_value());
		const auto UpdatedInstanceCamera = LinkedRoot->GetCamera();
		const auto UpdatedInstanceListener = LinkedRoot->GetAudioListener();
		PF_CHECK(Tests, UpdatedInstanceCamera && UpdatedInstanceCamera->has_value() &&
				(**UpdatedInstanceCamera).VerticalFieldOfViewRadians == UpdatedSourceCamera.VerticalFieldOfViewRadians &&
				!(**UpdatedInstanceCamera).IsPrimary);
		PF_CHECK(Tests, UpdatedInstanceListener && UpdatedInstanceListener->has_value() && !(**UpdatedInstanceListener).IsPrimary);
	}

	void TestMissingSourceConflictPreservesMaterializedInstance(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		auto Source = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, Source.has_value());
		if (!Source) return;

		Scene Destination;
		const auto Root = PrefabAssetService::InstantiateLinked(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, Root.has_value());
		if (!Root) return;
		const auto ChildID = FindMappedEntity(*Root, SourceChildKey);
		PF_CHECK(Tests, ChildID.has_value());
		if (!ChildID) return;
		const auto InitialMetadata = Root->GetPrefabInstance();
		PF_CHECK(Tests, InitialMetadata && InitialMetadata->has_value());
		if (!InitialMetadata || !InitialMetadata->has_value()) return;
		const UUID RootID = Root->GetUUID();
		const UUID InstanceID = (**InitialMetadata).InstanceID;
		const size_t MaterializedEntityCount = Destination.GetEntityCount();
		const auto BeforeMissingPlan = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, BeforeMissingPlan.has_value());
		if (!BeforeMissingPlan) return;

		struct SourceFileBackup final
		{
			std::filesystem::path SourcePath;
			std::filesystem::path BackupPath;
			bool IsMoved = false;

			~SourceFileBackup()
			{
				if (!IsMoved) return;
				std::error_code Error;
				std::filesystem::rename(BackupPath, SourcePath, Error);
			}

			[[nodiscard]] bool MoveAway()
			{
				std::error_code Error;
				std::filesystem::rename(SourcePath, BackupPath, Error);
				IsMoved = !Error;
				return IsMoved;
			}

			[[nodiscard]] bool Restore()
			{
				if (!IsMoved) return true;
				std::error_code Error;
				std::filesystem::rename(BackupPath, SourcePath, Error);
				if (Error) return false;
				IsMoved = false;
				return true;
			}
		};
		const std::filesystem::path SourcePath = Fixture.ProjectRoot / "Assets" / "Prefabs" / "linked.prefab";
		SourceFileBackup Backup{ SourcePath,
			SourcePath.parent_path() / (SourcePath.filename().string() + ".missing-source-backup") };
		PF_CHECK(Tests, Backup.MoveAway());
		if (!Backup.IsMoved) return;
		const auto MissingSourcePlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *Root);
		PF_CHECK(Tests, !MissingSourcePlan && MissingSourcePlan.error().Code == PrefabAssetErrorCode::SourceFileMissing);
		const auto AfterMissingPlan = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, AfterMissingPlan.has_value() && *AfterMissingPlan == *BeforeMissingPlan);
		PF_CHECK(Tests, Root->IsValid() && Destination.FindEntity(*ChildID).has_value());

		PF_CHECK(Tests, PrefabInstanceReconciler::RecordMissingSourceAssetConflict(
			*Root, "The linked prefab file is missing; the materialized instance remains available for recovery").has_value());
		const auto Serialized = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		Scene Reopened;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, Reopened).has_value());
		const auto ReopenedRoot = Reopened.FindEntity(Root->GetUUID());
		PF_CHECK(Tests, ReopenedRoot.has_value());
		if (!ReopenedRoot) return;
		const auto Metadata = ReopenedRoot->GetPrefabInstance();
		PF_CHECK(Tests, Metadata && Metadata->has_value() && (**Metadata).Conflicts.size() == 1);
		if (!Metadata || !Metadata->has_value() || (**Metadata).Conflicts.size() != 1) return;
		PF_CHECK(Tests, (**Metadata).Conflicts.front().Kind == PrefabConflictKind::MissingSourceAsset);
		PF_CHECK(Tests, Reopened.GetEntityCount() == MaterializedEntityCount);
		PF_CHECK(Tests, Reopened.FindEntity(*ChildID).has_value());
		PF_CHECK(Tests, FindMappedEntity(*ReopenedRoot, SourceChildKey) == ChildID);
		PF_CHECK(Tests, (**Metadata).InstanceID == InstanceID);

		PF_CHECK(Tests, Backup.Restore());
		if (Backup.IsMoved) return;
		auto RecoveredSource = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, RecoveredSource.has_value());
		if (!RecoveredSource) return;

		const auto RecoveryPlan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Reopened, *ReopenedRoot);
		PF_CHECK(Tests, RecoveryPlan.has_value() && RecoveryPlan->CanApply());
		if (!RecoveryPlan) return;
		PF_CHECK(Tests, PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Reopened, *RecoveryPlan).has_value());
		const auto RecoveredRoot = Reopened.FindEntity(RootID);
		PF_CHECK(Tests, RecoveredRoot.has_value());
		if (!RecoveredRoot) return;
		const auto RecoveredMetadata = RecoveredRoot->GetPrefabInstance();
		PF_CHECK(Tests, RecoveredMetadata && RecoveredMetadata->has_value() && (**RecoveredMetadata).Conflicts.empty() &&
				(**RecoveredMetadata).InstanceID == InstanceID &&
				(**RecoveredMetadata).AppliedSourceFingerprint == CanonicalPrefabFingerprint(*RecoveredSource));
		PF_CHECK(Tests, Reopened.FindEntity(*ChildID).has_value());
		PF_CHECK(Tests, FindMappedEntity(*RecoveredRoot, SourceChildKey) == ChildID);
	}

	void TestRemovedSourceEntityWithLocalChildrenRecordsConflict(TestRunner& Tests)
	{
		LinkedPrefabFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		auto Source = PrefabAssetService::LoadSource(Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry);
		PF_CHECK(Tests, Source.has_value());
		if (!Source) return;
		const auto SourceRoot = Source->GetScene().FindEntity(SourceRootKey);
		const auto SourceChild = Source->GetScene().FindEntity(SourceChildKey);
		PF_CHECK(Tests, SourceRoot.has_value() && SourceChild.has_value());
		if (!SourceRoot || !SourceChild) return;

		Scene Destination;
		const auto LinkedRootResult = PrefabAssetService::InstantiateLinked(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination);
		PF_CHECK(Tests, LinkedRootResult.has_value());
		if (!LinkedRootResult) return;
		const Entity LinkedRoot = *LinkedRootResult;
		const auto LinkedChildID = FindMappedEntity(LinkedRoot, SourceChildKey);
		PF_CHECK(Tests, LinkedChildID.has_value());
		if (!LinkedChildID) return;
		const auto LinkedChildResult = Destination.FindEntity(*LinkedChildID);
		PF_CHECK(Tests, LinkedChildResult.has_value());
		if (!LinkedChildResult) return;
		const Entity LinkedChild = *LinkedChildResult;
		const auto LocalChildResult = Destination.CreateEntity("Local child under a source entity");
		PF_CHECK(Tests, LocalChildResult.has_value());
		if (!LocalChildResult) return;
		const Entity LocalChild = *LocalChildResult;
		PF_CHECK(Tests, LocalChild.SetParent(LinkedChild).has_value());
		const auto InitialMetadata = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, InitialMetadata && InitialMetadata->has_value());
		if (!InitialMetadata || !InitialMetadata->has_value()) return;
		const std::string OriginalFingerprint = (**InitialMetadata).AppliedSourceFingerprint;
		const size_t OriginalEntityCount = Destination.GetEntityCount();

		// Add another source change so the conflict plan must preserve the whole materialized instance.
		auto SourceRootTag = SourceRoot->GetTag();
		PF_CHECK(Tests, SourceRootTag.has_value());
		if (!SourceRootTag) return;
		SourceRootTag->Name = "Updated source root that must wait for conflict resolution";
		PF_CHECK(Tests, SourceRoot->SetTag(*SourceRootTag).has_value());
		PF_CHECK(Tests, Source->GetScene().DestroyEntity(*SourceChild).has_value());
		const auto AddedSource = Source->GetScene().CreateEntityWithUUID(AddedSourceEntityKey, "Pending source addition");
		PF_CHECK(Tests, AddedSource.has_value());
		if (!AddedSource) return;
		PF_CHECK(Tests, AddedSource->SetParent(*SourceRoot).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, *Source).has_value());

		const auto Plan = PrefabAssetService::PlanReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, LinkedRoot);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, !Plan->CanApply() && Plan->GetSummary().ConflictCount > 0);
		PF_CHECK(Tests, std::any_of(Plan->GetConflicts().begin(), Plan->GetConflicts().end(),
			[&](const PrefabConflictRecord& Conflict)
			{
				return Conflict.Entity == PrefabEntityAddress{ SourceChildKey, false } &&
					Conflict.Kind == PrefabConflictKind::RemovedEntityHasLocalChildren;
			}));
		const auto Applied = PrefabAssetService::ApplyReconcile(
			Fixture.Asset, Fixture.ProjectRoot, Fixture.Registry, Destination, *Plan);
		PF_CHECK(Tests, Applied.has_value() && Applied->ConflictCount > 0);
		if (!Applied) return;

		PF_CHECK(Tests, Destination.GetEntityCount() == OriginalEntityCount);
		PF_CHECK(Tests, LinkedRoot.IsValid() && LinkedChild.IsValid() && LocalChild.IsValid());
		PF_CHECK(Tests, FindMappedEntity(LinkedRoot, SourceChildKey) == LinkedChildID);
		PF_CHECK(Tests, !FindMappedEntity(LinkedRoot, AddedSourceEntityKey).has_value());
		const auto LinkedChildParent = LocalChild.GetParent();
		PF_CHECK(Tests, LinkedChildParent && LinkedChildParent->has_value() && **LinkedChildParent == LinkedChild);
		const auto UpdatedMetadata = LinkedRoot.GetPrefabInstance();
		PF_CHECK(Tests, UpdatedMetadata && UpdatedMetadata->has_value());
		if (!UpdatedMetadata || !UpdatedMetadata->has_value()) return;
		PF_CHECK(Tests, (**UpdatedMetadata).AppliedSourceFingerprint == OriginalFingerprint);
		PF_CHECK(Tests, (**UpdatedMetadata).LocalEntities.size() == 1 &&
				(**UpdatedMetadata).LocalEntities.front().SceneEntity == LocalChild.GetUUID());
		PF_CHECK(Tests, (**UpdatedMetadata).Conflicts.size() == 1 &&
				(**UpdatedMetadata).Conflicts.front().Kind == PrefabConflictKind::RemovedEntityHasLocalChildren);
		const auto PreservedRootTag = LinkedRoot.GetTag();
		PF_CHECK(Tests, PreservedRootTag && PreservedRootTag->Name == "Source root");

		const auto Serialized = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		Scene Reopened;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, Reopened).has_value());
		const auto ReopenedRoot = Reopened.FindEntity(LinkedRoot.GetUUID());
		const auto ReopenedLinkedChild = Reopened.FindEntity(*LinkedChildID);
		const auto ReopenedLocalChild = Reopened.FindEntity(LocalChild.GetUUID());
		PF_CHECK(Tests, ReopenedRoot.has_value() && ReopenedLinkedChild.has_value() && ReopenedLocalChild.has_value());
		if (!ReopenedRoot || !ReopenedLinkedChild || !ReopenedLocalChild) return;
		const auto ReopenedMetadata = ReopenedRoot->GetPrefabInstance();
		PF_CHECK(Tests, ReopenedMetadata && ReopenedMetadata->has_value() &&
				(**ReopenedMetadata).Conflicts.size() == 1 &&
				(**ReopenedMetadata).Conflicts.front().Kind == PrefabConflictKind::RemovedEntityHasLocalChildren &&
				(**ReopenedMetadata).SourceEntities == (**UpdatedMetadata).SourceEntities &&
				(**ReopenedMetadata).LocalEntities == (**UpdatedMetadata).LocalEntities);
		const auto ReopenedLocalParent = ReopenedLocalChild->GetParent();
		PF_CHECK(Tests, ReopenedLocalParent && ReopenedLocalParent->has_value() && **ReopenedLocalParent == *ReopenedLinkedChild);
	}
} // namespace

void RunPrefabLinkedInstanceTests(TestRunner& Tests)
{
	TestPrefabInstanceMetadataRoundTripAndValidation(Tests);
	TestPrefabInstanceMappingsRemainStableAndDisjoint(Tests);
	TestLegacyScenesLoadWithoutLinkedPrefabMetadata(Tests);
	TestLinkedInstantiationReconcileOverridesAndStalePlans(Tests);
	TestDuplicateMissingPrefabMappingTargetsRejected(Tests);
	TestChangedPrefabOverrideTypePreservesMaterializedValue(Tests);
	TestPrefabSourcePrimaryCameraAndListenerPolicy(Tests);
	TestPrefabSourceSaveRejectsDetachedEntitiesWithoutDroppingThem(Tests);
	TestLinkedMappedChildCannotBeDetached(Tests);
	TestSourceDeleteAndSameNameRecreateUsesFreshLiveEntity(Tests);
	TestDetachedRemovedSourceConflictRetainsMappingAfterSourceDeletion(Tests);
	TestDeletedLocalOverridePrunesBeforePersistingOtherConflicts(Tests);
	TestLinkedPrefabRejectsNestedSourceBeforeDestinationMutation(Tests);
	TestApplyOverridesToSourceDraftAndRevertInstanceOperations(Tests);
	TestRemovedSourceEntityWithLocalChildrenRecordsConflict(Tests);
	TestMissingSourceConflictPreservesMaterializedInstance(Tests);
}
