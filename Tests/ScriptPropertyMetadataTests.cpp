#include "TestSceneSupport.h"

#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/PropertyMetadata.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

void RunScriptPropertyMetadataTests(TestRunner& Tests)
{
	using namespace PulseForge;
	Scene SceneValue;
	auto Created = SceneValue.CreateEntity("Property metadata target");
	PF_CHECK(Tests, Created.has_value());
	if (!Created) return;
	const Entity Target = *Created;

	const PropertyDescriptor* MeshDescriptor = FindPropertyDescriptor(PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset);
	PF_CHECK(Tests, MeshDescriptor && MeshDescriptor->Type == PropertyValueType::AssetId &&
						MeshDescriptor->ReferenceKind == PropertyReferenceKind::MeshAsset);
	MeshRendererComponent Mesh;
	Mesh.MeshAsset = UUID{ 0x123456789abcdef0ull, 0x1020304050607080ull };
	Mesh.MaterialAsset = AssetID{ 0x1111222233334444ull, 0x5555666677778888ull };
	PF_CHECK(Tests, Target.SetMeshRenderer(Mesh).has_value());
	auto MeshValue = ReadProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset);
	PF_CHECK(Tests, MeshValue && std::get<AssetID>(*MeshValue) == Mesh.MeshAsset);
	const AssetID ReplacementMesh{ 0x9988776655443322ull, 0x1234123412341234ull };
	PF_CHECK(Tests, WriteProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset, PropertyValue{ ReplacementMesh }));
	MeshValue = ReadProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset);
	PF_CHECK(Tests, MeshValue && std::get<AssetID>(*MeshValue) == ReplacementMesh);
	const auto MaterialValue = ReadProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MaterialAsset);
	PF_CHECK(Tests, MaterialValue && std::get<std::optional<AssetID>>(*MaterialValue) == Mesh.MaterialAsset);
	PF_CHECK(Tests, !WriteProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset, PropertyValue{ AssetID{} }));
	PF_CHECK(Tests, !WriteProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset, PropertyValue{ true }));
	MeshValue = ReadProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset);
	PF_CHECK(Tests, MeshValue && std::get<AssetID>(*MeshValue) == ReplacementMesh);
	PF_CHECK(Tests,
		!WriteProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MeshAsset, PropertyValue{ std::optional<AssetID>{} }));

	const AssetID ScriptAsset{ 0xabcdef0123456789ull, 0x8765432101234567ull };
	ScriptComponent Script{ ScriptAsset };
	Script.FloatProperties.emplace("asset", 9.0f);
	Script.FloatProperties.emplace("enabled", 2.0f);
	Script.FloatProperties.emplace("move.speed", 4.0f);
	PF_CHECK(Tests, Target.SetScript(Script).has_value());
	auto Dynamic = GetDynamicPropertyDescriptors(Target, PropertyComponentIds::ScriptFloat);
	PF_CHECK(Tests, Dynamic && Dynamic->size() == 3);
	if (Dynamic)
	{
		PF_CHECK(Tests, std::all_of(Dynamic->begin(), Dynamic->end(),
							[](const DynamicPropertyDescriptor& Descriptor)
							{
								return Descriptor.ComponentId == PropertyComponentIds::ScriptFloat &&
									   Descriptor.Type == PropertyValueType::Float && Descriptor.Editable && !Descriptor.DefaultValue &&
									   Descriptor.HardMinimum == -MaximumScriptFloatPropertyMagnitude &&
									   Descriptor.HardMaximum == MaximumScriptFloatPropertyMagnitude;
							}));
		PF_CHECK(Tests, std::any_of(Dynamic->begin(), Dynamic->end(), [](const DynamicPropertyDescriptor& Descriptor)
							{ return Descriptor.Id == "asset" && Descriptor.Label == "asset"; }));
	}
	auto AuthoredAsset = ReadProperty(Target, PropertyComponentIds::ScriptFloat, "asset");
	PF_CHECK(Tests, AuthoredAsset && std::get<float>(*AuthoredAsset) == 9.0f);
	auto StaticAsset = ReadProperty(Target, PropertyComponentIds::Script, PropertyIds::Asset);
	PF_CHECK(Tests, StaticAsset && std::get<AssetID>(*StaticAsset) == ScriptAsset);
	auto StaticEnabled = ReadProperty(Target, PropertyComponentIds::Script, PropertyIds::Enabled);
	PF_CHECK(Tests, StaticEnabled && std::get<bool>(*StaticEnabled));
	auto AuthoredEnabled = ReadProperty(Target, PropertyComponentIds::ScriptFloat, "enabled");
	PF_CHECK(Tests, AuthoredEnabled && std::get<float>(*AuthoredEnabled) == 2.0f);
	PF_CHECK(Tests, !ReadProperty(Target, PropertyComponentIds::ScriptFloat, "missing"));
	PF_CHECK(Tests, !ReadProperty(Target, PropertyComponentIds::ScriptFloat, "bad id"));
	PF_CHECK(Tests, WriteProperty(Target, PropertyComponentIds::ScriptFloat, "move.speed", PropertyValue{ 5.0f }));
	PF_CHECK(Tests, !WriteProperty(Target, PropertyComponentIds::ScriptFloat, "move.speed", PropertyValue{ true }));
	PF_CHECK(Tests,
		!WriteProperty(Target, PropertyComponentIds::ScriptFloat, "move.speed", PropertyValue{ std::numeric_limits<float>::infinity() }));
	auto PreservedValue = ReadProperty(Target, PropertyComponentIds::ScriptFloat, "move.speed");
	PF_CHECK(Tests, PreservedValue && std::get<float>(*PreservedValue) == 5.0f);
	PF_CHECK(Tests, AddScriptFloatProperty(Target, "new.value", -3.0f));
	PF_CHECK(Tests, !AddScriptFloatProperty(Target, "new.value", 2.0f));
	PF_CHECK(Tests, !AddScriptFloatProperty(Target, "bad id", 2.0f));
	PF_CHECK(Tests, !AddScriptFloatProperty(Target, "overflow", std::numeric_limits<float>::infinity()));
	PF_CHECK(Tests, RemoveScriptFloatProperty(Target, "new.value"));
	PF_CHECK(Tests, !RemoveScriptFloatProperty(Target, "new.value"));
	PF_CHECK(Tests, RemoveScriptFloatProperty(Target, "asset"));
	PF_CHECK(Tests, Dynamic && Dynamic->front().Id == "asset" && Dynamic->front().Label == "asset");
	auto CurrentScript = Target.GetScript();
	PF_CHECK(Tests, CurrentScript && CurrentScript->has_value());
	if (!CurrentScript || !CurrentScript->has_value()) return;
	for (size_t Index = CurrentScript->value().FloatProperties.size(); Index < MaximumScriptFloatPropertyCount; ++Index)
		PF_CHECK(Tests, AddScriptFloatProperty(Target, "value." + std::to_string(Index), static_cast<float>(Index)));
	PF_CHECK(Tests, !AddScriptFloatProperty(Target, "too.many", 1.0f));
	Dynamic = GetDynamicPropertyDescriptors(Target, PropertyComponentIds::ScriptFloat);
	PF_CHECK(Tests, Dynamic && Dynamic->size() == MaximumScriptFloatPropertyCount);
	auto ScriptAfter = Target.GetScript();
	PF_CHECK(Tests, ScriptAfter && ScriptAfter->has_value() && ScriptAfter->value().FindFloatProperty("move.speed") == 5.0f &&
						ScriptAfter->value().FindFloatProperty("new.value") == std::nullopt);
	PF_CHECK(Tests, !ReadProperty(Target, PropertyComponentIds::ScriptFloat, "asset"));
	const UUID ReusedID = Target.GetUUID();
	PF_CHECK(Tests, SceneValue.DestroyEntity(Target).has_value());
	auto Replacement = SceneValue.CreateEntityWithUUID(ReusedID, "Reused property target");
	PF_CHECK(Tests, Replacement.has_value());
	if (Replacement)
	{
		PF_CHECK(Tests, Replacement->SetMeshRenderer(MeshRendererComponent{ ReplacementMesh }).has_value());
		PF_CHECK(Tests, Replacement->SetScript(ScriptComponent{ ScriptAsset }).has_value());
		PF_CHECK(Tests, AddScriptFloatProperty(*Replacement, "move.speed", 100.0f).has_value());
		PF_CHECK(Tests, !ReadProperty(Target, PropertyComponentIds::ScriptFloat, "move.speed"));
		auto ReplacementValue = ReadProperty(*Replacement, PropertyComponentIds::ScriptFloat, "move.speed");
		PF_CHECK(Tests, ReplacementValue && std::get<float>(*ReplacementValue) == 100.0f);
	}
}
