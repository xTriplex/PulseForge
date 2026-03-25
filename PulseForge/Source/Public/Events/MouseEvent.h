#pragma once

#include "Events/Event.h"

namespace PulseForge
{
    class PULSEFORGE_API MouseMovedEvent : public Event
    {
    public:
        MouseMovedEvent(float X, float Y)
            : m_MouseX(X), m_MouseY(Y)
        {

        }

        inline float GetX() const { return m_MouseX; }
        inline float GetY() const { return m_MouseY; }

        std::string ToString() const override
        {
            return std::format("MouseMovedEvent: {0}, {1}", m_MouseX, m_MouseY);
        }

        EVENT_CLASS_TYPE(MouseMoved)
        EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)

    private:
        float m_MouseX;
        float m_MouseY;
    };

    class PULSEFORGE_API MouseScrolledEvent : public Event
    {
    public:
        MouseScrolledEvent(float XOffset, float YOffset)
            : m_XOffset(XOffset), m_YOffset(YOffset)
        {

        }

        inline float GetXOffset() const { return m_XOffset; }
        inline float GetYOffset() const { return m_YOffset; }

        std::string ToString() const override
        {
            return std::format("MouseScrolledEvent: {0}, {1}", m_XOffset, m_YOffset);
        }
        
        EVENT_CLASS_TYPE(MouseScrolled)
        EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)

    private:
        float m_XOffset;
        float m_YOffset;
    };

    class PULSEFORGE_API MouseButtonEvent : public Event
    {
    public:
        inline int GetMouseButton() const { return m_Button; }
        
        EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton )

    protected:
        MouseButtonEvent(int Button)
            : m_Button(Button)
        {

        }

        int m_Button;
    };

    class PULSEFORGE_API MouseButtonPressedEvent : public MouseButtonEvent
    {
    public:
        MouseButtonPressedEvent(int Button)
            : MouseButtonEvent(Button)
        {

        }

        std::string ToString() const override
        {
            return std::format("MouseButtonPressedEvent: {0}", m_Button);
        }

        EVENT_CLASS_TYPE(MouseButtonPressed)
    };

    class PULSEFORGE_API MouseButtonReleasedEvent : public MouseButtonEvent
    {
    public:
        MouseButtonReleasedEvent(int Button)
            : MouseButtonEvent(Button)
        {

        }

        std::string ToString() const override
        {
            return std::format("MouseButtonReleasedEvent: {0}", m_Button);
        }

        EVENT_CLASS_TYPE(MouseButtonReleased)
    };
}