#pragma once

#if defined(_DEBUG) || defined(PF_DEBUG)

#include "Editor/EditorDocumentManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	class Scene;
}

namespace PulseForgeEditor::Detail
{
	struct DocumentCloseUiSessionObservation
	{
		bool Pending = false;
		bool Open = false;
		bool HostSame = false;
		bool Dirty = false;
		bool HostExists = false;
		int Value = -1;
		int Saved = -1;
		std::string Error;
	};

	struct DocumentCloseUiTextObservation
	{
		std::string HostID;
		int X = 0;
		int Y = 0;
		bool Active = false;
		bool Hovered = false;
		bool Clicked = false;
		bool Changed = false;
		int Value = 0;
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		DocumentCloseUiSessionObservation Session;
	};

	struct DocumentCloseUiDecisionPointerObservation
	{
		std::string HostID;
		std::string Choice;
		bool Pending = false;
		bool Hovered = false;
		bool Active = false;
		bool Clicked = false;
		bool MouseDown = false;
		bool RawMouseDown = false;
		bool MouseClickedEvent = false;
		bool MouseReleasedEvent = false;
		bool NativeWindowFocused = false;
		bool NativeWindowHovered = false;
		int Frame = 0;
		float MinimumX = 0.0f;
		float MinimumY = 0.0f;
		float MaximumX = 0.0f;
		float MaximumY = 0.0f;
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		uint32_t WindowViewport = 0;
		uint32_t MouseViewport = 0;
	};

	enum class DocumentCloseUiMouseAction : uint8_t
	{
		Release,
		Press,
		Repeat
	};

	struct DocumentCloseUiMouseButtonObservation
	{
		uint32_t Viewport = 0;
		uintptr_t NativeWindowIdentity = 0;
		DocumentCloseUiMouseAction Action = DocumentCloseUiMouseAction::Release;
		int Modifiers = 0;
		double LocalX = 0.0;
		double LocalY = 0.0;
		double SteadyMilliseconds = 0.0;
		bool MainWindow = false;
		bool NativeWindowFocused = false;
		bool NativeWindowHovered = false;
	};

	struct DocumentCloseUiTabInputObservation
	{
		uint32_t WindowViewport = 0;
		uint32_t MouseViewport = 0;
		int MinimumX = 0;
		int MinimumY = 0;
		int MaximumX = 0;
		int MaximumY = 0;
		int MouseX = 0;
		int MouseY = 0;
		bool NativeWindowKnown = false;
		bool NativeWindowFocused = false;
		bool NativeWindowHovered = false;
		bool TabHovered = false;
		bool CloseHovered = false;
		bool ItemActive = false;
		bool MouseDown = false;
		bool MouseClicked = false;
		bool MouseReleased = false;
		bool Pressed = false;
	};

	struct DocumentCloseUiPromptGeometry
	{
		int SaveX = 0;
		int SaveY = 0;
		int DiscardX = 0;
		int DiscardY = 0;
		int CancelX = 0;
		int CancelY = 0;
	};

	enum class DocumentCloseUiPhase : uint8_t
	{
		Disabled,
		AwaitingTextEntry,
		AwaitingInitialClose,
		AwaitingFirstAction,
		AwaitingSecondClose,
		AwaitingSecondAction,
		AwaitingSaveRetry,
		AwaitingCloseRetryCancel,
		AwaitingAllowClose,
		AwaitingFinalClose,
		Complete,
		Failed
	};

	class DocumentCloseAcceptanceCoordinator final
	{
	public:
		explicit DocumentCloseAcceptanceCoordinator(
			std::optional<std::string> Scenario, std::function<void(std::string)> ReportApplicationError);

