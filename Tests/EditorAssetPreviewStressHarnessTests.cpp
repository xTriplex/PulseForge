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
	void TestEditorAssetPreviewPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const AssetPreviewDimensions Wide = CalculateAssetPreviewDimensions(4096, 2048, 192);
		PF_CHECK(Tests, Wide.Width == 192 && Wide.Height == 96);
		const AssetPreviewDimensions Tall = CalculateAssetPreviewDimensions(64, 4096, 192);
		PF_CHECK(Tests, Tall.Width == 3 && Tall.Height == 192);
		const AssetPreviewDimensions Tiny = CalculateAssetPreviewDimensions(3, 2, 192);
		PF_CHECK(Tests, Tiny.Width == 3 && Tiny.Height == 2);
		PF_CHECK(Tests, CalculateAssetPreviewDimensions(0, 4, 192).Width == 0);
		PF_CHECK(Tests, CalculateAssetPreviewDimensions(4, 4, 0).Height == 0);

		const AssetPreviewRect Fitted = FitAssetPreview(10.0f, 20.0f, 100.0f, 60.0f, 200, 100);
		PF_CHECK(Tests, std::isfinite(Fitted.X) && std::isfinite(Fitted.Y) && std::isfinite(Fitted.Width) && std::isfinite(Fitted.Height));
		PF_CHECK(Tests, Fitted.Width == 100.0f && Fitted.Height == 50.0f && Fitted.Y == 25.0f);
		const AssetPreviewRect Invalid = FitAssetPreview(0.0f, 0.0f, -1.0f, 10.0f, 1, 1);
		PF_CHECK(Tests, Invalid.Width == 0.0f && Invalid.Height == 0.0f);

		AssetPreviewKey First{ "project-a", "asset", { 1, 2 }, AssetPreviewKind::Image, 192, 96, 1 };
		AssetPreviewKey Same = First;
		AssetPreviewKey OtherProject = First;
		OtherProject.ProjectID = "project-b";
		AssetPreviewKey OtherRevision = First;
		OtherRevision.ContentRevision[1]++;
		PF_CHECK(Tests, First == Same && AssetPreviewKeyHash{}(First) == AssetPreviewKeyHash{}(Same));
		PF_CHECK(Tests, !(First == OtherProject) && !(First == OtherRevision));
		const auto OpaqueSwatch = MakeMaterialBaseColorSwatch(0.5f, 1.0f, 0.0f, 0.2f, true);
		PF_CHECK(Tests, OpaqueSwatch[0] == 188 && OpaqueSwatch[1] == 255 && OpaqueSwatch[2] == 0 && OpaqueSwatch[3] == 255);
		const auto BlendSwatch = MakeMaterialBaseColorSwatch(-1.0f, 0.0f, 0.0f, 0.25f, false);
		PF_CHECK(Tests, BlendSwatch[0] == 0 && BlendSwatch[3] == 64);

		AssetPreviewBudget Budget(100);
		PF_CHECK(Tests, Budget.Add(60) && Budget.GetUsed() == 60);
		PF_CHECK(Tests, !Budget.Add(41) && Budget.GetUsed() == 60);
		Budget.Remove(20);
		PF_CHECK(Tests, Budget.CanFit(60) && !Budget.CanFit(81));
		Budget.SetLimit(0);
		Budget.Clear();
		PF_CHECK(Tests, !Budget.Add(1) && Budget.Add(0));
		PF_CHECK(Tests, IsSupportedImagePreviewExtension("sample.PNG"));
		PF_CHECK(Tests, IsSupportedImagePreviewExtension("sample.ppm"));
		PF_CHECK(Tests, !IsSupportedImagePreviewExtension("sample.hdr"));
		PF_CHECK(Tests, !IsSupportedImagePreviewExtension("sample.material"));

		bool Visible = false;
		uint64_t LastUse = 0;
		uint64_t UseCounter = 0;
		TouchAssetPreviewEntry(Visible, LastUse, UseCounter);
		PF_CHECK(Tests, Visible && LastUse == 1);
		BeginAssetPreviewFrame(Visible);
		PF_CHECK(Tests, !Visible);
		TouchAssetPreviewEntry(Visible, LastUse, UseCounter);
		PF_CHECK(Tests, Visible && LastUse == 2);
		std::array<bool, 4> StateVisibility{}; // Queued, Decoding, Ready, and Failed entries use the same touch path.
		for (bool& StateVisible : StateVisibility)
		{
			BeginAssetPreviewFrame(StateVisible);
			TouchAssetPreviewEntry(StateVisible, LastUse, UseCounter);
		}
		PF_CHECK(Tests, std::ranges::all_of(StateVisibility, [](bool StateVisible) { return StateVisible; }));

		const std::array<AssetPreviewEvictionCandidate, 3> VisibilityCandidates = { { { "ready", true, false, LastUse, 8 },
			{ "queued", true, true, 1, 0 }, { "unused", false, false, 0, 8 } } };
		const auto ProtectedVisible = PlanAssetPreviewEvictions(VisibilityCandidates, 3, 2, 16, 16, 0, 0);
		PF_CHECK(Tests, ProtectedVisible && ProtectedVisible->size() == 1 && (*ProtectedVisible)[0] == "unused");
		const std::array<AssetPreviewEvictionCandidate, 2> FullCacheCandidates = { { { "old-cache-entry", false, false, 1, 0 },
			{ "visible-cache-entry", true, false, 2, 0 } } };
		const auto FullEntryCache = PlanAssetPreviewEvictions(FullCacheCandidates, 2, 2, 0, 0, 0, 1);
		PF_CHECK(Tests, FullEntryCache && FullEntryCache->size() == 1 && (*FullEntryCache)[0] == "old-cache-entry");

		const std::array<AssetPreviewEvictionCandidate, 3> UploadCandidates = { { { "uploading", false, false, 1, 4 },
			{ "recent-unused", false, false, 9, 8 }, { "old-unused", false, false, 2, 8 } } };
		const auto UploadEviction = PlanAssetPreviewEvictions(UploadCandidates, 3, 3, 20, 20, 12, 0, "uploading");
		PF_CHECK(Tests, UploadEviction && UploadEviction->size() == 2 && (*UploadEviction)[0] == "old-unused" &&
							(*UploadEviction)[1] == "recent-unused");
		const auto NoUploadVictim = PlanAssetPreviewEvictions(UploadCandidates, 3, 3, 20, 20, 17, 0, "uploading");
		PF_CHECK(Tests, !NoUploadVictim);

		const std::array<AssetPreviewEvictionCandidate, 2> BusyCache = { { { "visible", true, false, 4, 10 },
			{ "decoding", false, true, 1, 0 } } };
		PF_CHECK(Tests, !PlanAssetPreviewEvictions(BusyCache, 2, 2, 10, 10, 1, 0));
		const auto StaleQueuedResult = [](uint64_t Work, uint64_t Entry, uint64_t Current, bool Stopping)
		{ return IsCurrentAssetPreviewResult(Work, Entry, Current, Stopping); };
		PF_CHECK(Tests, StaleQueuedResult(5, 5, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(4, 4, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(5, 4, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(5, 5, 5, true));

		const std::array<AssetPreviewEvictionCandidate, 1> AfterEviction = { { { "remaining", true, false, 5, 0 } } };
		const auto ReRequest = PlanAssetPreviewEvictions(AfterEviction, 1, 2, 0, 0, 0, 1);
		PF_CHECK(Tests, ReRequest && ReRequest->empty());
	}

} // namespace

void RunEditorAssetPreviewStressHarnessTests(TestRunner& Tests)
{
	TestManifestParsing(Tests);
	TestBudgetConfiguration(Tests);
	TestEditorAssetPreviewPolicy(Tests);
}
