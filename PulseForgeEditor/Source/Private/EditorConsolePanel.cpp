#include "Editor/EditorConsolePanel.h"

#include "Editor/ConsoleFormatting.h"
#include "Editor/EditorStyle.h"
#include "Editor/EditorConsoleHistory.h"
#include "Editor/EditorConsoleSeverity.h"
#include "Core/Log.h"

#include <cfloat>
#include <cstdio>

namespace PulseForgeEditor
{
	namespace
	{
		ImVec4 SeverityColor(const EditorStyle& Style, EditorConsoleSeverity Severity)
		{
			switch (Severity)
			{
			case EditorConsoleSeverity::Warning:
				return Style.GetColor(EditorColorToken::Warning);
			case EditorConsoleSeverity::Error:
			case EditorConsoleSeverity::Critical:
				return Style.GetColor(EditorColorToken::Error);
			default:
				return Style.GetColor(EditorColorToken::TextMuted);
			}
		}

	} // namespace

	struct EditorConsolePanel::Impl final
	{
		EditorConsoleHistory History;
		bool Attached = false;
		uint64_t LastLogSequence = 0;
		std::vector<EditorConsoleMessage> Messages;
		bool AutoScroll = true;
		bool SelectableView = false;
		bool SelectableTextInteracting = false;
		bool SelectableTextInitialized = false;
		bool SelectableAutoScrollSuspended = false;
		uint64_t Revision = 0;
		uint64_t SelectableTextRevision = 0;
		std::string SelectableText;

		void CapturePendingLogs()
		{
			if (!Attached) return;
			std::vector<PulseForge::LogEntry> Recent = PulseForge::Log::GetRecentEntriesAfter(LastLogSequence);
			for (const PulseForge::LogEntry& Entry : Recent)
			{
				History.Append(Entry);
				LastLogSequence = Entry.Sequence;
			}
		}

		void Refresh()
		{
			CapturePendingLogs();
			uint64_t CurrentRevision = Revision;
			if (History.CopyMessagesIfChanged(Revision, Messages, CurrentRevision)) Revision = CurrentRevision;
		}

		[[nodiscard]] std::string FormatMessages() const
		{
			std::vector<ConsoleEntryView> Entries;
			Entries.reserve(Messages.size());
			for (const EditorConsoleMessage& Message : Messages)
				Entries.push_back({ Detail::ConsoleSeverityLabel(Message.Severity), Message.Logger, Message.Text });
			return FormatConsoleHistory(Entries);
		}

		[[nodiscard]] std::string FormatMessage(const EditorConsoleMessage& Message) const
		{
			return FormatConsoleEntry({ Detail::ConsoleSeverityLabel(Message.Severity), Message.Logger, Message.Text });
		}
	};

	EditorConsolePanel::EditorConsolePanel() : m_Impl(std::make_unique<Impl>())
	{
	}

	EditorConsolePanel::~EditorConsolePanel()
	{
		Detach();
	}

	void EditorConsolePanel::Attach()
	{
		Detach();
		m_Impl->Attached = PulseForge::Log::IsInitialized();
		m_Impl->Refresh();
	}

	void EditorConsolePanel::Detach() noexcept
	{
		m_Impl->Attached = false;
	}

	void EditorConsolePanel::CapturePendingLogs()
	{
		m_Impl->CapturePendingLogs();
	}

