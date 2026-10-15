#pragma once

#include <functional>
#include <optional>

namespace PulseForgeEditor
{
	class EditorSceneTransitionQueue final
	{
	public:
		using Action = std::function<void()>;

		enum class RequestStatus
		{
			Ready,
			AwaitingDecision,
			Rejected
		};

		struct RequestResult final
		{
			RequestStatus Status = RequestStatus::Rejected;
			Action ReadyAction;
		};

		[[nodiscard]] RequestResult Request(bool SceneDirty, Action Continuation);
		[[nodiscard]] std::optional<Action> Resolve(bool SceneChangesResolved);
		void Cancel() noexcept;
		[[nodiscard]] bool HasPendingDecision() const noexcept { return static_cast<bool>(m_PendingAction); }

	private:
		Action m_PendingAction;
	};
} // namespace PulseForgeEditor
