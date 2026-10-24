#include "TestSceneSupport.h"

#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/PropertyMetadata.h"

#include <algorithm>
#include <limits>
#include <variant>

void RunPropertyMetadataTests(TestRunner& Tests)
{
	using namespace PulseForge;
	Scene SceneValue;
	const UUID Identifier{ 0x1111222233334444ull, 0x5555666677778888ull };
	auto EntityResult = SceneValue.CreateEntityWithUUID(Identifier, "Metadata target");
	PF_CHECK(Tests, EntityResult.has_value());
	if (!EntityResult) return;
	const Entity Target = *EntityResult;

	const auto Descriptors = GetPropertyDescriptors();
	PF_CHECK(Tests, !Descriptors.empty());
	PF_CHECK(Tests, std::all_of(Descriptors.begin(), Descriptors.end(),
						[](const PropertyDescriptor& Descriptor)
						{
							return !Descriptor.ComponentId.empty() && !Descriptor.Id.empty() && !Descriptor.Label.empty() &&
								   Descriptor.Type == static_cast<PropertyValueType>(Descriptor.DefaultValue.index());
						}));
	for (size_t First = 0; First < Descriptors.size(); ++First)
		for (size_t Second = First + 1; Second < Descriptors.size(); ++Second)
			PF_CHECK(Tests,
				Descriptors[First].ComponentId != Descriptors[Second].ComponentId || Descriptors[First].Id != Descriptors[Second].Id);
	PF_CHECK(Tests, FindPropertyDescriptor(PropertyComponentIds::Transform, "translation") != nullptr);
	PF_CHECK(Tests, FindPropertyDescriptor("TransformComponent", "translation") == nullptr);
	PF_CHECK(Tests, FindPropertyDescriptor(PropertyComponentIds::Transform, "display label") == nullptr);

	auto Translation = ReadProperty(Target, PropertyComponentIds::Transform, PropertyIds::Translation);
	PF_CHECK(Tests, Translation && std::get<glm::vec3>(*Translation) == glm::vec3(0.0f));
	PF_CHECK(Tests, !ReadProperty(Target, "missing-component", "value"));
	PF_CHECK(Tests, !ReadProperty(Target, PropertyComponentIds::PointLight, PropertyIds::Intensity));
	PF_CHECK(Tests, !WriteProperty(Target, PropertyComponentIds::Transform, PropertyIds::Translation, PropertyValue{ 2.0f }));

	DirectionalLightComponent Light;
	PF_CHECK(Tests, Target.SetDirectionalLight(Light).has_value());
	auto Intensity = ReadProperty(Target, PropertyComponentIds::DirectionalLight, PropertyIds::Intensity);
	PF_CHECK(Tests, Intensity && std::get<float>(*Intensity) == Light.Intensity);
	PF_CHECK(Tests, !WriteProperty(Target, PropertyComponentIds::DirectionalLight, PropertyIds::Intensity,
						PropertyValue{ std::numeric_limits<float>::infinity() }));
	auto UnchangedIntensity = ReadProperty(Target, PropertyComponentIds::DirectionalLight, PropertyIds::Intensity);
	PF_CHECK(Tests, UnchangedIntensity && std::get<float>(*UnchangedIntensity) == Light.Intensity);
	PF_CHECK(Tests, WriteProperty(Target, PropertyComponentIds::DirectionalLight, PropertyIds::Intensity, PropertyValue{ 3.0f }));
	Intensity = ReadProperty(Target, PropertyComponentIds::DirectionalLight, PropertyIds::Intensity);
	PF_CHECK(Tests, Intensity && std::get<float>(*Intensity) == 3.0f);

	MeshRendererComponent Mesh;
	Mesh.MeshAsset = UUID{ 0xabcdef0123456789ull, 0xabcdef0123456789ull };
	PF_CHECK(Tests, Target.SetMeshRenderer(Mesh).has_value());
	auto Material = ReadProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MaterialAsset);
	PF_CHECK(Tests, Material && std::get<std::optional<AssetID>>(*Material) == std::nullopt);
	const AssetID MaterialId{ 0x1020304050607080ull, 0x9080706050403020ull };
	PF_CHECK(Tests, WriteProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MaterialAsset,
						PropertyValue{ std::optional<AssetID>{ MaterialId } }));
	Material = ReadProperty(Target, PropertyComponentIds::MeshRenderer, PropertyIds::MaterialAsset);
	PF_CHECK(Tests, Material && std::get<std::optional<AssetID>>(*Material) == MaterialId);

	const AssetID ScriptId{ 0x123456789abcdef0ull, 0x0fedcba987654321ull };
	ScriptComponent Script{ ScriptId };
	Script.FloatProperties.emplace("move.speed", 4.0f);
	PF_CHECK(Tests, Target.SetScript(Script).has_value());
	PF_CHECK(Tests, WriteProperty(Target, PropertyComponentIds::Script, PropertyIds::Enabled, PropertyValue{ false }));
	const auto ScriptAfter = Target.GetScript();
	PF_CHECK(Tests, ScriptAfter && ScriptAfter->has_value() && !ScriptAfter->value().Enabled &&
						ScriptAfter->value().FindFloatProperty("move.speed") == 4.0f);

	const auto Deleted = SceneValue.DestroyEntity(Target);
	PF_CHECK(Tests, Deleted.has_value());
	PF_CHECK(Tests, !ReadProperty(Target, PropertyComponentIds::Transform, PropertyIds::Translation));
}
