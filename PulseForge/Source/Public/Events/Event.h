#pragma once

#include "Core/Core.h"
#include "Core/Log.h"

#include <string>
#include <functional>

namespace PulseForge
{
	enum class EEventType
	{
		None = 0,
		WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMoved,
		AppTick, AppUpdate, AppRender,
		KeyPressed, KeyReleased, KeyTyped,
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled
	};

	enum EEventCategory
	{
		None = 0,
		EventCategoryApplication = BIT(0),
		EventCategoryInput       = BIT(1),
		EventCategoryKeyboard    = BIT(2),
		EventCategoryMouse       = BIT(3),
		EventCategoryMouseButton = BIT(4)
	};

	#define EVENT_CLASS_TYPE(Type) \
		static EEventType GetStaticType() { return EEventType::Type; }\
		virtual EEventType GetEventType() const override { return GetStaticType(); }\
		virtual const char* GetName() const override { return #Type; }

	#define EVENT_CLASS_CATEGORY(Category) \
        virtual int GetCategoryFlags() const override { return Category; }

	class PULSEFORGE_API Event
	{
	public:
		bool bHandled = false;

		virtual EEventType GetEventType() const = 0;
		virtual const char* GetName() const = 0;
		virtual int GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		inline bool IsInCategory(EEventCategory Category) const
		{
			return GetCategoryFlags() & Category;
		}
	};

	class EventDispatcher
	{
	public:
		EventDispatcher(Event& Event)
			: m_Event(Event)
		{

		}

		template<typename T, typename F>
		bool Dispatch(const F& Func)
		{
			if (m_Event.GetEventType() == T::GetStaticType())
			{
				m_Event.bHandled |= Func(static_cast<T&>(m_Event));
				return true;
			}
			return false;
		}

	private: 
		Event& m_Event;
	};
} 
