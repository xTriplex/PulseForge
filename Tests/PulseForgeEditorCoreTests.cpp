#include "TestSupport.h"

#include "Core/Log.h"
#include "Editor/EditorConsoleHistory.h"
#include "Editor/EditorCommandHistory.h"
#include "Editor/EditorMaterialDraftStore.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_set>

void RunEditorDocumentCloseRequestTests(TestRunner& Tests);
void RunEditorDocumentTabInteractionTests(TestRunner& Tests);

namespace
{
	void TestStartupLogReplay(TestRunner& Tests)
	{
		constexpr std::string_view InfoMarker = "editor console startup info replay marker";
		constexpr std::string_view WarningMarker = "editor console startup warning replay marker";
		constexpr std::string_view AppMarker = "editor console startup app replay marker";
		PF_CORE_INFO("{0}", InfoMarker);
		PF_CORE_WARN("{0}", WarningMarker);
		PF_INFO("{0}", AppMarker);

		const std::vector<PulseForge::LogEntry> Recent = PulseForge::Log::GetRecentEntries();
		const auto FindEntry = [&Recent](std::string_view Logger, std::string_view Message)
		{
			return std::find_if(Recent.begin(), Recent.end(), [Logger, Message](const PulseForge::LogEntry& Candidate)
				{ return Candidate.Logger == Logger && Candidate.Message == Message; });
		};
		const auto InfoEntry = FindEntry("PULSEFORGE", InfoMarker);
		const auto WarningEntry = FindEntry("PULSEFORGE", WarningMarker);
		const auto AppEntry = FindEntry("APP", AppMarker);
		PF_CHECK(Tests, InfoEntry != Recent.end());
		PF_CHECK(Tests, WarningEntry != Recent.end());
		PF_CHECK(Tests, AppEntry != Recent.end());
		if (InfoEntry == Recent.end() || WarningEntry == Recent.end() || AppEntry == Recent.end()) return;

		PulseForgeEditor::EditorConsoleHistory History;
		const std::vector<PulseForge::LogEntry> ReplayedEntries = { *InfoEntry, *WarningEntry, *AppEntry };
		History.AppendRecent(ReplayedEntries);
		std::vector<PulseForgeEditor::EditorConsoleMessage> Messages;
		uint64_t Revision = 0;
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.size() == 3);
		PF_CHECK(Tests, Messages.front().Logger == "PULSEFORGE");
		PF_CHECK(Tests, Messages.front().Severity == PulseForgeEditor::EditorConsoleSeverity::Info);
		PF_CHECK(Tests, Messages.front().Text == InfoMarker);
		PF_CHECK(Tests, Messages[1].Severity == PulseForgeEditor::EditorConsoleSeverity::Warning);
		PF_CHECK(Tests, Messages[1].Text == WarningMarker);
		PF_CHECK(Tests, Messages[2].Logger == "APP");
		PF_CHECK(Tests, Messages[2].Severity == PulseForgeEditor::EditorConsoleSeverity::Info);
		PF_CHECK(Tests, Messages[2].Text == AppMarker);
		PF_CHECK(Tests, Messages.front().Sequence < Messages.back().Sequence);

