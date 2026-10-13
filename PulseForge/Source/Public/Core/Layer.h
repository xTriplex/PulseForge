#pragma once

#include "Core/Core.h"
#include "Events/Event.h"
#include "Core/Timestep.h"

#include <string>

namespace PulseForge
{
	class PULSEFORGE_API Layer
	{
	public:
		Layer(const std::string& Name = "Layer");
		virtual ~Layer() = default;

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		// Runs after event polling and before BeginFrame so updates can safely prepare renderer resources.
		virtual void OnUpdate(Timestep DeltaTime) { (void)DeltaTime; }
		virtual void OnRender() {}
		// Runs after the main frame is submitted so layers can render native platform windows safely.
		virtual void OnRenderPlatformWindows() {}
		virtual void OnEvent(Event& Event) {}

		inline const std::string& GetName() const { return m_DebugName; }

	protected:
		std::string m_DebugName;
	};
}
