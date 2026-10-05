#pragma once

#include "Core/Core.h"

#include <unordered_set>

namespace PulseForge
{
	class Event;

	struct MousePosition
	{
		double X = 0.0;
		double Y = 0.0;
	};

	class PULSEFORGE_API Input
	{
	public:
		bool IsKeyPressed(int KeyCode) const;
		bool IsMouseButtonPressed(int Button) const;
		MousePosition GetMousePosition() const { return m_MousePosition; }

		// Codes currently match GLFW's desktop values; the platform layer can
		// translate them to engine-owned codes as platform support is added.
		void OnEvent(Event& Event);

	private:
		std::unordered_set<int> m_PressedKeys;
		std::unordered_set<int> m_PressedMouseButtons;
		MousePosition m_MousePosition;
	};
}
