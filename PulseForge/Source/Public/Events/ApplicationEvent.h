#pragma once

#include "Events/Event.h"

namespace PulseForge
{
    class PULSEFORGE_API WindowResizeEvent : public Event
    {
    public:
        WindowResizeEvent(unsigned int Width, unsigned int Height)
            : m_Width(Width), m_Height(Height)
        {

        }

        inline unsigned int GetWidth() const { return m_Width; }
        inline unsigned int GetHeight() const { return m_Height; }

        std::string ToString() const override
        {
            return std::format("WindowResizeEvent: {0}, {1}", m_Width, m_Height);
        }

        EVENT_CLASS_TYPE(WindowResize)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)

    private:
        unsigned int m_Width;
        unsigned int m_Height;
    };

    class PULSEFORGE_API WindowCloseEvent : public Event
    {
    public:
        WindowCloseEvent() = default;

        EVENT_CLASS_TYPE(WindowClose)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
    };

    class PULSEFORGE_API WindowFocusEvent : public Event
    {
    public:
        WindowFocusEvent() = default;

        EVENT_CLASS_TYPE(WindowFocus)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
    };

    class PULSEFORGE_API WindowLostFocusEvent : public Event
    {
    public:
        WindowLostFocusEvent() = default;

        EVENT_CLASS_TYPE(WindowLostFocus)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
    };

    class PULSEFORGE_API WindowMovedEvent : public Event
    {
    public:
        WindowMovedEvent(unsigned int X, unsigned int Y)
            : m_X(X), m_Y(Y)
        {

        }

        inline unsigned int GetX() const { return m_X; }
        inline unsigned int GetY() const { return m_Y; }

        std::string ToString() const override
        {
            return std::format("WindowMovedEvent: {0}, {1}", m_X, m_Y);
        }

        EVENT_CLASS_TYPE(WindowMoved)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)

    private:
        unsigned int m_X, m_Y;
    };

    class PULSEFORGE_API AppTickEvent : public Event
    {
    public:
        AppTickEvent() = default;
        
        EVENT_CLASS_TYPE(AppTick)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
    };

    class PULSEFORGE_API AppUpdateEvent : public Event
    {
    public:
        AppUpdateEvent() = default;

        EVENT_CLASS_TYPE(AppUpdate)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
    };

    class PULSEFORGE_API AppRenderEvent : public Event
    {
    public:
        AppRenderEvent() = default;

        EVENT_CLASS_TYPE(AppRender)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
    };
}