		const uint64_t Cursor = Recent.back().Sequence;
		constexpr std::string_view LiveMarker = "editor console live cursor marker";
		PF_INFO("{0}", LiveMarker);
		const std::vector<PulseForge::LogEntry> LiveEntries = PulseForge::Log::GetRecentEntriesAfter(Cursor);
		PF_CHECK(Tests, LiveEntries.size() == 1);
		if (LiveEntries.size() == 1)
		{
			PF_CHECK(Tests, LiveEntries.front().Message == LiveMarker);
			PF_CHECK(Tests, LiveEntries.front().Sequence > Cursor);
		}
	}

	void TestConcurrentLogHistory(TestRunner& Tests)
	{
		const uint64_t Cursor = PulseForge::Log::GetRecentEntries().back().Sequence;
		constexpr int ThreadCount = 4;
		constexpr int MessagesPerThread = 32;
		std::vector<std::thread> Workers;
		Workers.reserve(ThreadCount);
		std::atomic<int> RemainingWorkers = ThreadCount;
		for (int ThreadIndex = 0; ThreadIndex < ThreadCount; ++ThreadIndex)
			Workers.emplace_back(
				[ThreadIndex, &RemainingWorkers]
				{
					for (int MessageIndex = 0; MessageIndex < MessagesPerThread; ++MessageIndex)
						PF_CORE_INFO("log cursor regression {0}:{1}", ThreadIndex, MessageIndex);
					RemainingWorkers.fetch_sub(1, std::memory_order_release);
				});
		std::unordered_set<std::string> Messages;
		uint64_t PreviousSequence = Cursor;
		auto CaptureNewMessages = [&]
		{
			const std::vector<PulseForge::LogEntry> Entries = PulseForge::Log::GetRecentEntriesAfter(PreviousSequence);
			for (const PulseForge::LogEntry& Entry : Entries)
			{
				PF_CHECK(Tests, Entry.Sequence > PreviousSequence);
				PreviousSequence = Entry.Sequence;
				if (Entry.Message.starts_with("log cursor regression ")) Messages.insert(Entry.Message);
			}
		};
		while (RemainingWorkers.load(std::memory_order_acquire) != 0)
		{
			CaptureNewMessages();
			std::this_thread::yield();
		}
		for (std::thread& Worker : Workers)
			Worker.join();
		CaptureNewMessages();
		PF_CHECK(Tests, Messages.size() == ThreadCount * MessagesPerThread);
	}
} // namespace

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunEditorSceneEntityCommandTests(Tests);
	RunEditorSceneViewportInteractionTests(Tests);
	RunEditorViewportGizmoGeometryTests(Tests);
	RunEditorViewportGizmoInteractionTests(Tests);
	RunEditorDocumentTabInteractionTests(Tests);
	PulseForgeEditor::EditorConsoleHistory Console;
	std::vector<PulseForgeEditor::EditorConsoleMessage> Messages;
	uint64_t Revision = 0;
	Console.Append("APP", PulseForgeEditor::EditorConsoleSeverity::Info, "engine ready");
	PF_CHECK(Tests, Console.CopyMessagesIfChanged(Revision, Messages, Revision));
	PF_CHECK(Tests, Messages.size() == 1 && Messages.front().Text == "engine ready");
	TestStartupLogReplay(Tests);
	TestConcurrentLogHistory(Tests);
	RunEditorDocumentCloseRequestTests(Tests);

	PulseForgeEditor::EditorCommandHistory History;
	PF_CHECK(Tests, History.Begin("Rename", "before"));
	const auto Committed = History.Commit("after");
	PF_CHECK(Tests, Committed && *Committed && History.IsDirty() && History.CanUndo());
	std::string State = "after";
	const auto Undone = History.Undo(
		[&State](std::string_view Before) -> std::expected<void, std::string>
		{
			State.assign(Before);
			return {};
		});
	PF_CHECK(Tests, Undone && *Undone && State == "before" && !History.IsDirty());

	PulseForgeEditor::EditorMaterialDraftStore Drafts;
	PulseForge::MaterialAssetDesc Original;
	Original.RoughnessFactor = 0.6f;
	Drafts.PutDraft("project:material", Original);
	PF_CHECK(Tests, Drafts.BeginEdit("project:material", Original));
	Drafts.FindDraft("project:material")->RoughnessFactor = 0.25f;
	Drafts.FinalizePendingEdits(false);
	PF_CHECK(Tests, Drafts.IsDirty("project:material") && Drafts.CanUndo("project:material"));
	const auto DraftUndo = Drafts.ApplyHistoryStep("project:material", false);
	PF_CHECK(Tests, DraftUndo && *DraftUndo && !Drafts.IsDirty("project:material"));

	return Tests.Finish();
}
