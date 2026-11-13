#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

int main(int ArgumentCount, char** Arguments)
{
	std::string OutputPath;
	std::string IncludeDirectory;
	for (int Index = 1; Index < ArgumentCount; ++Index)
	{
		if (std::string_view(Arguments[Index]) == "-Fo" && Index + 1 < ArgumentCount) OutputPath = Arguments[++Index];
		else if (std::string_view(Arguments[Index]) == "-I" && Index + 1 < ArgumentCount) IncludeDirectory = Arguments[++Index];
	}
	std::ifstream Include(IncludeDirectory + "/MaterialGraph.generated.hlsli", std::ios::binary);
	std::string Source{ std::istreambuf_iterator<char>(Include), std::istreambuf_iterator<char>() };
	if (Source.find("777.0") != std::string::npos) return 23;
	if (Source.find("778.0") != std::string::npos) std::this_thread::sleep_for(std::chrono::seconds(3));
	std::ofstream Output(OutputPath, std::ios::binary | std::ios::trunc);
	if (Source.find("g_MissingBinding") != std::string::npos)
	{
		const uint32_t FragmentWithoutBinding[] = {
			0x07230203, 0x00010600, 0, 2, 0,
			0x0003000e, 0, 1,
			0x0004000f, 4, 1, 0x6e69616d
		};
		Output.write(reinterpret_cast<const char*>(FragmentWithoutBinding), sizeof(FragmentWithoutBinding));
	}
	else
	{
		const uint32_t MalformedSpirVHeader[] = { 0x07230203, 0x00010600, 0, 1, 0 };
		Output.write(reinterpret_cast<const char*>(MalformedSpirVHeader), sizeof(MalformedSpirVHeader));
	}
	return Output ? 0 : 24;
}
