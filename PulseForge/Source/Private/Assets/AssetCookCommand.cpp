#include "Assets/AssetCookService.h"

#include "Assets/Project.h"
#include "Core/Log.h"

#include <charconv>
#include <iostream>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace
{
	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string Encoded = Path.u8string();
		return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
	}

	struct Arguments final
	{
		std::filesystem::path ProjectFile;
		std::filesystem::path Output;
		std::optional<PulseForge::AssetID> Startup;
		std::vector<PulseForge::AssetID> DynamicRoots;
		std::string Target;
		std::string Backend = "Vulkan";
		std::string ABI = std::string(PulseForge::CookedRuntimeABIIdentifier);
		PulseForge::CookedBuildConfiguration Configuration = PulseForge::CookedBuildConfiguration::Debug;
	};

	std::expected<Arguments, std::string> Parse(int Count, char** Values)
	{
		Arguments Parsed;
#if defined(PF_BUILD_CONFIGURATION)
		const auto BuiltConfiguration = PulseForge::ParseCookedBuildConfiguration(PF_BUILD_CONFIGURATION);
		if (!BuiltConfiguration)
			return std::unexpected("PulseForgeCook was built with an unsupported configuration: " + BuiltConfiguration.error());
		Parsed.Configuration = *BuiltConfiguration;
#else
		return std::unexpected("PulseForgeCook must be built with an explicit Debug, Release, or Dist configuration");
#endif
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
			else if (Option == "--configuration")
			{
				auto Configuration = PulseForge::ParseCookedBuildConfiguration(Value);
				if (!Configuration) return std::unexpected(Configuration.error());
				Parsed.Configuration = *Configuration;
			}
			else return std::unexpected("Unknown option: " + std::string(Option));
		}
		if (Parsed.ProjectFile.empty() || Parsed.Output.empty() || Parsed.Target.empty())
			return std::unexpected("Usage: PulseForgeCook --project <file.pfproj> --output <directory> --target <Windows|Linux|macOS> [--configuration Debug|Release|Dist] [--startup <uuid>] [--dynamic <uuid> ...] [--backend Vulkan|OpenGL] [--abi <id>]");
		return Parsed;
	}
}

int RunAssetCookCommand(int ArgumentCount, char** ArgumentValues)
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
	Request.Compatibility = { Parsed->Target, Parsed->Backend, Parsed->ABI, Parsed->Configuration };
	Request.OutputRoot = Parsed->Output;
	auto Result = PulseForge::AssetCookService::Cook(Request);
	if (!Result)
	{
		std::cerr << "Cook failed";
		if (!Result.error().Asset.IsNil()) std::cerr << " for asset " << Result.error().Asset.ToString();
		if (!Result.error().Path.empty()) std::cerr << " at " << PathToUtf8(Result.error().Path);
		std::cerr << ": " << Result.error().Message << '\n';
		return 1;
	}
	PulseForge::Log::Info("Cook service returned successfully");
	std::cout << "Cooked " << Result->ProductCount << " products (" << Result->PayloadBytes
		<< " payload bytes) to " << PathToUtf8(Result->ManifestPath) << "\nGeneration: " << Result->GenerationIdentity << '\n';
	return 0;
}

#if defined(_WIN32)
int wmain(int ArgumentCount, wchar_t** WideArguments)
{
	std::vector<std::string> Utf8Arguments;
	std::vector<char*> Arguments;
	Utf8Arguments.reserve(static_cast<size_t>(ArgumentCount));
	Arguments.reserve(static_cast<size_t>(ArgumentCount));
	for (int Index = 0; Index < ArgumentCount; ++Index)
	{
		const int ByteCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideArguments[Index], -1, nullptr, 0, nullptr, nullptr);
		if (ByteCount <= 0) return 2;
		std::string Encoded(static_cast<size_t>(ByteCount), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideArguments[Index], -1, Encoded.data(), ByteCount, nullptr, nullptr) <= 0)
			return 2;
		Encoded.pop_back();
		Utf8Arguments.push_back(std::move(Encoded));
	}
	for (std::string& Argument : Utf8Arguments)
		Arguments.push_back(Argument.data());
	return RunAssetCookCommand(ArgumentCount, Arguments.data());
}
#else
int main(int ArgumentCount, char** ArgumentValues)
{
	return RunAssetCookCommand(ArgumentCount, ArgumentValues);
}
#endif
