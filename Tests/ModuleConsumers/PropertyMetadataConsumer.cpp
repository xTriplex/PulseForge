#include "Scene/Entity.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/PropertyMetadata.h"
#include "Scene/Scene.h"

#include <variant>

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
	return 0;
}
