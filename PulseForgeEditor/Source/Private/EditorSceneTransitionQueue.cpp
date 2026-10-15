#include "Editor/EditorSceneTransitionQueue.h"

#include <utility>

namespace PulseForgeEditor
{
	EditorSceneTransitionQueue::RequestResult EditorSceneTransitionQueue::Request(
		bool SceneDirty,
		Action Continuation)
	{
		if (!Continuation || m_PendingAction)
			return {};

		if (!SceneDirty)
			return { RequestStatus::Ready, std::move(Continuation) };

		m_PendingAction = std::move(Continuation);
		return { RequestStatus::AwaitingDecision, {} };
	}

	std::optional<EditorSceneTransitionQueue::Action> EditorSceneTransitionQueue::Resolve(bool SceneChangesResolved)
	{
		if (!SceneChangesResolved || !m_PendingAction)
			return std::nullopt;

		return std::exchange(m_PendingAction, {});
	}

	void EditorSceneTransitionQueue::Cancel() noexcept
	{
		m_PendingAction = {};
	}
}
