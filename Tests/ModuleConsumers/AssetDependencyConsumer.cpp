#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetEditTransaction.h"
#include "Assets/AssetMetadata.h"
#include "Assets/MaterialAsset.h"
#include "Core/Log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main()
{
	PulseForge::Log::Init();
	const auto ProjectID = PulseForge::UUID::Generate();
	if (!ProjectID) return 1;
	std::error_code Error;
	const std::filesystem::path TemporaryDirectoryAlias = std::filesystem::temp_directory_path(Error);
	if (Error) return 2;
	const std::filesystem::path TemporaryDirectory = std::filesystem::canonical(TemporaryDirectoryAlias, Error);
	if (Error) return 2;
	const std::filesystem::path Root = TemporaryDirectory / ("PulseForgeCatalogConsumer-" + ProjectID->ToString());
	struct Cleanup
	{
		std::filesystem::path Path;
		~Cleanup()
		{
			std::error_code Ignored;
			std::filesystem::remove_all(Path, Ignored);
		}
	} CleanupRoot{ Root };
	std::filesystem::create_directories(Root / "Assets", Error);
	if (Error) return 3;

	const std::filesystem::path ImagePath = Root / "Assets" / "Albedo.png";
	{
		std::ofstream Image(ImagePath, std::ios::binary);
		Image << "runtime-only catalog consumer fixture";
		if (!Image) return 4;
	}
	const auto ImageMetadata = PulseForge::AssetMetadataSerializer::CreateForNewAsset(ImagePath);
	if (!ImageMetadata) return 5;

	PulseForge::MaterialAssetDesc Material;
	Material.BaseColorTexture = ImageMetadata->ID;
	const auto MaterialData = PulseForge::MaterialAssetSerializer::Serialize(Material);
	if (!MaterialData) return 6;
	const std::filesystem::path MaterialPath = Root / "Assets" / "Surface.material";
	{
		std::ofstream MaterialFile(MaterialPath, std::ios::binary);
		MaterialFile.write(MaterialData->data(), static_cast<std::streamsize>(MaterialData->size()));
		if (!MaterialFile) return 7;
	}
	const auto MaterialMetadata = PulseForge::AssetMetadataSerializer::CreateForNewAsset(MaterialPath);
	if (!MaterialMetadata) return 8;

	PulseForge::AssetDependencyCatalog Catalog;
	const PulseForge::AssetDependencyCatalogTicket Ticket = Catalog.BeginRequest(*ProjectID);
	const auto Candidate = PulseForge::AssetDependencyCatalogBuilder::Build(Root, *ProjectID, Ticket.Generation);
	if (!Candidate || !Catalog.Publish(Ticket, *Candidate)) return 9;
	const auto Snapshot = Catalog.GetSnapshot();
	if (!Snapshot || Snapshot->GetProjectID() != *ProjectID || Snapshot->GetRegistry().GetAssets().size() != 2) return 10;
	if (Snapshot->GetInputReadSet().empty()) return 13;

	const PulseForge::AssetDependencyNode MaterialNode{ MaterialMetadata->ID, "Assets/Surface.material", {} };
	const auto Dependencies = Snapshot->GetDirectDependencies(MaterialNode);
	if (Dependencies.size() != 1 || Dependencies.front().Target.Asset != ImageMetadata->ID ||
		Dependencies.front().Kind != PulseForge::AssetDependencyEdgeKind::RuntimeReference ||
		Dependencies.front().Status != PulseForge::AssetDependencyStatus::Resolved)
		return 11;
	const auto Referencers = Snapshot->GetDirectReferencers(Dependencies.front().Target);
	if (Referencers.size() != 1 || Referencers.front().Referrer != MaterialNode) return 12;
	const auto MovePlan = PulseForge::AssetEditTransaction::PlanMove(
		*Snapshot, MaterialMetadata->ID, std::filesystem::path("Assets") / "RenamedSurface.material");
	if (!MovePlan)
	{
		std::cerr << "Runtime-only dependency consumer could not create its asset move plan: " << MovePlan.error().Message << '\n';
		return 14;
	}
	return (*MovePlan)->GetAsset() == MaterialMetadata->ID ? 0 : 15;
}
