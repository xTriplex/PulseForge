#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int ArgumentCount, char** Arguments)
{
	std::string OutputPath;
	std::string IncludeDirectory;
	std::vector<std::string> ArgumentTokens;
	for (int Index = 1; Index < ArgumentCount; ++Index)
	{
		ArgumentTokens.emplace_back(Arguments[Index]);
		if (std::string_view(Arguments[Index]) == "-Fo" && Index + 1 < ArgumentCount) OutputPath = Arguments[++Index];
		else if (std::string_view(Arguments[Index]) == "-I" && Index + 1 < ArgumentCount) IncludeDirectory = Arguments[++Index];
	}
	std::ifstream Include(IncludeDirectory + "/MaterialGraph.generated.hlsli", std::ios::binary);
	std::string Source{ std::istreambuf_iterator<char>(Include), std::istreambuf_iterator<char>() };
	if (Source.find("777.0") != std::string::npos) return 23;
	if (Source.find("778.0") != std::string::npos) std::this_thread::sleep_for(std::chrono::seconds(3));
	const bool VerifyVulkanBindings = Source.find("779.0") != std::string::npos;
	const bool HoldWorkspace = Source.find("781.0") != std::string::npos;
	if (HoldWorkspace)
	{
		const std::string StartedPath = IncludeDirectory + "/fixture.started";
		std::ofstream Started(StartedPath, std::ios::binary | std::ios::trunc);
		if (!Started) return 26;
		Started.close();
		const std::string ReleasePath = IncludeDirectory + "/fixture.release";
		for (size_t Attempt = 0; Attempt < 10000; ++Attempt)
		{
			std::ifstream Release(ReleasePath, std::ios::binary);
			if (Release) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (Attempt == 9999) return 27;
		}
	}
	if (VerifyVulkanBindings)
	{
		const std::vector<std::string> Expected{
			"-fvk-b-shift", "256", "0", "-fvk-t-shift", "0", "0", "-fvk-s-shift", "128", "0",
			"-fvk-b-shift", "256", "1", "-fvk-t-shift", "0", "1", "-fvk-s-shift", "128", "1",
			"-fvk-b-shift", "256", "2", "-fvk-t-shift", "0", "2", "-fvk-s-shift", "128", "2",
			"-fvk-b-shift", "256", "3", "-fvk-t-shift", "0", "3", "-fvk-s-shift", "128", "3"
		};
		if (std::search(ArgumentTokens.begin(), ArgumentTokens.end(), Expected.begin(), Expected.end()) == ArgumentTokens.end()) return 25;
	}
	std::ofstream Output(OutputPath, std::ios::binary | std::ios::trunc);
	if (VerifyVulkanBindings || HoldWorkspace || Source.find("g_MissingBinding") != std::string::npos)
	{
		const uint32_t ValidNoOpFragment[] = {
			0x07230203, 0x00010000, 0, 5, 0,
			0x00020011, 1,
			0x0003000e, 0, 1,
			0x0005000f, 4, 1, 0x6e69616d, 0,
			0x00030010, 1, 7,
			0x00020013, 2,
			0x00030021, 3, 2,
			0x00050036, 2, 1, 0, 3,
			0x000200f8, 4,
			0x000100fd,
			0x00010038
		};
		Output.write(reinterpret_cast<const char*>(ValidNoOpFragment), sizeof(ValidNoOpFragment));
	}
	else
	{
		const uint32_t MalformedSpirVHeader[] = { 0x07230203, 0x00010600, 0, 1, 0 };
		Output.write(reinterpret_cast<const char*>(MalformedSpirVHeader), sizeof(MalformedSpirVHeader));
	}
	return Output ? 0 : 24;
}
