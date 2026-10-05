#pragma once

#include "Core/Core.h"
#include "Core/Layer.h"

#include <vector>
#include <memory>

namespace PulseForge
{
	class PULSEFORGE_API LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack();
		void Clear();

		Layer& PushLayer(std::unique_ptr<Layer> NewLayer);
		Layer& PushOverlay(std::unique_ptr<Layer> NewOverlay);
		// Pop destroys a found layer. Detach-hook failures are reported but do not retain ownership.
		bool PopLayer(Layer& Layer);
		bool PopOverlay(Layer& Overlay);

		using Container = std::vector<std::unique_ptr<Layer>>;
		Container::iterator begin() { return m_Layers.begin(); }
		Container::iterator end() { return m_Layers.end(); }
		Container::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		Container::reverse_iterator rend() { return m_Layers.rend(); }

		Container::const_iterator begin() const { return m_Layers.begin(); }
		Container::const_iterator end() const { return m_Layers.end(); }
		Container::const_reverse_iterator rbegin() const { return m_Layers.rbegin(); }
		Container::const_reverse_iterator rend() const { return m_Layers.rend(); }


	private:
		void DetachLayer(Layer& Layer) noexcept;

	private:
		Container m_Layers;
		Container::size_type m_LayerInsertIndex = 0;
	};
}
