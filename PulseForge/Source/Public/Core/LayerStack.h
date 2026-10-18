#pragma once

#include "Core/Core.h"
#include "Core/Layer.h"

#include <cstddef>
#include <vector>
#include <memory>

namespace PulseForge
{
	class PULSEFORGE_API LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack();
		// Structural changes are synchronous and rejected during dispatch or layer lifecycle callbacks.
		void Clear();
		void UpdateLayers(Timestep DeltaTime);
		void RenderLayers();
		void RenderPlatformWindows();
		void DispatchEvent(Event& EventValue);

		Layer& PushLayer(std::unique_ptr<Layer> NewLayer);
		Layer& PushOverlay(std::unique_ptr<Layer> NewOverlay);
		// Pop destroys a found layer. Detach-hook failures are reported but do not retain ownership.
		bool PopLayer(Layer& Layer);
		bool PopOverlay(Layer& Overlay);
		[[nodiscard]] std::size_t GetLayerCount() const noexcept { return m_Layers.size(); }

	private:
		using Container = std::vector<std::unique_ptr<Layer>>;

		void EnsureMutationAllowed() const;
		void AttachLayer(Layer& Layer);
		void DetachLayer(Layer& Layer) noexcept;

	private:
		Container m_Layers;
		Container::size_type m_LayerInsertIndex = 0;
		std::size_t m_DispatchDepth = 0;
		std::size_t m_LifecycleCallbackDepth = 0;
	};
}
