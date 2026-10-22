#include "Editor/EditorPerformancePanel.h"

#include "Editor/EditorStyle.h"

#include <algorithm>

namespace PulseForgeEditor
{
	namespace
	{
		const char* EventKindName(PulseForge::DiagnosticEventKind Kind) noexcept
		{
			switch (Kind)
			{
			case PulseForge::DiagnosticEventKind::Duration:
				return "Duration";
			case PulseForge::DiagnosticEventKind::Counter:
				return "Counter";
			case PulseForge::DiagnosticEventKind::Resource:
				return "Resource";
			default:
				return "Unknown";
			}
		}

		const char* ResourceQualityName(EditorResourceMetricQuality Quality) noexcept
		{
			switch (Quality)
			{
			case EditorResourceMetricQuality::ExactPayload:
				return "Exact payload";
			case EditorResourceMetricQuality::LogicalAllocation:
				return "Logical allocation";
			case EditorResourceMetricQuality::ProcessReported:
				return "OS-reported process";
			case EditorResourceMetricQuality::Unavailable:
				return "Unavailable";
			default:
				return "Unknown";
			}
		}

		const char* ResourceKindUnit(EditorResourceMetricKind Kind) noexcept
		{
			return Kind == EditorResourceMetricKind::Bytes ? "bytes" : "count";
		}

