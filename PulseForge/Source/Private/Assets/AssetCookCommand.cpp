#include "Assets/AssetCookService.h"

#include "Assets/Project.h"
#include "Core/Log.h"

#include <charconv>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
	struct Arguments final
	{
		std::filesystem::path ProjectFile;
		std::filesystem::path Output;
		std::optional<PulseForge::AssetID> Startup;
		std::vector<PulseForge::AssetID> DynamicRoots;
		std::string Target;
		std::string Backend = "Vulkan";
		std::string ABI = "pulseforge-runtime-v1";
	};

	std::expected<Arguments, std::string> Parse(int Count, char** Values)
	{
		Arguments Parsed;
		for (int Index = 1; Index < Count; ++Index)
		{
			const std::string_view Option = Values[Index];
			if (Index + 1 >= Count) return std::unexpected("Missing value for " + std::string(Option));
			const std::string_view Value = Values[++Index];
			if (Option == "--project") Parsed.ProjectFile = std::filesystem::u8path(Value);
			else if (Option == "--output") Parsed.Output = std::filesystem::u8path(Value);
			else if (Option == "--startup")
			{
				auto ID = PulseForge::UUID::Parse(Value);
				if (!ID || ID->IsNil()) return std::unexpected("--startup requires a non-nil asset UUID");
				Parsed.Startup = *ID;
			}
			else if (Option == "--dynamic")
			{
				auto ID = PulseForge::UUID::Parse(Value);
				if (!ID || ID->IsNil()) return std::unexpected("--dynamic requires a non-nil asset UUID");
				Parsed.DynamicRoots.push_back(*ID);
			}
			else if (Option == "--target") Parsed.Target = Value;
			else if (Option == "--backend") Parsed.Backend = Value;
			else if (Option == "--abi") Parsed.ABI = Value;
			else return std::unexpected("Unknown option: " + std::string(Option));
		}
		if (Parsed.ProjectFile.empty() || Parsed.Output.empty() || Parsed.Target.empty())
			return std::unexpected("Usage: PulseForgeCook --project <file.pfproj> --output <directory> --target <Windows|Linux|macOS> [--startup <uuid>] [--dynamic <uuid> ...] [--backend Vulkan|OpenGL] [--abi <id>]");
		return Parsed;
	}
}

int main(int ArgumentCount, char** ArgumentValues)
{
	PulseForge::Log::Init();
	const auto Parsed = Parse(ArgumentCount, ArgumentValues);
	if (!Parsed)
	{
		std::cerr << Parsed.error() << '\n';
		return 2;
	}
	auto SourceProject = PulseForge::Project::Open(Parsed->ProjectFile);
	if (!SourceProject)
	{
		std::cerr << "Could not open source project: " << SourceProject.error().Message << '\n';
		return 3;
	}
	const PulseForge::AssetID Startup = Parsed->Startup.value_or(SourceProject->GetDescription().StartScene.value_or(PulseForge::AssetID{}));
	if (Startup.IsNil())
	{
		std::cerr << "The project has no startup scene; pass --startup <uuid>.\n";
		return 2;
	}
	auto Snapshot = PulseForge::AssetDependencyCatalogBuilder::Build(
		SourceProject->GetRootPath(), SourceProject->GetDescription().ID, 1);
	if (!Snapshot)
	{
		std::cerr << "Could not analyze project dependencies: " << Snapshot.error().Message << '\n';
		return 4;
	}
	PulseForge::AssetCookRequest Request;
	Request.SourceSnapshot = std::move(*Snapshot);
	Request.SourceProjectFile = SourceProject->GetFilePath();
	Request.StartupScene = Startup;
	Request.DynamicRoots = Parsed->DynamicRoots;
	Request.Compatibility = { Parsed->Target, Parsed->Backend, Parsed->ABI };
	Request.OutputRoot = Parsed->Output;
	auto Result = PulseForge::AssetCookService::Cook(Request);
	if (!Result)
	{
		std::cerr << "Cook failed";
		if (!Result.error().Asset.IsNil()) std::cerr << " for asset " << Result.error().Asset.ToString();
		if (!Result.error().Path.empty()) std::cerr << " at " << Result.error().Path.string();
		std::cerr << ": " << Result.error().Message << '\n';
		return 1;
	}
	std::cout << "Cooked " << Result->ProductCount << " products (" << Result->PayloadBytes
		<< " payload bytes) to " << Result->ManifestPath.string() << "\nGeneration: " << Result->GenerationIdentity << '\n';
	return 0;
}
