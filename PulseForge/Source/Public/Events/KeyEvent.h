#pragma once

#include "Events/Event.h"

#include <format>

namespace PulseForge
{
    class PULSEFORGE_API KeyEvent : public Event
    {
    public:
        inline int GetKeyCode() const { return m_KeyCode; }

        EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

    protected:
        KeyEvent(int KeyCode, uint16_t Modifiers)
            : m_KeyCode(KeyCode), m_Modifiers(Modifiers)
        {

        }

        int m_KeyCode;
        uint16_t m_Modifiers;
    };

    class PULSEFORGE_API KeyPressedEvent : public KeyEvent
    {
    public:
        KeyPressedEvent(int KeyCode, int RepeatCount, uint16_t Modifiers = 0)
            : KeyEvent(KeyCode, Modifiers), m_RepeatCount(RepeatCount)
        {

        }

        inline int GetRepeatCount() const { return m_RepeatCount; }
        inline uint16_t GetModifiers() const { return m_Modifiers; }

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
        KeyReleasedEvent(int KeyCode, uint16_t Modifiers = 0)
            : KeyEvent(KeyCode, Modifiers)
        {

        }

        inline uint16_t GetModifiers() const { return m_Modifiers; }

        std::string ToString() const override
        {
            return std::format("KeyReleasedEvent: {0}", m_KeyCode);
        }

        EVENT_CLASS_TYPE(KeyReleased)
    };

    class PULSEFORGE_API KeyTypedEvent : public KeyEvent
    {
    public:
        KeyTypedEvent(int KeyCode, uint16_t Modifiers = 0)
            : KeyEvent(KeyCode, Modifiers)
        {

        }

        inline uint16_t GetModifiers() const { return m_Modifiers; }

        std::string ToString() const override
        {
            return std::format("KeyTypedEvent: {0}", m_KeyCode);
        }

        EVENT_CLASS_TYPE(KeyTyped)
    };
}