		void DrawDiagnosticEvents(const PulseForge::DiagnosticSnapshot& Snapshot, EditorStyle& Style)
		{
			const size_t FirstVisible = Snapshot.Events.size() > EditorPerformancePanel::MaximumVisibleEvents
				? Snapshot.Events.size() - EditorPerformancePanel::MaximumVisibleEvents
				: 0;
			const size_t VisibleCount = Snapshot.Events.size() - FirstVisible;
			ImGui::Text("Events: %zu total, showing latest %zu", Snapshot.Events.size(), VisibleCount);
			if (Snapshot.EvictedCount != 0 || Snapshot.DroppedCount != 0)
			{
				ImGui::SameLine();
				ImGui::TextColored(Style.GetColor(EditorColorToken::TextMuted), "Ring evicted: %llu | record drops: %llu",
					static_cast<unsigned long long>(Snapshot.EvictedCount),
					static_cast<unsigned long long>(Snapshot.DroppedCount));
			}
			if (Snapshot.MalformedUtf8Count != 0 || Snapshot.TruncatedStringCount != 0 ||
				Snapshot.RetiredDurationScopesDiscarded != 0)
				ImGui::TextColored(Style.GetColor(EditorColorToken::TextMuted),
					"Diagnostic data issues: %llu malformed UTF-8 replacements, %llu truncated strings, %llu retired scopes discarded",
					static_cast<unsigned long long>(Snapshot.MalformedUtf8Count),
					static_cast<unsigned long long>(Snapshot.TruncatedStringCount),
					static_cast<unsigned long long>(Snapshot.RetiredDurationScopesDiscarded));

			if (!ImGui::BeginTable("DiagnosticEvents", 6,
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
				ImVec2(0.0f, 220.0f)))
				return;

			ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 92.0f);
			ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 72.0f);
			ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 1.2f);
			ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 130.0f);
			ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 104.0f);
			ImGui::TableHeadersRow();
			for (size_t Index = FirstVisible; Index < Snapshot.Events.size(); ++Index)
			{
				const PulseForge::DiagnosticEvent& Event = Snapshot.Events[Index];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const uint64_t EventTimestamp = Event.Kind == PulseForge::DiagnosticEventKind::Duration &&
					Event.StartTimestampNanoseconds != 0 ? Event.StartTimestampNanoseconds : Event.TimestampNanoseconds;
				const uint64_t RelativeNanoseconds = EventTimestamp >= Snapshot.CaptureStartTimestampNanoseconds
					? EventTimestamp - Snapshot.CaptureStartTimestampNanoseconds
					: 0;
				ImGui::Text("%.3f ms", static_cast<double>(RelativeNanoseconds) / 1'000'000.0);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(EventKindName(Event.Kind));
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(Event.Name.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(Event.Owner.c_str());
				ImGui::TableSetColumnIndex(4);
				if (Event.Kind != PulseForge::DiagnosticEventKind::Duration)
				{
					if (Event.Unit.empty()) ImGui::Text("%llu", static_cast<unsigned long long>(Event.Value));
					else ImGui::Text("%llu %s", static_cast<unsigned long long>(Event.Value), Event.Unit.c_str());
				}
				else
					ImGui::TextUnformatted("—");
				ImGui::TableSetColumnIndex(5);
				if (Event.Kind == PulseForge::DiagnosticEventKind::Duration)
					ImGui::Text("%.3f ms", static_cast<double>(Event.DurationNanoseconds) / 1'000'000.0);
				else
					ImGui::TextUnformatted("—");
			}
			ImGui::EndTable();
		}

		void DrawResourceMetrics(const EditorPerformanceSnapshot& Snapshot, EditorStyle& Style)
		{
			ImGui::TextUnformatted("Reported values keep their source scope; they are not an aggregate memory total.");
			const size_t Count = (std::min)(Snapshot.Resources.size(), EditorPerformancePanel::MaximumResourceMetrics);
			if (Snapshot.Resources.size() > Count)
			{
				ImGui::SameLine();
				ImGui::TextColored(Style.GetColor(EditorColorToken::TextMuted), "Showing %zu of %zu metrics", Count, Snapshot.Resources.size());
			}
			if (!ImGui::BeginTable("PerformanceResources", 5,
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
					ImGuiTableFlags_SizingStretchProp))
				return;

			ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 0.8f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 144.0f);
			ImGui::TableSetupColumn("Measure", ImGuiTableColumnFlags_WidthFixed, 128.0f);
			ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 1.4f);
			ImGui::TableHeadersRow();
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const EditorResourceMetric& Metric = Snapshot.Resources[Index];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(Metric.Label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(Metric.Owner.c_str());
				ImGui::TableSetColumnIndex(2);
				if (Metric.Quality == EditorResourceMetricQuality::Unavailable)
					ImGui::TextUnformatted(Metric.AvailabilityReason.empty() ? "Unavailable" : Metric.AvailabilityReason.c_str());
				else
					ImGui::Text("%llu %s", static_cast<unsigned long long>(Metric.Value), ResourceKindUnit(Metric.Kind));
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(ResourceQualityName(Metric.Quality));
				if (!Metric.QualityReason.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", Metric.QualityReason.c_str());
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(Metric.Scope.c_str());
			}
			ImGui::EndTable();
		}

		void DrawGpuTimings(const EditorPerformanceSnapshot& Snapshot, EditorStyle& Style)
		{
			if (Snapshot.GpuTimingRendererOverwriteCount != 0 || Snapshot.GpuTimingCaptureHistoryEvictionCount != 0 ||
				Snapshot.GpuTimingStaleSessionDiscardCount != 0 || Snapshot.GpuTimingInvalidResultCount != 0)
				ImGui::TextColored(Style.GetColor(EditorColorToken::TextMuted),
					"GPU samples: %llu renderer overwrites, %llu capture-history evictions, %llu stale discarded, %llu invalid",
					static_cast<unsigned long long>(Snapshot.GpuTimingRendererOverwriteCount),
					static_cast<unsigned long long>(Snapshot.GpuTimingCaptureHistoryEvictionCount),
					static_cast<unsigned long long>(Snapshot.GpuTimingStaleSessionDiscardCount),
					static_cast<unsigned long long>(Snapshot.GpuTimingInvalidResultCount));
			ImGui::Text("GPU timing: %s | requested: %s | collection enabled: %s",
				Snapshot.GpuTimingSupported ? "supported" : "unsupported",
				Snapshot.GpuTimingRequested ? "yes" : "no",
				Snapshot.GpuTimingEnabled ? "yes" : "no");
			const char* ResultStatus = "no samples";
			switch (Snapshot.GpuTimingResultStatus)
			{
			case EditorGpuTimingResultStatus::Pending: ResultStatus = "pending"; break;
			case EditorGpuTimingResultStatus::Informative: ResultStatus = "informative"; break;
			case EditorGpuTimingResultStatus::ZeroDurationOnly: ResultStatus = "zero-duration only"; break;
			case EditorGpuTimingResultStatus::Failed: ResultStatus = "failed"; break;
			case EditorGpuTimingResultStatus::NoSamples: break;
			}
			ImGui::Text("Measurement status: %s | positive: %llu | zero: %llu | retained: %zu / %zu",
				ResultStatus, static_cast<unsigned long long>(Snapshot.GpuTimingPositiveDurationCount),
				static_cast<unsigned long long>(Snapshot.GpuTimingZeroDurationCount), Snapshot.CompletedGpuTimings.size(),
				EditorPerformanceCaptureService::MaximumGpuTimingSamples);
			if (!Snapshot.GpuTimingSupported || !Snapshot.GpuTimingEnabled)
			{
				Style.TextMuted(Snapshot.GpuTimingUnavailableReason.empty()
					? "GPU frame timing is unavailable on this renderer."
					: Snapshot.GpuTimingUnavailableReason.c_str());
				return;
			}
			Style.TextMuted("Vulkan main-frame query results arrive after frame completion and are not synchronized to the CPU clock.");
			const size_t FirstVisible = Snapshot.CompletedGpuTimings.size() > EditorPerformancePanel::MaximumVisibleGpuTimings
				? Snapshot.CompletedGpuTimings.size() - EditorPerformancePanel::MaximumVisibleGpuTimings
				: 0;
			const size_t VisibleCount = Snapshot.CompletedGpuTimings.size() - FirstVisible;
			if (VisibleCount == 0)
			{
				Style.TextMuted("No completed GPU timing samples.");
				return;
			}
			ImGui::Text("Completed samples: %zu total, showing latest %zu", Snapshot.CompletedGpuTimings.size(), VisibleCount);
			if (!ImGui::BeginTable("CompletedGpuTimings", 5,
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
				ImVec2(0.0f, 150.0f)))
				return;

			ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 72.0f);
			ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 0.8f);
			ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableHeadersRow();
			for (size_t Index = FirstVisible; Index < Snapshot.CompletedGpuTimings.size(); ++Index)
			{
				const EditorGpuTimingSample& Sample = Snapshot.CompletedGpuTimings[Index];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%llu", static_cast<unsigned long long>(Sample.FrameSequence));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(Sample.Name.c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(Sample.Owner.c_str());
				ImGui::TableSetColumnIndex(3);
				if (Sample.Quality == EditorGpuTimingSampleQuality::ZeroDuration)
					ImGui::TextUnformatted("0 ns (uninformative)");
				else
					ImGui::Text("%.3f ms", static_cast<double>(Sample.DurationNanoseconds) / 1'000'000.0);
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(Sample.Source.c_str());
			}
			ImGui::EndTable();
		}
	} // namespace

	EditorPerformanceAction EditorPerformancePanel::DrawContents(const EditorPerformanceSnapshot& Snapshot, EditorStyle& Style) const
	{
		EditorPerformanceAction Action;

		const PulseForge::DiagnosticSnapshot& Diagnostics = Snapshot.Diagnostics;
		if (Diagnostics.CaptureEnabled)
		{
			ImGui::TextColored(Style.GetColor(EditorColorToken::Success), "Capture running");
			ImGui::SameLine();
			if (ImGui::Button("Stop capture")) Action.Kind = EditorPerformanceActionKind::StopCapture;
		}
		else
		{
			Style.TextMuted("Capture stopped");
			ImGui::SameLine();
			if (ImGui::Button("Start capture")) Action.Kind = EditorPerformanceActionKind::StartCapture;
		}
		ImGui::SameLine();
		if (ImGui::Button("Export capture")) Action.Kind = EditorPerformanceActionKind::ExportCapture;

		const uint64_t CaptureEndTimestamp = Diagnostics.CaptureEndTimestampNanoseconds != 0
			? Diagnostics.CaptureEndTimestampNanoseconds : Diagnostics.SnapshotTimestampNanoseconds;
		if (Diagnostics.CaptureStartTimestampNanoseconds != 0 && CaptureEndTimestamp >= Diagnostics.CaptureStartTimestampNanoseconds)
		{
			const double DurationMilliseconds = static_cast<double>(
			CaptureEndTimestamp - Diagnostics.CaptureStartTimestampNanoseconds) / 1'000'000.0;
			ImGui::Text("Capture window: %.2f ms", DurationMilliseconds);
		}
		else
			Style.TextMuted("Capture window unavailable");

		if (ImGui::CollapsingHeader("Resource metrics", ImGuiTreeNodeFlags_DefaultOpen))
			DrawResourceMetrics(Snapshot, Style);
		if (ImGui::CollapsingHeader("GPU timings", ImGuiTreeNodeFlags_DefaultOpen))
			DrawGpuTimings(Snapshot, Style);
		if (ImGui::CollapsingHeader("CPU and resource events", ImGuiTreeNodeFlags_DefaultOpen))
			DrawDiagnosticEvents(Diagnostics, Style);

		return Action;
	}
} // namespace PulseForgeEditor
