#pragma once

#include <chrono>

namespace PulseForge
{
	class Timestep
	{
	public:
		explicit Timestep(double Seconds = 0.0) : m_Seconds(Seconds) {}

		template<typename Rep, typename Period>
		explicit Timestep(std::chrono::duration<Rep, Period> Duration)
			: m_Seconds(std::chrono::duration<double>(Duration).count())
		{
		}

		double GetSeconds() const { return m_Seconds; }
		double GetMilliseconds() const { return m_Seconds * 1000.0; }

	private:
		double m_Seconds;
	};
}
