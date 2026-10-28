#include "Assets/AssetExportService.h"
#include "Core/Log.h"

#include <iostream>
#include <string_view>
#include <thread>
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
		const auto Value = Path.u8string();
		return { reinterpret_cast<const char*>(Value.data()), Value.size() };
	}

	struct Arguments final
	{
		std::filesystem::path Profile;
		std::filesystem::path Project;
		std::filesystem::path TemplateRoot;
		std::filesystem::path Descriptor;
		std::optional<std::filesystem::path> Output;
	};

	std::expected<Arguments, std::string> Parse(int Count, char** Values)
	{
		Arguments Result;
		for (int Index = 1; Index < Count; ++Index)
		{
			const std::string_view Option = Values[Index];
			if (Index + 1 >= Count) return std::unexpected("Missing value for " + std::string(Option));
			const auto Value = std::filesystem::u8path(Values[++Index]);
			if (Option == "--profile") Result.Profile = Value;
			else if (Option == "--project") Result.Project = Value;
			else if (Option == "--template-root") Result.TemplateRoot = Value;
			else if (Option == "--template-descriptor") Result.Descriptor = Value;
			else if (Option == "--output") Result.Output = Value;
			else return std::unexpected("Unknown option: " + std::string(Option));
		}
		if (Result.Profile.empty() || Result.Project.empty() || Result.Descriptor.empty())
			return std::unexpected("Usage: PulseForgeExport --profile <profile.json> --project <project.pfproj> --template-descriptor <descriptor.json> [--template-root <dir>] [--output <dir>]");
		return Result;
	}

	int Run(int Count, char** Values)
	{
		PulseForge::Log::Init();
		if (Count == 3 && std::string_view(Values[1]) == "--recover")
		{
			auto Recovered = PulseForge::AssetExportService::RecoverInterruptedPublication(std::filesystem::u8path(Values[2]));
			if (!Recovered)
			{
				std::cerr << "Export recovery requires attention: " << Recovered.error().Message;
				if (!Recovered.error().Path.empty()) std::cerr << " [" << PathToUtf8(Recovered.error().Path) << ']';
				std::cerr << '\n';
				return 1;
			}
			std::cout << "Export recovery complete.\n";
			return 0;
		}
		auto Parsed = Parse(Count, Values);
		if (!Parsed) { std::cerr << Parsed.error() << '\n'; return 2; }
		auto Profile = PulseForge::ExportBuildProfileSerializer::Load(Parsed->Profile);
		if (!Profile) { std::cerr << "Invalid profile: " << Profile.error().Message << '\n'; return 2; }
		std::optional<std::filesystem::path> ExplicitTemplateRoot;
		if (!Parsed->TemplateRoot.empty()) ExplicitTemplateRoot = Parsed->TemplateRoot;
		auto TemplateRoot = PulseForge::ResolveExportTemplateRoot(Parsed->Descriptor, ExplicitTemplateRoot);
		if (!TemplateRoot)
		{
			std::cerr << "Invalid player template: " << TemplateRoot.error().Message;
			if (!TemplateRoot.error().Path.empty()) std::cerr << " [" << PathToUtf8(TemplateRoot.error().Path) << ']';
			std::cerr << '\n';
			return 2;
		}
		PulseForge::AssetExportService Service;
		PulseForge::AssetExportRequest Request;
		Request.SourceProjectFile = Parsed->Project;
		Request.Profile = std::move(*Profile);
		Request.TemplateRoot = std::move(*TemplateRoot);
		Request.TemplateDescriptor = Parsed->Descriptor;
		Request.OutputOverride = Parsed->Output;
		auto Started = Service.Start(std::move(Request));
		if (!Started) { std::cerr << "Could not start export: " << Started.error().Message << '\n'; return 1; }
		size_t PrintedLogBytes = 0;
		while (true)
		{
			auto Snapshot = Service.GetSnapshot(*Started);
			if (Snapshot.Log.size() > PrintedLogBytes)
			{
				std::cout << Snapshot.Log.substr(PrintedLogBytes) << std::flush;
				PrintedLogBytes = Snapshot.Log.size();
			}
			if (!Snapshot.Running)
			{
				if (Snapshot.Error)
				{
					std::cerr << "Export failed: " << Snapshot.Error->Message;
					if (!Snapshot.Error->Path.empty()) std::cerr << " [" << PathToUtf8(Snapshot.Error->Path) << ']';
					std::cerr << '\n';
					return 1;
				}
				std::cout << "Exported package: " << PathToUtf8(Snapshot.PublishedOutput) << '\n';
				return 0;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
}

#if defined(_WIN32)
int wmain(int Count, wchar_t** WideValues)
{
	std::vector<std::string> Utf8;
	std::vector<char*> Values;
	Utf8.reserve(static_cast<size_t>(Count));
	for (int Index = 0; Index < Count; ++Index)
	{
		const int Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideValues[Index], -1, nullptr, 0, nullptr, nullptr);
		if (Size <= 0) return 2;
		std::string Value(static_cast<size_t>(Size), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideValues[Index], -1, Value.data(), Size, nullptr, nullptr) <= 0) return 2;
		Value.pop_back();
		Utf8.push_back(std::move(Value));
	}
	for (std::string& Value : Utf8) Values.push_back(Value.data());
	return Run(Count, Values.data());
}
#else
int main(int Count, char** Values) { return Run(Count, Values); }
#endif
