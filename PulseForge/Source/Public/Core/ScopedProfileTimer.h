#pragma once

#include "Core/Log.h"

#include <chrono>
#include <string_view>

namespace PulseForge::Detail
{
	class ScopedProfileTimer final
	{
	public:
		explicit ScopedProfileTimer(std::string_view Label) noexcept
			: m_Label(Label), m_Start(std::chrono::steady_clock::now())
		{
		}

		~ScopedProfileTimer()
		{
			const double Milliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - m_Start).count();
			PF_INFO("Profile: {} took {:.2f} ms", m_Label, Milliseconds);
		}

		ScopedProfileTimer(const ScopedProfileTimer&) = delete;
		ScopedProfileTimer& operator=(const ScopedProfileTimer&) = delete;

	private:
		std::string_view m_Label;
		std::chrono::steady_clock::time_point m_Start;
	};
}
