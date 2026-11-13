#include "Assets/MaterialGraphSource.h"

int main()
{
	using namespace PulseForge;
	MaterialGraphSource Source;
	Source.GraphID = UUID(11, 1);
	Source.Nodes = {
		{ UUID(11, 2), MaterialGraphNodeKind::FloatConstant, 0.4f, {} },
		{ UUID(11, 3), MaterialGraphNodeKind::PbrOutput, 0.0f, {} }
	};
	Source.Connections = { { UUID(12, 1), UUID(11, 2), UUID(11, 3), 0 } };
	if (!ValidateMaterialGraphSource(Source).IsValid()) return 1;
	const auto Bytes = SerializeMaterialGraphSource(Source);
	if (!Bytes) return 2;
	const auto Restored = DeserializeMaterialGraphSource(*Bytes);
	return Restored && *Restored == Source ? 0 : 3;
}