		[[nodiscard]] bool IsEnabled() const noexcept { return m_Scenario.has_value(); }
		[[nodiscard]] std::string_view Scenario() const noexcept;
		[[nodiscard]] DocumentCloseUiPhase Phase() const noexcept { return m_Phase; }
		[[nodiscard]] bool Complete() const noexcept { return m_Phase == DocumentCloseUiPhase::Complete; }
		[[nodiscard]] const std::optional<EditorDocumentIdentity>& Identity() const noexcept { return m_Identity; }
		[[nodiscard]] std::string_view HostID() const noexcept { return m_HostID; }
		[[nodiscard]] bool IsFixtureIdentity(const EditorDocumentIdentity& Identity) const noexcept;
		[[nodiscard]] bool IsFixtureHost(std::string_view HostID) const noexcept;
		[[nodiscard]] bool IsAwaitingFinalClose() const noexcept { return m_Phase == DocumentCloseUiPhase::AwaitingFinalClose; }
		[[nodiscard]] size_t PromptGeneration() const noexcept { return m_PromptGeneration; }
		[[nodiscard]] bool IsAwaitingTabClose() const noexcept;
		[[nodiscard]] std::string_view ExpectedChoice() const noexcept;

		void SetIdentity(EditorDocumentIdentity Identity);
		void SetHost(std::string HostID);
		void Fail(std::string_view Reason);
		void FixtureReady(
			const DocumentCloseUiSessionObservation& Session, size_t FixtureSessions, size_t SelectedSessions, size_t NativeHosts);
		[[nodiscard]] bool BeginCloseRequest(std::string_view HostID, bool Pending, std::string_view PendingHostID);
		void ReportTextField(const DocumentCloseUiTextObservation& Observation);
		void ReportDecisionPointer(const DocumentCloseUiDecisionPointerObservation& Observation);
		void ReportGlfwMouseButton(const DocumentCloseUiMouseButtonObservation& Observation);
		void ReportCloseControl(std::string_view HostID, const EditorDocumentIdentity& Identity, int X, int Y);
		void ReportTabInput(
			std::string_view HostID, const EditorDocumentIdentity& Identity, const DocumentCloseUiTabInputObservation& Observation);
		void ReportTabCloseRequest(std::string_view HostID, const EditorDocumentIdentity& Identity, bool RequestAlreadyPending,
			std::string_view PendingHostID, const DocumentCloseUiSessionObservation& Session);
		void ReportPrompt(std::string_view HostID, bool Pending, const DocumentCloseUiPromptGeometry& Geometry);
		void ReportAction(std::string_view Choice, bool Resolved, const DocumentCloseUiSessionObservation& Session);
		void ReportAllowClose(std::string_view HostID, int X, int Y, bool Clicked, const DocumentCloseUiSessionObservation& Session);
		void ReportCleanClose(bool IdentityClosed, bool HostRemoved, int SavedValue);

	private:
		[[nodiscard]] bool StateMatches(const DocumentCloseUiSessionObservation& Session, bool Pending, bool Open, bool HostSame,
			bool Dirty, int Value, int Saved) const noexcept;
		void AdvanceGeneration() noexcept;
		void Pass(int ExpectedSavedValue);

		std::optional<std::string> m_Scenario;
		std::function<void(std::string)> m_ReportApplicationError;
		DocumentCloseUiPhase m_Phase = DocumentCloseUiPhase::Disabled;
		std::optional<EditorDocumentIdentity> m_Identity;
		std::string m_HostID;
		size_t m_PromptGeneration = 0;
		size_t m_PromptGenerationReported = 0;
		size_t m_PromptGeometrySample = 0;
		DocumentCloseUiPromptGeometry m_LastPromptGeometry{};
		size_t m_ActionSequence = 0;
		bool m_AllowCloseRectReported = false;
		bool m_AllowCloseControlReported = false;
		size_t m_AllowCloseControlSample = 0;
		int m_LastAllowCloseX = 0;
		int m_LastAllowCloseY = 0;
		bool m_CloseControlReported = false;
		size_t m_CloseControlSample = 0;
		int m_LastCloseControlX = 0;
		int m_LastCloseControlY = 0;
		std::optional<DocumentCloseUiTabInputObservation> m_LastTabInput;
		size_t m_TabInputSample = 0;
		bool m_TextControlReported = false;
		bool m_TextFocusReported = false;
		bool m_TextPointerStateReported = false;
		float m_LastTextMouseX = 0.0f;
		float m_LastTextMouseY = 0.0f;
		bool m_LastTextHovered = false;
		bool m_LastTextActive = false;
		size_t m_TextPointerReportCount = 0;
		bool m_DecisionPointerStateReported = false;
		DocumentCloseUiDecisionPointerObservation m_LastDecisionPointer{};
		size_t m_DecisionPointerReportCount = 0;
		int m_LastDecisionHeartbeatFrame = 0;
		size_t m_DecisionHeartbeatCount = 0;
		size_t m_GlfwMouseButtonReportCount = 0;
	};

