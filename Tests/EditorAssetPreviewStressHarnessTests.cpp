#include "TestEditorSupport.h"
#include "EditorAssetPreviewStressHarness.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace
{
	std::string MakeManifest(size_t Count)
	{
		std::ostringstream Output;
		for (size_t Index = 0; Index < Count; ++Index)
			Output << "asset-" << Index << "|Assets/Textures/Preview" << Index << ".png\r\n";
		return Output.str();
	}

	void TestManifestParsing(TestRunner& Tests)
	{
		std::istringstream Input(MakeManifest(32));
		const auto Parsed = PulseForgeEditor::EditorAssetPreviewStressHarness::ParseManifest(Input);
		PF_CHECK(Tests, Parsed.has_value());
		PF_CHECK(Tests, Parsed && Parsed->size() == 32);
		PF_CHECK(Tests, Parsed && Parsed->front().ID == "asset-0");
		PF_CHECK(Tests, Parsed && Parsed->front().SourcePath == "Assets/Textures/Preview0.png");

		std::istringstream TooSmall(MakeManifest(31));
		const auto SmallResult = PulseForgeEditor::EditorAssetPreviewStressHarness::ParseManifest(TooSmall);
		PF_CHECK(Tests, !SmallResult && SmallResult.error().find("between 32 and 256") != std::string::npos);

		std::istringstream TooLarge(MakeManifest(257));
		const auto LargeResult = PulseForgeEditor::EditorAssetPreviewStressHarness::ParseManifest(TooLarge);
		PF_CHECK(Tests, !LargeResult && LargeResult.error().find("between 32 and 256") != std::string::npos);

		std::istringstream Malformed("missing-separator\n");
		const auto MalformedResult = PulseForgeEditor::EditorAssetPreviewStressHarness::ParseManifest(Malformed);
		PF_CHECK(Tests, !MalformedResult && MalformedResult.error() == "Invalid asset preview stress manifest row");
	}

	void TestBudgetConfiguration(TestRunner& Tests)
	{
		using Harness = PulseForgeEditor::EditorAssetPreviewStressHarness;
		Harness Disabled(std::nullopt, {});
		const auto Defaults = Disabled.GetServiceLimits();
		PF_CHECK(Tests, Defaults.MaximumEntries == 512);
		PF_CHECK(Tests, Defaults.MaximumGpuBytes == 64ull * 1024ull * 1024ull);

		Harness EntryBudget(std::filesystem::path("unused.manifest"), "entries");
		const auto EntryLimits = EntryBudget.GetServiceLimits();
		PF_CHECK(Tests, EntryLimits.MaximumEntries == 8);
		PF_CHECK(Tests, EntryLimits.MaximumGpuBytes == 64ull * 1024ull * 1024ull);
		PF_CHECK(Tests, EntryLimits.MaximumUploadsPerUpdate == 4);

		Harness GpuBudget(std::filesystem::path("unused.manifest"), "gpu");
		const auto GpuLimits = GpuBudget.GetServiceLimits();
		PF_CHECK(Tests, GpuLimits.MaximumEntries == 512);
		PF_CHECK(Tests, GpuLimits.MaximumGpuBytes == 4ull * 192ull * 192ull * 4ull);
		PF_CHECK(Tests, GpuLimits.MaximumUploadBytesPerUpdate == 1024ull * 1024ull);

		Harness InvalidBudget(std::filesystem::path("unused.manifest"), "unbounded");
		bool Rejected = false;
		try
		{
			(void)InvalidBudget.GetServiceLimits();
		}
		catch (const std::runtime_error&)
		{
			Rejected = true;
		}
		PF_CHECK(Tests, Rejected);
	}
} // namespace

void RunEditorAssetPreviewStressHarnessTests(TestRunner& Tests)
{
	TestManifestParsing(Tests);
	TestBudgetConfiguration(Tests);
}
