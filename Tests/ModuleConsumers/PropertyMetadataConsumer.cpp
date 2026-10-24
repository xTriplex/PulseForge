#include "Scene/Entity.h"
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
	return 0;
}
