#include "Assets/PrefabAssetService.h"
#include "Assets/PrefabInstanceReconciler.h"
#include "Assets/PrefabSerializer.h"
#include "Scene/Entity.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/PrefabInstanceComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/PropertyMetadata.h"
#include "Scene/Scene.h"

#include <type_traits>
#include <variant>

static_assert(std::is_move_constructible_v<PulseForge::PrefabSourceDocument>);
static_assert(std::is_move_constructible_v<PulseForge::PrefabReconcilePlan>);

int main()
{
	PulseForge::Scene Scene;
	auto Entity = Scene.CreateEntity("Runtime metadata consumer");
	if (!Entity) return 1;
	const auto Value = PulseForge::ReadProperty(*Entity, PulseForge::PropertyComponentIds::Transform, PulseForge::PropertyIds::Translation);
	if (!Value || !std::holds_alternative<glm::vec3>(*Value)) return 2;
	const PulseForge::UUID ScriptID{ 0x1122334455667788ull, 0x8877665544332211ull };
	if (!Entity->SetScript(PulseForge::ScriptComponent{ ScriptID })) return 3;
	if (!PulseForge::AddScriptFloatProperty(*Entity, "health.value", 12.5f)) return 4;
	const auto ScriptProperties = PulseForge::GetDynamicPropertyDescriptors(*Entity, PulseForge::PropertyComponentIds::ScriptFloat);
	if (!ScriptProperties || ScriptProperties->size() != 1 || ScriptProperties->front().Id != "health.value") return 5;
	const auto ScriptValue = PulseForge::ReadProperty(*Entity, PulseForge::PropertyComponentIds::ScriptFloat, "health.value");
	if (!ScriptValue || !std::holds_alternative<float>(*ScriptValue) || std::get<float>(*ScriptValue) != 12.5f) return 6;
	const auto MeshID = PulseForge::UUID{ 0x9988776655443322ull, 0x2211334455667788ull };
	if (!Entity->SetMeshRenderer(PulseForge::MeshRendererComponent{ MeshID })) return 7;
	const auto Mesh = PulseForge::ReadProperty(*Entity, PulseForge::PropertyComponentIds::MeshRenderer, PulseForge::PropertyIds::MeshAsset);
	if (!Mesh || !std::holds_alternative<PulseForge::AssetID>(*Mesh) || std::get<PulseForge::AssetID>(*Mesh) != MeshID) return 8;
	PulseForge::PrefabInstanceComponent PrefabInstance;
	PrefabInstance.InstanceID = { 0x1234567812345678ull, 0x8877665544332211ull };
	PrefabInstance.SourceAsset = { 0x9988776699887766ull, 0x1122334455667788ull };
	PrefabInstance.SourceRootKey = { 0xabcdef01abcdef01ull, 0x1020304050607080ull };
	PrefabInstance.AppliedSourceFingerprint = "test-fingerprint";
	PrefabInstance.SourceEntities.push_back({ PrefabInstance.SourceRootKey, { 0xfeedfacefeedfaceull, 0x123456789abcdef0ull } });
	if (!PrefabInstance.Validate(PrefabInstance.SourceEntities.front().SceneEntity)) return 9;
	return 0;
}
