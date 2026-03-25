#pragma once

#include "Events/Event.h"

namespace PulseForge
{
    class PULSEFORGE_API KeyEvent : public Event
    {
    public:
        inline int GetKeyCode() const { return m_KeyCode; }

        EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

    protected:
        KeyEvent(int KeyCode)
            : m_KeyCode(KeyCode)
        {

        }

        int m_KeyCode;
    };

    class PULSEFORGE_API KeyPressedEvent : public KeyEvent
    {
    public:
        KeyPressedEvent(int KeyCode, int RepeatCount)
            : KeyEvent(KeyCode), m_RepeatCount(RepeatCount)
        {

        }

        inline int GetRepeatCount() const { return m_RepeatCount; }

        std::string ToString() const override
        {
            return std::format("KeyPressedEvent: {0} ({1} repeats)", m_KeyCode, m_RepeatCount);
        }

        EVENT_CLASS_TYPE(KeyPressed)

    private:
        int m_RepeatCount;
    };

    class PULSEFORGE_API KeyReleasedEvent : public KeyEvent
    {
    public:
        KeyReleasedEvent(int KeyCode)
            : KeyEvent(KeyCode)
        {

        }

        std::string ToString() const override
        {
            return std::format("KeyReleasedEvent: {0}", m_KeyCode);
        }

        EVENT_CLASS_TYPE(KeyReleased)
    };

    class PULSEFORGE_API KeyTypedEvent : public KeyEvent
    {
    public:
        KeyTypedEvent(int KeyCode)
            : KeyEvent(KeyCode)
        {

        }

        std::string ToString() const override
        {
            return std::format("KeyTypedEvent: {0}", m_KeyCode);
        }

        EVENT_CLASS_TYPE(KeyTyped)
    };
}