	void EditorConsolePanel::DrawContents(EditorStyle& Style)
	{
		Impl& State = *m_Impl;
		if (!State.Attached)
		{
			Style.EmptyState(EditorIcon::Warning, "The editor could not connect to the engine loggers.");
			return;
		}

		// Refresh before toolbar actions so Copy All uses a coherent current sink snapshot.
		State.Refresh();
		if (Style.BeginToolbar("ConsoleToolbar", EditorStyleMetrics::PanelToolbarHeight))
		{
			if (Style.SmallButton(EditorIcon::Clear, "Clear"))
			{
				State.History.Clear();
				State.Refresh();
				State.SelectableText.clear();
				State.SelectableTextRevision = State.Revision;
				State.SelectableTextInitialized = true;
				State.SelectableTextInteracting = false;
				State.SelectableAutoScrollSuspended = false;
			}
			ImGui::SameLine();
			if (ImGui::Checkbox("Auto-scroll", &State.AutoScroll) && State.AutoScroll) State.SelectableAutoScrollSuspended = false;
			ImGui::SameLine();
			ImGui::BeginDisabled(State.Messages.empty());
			if (ImGui::Button("Copy All"))
			{
				const std::string Formatted = State.FormatMessages();
				if (!Formatted.empty()) ImGui::SetClipboardText(Formatted.c_str());
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Checkbox("Select text", &State.SelectableView)) State.SelectableTextInteracting = false;
		}
		Style.EndToolbar();

		if (ImGui::BeginChild("ConsoleMessages", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
		{
			[[maybe_unused]] const auto MonospaceFont = Style.PushMonospaceFont();
			if (State.Messages.empty()) Style.EmptyState(EditorIcon::Terminal, "No console messages yet.");
			else if (State.SelectableView)
			{
				const bool TextViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
				if (TextViewHovered && ImGui::GetIO().MouseWheel != 0.0f) State.SelectableAutoScrollSuspended = true;
				const bool CanReplaceText = !State.SelectableTextInteracting;
				const bool TextNeedsRefresh = !State.SelectableTextInitialized || State.SelectableTextRevision != State.Revision;
				const bool TextChanged = CanReplaceText && TextNeedsRefresh;
				if (TextChanged)
				{
					State.SelectableText = State.FormatMessages();
					State.SelectableTextRevision = State.Revision;
					State.SelectableTextInitialized = true;
				}
				if (TextChanged && State.AutoScroll && !State.SelectableAutoScrollSuspended)
					ImGui::SetNextWindowScroll(ImVec2(-1.0f, FLT_MAX));

				const ImVec2 TextViewSize = ImGui::GetContentRegionAvail();
				ImGui::InputTextMultiline("##ConsoleSelectableText", State.SelectableText.data(), State.SelectableText.size() + 1,
					TextViewSize, ImGuiInputTextFlags_ReadOnly);
				State.SelectableTextInteracting = ImGui::IsItemActive();
			}
			else
			{
				const bool WasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
				const float SourceWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.20f, 90.0f, 156.0f);
				if (ImGui::BeginTable("ConsoleMessageRows", 3,
						ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
							ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 88.0f);
					ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, SourceWidth);
					ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.0f);
					ImGuiListClipper Clipper;
					Clipper.Begin(static_cast<int>(State.Messages.size()));
					while (Clipper.Step())
					{
						for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
						{
							const EditorConsoleMessage& Message = State.Messages[static_cast<size_t>(Index)];
							ImGui::PushID(static_cast<int>(Message.Sequence >> 32));
							ImGui::PushID(static_cast<int>(Message.Sequence & 0xffffffffu));
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							const ImVec2 RowStart = ImGui::GetCursorScreenPos();
							const float RowHeight = ImGui::GetTextLineHeightWithSpacing();
							char SeverityLabel[16]{};
							std::snprintf(SeverityLabel, sizeof(SeverityLabel), "[%s]", Detail::ConsoleSeverityLabel(Message.Severity));
							ImGui::Selectable("##ConsoleMessageRow", false,
								ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, RowHeight));
							if (ImGui::BeginPopupContextItem("ConsoleEntryContextMenu"))
							{
								if (ImGui::MenuItem("Copy Message")) ImGui::SetClipboardText(Message.Text.c_str());
								if (ImGui::MenuItem("Copy Entry"))
								{
									const std::string Formatted = State.FormatMessage(Message);
									ImGui::SetClipboardText(Formatted.c_str());
								}
								ImGui::EndPopup();
							}
							ImGui::SetCursorScreenPos(RowStart);
							ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(Style, Message.Severity));
							ImGui::TextUnformatted(SeverityLabel);
							ImGui::PopStyleColor();
							ImGui::TableNextColumn();
							const float LoggerWidth = ImGui::GetContentRegionAvail().x;
							Style.TextMuted(Message.Logger);
							if (ImGui::IsItemHovered() && ImGui::CalcTextSize(Message.Logger.c_str()).x > LoggerWidth)
								ImGui::SetTooltip("%s", Message.Logger.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(Message.Text.data(), Message.Text.data() + Message.Text.size());
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Message.Text.c_str());
							ImGui::PopID();
							ImGui::PopID();
						}
					}
					ImGui::EndTable();
				}
				if (State.AutoScroll && WasAtBottom) ImGui::SetScrollHereY(1.0f);
			}
		}
		ImGui::EndChild();
	}
} // namespace PulseForgeEditor
