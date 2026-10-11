#pragma once

#include "Scene/UUID.h"

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace PulseForgeEditor
{
	class EditorSelection final
	{
	public:
		void Set(std::vector<PulseForge::UUID> Identifiers, std::optional<PulseForge::UUID> Primary = std::nullopt)
		{
			m_Entities.clear();
			for (const PulseForge::UUID& Identifier : Identifiers)
				if (!Contains(Identifier))
					m_Entities.push_back(Identifier);

			if (Primary && Contains(*Primary))
				m_Primary = Primary;
			else if (!m_Entities.empty())
				m_Primary = m_Entities.back();
			else
				m_Primary.reset();

			if (m_Primary)
				m_Anchor = m_Primary;
			else
				m_Anchor.reset();
		}

		void SelectSingle(const PulseForge::UUID& Identifier)
		{
			m_Entities.assign(1, Identifier);
			m_Primary = Identifier;
			m_Anchor = Identifier;
		}

		void Clear() noexcept
		{
			m_Entities.clear();
			m_Primary.reset();
			m_Anchor.reset();
		}

		void Add(const PulseForge::UUID& Identifier)
		{
			if (!Contains(Identifier))
				m_Entities.push_back(Identifier);
			m_Primary = Identifier;
			m_Anchor = Identifier;
		}

		void Toggle(const PulseForge::UUID& Identifier)
		{
			const auto Found = std::find(m_Entities.begin(), m_Entities.end(), Identifier);
			if (Found == m_Entities.end())
				m_Entities.push_back(Identifier);
			else
				m_Entities.erase(Found);

			if (Contains(Identifier))
				m_Primary = Identifier;
			else if (m_Primary == Identifier)
				m_Primary = m_Entities.empty()
					? std::nullopt
					: std::optional<PulseForge::UUID>{ m_Entities.back() };
			if (!m_Anchor)
				m_Anchor = Identifier;
		}

		[[nodiscard]] bool SelectRange(
			std::span<const PulseForge::UUID> VisibleOrder,
			const PulseForge::UUID& Target,
			bool AddToSelection = false)
		{
			if (!m_Anchor)
			{
				SelectSingle(Target);
				return false;
			}
			const auto AnchorIndex = std::find(VisibleOrder.begin(), VisibleOrder.end(), *m_Anchor);
			const auto TargetIndex = std::find(VisibleOrder.begin(), VisibleOrder.end(), Target);
			if (AnchorIndex == VisibleOrder.end() || TargetIndex == VisibleOrder.end())
			{
				SelectSingle(Target);
				return false;
			}

			if (!AddToSelection)
				m_Entities.clear();
			const size_t First = static_cast<size_t>(std::min(AnchorIndex, TargetIndex) - VisibleOrder.begin());
			const size_t Last = static_cast<size_t>(std::max(AnchorIndex, TargetIndex) - VisibleOrder.begin());
			for (size_t Index = First; Index <= Last; ++Index)
				if (!Contains(VisibleOrder[Index]))
					m_Entities.push_back(VisibleOrder[Index]);
			m_Primary = Target;
			return true;
		}

		void Remove(const PulseForge::UUID& Identifier) noexcept
		{
			std::erase(m_Entities, Identifier);
			if (m_Primary == Identifier)
				m_Primary = m_Entities.empty()
					? std::nullopt
					: std::optional<PulseForge::UUID>{ m_Entities.back() };
			if (m_Anchor == Identifier)
				m_Anchor = m_Primary;
		}

		[[nodiscard]] bool Contains(const PulseForge::UUID& Identifier) const noexcept
		{
			return std::find(m_Entities.begin(), m_Entities.end(), Identifier) != m_Entities.end();
		}

		[[nodiscard]] const std::vector<PulseForge::UUID>& GetEntities() const noexcept { return m_Entities; }
		[[nodiscard]] const std::optional<PulseForge::UUID>& GetPrimary() const noexcept { return m_Primary; }
		[[nodiscard]] const std::optional<PulseForge::UUID>& GetAnchor() const noexcept { return m_Anchor; }

	private:
		std::vector<PulseForge::UUID> m_Entities;
		std::optional<PulseForge::UUID> m_Primary;
		std::optional<PulseForge::UUID> m_Anchor;
	};
}
