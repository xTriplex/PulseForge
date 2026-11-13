#include <chrono>
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
	return Output ? 0 : 24;
}
