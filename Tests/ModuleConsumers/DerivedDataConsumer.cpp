#include "Assets/AssetPreparationService.h"
#include "Assets/Detail/AssetCpuProducts.h"
#include "Assets/DerivedDataArtifactStore.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

int main()
{
	PulseForge::AssetPreparationService Service({ .MaximumWorkers = 1 });
	PulseForge::DerivedDataArtifactStore Store({ .Root = {}, .Enabled = false });
	const std::array<std::byte, 1> Payload{ std::byte{ 0x01 } };
	const auto Key = PulseForge::DerivedDataArtifactKey{
		.ProductKind = "consumer-probe",
		.RecipeDigest = PulseForge::ComputeAssetContentFingerprint(Payload),
		.ProductSchemaVersion = 1
	};
	PulseForge::AssetRegistry Registry;
	const auto CpuProduct = PulseForge::Detail::PrepareTextureCpuProduct({}, {}, Registry,
		PulseForge::Detail::ImageColorSpace::Srgb, nullptr);
	return Store.Read(Key).Status == PulseForge::DerivedDataArtifactReadStatus::Unavailable &&
		!CpuProduct && !CpuProduct.error().empty() ? 0 : 1;
}
