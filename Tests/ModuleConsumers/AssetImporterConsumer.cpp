#include "Assets/GltfMeshImporter.h"
#include "Assets/ImageAssetImporter.h"

int main()
{
	const auto Decode = PulseForge::ImageAssetImporter::DecodePreviewRGBA8("missing-consumer-image.png", 16);
	PulseForge::ImportedGltfMesh ImportedMesh;
	const PulseForge::MeshDesc Description = ImportedMesh.GetMeshDescription();
	const auto Validation = PulseForge::ValidateMeshDescription(Description);
	return !Decode && !Validation ? 0 : 1;
}