	enum class EditorRuntimePlayStopAction : uint8_t
	{
		Wait,
		BeginPlay,
		StopRuntime
	};

	class EditorRuntimePlayStopCoordinator final
	{
	public:
		EditorRuntimePlayStopCoordinator() = default;
		explicit EditorRuntimePlayStopCoordinator(bool Enabled) noexcept;

		[[nodiscard]] bool IsEnabled() const noexcept { return m_Phase != Phase::Disabled; }
		void ObserveViewportRender(bool Runtime) noexcept;
		[[nodiscard]] EditorRuntimePlayStopAction Advance(bool RuntimeRunning);
		[[nodiscard]] std::string_view FailureReason() const noexcept { return m_FailureReason; }
		[[nodiscard]] bool BeginPlaySnapshot(const PulseForge::Scene* AuthoredScene, size_t EntityCount, std::string Snapshot);
		[[nodiscard]] bool ObservePlayStarted(bool Running, const PulseForge::Scene* RuntimeScene);
		[[nodiscard]] bool ObserveStop(const PulseForge::Scene* AuthoredScene, size_t EntityCount, std::optional<std::string> Snapshot);
		void Fail(std::string Reason);
		[[nodiscard]] bool Complete() const noexcept { return m_Phase == Phase::Complete; }
		[[nodiscard]] bool WaitingForPlayViewportRender() const noexcept { return m_Phase == Phase::WaitingForPlayViewportRender; }

	private:
		enum class Phase : uint8_t
		{
			Disabled,
			WaitingForEditViewportRender,
			WaitingForPlayViewportRender,
			Complete,
			Failed
		};
		void SetFailure(std::string Reason);

		Phase m_Phase = Phase::Disabled;
		bool m_EditViewportRendered = false;
		bool m_PlayViewportRendered = false;
		const PulseForge::Scene* m_AuthoredSceneIdentity = nullptr;
		size_t m_AuthoredEntityCount = 0;
		size_t m_Frames = 0;
		std::string m_AuthoredSnapshot;
		std::string m_FailureReason;
	};

	struct SceneViewportNativeSmokeObservation
	{
		uint32_t MainViewportID = 0;
		uint32_t OwnerViewportID = 0;
		uintptr_t OwnerWindow = 0;
		bool SceneImageReady = false;
		bool CameraCaptureActive = false;
		std::array<float, 3> CameraForward{};
		std::array<float, 2> FramebufferScale{ 1.0f, 1.0f };
		std::array<float, 2> TitleCenter{};
		std::array<float, 2> ImageMinimum{};
		std::array<float, 2> ImageMaximum{};
		std::array<float, 4> MainWindowRect{};
		std::array<float, 2> TearOffPoint{};
	};

	class SceneViewportNativeSmokeCoordinator final
	{
	public:
		explicit SceneViewportNativeSmokeCoordinator(bool Enabled) noexcept;
		[[nodiscard]] bool IsEnabled() const noexcept;
		[[nodiscard]] bool Complete() const noexcept;
		[[nodiscard]] std::optional<std::string> Observe(const SceneViewportNativeSmokeObservation& Observation);

	private:
		enum class Phase : uint8_t
		{
			Disabled,
			AwaitingMainScene,
			AwaitingNativeDetach,
			AwaitingCameraCapture,
			AwaitingCameraRelease,
			Complete
		};

		[[nodiscard]] static bool IsFinite(const std::array<float, 3>& Vector) noexcept;
		[[nodiscard]] static float Distance(const std::array<float, 3>& Left, const std::array<float, 3>& Right) noexcept;
		[[nodiscard]] std::string FormatGeometry(const SceneViewportNativeSmokeObservation& Observation) const;

		Phase m_Phase = Phase::Disabled;
		uint32_t m_MainViewportID = 0;
		uint32_t m_DetachedViewportID = 0;
		uintptr_t m_DetachedWindow = 0;
		std::array<float, 3> m_InitialCameraForward{};
		bool m_CaptureWasObserved = false;
	};
} // namespace PulseForgeEditor::Detail

#endif
