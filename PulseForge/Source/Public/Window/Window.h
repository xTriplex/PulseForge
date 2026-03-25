#pragma once

#include "Core/Core.h"
#include "Events/Event.h"
#include <iostream>

namespace PulseForge
{
	enum class EWindowStyle
	{
		None       = 0,
		Titlebar   = BIT(0),
		Resize     = BIT(1),
		Close      = BIT(2),
		Fullscreen = BIT(3),
		Borderless = BIT(4),
		Default    = Titlebar | Resize | Close
	};

	inline EWindowStyle operator | (EWindowStyle a, EWindowStyle b)
	{
		return static_cast<EWindowStyle>(static_cast<int>(a) | static_cast<int>(b));
	}

	inline bool HasStyle(EWindowStyle flags, EWindowStyle target)
	{
		return (static_cast<int>(flags) & static_cast<int>(target)) != 0;
	}

	struct WindowProps
	{
		std::string m_Title;
		unsigned int m_Width;
		unsigned int m_Height;
		EWindowStyle m_Style;

		WindowProps(const std::string& Title = "PulseForge",
					unsigned int Width = 1280,
					unsigned int Height = 720,
					EWindowStyle Style = EWindowStyle::Default)
			: m_Title(Title), m_Width(Width), m_Height(Height), m_Style(Style)
		{

		}
	};

	class PULSEFORGE_API Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() = default;

		virtual void OnUpdate() = 0;

		virtual unsigned int GetWidth() const = 0;
		virtual unsigned int GetHeight() const = 0;

		virtual void SetEventCallback(const EventCallbackFn& Callback) = 0;

		virtual void SetVSync(bool bEnabled) = 0;
		virtual bool IsVSync() const = 0;

		virtual void* GetNativeWindow() const = 0;

		static Window* Create(const WindowProps& Props = WindowProps());
	};

}
