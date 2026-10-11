#pragma once

#include "Scene/UUID.h"

#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	class EditorVisibility final
	{
	public:
		void SetVisible(const PulseForge::UUID& Identifier, bool Visible)
		{
			if (Visible)
				m_Hidden.erase(Identifier);
			else
				m_Hidden.insert(Identifier);
		}

		void Toggle(const PulseForge::UUID& Identifier)
		{
			SetVisible(Identifier, !IsLocallyVisible(Identifier));
		}

		[[nodiscard]] bool IsLocallyVisible(const PulseForge::UUID& Identifier) const noexcept
		{
			return !m_Hidden.contains(Identifier);
		}

		[[nodiscard]] bool IsEffectivelyVisible(
			const PulseForge::UUID& Identifier,
			const std::function<std::optional<PulseForge::UUID>(const PulseForge::UUID&)>& ParentLookup) const
		{
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
			PulseForge::UUID Current = Identifier;
			while (true)
			{
				if (!Visited.insert(Current).second || !IsLocallyVisible(Current))
					return false;
				const auto Parent = ParentLookup(Current);
				if (!Parent)
					return true;
				Current = *Parent;
			}
		}

		void ShowAll() noexcept
		{
			m_Hidden.clear();
			m_IsolationBaseline.reset();
		}

		void Remove(const PulseForge::UUID& Identifier) noexcept
		{
			m_Hidden.erase(Identifier);
			if (m_IsolationBaseline)
				m_IsolationBaseline->erase(Identifier);
		}

		[[nodiscard]] bool Hide(std::span<const PulseForge::UUID> Identifiers)
		{
			bool Changed = false;
			for (const PulseForge::UUID& Identifier : Identifiers)
				Changed |= m_Hidden.insert(Identifier).second;
			return Changed;
		}

		[[nodiscard]] bool Isolate(
			std::span<const PulseForge::UUID> KeepVisible,
			std::span<const PulseForge::UUID> AllEntities)
		{
			if (KeepVisible.empty() || AllEntities.empty())
				return false;
			if (!m_IsolationBaseline)
				m_IsolationBaseline = m_Hidden;

			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Keep(
				KeepVisible.begin(), KeepVisible.end());
			m_Hidden = m_IsolationBaseline.value();
			for (const PulseForge::UUID& Identifier : Keep)
				m_Hidden.erase(Identifier);
			for (const PulseForge::UUID& Identifier : AllEntities)
				if (!Keep.contains(Identifier))
					m_Hidden.insert(Identifier);
			return true;
		}

		[[nodiscard]] bool RestoreIsolation()
		{
			if (!m_IsolationBaseline)
				return false;
			m_Hidden = std::move(*m_IsolationBaseline);
			m_IsolationBaseline.reset();
			return true;
		}

		[[nodiscard]] const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>& GetHiddenEntities() const noexcept
		{
			return m_Hidden;
		}

		[[nodiscard]] const std::optional<
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>>& GetIsolationBaseline() const noexcept
		{
			return m_IsolationBaseline;
		}

		void RestoreState(
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Hidden,
			std::optional<std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>> IsolationBaseline)
		{
			m_Hidden = std::move(Hidden);
			m_IsolationBaseline = std::move(IsolationBaseline);
		}

	private:
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> m_Hidden;
		std::optional<std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>> m_IsolationBaseline;
	};
}
