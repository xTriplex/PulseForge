#include "Editor/EditorConsolePanel.h"

#include "Editor/ConsoleFormatting.h"
#include "Editor/EditorStyle.h"
#include "EditorConsoleHistory.h"
#include "EditorConsoleSeverity.h"

#include <imgui.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	namespace
	{
		const char* SeverityName(EditorConsoleSeverity Severity)
		{
			switch (Severity)
			{
			case EditorConsoleSeverity::Unknown:
				return "unknown";
			case EditorConsoleSeverity::Trace:
				return "trace";
			case EditorConsoleSeverity::Debug:
				return "debug";
			case EditorConsoleSeverity::Info:
				return "info";
			case EditorConsoleSeverity::Warning:
				return "warning";
			case EditorConsoleSeverity::Error:
				return "error";
			case EditorConsoleSeverity::Critical:
				return "critical";
			default:
				return "unknown";
			}
		}

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

		class EditorConsoleSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			explicit EditorConsoleSink(EditorConsoleHistory& History) : m_History(History) {}

		protected:
			void sink_it_(const spdlog::details::log_msg& Message) override
			{
				m_History.Append(std::string_view(Message.logger_name.data(), Message.logger_name.size()),
					Detail::ConvertConsoleSeverity(Message.level), std::string_view(Message.payload.data(), Message.payload.size()));
			}

			void flush_() override {}

		private:
			EditorConsoleHistory& m_History;
		};
	} // namespace

	struct EditorConsolePanel::Impl final
	{
		EditorConsoleHistory History;
		std::shared_ptr<EditorConsoleSink> Sink;
		std::vector<std::shared_ptr<spdlog::logger>> Loggers;
		std::vector<EditorConsoleMessage> Messages;
		bool AutoScroll = true;
		bool SelectableView = false;
		bool SelectableTextInteracting = false;
		bool SelectableTextInitialized = false;
		bool SelectableAutoScrollSuspended = false;
		uint64_t Revision = 0;
		uint64_t SelectableTextRevision = 0;
		std::string SelectableText;

		void Refresh()
		{
			if (!Sink) return;
			uint64_t CurrentRevision = Revision;
			if (History.CopyMessagesIfChanged(Revision, Messages, CurrentRevision)) Revision = CurrentRevision;
		}

		[[nodiscard]] std::string FormatMessages() const
		{
			std::vector<ConsoleEntryView> Entries;
			Entries.reserve(Messages.size());
			for (const EditorConsoleMessage& Message : Messages)
				Entries.push_back({ SeverityName(Message.Severity), Message.Logger, Message.Text });
			return FormatConsoleHistory(Entries);
		}

		[[nodiscard]] std::string FormatMessage(const EditorConsoleMessage& Message) const
		{
			return FormatConsoleEntry({ SeverityName(Message.Severity), Message.Logger, Message.Text });
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
		m_Impl->Sink = std::make_shared<EditorConsoleSink>(m_Impl->History);
		for (const char* LoggerName : { "PULSEFORGE", "APP" })
		{
			if (auto Logger = spdlog::get(LoggerName))
			{
				Logger->sinks().push_back(m_Impl->Sink);
				m_Impl->Loggers.push_back(std::move(Logger));
			}
		}
		if (m_Impl->Loggers.empty()) m_Impl->Sink.reset();
	}

	void EditorConsolePanel::Detach() noexcept
	{
		if (!m_Impl->Sink) return;
		const std::shared_ptr<spdlog::sinks::sink> Sink = m_Impl->Sink;
		for (const std::shared_ptr<spdlog::logger>& Logger : m_Impl->Loggers)
			std::erase(Logger->sinks(), Sink);
		m_Impl->Loggers.clear();
		m_Impl->Sink.reset();
	}

	void EditorConsolePanel::DrawContents(EditorStyle& Style)
	{
		Impl& State = *m_Impl;
		if (!State.Sink)
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
							std::snprintf(SeverityLabel, sizeof(SeverityLabel), "[%s]", SeverityName(Message.Severity));
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
