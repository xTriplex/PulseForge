#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <imgui.h>

namespace PulseForgeEditor
{
	class EditorViewportInitializationRetry final
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;

		[[nodiscard]] bool CanAttempt(TimePoint Now) const noexcept { return !m_HasFailure || Now >= m_RetryAfter; }

		void RecordFailure(TimePoint Now) noexcept
		{
			m_HasFailure = true;
			m_RetryAfter = Now + m_NextDelay;
			m_NextDelay = (std::min)(m_NextDelay * 2, MaximumDelay);
		}

		void Reset() noexcept
		{
			m_HasFailure = false;
			m_RetryAfter = {};
			m_NextDelay = InitialDelay;
		}

	private:
		static constexpr std::chrono::milliseconds InitialDelay{ 250 };
		static constexpr std::chrono::milliseconds MaximumDelay{ 5000 };
		TimePoint m_RetryAfter{};
		std::chrono::milliseconds m_NextDelay = InitialDelay;
		bool m_HasFailure = false;
	};

	struct EditorViewportTargetExtent
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	[[nodiscard]] inline ImVec2 GetEditorViewportFramebufferScale(const ImGuiViewport* OwnerViewport) noexcept
	{
		return OwnerViewport ? OwnerViewport->FramebufferScale : ImVec2(1.0f, 1.0f);
	}

	struct EditorViewportSceneMode
	{
		bool UseRuntimeScene = false;
		bool ViewProjectionValid = false;
	};

	[[nodiscard]] constexpr EditorViewportSceneMode GetEditorViewportSceneMode(
		bool RuntimeActive, bool HasEditScene, bool HasRuntimeScene) noexcept
	{
		return { RuntimeActive && HasRuntimeScene, !RuntimeActive && HasEditScene };
	}

	[[nodiscard]] inline std::expected<EditorViewportTargetExtent, std::string> CalculateEditorViewportTargetExtent(
		double LogicalWidth, double LogicalHeight, double ScaleX, double ScaleY)
	{
		const double WidthInPixels = std::ceil(LogicalWidth * (ScaleX > 0.0 ? ScaleX : 1.0));
		const double HeightInPixels = std::ceil(LogicalHeight * (ScaleY > 0.0 ? ScaleY : 1.0));
		constexpr uint32_t ResolutionQuantum = 32;
		if (!std::isfinite(WidthInPixels) || !std::isfinite(HeightInPixels) || WidthInPixels < 1.0 || HeightInPixels < 1.0 ||
			WidthInPixels > (std::numeric_limits<uint32_t>::max)() - ResolutionQuantum ||
			HeightInPixels > (std::numeric_limits<uint32_t>::max)() - ResolutionQuantum)
			return std::unexpected("Viewport dimensions are outside the supported range");

		const auto Quantize = [](double Dimension)
		{ return static_cast<uint32_t>(std::ceil(Dimension / ResolutionQuantum) * ResolutionQuantum); };
		return EditorViewportTargetExtent{ Quantize(WidthInPixels), Quantize(HeightInPixels) };
	}
} // namespace PulseForgeEditor
