#include "pfpch.h"
#include "Core/LayerStack.h"

namespace PulseForge
{
	LayerStack::~LayerStack()
	{
		for (Layer* Layer : m_Layers)
		{
			Layer->OnDetach();
			delete Layer;
		}
	}

	void LayerStack::PushLayer(Layer* Layer)
	{
		m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, Layer);
		m_LayerInsertIndex++;
		Layer->OnAttach();
	}

	void LayerStack::PushOverlay(Layer* Overlay)
	{
		m_Layers.emplace_back(Overlay);
		Overlay->OnAttach();
	}

	void LayerStack::PopLayer(Layer* Layer)
	{
		auto It = std::find(m_Layers.begin(), m_Layers.begin() + m_LayerInsertIndex, Layer);
		if (It != m_Layers.begin() + m_LayerInsertIndex)
		{
			Layer->OnDetach();
			m_Layers.erase(It);
			m_LayerInsertIndex--;
		}
	}

	void LayerStack::PopOverlay(Layer* Overlay)
	{
		auto It = std::find(m_Layers.begin() + m_LayerInsertIndex, m_Layers.end(), Overlay);
		if (It != m_Layers.end())
		{
			Overlay->OnDetach();
			m_Layers.erase(It);
		}
	}
}
