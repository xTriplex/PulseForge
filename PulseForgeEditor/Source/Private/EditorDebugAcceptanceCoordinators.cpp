#include "Editor/EditorDebugAcceptanceCoordinators.h"

#if defined(_DEBUG) || defined(PF_DEBUG)

#include "Core/Log.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace PulseForgeEditor::Detail
{
	namespace
	{
		[[nodiscard]] const char* DocumentCloseUiPhaseName(DocumentCloseUiPhase Phase) noexcept
		{
			switch (Phase)
			{
			case DocumentCloseUiPhase::Disabled: return "disabled";
			case DocumentCloseUiPhase::AwaitingTextEntry: return "awaiting-text-entry";
			case DocumentCloseUiPhase::AwaitingInitialClose: return "awaiting-initial-close";
			case DocumentCloseUiPhase::AwaitingFirstAction: return "awaiting-first-action";
			case DocumentCloseUiPhase::AwaitingSecondClose: return "awaiting-second-close";
			case DocumentCloseUiPhase::AwaitingSecondAction: return "awaiting-second-action";
			case DocumentCloseUiPhase::AwaitingSaveRetry: return "awaiting-save-retry";
			case DocumentCloseUiPhase::AwaitingCloseRetryCancel: return "awaiting-close-retry-cancel";
			case DocumentCloseUiPhase::AwaitingAllowClose: return "awaiting-allow-close";
			case DocumentCloseUiPhase::AwaitingFinalClose: return "awaiting-final-close";
			case DocumentCloseUiPhase::Complete: return "complete";
			case DocumentCloseUiPhase::Failed: return "failed";
			}
			return "unknown";
		}

		[[nodiscard]] std::string EscapeDocumentCloseUiField(std::string_view Value)
		{
			constexpr char Hex[] = "0123456789ABCDEF";
			std::string Escaped;
			Escaped.reserve(Value.size());
			for (const char RawCharacter : Value)
			{
				const auto Character = static_cast<unsigned char>(RawCharacter);
				const bool Safe = (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
								  (Character >= '0' && Character <= '9') || Character == '-' || Character == '_' || Character == '.' ||
								  Character == '~';
				if (Safe) Escaped.push_back(static_cast<char>(Character));
				else
				{
					Escaped.push_back('%');
					Escaped.push_back(Hex[Character >> 4]);
					Escaped.push_back(Hex[Character & 0x0f]);
				}
			}
			return Escaped;
		}
	} // namespace

	DocumentCloseAcceptanceCoordinator::DocumentCloseAcceptanceCoordinator(
		std::optional<std::string> Scenario, std::function<void(std::string)> ReportApplicationError)
		: m_Scenario(std::move(Scenario)), m_ReportApplicationError(std::move(ReportApplicationError))
	{
		if (m_Scenario) m_Phase = DocumentCloseUiPhase::Disabled;
	}

	std::string_view DocumentCloseAcceptanceCoordinator::Scenario() const noexcept
	{
		return m_Scenario ? std::string_view(*m_Scenario) : std::string_view{};
	}

	bool DocumentCloseAcceptanceCoordinator::IsFixtureIdentity(const EditorDocumentIdentity& Identity) const noexcept
	{
		return m_Scenario && m_Identity && Identity == *m_Identity;
	}

	bool DocumentCloseAcceptanceCoordinator::IsFixtureHost(std::string_view HostID) const noexcept
	{
		return m_Scenario && !m_HostID.empty() && HostID == m_HostID;
	}

	bool DocumentCloseAcceptanceCoordinator::IsAwaitingTabClose() const noexcept
	{
		return m_Phase == DocumentCloseUiPhase::AwaitingInitialClose || m_Phase == DocumentCloseUiPhase::AwaitingSecondClose ||
			   m_Phase == DocumentCloseUiPhase::AwaitingFinalClose;
	}

	std::string_view DocumentCloseAcceptanceCoordinator::ExpectedChoice() const noexcept
	{
		if (!m_Scenario) return {};
		if (m_Phase == DocumentCloseUiPhase::AwaitingFirstAction)
		{
			if (*m_Scenario == "cancel-save") return "cancel";
			if (*m_Scenario == "discard") return "discard";
			return "save";
		}
		if (m_Phase == DocumentCloseUiPhase::AwaitingSecondAction || m_Phase == DocumentCloseUiPhase::AwaitingSaveRetry) return "save";
		if (m_Phase == DocumentCloseUiPhase::AwaitingCloseRetryCancel) return "cancel";
		return {};
	}

	void DocumentCloseAcceptanceCoordinator::SetIdentity(EditorDocumentIdentity Identity)
	{
		m_Identity = std::move(Identity);
	}

	void DocumentCloseAcceptanceCoordinator::SetHost(std::string HostID)
	{
		m_HostID = std::move(HostID);
	}

	void DocumentCloseAcceptanceCoordinator::Fail(std::string_view Reason)
	{
		if (!m_Scenario || m_Phase == DocumentCloseUiPhase::Failed || m_Phase == DocumentCloseUiPhase::Complete) return;
		m_Phase = DocumentCloseUiPhase::Failed;
		PF_ERROR("PF_DOC_CLOSE_UI|FAIL|scenario={}|reason={}", *m_Scenario, EscapeDocumentCloseUiField(Reason));
		if (m_ReportApplicationError) m_ReportApplicationError("The Debug document-close UI fixture failed: " + std::string(Reason));
	}

	void DocumentCloseAcceptanceCoordinator::FixtureReady(
		const DocumentCloseUiSessionObservation& Session, size_t FixtureSessions, size_t SelectedSessions, size_t NativeHosts)
	{
		if (!m_Scenario || !m_Identity || m_HostID.empty()) return;
		if (FixtureSessions != 1 || SelectedSessions != 1 || NativeHosts != 1 || !Session.Open || !Session.HostSame || !Session.Dirty ||
			Session.Value != 20 || Session.Saved != 10)
		{
			Fail("The isolated setup did not contain exactly one fresh dirty native fixture document.");
			return;
		}
		m_Phase = DocumentCloseUiPhase::AwaitingTextEntry;
		PF_INFO(
			"PF_DOC_CLOSE_UI|READY|scenario={}|host={}|asset={}|saved=10|value=20|dirty=1|fixture_sessions=1|fixture_hosts=1|generation=0",
			*m_Scenario, m_HostID, m_Identity->Asset.ToString());
	}

	bool DocumentCloseAcceptanceCoordinator::BeginCloseRequest(std::string_view HostID, bool Pending, std::string_view PendingHostID)
	{
		if (!IsFixtureHost(HostID)) return true;
		if (Pending && PendingHostID == HostID) return false;
		if (m_Phase == DocumentCloseUiPhase::AwaitingInitialClose)
		{
			AdvanceGeneration();
			m_Phase = DocumentCloseUiPhase::AwaitingFirstAction;
		}
		else if (m_Phase == DocumentCloseUiPhase::AwaitingSecondClose)
		{
			AdvanceGeneration();
			m_Phase = DocumentCloseUiPhase::AwaitingSecondAction;
		}
		else if (m_Phase != DocumentCloseUiPhase::AwaitingFinalClose)
		{
			Fail("A document tab close request arrived in an unexpected test phase.");
			return false;
		}
		return true;
	}

	void DocumentCloseAcceptanceCoordinator::ReportTextField(const DocumentCloseUiTextObservation& Observation)
	{
		if (!m_Scenario || m_Phase != DocumentCloseUiPhase::AwaitingTextEntry || !IsFixtureHost(Observation.HostID) || !m_Identity) return;
		if (!m_TextControlReported)
		{
			PF_INFO("PF_DOC_CLOSE_UI|TEXT_CONTROL|scenario={}|host={}|asset={}|generation=0|x={}|y={}|value=20|active={}|hovered={}|mouse_"
					"x={:.1f}|mouse_y={:.1f}",
				*m_Scenario, m_HostID, m_Identity->Asset.ToString(), Observation.X, Observation.Y, Observation.Active ? 1 : 0,
				Observation.Hovered ? 1 : 0, Observation.MouseX, Observation.MouseY);
			m_LastTextMouseX = Observation.MouseX;
			m_LastTextMouseY = Observation.MouseY;
			m_LastTextHovered = Observation.Hovered;
			m_LastTextActive = Observation.Active;
			m_TextPointerStateReported = true;
			m_TextControlReported = true;
		}
		const bool PointerChanged = Observation.MouseX != m_LastTextMouseX || Observation.MouseY != m_LastTextMouseY ||
									Observation.Hovered != m_LastTextHovered || Observation.Active != m_LastTextActive;
		if (m_TextPointerStateReported && PointerChanged && m_TextPointerReportCount < 8)
		{
			PF_INFO("PF_DOC_CLOSE_UI|TEXT_POINTER|scenario={}|host={}|asset={}|generation=0|x={}|y={}|hovered={}|active={}|mouse_x={:.1f}|"
					"mouse_y={:.1f}",
				*m_Scenario, m_HostID, m_Identity->Asset.ToString(), Observation.X, Observation.Y, Observation.Hovered ? 1 : 0,
				Observation.Active ? 1 : 0, Observation.MouseX, Observation.MouseY);
			m_LastTextMouseX = Observation.MouseX;
			m_LastTextMouseY = Observation.MouseY;
			m_LastTextHovered = Observation.Hovered;
			m_LastTextActive = Observation.Active;
			++m_TextPointerReportCount;
		}
		if (Observation.Clicked && Observation.Active && !m_TextFocusReported)
		{
			PF_INFO("PF_DOC_CLOSE_UI|TEXT_FOCUS|scenario={}|host={}|asset={}|generation=0|active=1|clicked=1|value={}", *m_Scenario,
				m_HostID, m_Identity->Asset.ToString(), Observation.Value);
			m_TextFocusReported = true;
		}
		if (!Observation.Changed || Observation.Value != 42) return;
		if (!Observation.Session.Open || !Observation.Session.HostSame || !Observation.Session.Dirty || Observation.Session.Value != 42 ||
			Observation.Session.Saved != 10 || !m_TextFocusReported)
		{
			Fail("OS text entry did not leave the focused document dirty with its original saved value.");
			return;
		}
		m_Phase = DocumentCloseUiPhase::AwaitingInitialClose;
		m_CloseControlReported = false;
		PF_INFO("PF_DOC_CLOSE_UI|TEXT_ENTRY|scenario={}|host={}|asset={}|generation=0|value=42|saved=10|dirty=1", *m_Scenario, m_HostID,
			m_Identity->Asset.ToString());
	}

	void DocumentCloseAcceptanceCoordinator::ReportDecisionPointer(const DocumentCloseUiDecisionPointerObservation& Observation)
	{
		if (!m_Scenario || !m_Identity || !IsFixtureHost(Observation.HostID) || !Observation.Pending ||
			ExpectedChoice() != Observation.Choice)
			return;
		const auto& Previous = m_LastDecisionPointer;
		const bool Changed = !m_DecisionPointerStateReported || Observation.MouseX != Previous.MouseX ||
							 Observation.MouseY != Previous.MouseY || Observation.Hovered != Previous.Hovered ||
							 Observation.Active != Previous.Active || Observation.MouseDown != Previous.MouseDown ||
							 Observation.Clicked != Previous.Clicked || Observation.RawMouseDown != Previous.RawMouseDown ||
							 Observation.MouseClickedEvent != Previous.MouseClickedEvent ||
							 Observation.MouseReleasedEvent != Previous.MouseReleasedEvent ||
							 Observation.NativeWindowFocused != Previous.NativeWindowFocused ||
							 Observation.NativeWindowHovered != Previous.NativeWindowHovered;
		if (Changed && m_DecisionPointerReportCount < 8)
		{
			PF_INFO(
				"PF_DOC_CLOSE_UI|DECISION_POINTER|scenario={}|host={}|asset={}|generation={}|choice={}|sample={}|frame={}|rect_min={:.1f},{:.1f}|rect_"
				"max={:.1f},{:.1f}|mouse={:.1f},{:.1f}|hovered={}|active={}|filtered_down={}|raw_down={}|raw_clicked={}|raw_released={}|"
				"pressed={}|native_focused={}|native_hovered={}|window_viewport={}|mouse_viewport={}",
				*m_Scenario, m_HostID, m_Identity->Asset.ToString(), m_PromptGeneration, Observation.Choice, m_DecisionPointerReportCount + 1,
				Observation.Frame, Observation.MinimumX, Observation.MinimumY, Observation.MaximumX, Observation.MaximumY,
				Observation.MouseX, Observation.MouseY, Observation.Hovered ? 1 : 0, Observation.Active ? 1 : 0,
				Observation.MouseDown ? 1 : 0, Observation.RawMouseDown ? 1 : 0, Observation.MouseClickedEvent ? 1 : 0,
				Observation.MouseReleasedEvent ? 1 : 0, Observation.Clicked ? 1 : 0, Observation.NativeWindowFocused ? 1 : 0,
				Observation.NativeWindowHovered ? 1 : 0, Observation.WindowViewport, Observation.MouseViewport);
			++m_DecisionPointerReportCount;
		}
		if (!m_LastDecisionHeartbeatFrame || Observation.Frame - m_LastDecisionHeartbeatFrame >= 60)
		{
			if (m_DecisionHeartbeatCount < 8)
			{
				PF_INFO("PF_DOC_CLOSE_DIAG|DECISION_HEARTBEAT|scenario={}|host={}|asset={}|generation={}|choice={}|frame={}|phase={}|"
						"hovered={}|active={}|filtered_down={}|raw_down={}|raw_clicked={}|raw_released={}|pressed={}|native_focused={}|"
						"native_hovered={}|window_viewport={}|mouse_viewport={}",
					*m_Scenario, m_HostID, m_Identity->Asset.ToString(), m_PromptGeneration, Observation.Choice, Observation.Frame,
					DocumentCloseUiPhaseName(m_Phase), Observation.Hovered ? 1 : 0, Observation.Active ? 1 : 0,
					Observation.MouseDown ? 1 : 0, Observation.RawMouseDown ? 1 : 0, Observation.MouseClickedEvent ? 1 : 0,
					Observation.MouseReleasedEvent ? 1 : 0, Observation.Clicked ? 1 : 0, Observation.NativeWindowFocused ? 1 : 0,
					Observation.NativeWindowHovered ? 1 : 0, Observation.WindowViewport, Observation.MouseViewport);
				++m_DecisionHeartbeatCount;
			}
			m_LastDecisionHeartbeatFrame = Observation.Frame;
		}
		m_LastDecisionPointer = Observation;
		m_DecisionPointerStateReported = true;
	}

	void DocumentCloseAcceptanceCoordinator::ReportGlfwMouseButton(
		const DocumentCloseUiMouseButtonObservation& Observation)
	{
		if (!m_Scenario || !m_Identity || m_Phase == DocumentCloseUiPhase::Failed || m_Phase == DocumentCloseUiPhase::Complete ||
			m_GlfwMouseButtonReportCount >= 16)
			return;
		const char* ActionName = "unknown";
		switch (Observation.Action)
		{
		case DocumentCloseUiMouseAction::Release: ActionName = "release"; break;
		case DocumentCloseUiMouseAction::Press: ActionName = "press"; break;
		case DocumentCloseUiMouseAction::Repeat: ActionName = "repeat"; break;
		}
		PF_INFO("PF_DOC_CLOSE_DIAG|GLFW_MOUSE_BUTTON|scenario={}|host={}|asset={}|generation={}|sequence={}|viewport={}|main_window={}|"
				"native_window=0x{:x}|button=left|action={}|modifiers={}|local={},{}|steady_ms={:.3f}|native_focused={}|native_hovered={}",
			*m_Scenario, m_HostID, m_Identity->Asset.ToString(), m_PromptGeneration, ++m_GlfwMouseButtonReportCount, Observation.Viewport,
			Observation.MainWindow ? 1 : 0, Observation.NativeWindowIdentity, ActionName, Observation.Modifiers,
			Observation.LocalX, Observation.LocalY, Observation.SteadyMilliseconds, Observation.NativeWindowFocused ? 1 : 0,
			Observation.NativeWindowHovered ? 1 : 0);
	}

	void DocumentCloseAcceptanceCoordinator::ReportCloseControl(
		std::string_view HostID, const EditorDocumentIdentity& Identity, int X, int Y)
	{
		if (!m_Scenario || !IsFixtureHost(HostID) || !IsFixtureIdentity(Identity) || !IsAwaitingTabClose()) return;
		if (m_CloseControlReported && X == m_LastCloseControlX && Y == m_LastCloseControlY) return;
		PF_INFO("PF_DOC_CLOSE_UI|CLOSE_CONTROL|scenario={}|host={}|asset={}|generation={}|sample={}|x={}|y={}", *m_Scenario, m_HostID,
			Identity.Asset.ToString(), m_PromptGeneration, ++m_CloseControlSample, X, Y);
		m_CloseControlReported = true;
		m_LastCloseControlX = X;
		m_LastCloseControlY = Y;
	}

	void DocumentCloseAcceptanceCoordinator::ReportTabInput(
		std::string_view HostID, const EditorDocumentIdentity& Identity, const DocumentCloseUiTabInputObservation& Observation)
	{
		if (!m_Scenario || !IsFixtureHost(HostID) || !IsFixtureIdentity(Identity) || !IsAwaitingTabClose()) return;
		const bool Relevant = Observation.TabHovered || Observation.CloseHovered || Observation.ItemActive || Observation.MouseDown ||
							  Observation.MouseClicked || Observation.MouseReleased || Observation.Pressed;
		const bool WasRelevant =
			m_LastTabInput &&
			(m_LastTabInput->TabHovered || m_LastTabInput->CloseHovered || m_LastTabInput->ItemActive || m_LastTabInput->MouseDown ||
				m_LastTabInput->MouseClicked || m_LastTabInput->MouseReleased || m_LastTabInput->Pressed);
		const bool InputBoundary = Observation.MouseDown || Observation.MouseClicked || Observation.MouseReleased || Observation.Pressed ||
								   (m_LastTabInput && (m_LastTabInput->MouseDown || m_LastTabInput->MouseClicked ||
														  m_LastTabInput->MouseReleased || m_LastTabInput->Pressed));
		const bool Changed =
			!m_LastTabInput || Observation.WindowViewport != m_LastTabInput->WindowViewport ||
			Observation.MouseViewport != m_LastTabInput->MouseViewport ||
			Observation.NativeWindowKnown != m_LastTabInput->NativeWindowKnown ||
			Observation.NativeWindowFocused != m_LastTabInput->NativeWindowFocused ||
			Observation.NativeWindowHovered != m_LastTabInput->NativeWindowHovered ||
			(InputBoundary && (Observation.MouseX != m_LastTabInput->MouseX || Observation.MouseY != m_LastTabInput->MouseY)) ||
			Observation.TabHovered != m_LastTabInput->TabHovered || Observation.CloseHovered != m_LastTabInput->CloseHovered ||
			Observation.ItemActive != m_LastTabInput->ItemActive || Observation.MouseDown != m_LastTabInput->MouseDown ||
			Observation.MouseClicked != m_LastTabInput->MouseClicked || Observation.MouseReleased != m_LastTabInput->MouseReleased ||
			Observation.Pressed != m_LastTabInput->Pressed || Observation.MinimumX != m_LastTabInput->MinimumX ||
			Observation.MinimumY != m_LastTabInput->MinimumY || Observation.MaximumX != m_LastTabInput->MaximumX ||
			Observation.MaximumY != m_LastTabInput->MaximumY;
		m_LastTabInput = Observation;
		if (!Relevant && !WasRelevant) return;
		if (!Changed) return;
		PF_INFO("PF_DOC_CLOSE_DIAG|TAB_INPUT|scenario={}|host={}|asset={}|generation={}|sample={}|viewport={}|mouse_viewport={}|rect={},{},"
				"{},{}|mouse={},{}|native_known={}|native_focused={}|native_hovered={}|tab_hovered={}|close_hovered={}|active={}|down={}|"
				"clicked={}|released={}|pressed={}",
			*m_Scenario, m_HostID, Identity.Asset.ToString(), m_PromptGeneration, ++m_TabInputSample, Observation.WindowViewport,
			Observation.MouseViewport, Observation.MinimumX, Observation.MinimumY, Observation.MaximumX, Observation.MaximumY,
			Observation.MouseX, Observation.MouseY, Observation.NativeWindowKnown ? 1 : 0, Observation.NativeWindowFocused ? 1 : 0,
			Observation.NativeWindowHovered ? 1 : 0, Observation.TabHovered ? 1 : 0, Observation.CloseHovered ? 1 : 0,
			Observation.ItemActive ? 1 : 0, Observation.MouseDown ? 1 : 0, Observation.MouseClicked ? 1 : 0,
			Observation.MouseReleased ? 1 : 0, Observation.Pressed ? 1 : 0);
	}

	void DocumentCloseAcceptanceCoordinator::ReportTabCloseRequest(std::string_view HostID, const EditorDocumentIdentity& Identity,
		bool RequestAlreadyPending, std::string_view PendingHostID, const DocumentCloseUiSessionObservation& Session)
	{
		if (!m_Scenario || !IsFixtureHost(HostID) || !IsFixtureIdentity(Identity) || !IsAwaitingTabClose()) return;
		PF_INFO("PF_DOC_CLOSE_DIAG|TAB_CLOSE_REQUEST|scenario={}|host={}|asset={}|generation={}|pending_before={}|pending_host={}|open={}|"
				"dirty={}|value={}|saved={}",
			*m_Scenario, m_HostID, Identity.Asset.ToString(), m_PromptGeneration, RequestAlreadyPending ? 1 : 0, PendingHostID,
			Session.Open ? 1 : 0, Session.Dirty ? 1 : 0, Session.Value, Session.Saved);
	}

	void DocumentCloseAcceptanceCoordinator::ReportPrompt(
		std::string_view HostID, bool Pending, const DocumentCloseUiPromptGeometry& Geometry)
	{
		if (!m_Scenario || !m_Identity || !IsFixtureHost(HostID) || !Pending) return;
		const bool First = m_PromptGenerationReported != m_PromptGeneration;
		const bool Changed = First || Geometry.SaveX != m_LastPromptGeometry.SaveX || Geometry.SaveY != m_LastPromptGeometry.SaveY ||
							 Geometry.DiscardX != m_LastPromptGeometry.DiscardX || Geometry.DiscardY != m_LastPromptGeometry.DiscardY ||
							 Geometry.CancelX != m_LastPromptGeometry.CancelX || Geometry.CancelY != m_LastPromptGeometry.CancelY;
		if (!Changed) return;
		const size_t Sample = ++m_PromptGeometrySample;
		if (First)
		{
			m_PromptGenerationReported = m_PromptGeneration;
			PF_INFO("PF_DOC_CLOSE_UI|PROMPT|scenario={}|host={}|asset={}|generation={}|sample={}|save={},{}|discard={},{}|cancel={},{}",
				*m_Scenario, m_HostID, m_Identity->Asset.ToString(), m_PromptGeneration, Sample, Geometry.SaveX, Geometry.SaveY,
				Geometry.DiscardX, Geometry.DiscardY, Geometry.CancelX, Geometry.CancelY);
		}
		else
		{
			PF_INFO("PF_DOC_CLOSE_UI|PROMPT_GEOMETRY|scenario={}|host={}|asset={}|generation={}|sample={}|save={},{}|discard={},{}|cancel={"
					"},{}",
				*m_Scenario, m_HostID, m_Identity->Asset.ToString(), m_PromptGeneration, Sample, Geometry.SaveX, Geometry.SaveY,
				Geometry.DiscardX, Geometry.DiscardY, Geometry.CancelX, Geometry.CancelY);
		}
		m_LastPromptGeometry = Geometry;
	}

	void DocumentCloseAcceptanceCoordinator::ReportAction(
		std::string_view Choice, bool Resolved, const DocumentCloseUiSessionObservation& Session)
	{
		if (!m_Scenario || !m_Identity || m_Phase == DocumentCloseUiPhase::Failed || m_Phase == DocumentCloseUiPhase::Complete) return;
		const std::string Error = EscapeDocumentCloseUiField(Session.Error);
		++m_ActionSequence;
		PF_INFO("PF_DOC_CLOSE_UI|ACTION|scenario={}|host={}|asset={}|sequence={}|generation={}|choice={}|resolved={}|pending={}|open={}|"
				"host_same={}|dirty={}|value={}|saved={}|error={}",
			*m_Scenario, m_HostID, m_Identity->Asset.ToString(), m_ActionSequence, m_PromptGeneration, Choice, Resolved ? 1 : 0,
			Session.Pending ? 1 : 0, Session.Open ? 1 : 0, Session.HostSame ? 1 : 0, Session.Dirty ? 1 : 0, Session.Value, Session.Saved,
			Error);
		const bool InitialDirtyState =
			Session.Pending && Session.Open && Session.HostSame && Session.Dirty && Session.Value == 42 && Session.Saved == 10;
		if (*m_Scenario == "cancel-save")
		{
			if (m_Phase == DocumentCloseUiPhase::AwaitingFirstAction && Choice == "cancel" && Resolved &&
				StateMatches(Session, false, true, true, true, 42, 10))
			{
				m_Phase = DocumentCloseUiPhase::AwaitingSecondClose;
				AdvanceGeneration();
				return;
			}
			if (m_Phase == DocumentCloseUiPhase::AwaitingSecondAction && Choice == "save" && Resolved &&
				StateMatches(Session, false, false, false, false, 42, 42))
			{
				Pass(42);
				return;
			}
		}
		else if (*m_Scenario == "discard")
		{
			if (m_Phase == DocumentCloseUiPhase::AwaitingFirstAction && Choice == "discard" && Resolved &&
				StateMatches(Session, false, false, false, false, 10, 10))
			{
				Pass(10);
				return;
			}
		}
		else if (*m_Scenario == "save-retry")
		{
			if (m_Phase == DocumentCloseUiPhase::AwaitingFirstAction && Choice == "save" && !Resolved && InitialDirtyState &&
				Session.Error == "The document could not be saved; it remains open.")
			{
				m_Phase = DocumentCloseUiPhase::AwaitingSaveRetry;
				AdvanceGeneration();
				return;
			}
			if (m_Phase == DocumentCloseUiPhase::AwaitingSaveRetry && Choice == "save" && Resolved &&
				StateMatches(Session, false, false, false, false, 42, 42))
			{
				Pass(42);
				return;
			}
		}
		else if (*m_Scenario == "close-retry")
		{
			if (m_Phase == DocumentCloseUiPhase::AwaitingFirstAction && Choice == "save" && !Resolved &&
				StateMatches(Session, true, true, true, false, 42, 42) &&
				Session.Error == "The Debug fixture is configured to reject close.")
			{
				m_Phase = DocumentCloseUiPhase::AwaitingCloseRetryCancel;
				AdvanceGeneration();
				return;
			}
			if (m_Phase == DocumentCloseUiPhase::AwaitingCloseRetryCancel && Choice == "cancel" && Resolved &&
				StateMatches(Session, false, true, true, false, 42, 42))
			{
				m_Phase = DocumentCloseUiPhase::AwaitingAllowClose;
				AdvanceGeneration();
				m_AllowCloseRectReported = false;
				return;
			}
		}
		Fail("The real close-dialog action produced an unexpected document/session state.");
	}

	void DocumentCloseAcceptanceCoordinator::ReportAllowClose(
		std::string_view HostID, int X, int Y, bool Clicked, const DocumentCloseUiSessionObservation& Session)
	{
		if (!m_Scenario || *m_Scenario != "close-retry" || m_Phase != DocumentCloseUiPhase::AwaitingAllowClose || !IsFixtureHost(HostID) ||
			!m_Identity)
			return;
		if (!m_AllowCloseRectReported)
		{
			PF_INFO("PF_DOC_CLOSE_UI|FIXTURE|scenario=close-retry|host={}|asset={}|generation={}", m_HostID, m_Identity->Asset.ToString(),
				m_PromptGeneration);
			m_AllowCloseRectReported = true;
		}
		if (!m_AllowCloseControlReported || X != m_LastAllowCloseX || Y != m_LastAllowCloseY)
		{
			PF_INFO("PF_DOC_CLOSE_UI|ALLOW_CLOSE_CONTROL|scenario=close-retry|host={}|asset={}|generation={}|sample={}|x={}|y={}", m_HostID,
				m_Identity->Asset.ToString(), m_PromptGeneration, ++m_AllowCloseControlSample, X, Y);
			m_AllowCloseControlReported = true;
			m_LastAllowCloseX = X;
			m_LastAllowCloseY = Y;
		}
		if (!Clicked) return;
		++m_ActionSequence;
		PF_INFO("PF_DOC_CLOSE_UI|ACTION|scenario=close-retry|host={}|asset={}|sequence={}|generation={}|choice=allow-close|resolved=1|"
				"pending={}|open={}|host_same={}|dirty={}|value={}|saved={}|error=",
			m_HostID, m_Identity->Asset.ToString(), m_ActionSequence, m_PromptGeneration, Session.Pending ? 1 : 0, Session.Open ? 1 : 0,
			Session.HostSame ? 1 : 0, Session.Dirty ? 1 : 0, Session.Value, Session.Saved);
		if (StateMatches(Session, false, true, true, false, 42, 42))
		{
			m_Phase = DocumentCloseUiPhase::AwaitingFinalClose;
			AdvanceGeneration();
		}
		else Fail("The Allow close control did not preserve the saved document session.");
	}

	void DocumentCloseAcceptanceCoordinator::ReportCleanClose(bool IdentityClosed, bool HostRemoved, int SavedValue)
	{
		if (!m_Scenario || *m_Scenario != "close-retry" || m_Phase != DocumentCloseUiPhase::AwaitingFinalClose || !m_Identity) return;
		if (!IdentityClosed || !HostRemoved || SavedValue != 42)
		{
			Fail("Clean tab close did not remove the document and native host after recovery.");
			return;
		}
		PF_INFO("PF_DOC_CLOSE_UI|CLOSED|scenario=close-retry|host={}|asset={}|sequence={}|generation={}|open=0|saved=42", m_HostID,
			m_Identity->Asset.ToString(), m_ActionSequence, m_PromptGeneration);
		Pass(42);
	}

	bool DocumentCloseAcceptanceCoordinator::StateMatches(const DocumentCloseUiSessionObservation& Session, bool Pending, bool Open,
		bool HostSame, bool Dirty, int Value, int Saved) const noexcept
	{
		return Session.Pending == Pending && Session.Open == Open && Session.HostSame == HostSame && Session.HostExists == HostSame &&
			   Session.Dirty == Dirty && Session.Value == Value && Session.Saved == Saved;
	}

	void DocumentCloseAcceptanceCoordinator::AdvanceGeneration() noexcept
	{
		++m_PromptGeneration;
		m_CloseControlReported = false;
		m_AllowCloseControlReported = false;
		m_DecisionPointerStateReported = false;
		m_DecisionPointerReportCount = 0;
		m_LastDecisionHeartbeatFrame = 0;
		m_DecisionHeartbeatCount = 0;
		m_GlfwMouseButtonReportCount = 0;
	}

	void DocumentCloseAcceptanceCoordinator::Pass(int ExpectedSavedValue)
	{
		if (!m_Scenario || !m_Identity) return;
		m_Phase = DocumentCloseUiPhase::Complete;
		PF_INFO("PF_DOC_CLOSE_UI|PASS|scenario={}|host={}|asset={}|sequence={}|generation={}|open=0|saved={}", *m_Scenario, m_HostID,
			m_Identity->Asset.ToString(), m_ActionSequence, m_PromptGeneration, ExpectedSavedValue);
	}

	EditorRuntimePlayStopCoordinator::EditorRuntimePlayStopCoordinator(bool Enabled) noexcept
		: m_Phase(Enabled ? Phase::WaitingForEditViewportRender : Phase::Disabled)
	{
	}

	void EditorRuntimePlayStopCoordinator::ObserveViewportRender(bool Runtime) noexcept
	{
		if (Runtime) m_PlayViewportRendered = true;
		else m_EditViewportRendered = true;
	}

	EditorRuntimePlayStopAction EditorRuntimePlayStopCoordinator::Advance(bool RuntimeRunning)
	{
		if (!IsEnabled() || m_Phase == Phase::Complete || m_Phase == Phase::Failed) return EditorRuntimePlayStopAction::Wait;
		if (++m_Frames > 600)
		{
			SetFailure(m_Phase == Phase::WaitingForEditViewportRender
						   ? "Timed out waiting for a successful Edit-mode scene viewport render."
						   : "Timed out waiting for a successful Play-mode scene viewport render.");
			return EditorRuntimePlayStopAction::Wait;
		}
		if (m_Phase == Phase::WaitingForPlayViewportRender && !RuntimeRunning)
		{
			SetFailure("The runtime stopped before a Play-mode scene viewport render completed.");
			return EditorRuntimePlayStopAction::Wait;
		}
		if (m_Phase == Phase::WaitingForEditViewportRender && m_EditViewportRendered) return EditorRuntimePlayStopAction::BeginPlay;
		if (m_Phase == Phase::WaitingForPlayViewportRender && m_PlayViewportRendered) return EditorRuntimePlayStopAction::StopRuntime;
		return EditorRuntimePlayStopAction::Wait;
	}

	bool EditorRuntimePlayStopCoordinator::BeginPlaySnapshot(
		const PulseForge::Scene* AuthoredScene, size_t EntityCount, std::string Snapshot)
	{
		if (m_Phase != Phase::WaitingForEditViewportRender || !AuthoredScene)
		{
			SetFailure("The Edit-mode viewport rendered without an active project scene.");
			return false;
		}
		m_AuthoredSceneIdentity = AuthoredScene;
		m_AuthoredEntityCount = EntityCount;
		m_AuthoredSnapshot = std::move(Snapshot);
		return true;
	}

	bool EditorRuntimePlayStopCoordinator::ObservePlayStarted(bool Running, const PulseForge::Scene* RuntimeScene)
	{
		if (m_Phase != Phase::WaitingForEditViewportRender || !m_AuthoredSceneIdentity || !Running || !RuntimeScene ||
			RuntimeScene == m_AuthoredSceneIdentity)
		{
			SetFailure("Play did not start a running runtime over an isolated scene copy.");
			return false;
		}
		m_Phase = Phase::WaitingForPlayViewportRender;
		PF_INFO("PULSEFORGE_EDITOR_RUNTIME_PLAY_STOP=PLAY_STARTED; the isolated runtime scene is active.");
		return true;
	}

	bool EditorRuntimePlayStopCoordinator::ObserveStop(
		const PulseForge::Scene* AuthoredScene, size_t EntityCount, std::optional<std::string> Snapshot)
	{
		if (m_Phase != Phase::WaitingForPlayViewportRender) return false;
		if (!AuthoredScene || AuthoredScene != m_AuthoredSceneIdentity || EntityCount != m_AuthoredEntityCount)
		{
			SetFailure("Stop did not restore the same authored scene and entity count.");
			return false;
		}
		if (!Snapshot || *Snapshot != m_AuthoredSnapshot)
		{
			SetFailure("The authored scene changed during the isolated Play/Stop transition.");
			return false;
		}
		m_Phase = Phase::Complete;
		PF_INFO("PULSEFORGE_EDITOR_RUNTIME_PLAY_STOP=PASS; synthetic EditorApplication Play/Stop transition rendered successfully in "
				"Edit and Play states; the authored scene was preserved.");
		return true;
	}

	void EditorRuntimePlayStopCoordinator::Fail(std::string Reason)
	{
		SetFailure(std::move(Reason));
	}

	void EditorRuntimePlayStopCoordinator::SetFailure(std::string Reason)
	{
		if (m_Phase == Phase::Failed || m_Phase == Phase::Complete || m_Phase == Phase::Disabled) return;
		m_Phase = Phase::Failed;
		m_FailureReason = std::move(Reason);
	}

	SceneViewportNativeSmokeCoordinator::SceneViewportNativeSmokeCoordinator(bool Enabled) noexcept
		: m_Phase(Enabled ? Phase::AwaitingMainScene : Phase::Disabled)
	{
	}

	bool SceneViewportNativeSmokeCoordinator::IsEnabled() const noexcept
	{
		return m_Phase != Phase::Disabled;
	}

	bool SceneViewportNativeSmokeCoordinator::Complete() const noexcept
	{
		return m_Phase == Phase::Complete;
	}

	bool SceneViewportNativeSmokeCoordinator::IsFinite(const std::array<float, 3>& Vector) noexcept
	{
		return std::isfinite(Vector[0]) && std::isfinite(Vector[1]) && std::isfinite(Vector[2]);
	}

	float SceneViewportNativeSmokeCoordinator::Distance(
		const std::array<float, 3>& Left, const std::array<float, 3>& Right) noexcept
	{
		const float X = Left[0] - Right[0];
		const float Y = Left[1] - Right[1];
		const float Z = Left[2] - Right[2];
		return std::sqrt(X * X + Y * Y + Z * Z);
	}

	std::string SceneViewportNativeSmokeCoordinator::FormatGeometry(const SceneViewportNativeSmokeObservation& Observation) const
	{
		std::ostringstream Stream;
		Stream << std::fixed << std::setprecision(1) << "title_center=" << Observation.TitleCenter[0] << ','
			   << Observation.TitleCenter[1] << "; dock_tab_rect=" << Observation.DockTabRect[0] << ',' << Observation.DockTabRect[1]
			   << ',' << Observation.DockTabRect[2] << ',' << Observation.DockTabRect[3] << "; image_min="
			   << Observation.ImageMinimum[0] << ',' << Observation.ImageMinimum[1]
			   << "; image_max=" << Observation.ImageMaximum[0] << ',' << Observation.ImageMaximum[1] << "; main_rect="
			   << Observation.MainWindowRect[0] << ',' << Observation.MainWindowRect[1] << ',' << Observation.MainWindowRect[2] << ','
			   << Observation.MainWindowRect[3] << "; owner_framebuffer_scale=" << Observation.FramebufferScale[0] << ','
			   << Observation.FramebufferScale[1] << "; tear_off_point=" << Observation.TearOffPoint[0] << ','
			   << Observation.TearOffPoint[1];
		return Stream.str();
	}

	std::optional<std::string> SceneViewportNativeSmokeCoordinator::Observe(
		const SceneViewportNativeSmokeObservation& Observation)
	{
		if (!IsEnabled() || m_Phase == Phase::Complete || !Observation.SceneImageReady || Observation.MainViewportID == 0 ||
			Observation.OwnerViewportID == 0 || Observation.OwnerWindow == 0 || !IsFinite(Observation.CameraForward))
			return std::nullopt;

		if (m_Phase == Phase::AwaitingMainScene)
		{
			if (Observation.OwnerViewportID != Observation.MainViewportID || !Observation.DockTabReady) return std::nullopt;
			m_MainViewportID = Observation.MainViewportID;
			m_Phase = Phase::AwaitingNativeDetach;
			std::ostringstream Stream;
			Stream << "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=READY; main_viewport=" << m_MainViewportID << "; owner_viewport="
				   << Observation.OwnerViewportID << "; owner_window=0x" << std::hex << Observation.OwnerWindow << std::dec << "; "
				   << FormatGeometry(Observation);
			return Stream.str();
		}

		if (m_Phase == Phase::AwaitingNativeDetach)
		{
			if (Observation.MainViewportID != m_MainViewportID || Observation.OwnerViewportID == m_MainViewportID) return std::nullopt;
			m_DetachedViewportID = Observation.OwnerViewportID;
			m_DetachedWindow = Observation.OwnerWindow;
			m_InitialCameraForward = Observation.CameraForward;
			m_Phase = Phase::AwaitingCameraCapture;
			std::ostringstream Stream;
			Stream << "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=DETACHED; main_viewport=" << m_MainViewportID << "; owner_viewport="
				   << m_DetachedViewportID << "; owner_window=0x" << std::hex << m_DetachedWindow << std::dec << "; "
				   << FormatGeometry(Observation);
			return Stream.str();
		}

		if (Observation.OwnerViewportID != m_DetachedViewportID || Observation.OwnerWindow != m_DetachedWindow) return std::nullopt;

		if (m_Phase == Phase::AwaitingCameraCapture && Observation.CameraCaptureActive)
		{
			m_CaptureWasObserved = true;
			m_Phase = Phase::AwaitingCameraRelease;
			return "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=CAPTURED; detached owner accepted right-button camera capture";
		}

		if (m_Phase == Phase::AwaitingCameraRelease && m_CaptureWasObserved && !Observation.CameraCaptureActive &&
			Distance(m_InitialCameraForward, Observation.CameraForward) > 0.02f)
		{
			m_Phase = Phase::Complete;
			std::ostringstream Stream;
			Stream << std::fixed << std::setprecision(4) << "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=PASS; main_viewport=" << m_MainViewportID
				   << "; detached_viewport=" << m_DetachedViewportID << "; owner_window=0x" << std::hex << m_DetachedWindow << std::dec
				   << "; right-button capture began and ended on the detached owner; camera_forward_before="
				   << m_InitialCameraForward[0] << ',' << m_InitialCameraForward[1] << ',' << m_InitialCameraForward[2]
				   << "; camera_forward_after=" << Observation.CameraForward[0] << ',' << Observation.CameraForward[1] << ','
				   << Observation.CameraForward[2];
			return Stream.str();
		}

		return std::nullopt;
	}
} // namespace PulseForgeEditor::Detail

#endif
