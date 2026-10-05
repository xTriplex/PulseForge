#include "Core/PulseForgePCH.h"
#include "Core/Input.h"

#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

namespace PulseForge
{
	bool Input::IsKeyPressed(int KeyCode) const
	{
		return m_PressedKeys.contains(KeyCode);
	}

	bool Input::IsMouseButtonPressed(int Button) const
	{
		return m_PressedMouseButtons.contains(Button);
	}

	void Input::OnEvent(Event& Event)
	{
		switch (Event.GetEventType())
		{
			case EEventType::KeyPressed:
				m_PressedKeys.insert(static_cast<KeyPressedEvent&>(Event).GetKeyCode());
				break;
			case EEventType::KeyReleased:
				m_PressedKeys.erase(static_cast<KeyReleasedEvent&>(Event).GetKeyCode());
				break;
			case EEventType::MouseButtonPressed:
				m_PressedMouseButtons.insert(static_cast<MouseButtonPressedEvent&>(Event).GetMouseButton());
				break;
			case EEventType::MouseButtonReleased:
				m_PressedMouseButtons.erase(static_cast<MouseButtonReleasedEvent&>(Event).GetMouseButton());
				break;
			case EEventType::MouseMoved:
			{
				const auto& MouseEvent = static_cast<MouseMovedEvent&>(Event);
				m_MousePosition = { MouseEvent.GetX(), MouseEvent.GetY() };
				break;
			}
			case EEventType::WindowLostFocus:
				m_PressedKeys.clear();
				m_PressedMouseButtons.clear();
				break;
			default:
				break;
		}
	}
}
