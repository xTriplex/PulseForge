#include "Core/PulseForgePCH.h"
#include "Core/LayerStack.h"

#include <cstdio>
#include <exception>
#include <stdexcept>

namespace PulseForge
{
	LayerStack::~LayerStack()
	{
		Clear();
	}

	void LayerStack::Clear()
	{
		for (auto It = m_Layers.rbegin(); It != m_Layers.rend(); ++It)
			DetachLayer(**It);
		m_Layers.clear();
		m_LayerInsertIndex = 0;
	}

	void LayerStack::DetachLayer(Layer& Layer) noexcept
	{
		try
		{
			Layer.OnDetach();
		}
		catch (const std::exception& Exception)
		{
			std::fprintf(stderr, "PulseForge: layer '%s' threw while detaching: %s\n",
				Layer.GetName().c_str(), Exception.what());
		}
		catch (...)
		{
			std::fprintf(stderr, "PulseForge: layer '%s' threw an unknown exception while detaching\n",
				Layer.GetName().c_str());
		}
	}

	Layer& LayerStack::PushLayer(std::unique_ptr<Layer> NewLayer)
	{
		if (!NewLayer)
			throw std::invalid_argument("Cannot push a null layer");

		Layer* LayerPointer = NewLayer.get();
		m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, std::move(NewLayer));
		m_LayerInsertIndex++;
		try
		{
			LayerPointer->OnAttach();
		}
		catch (...)
		{
			DetachLayer(*LayerPointer);
			m_LayerInsertIndex--;
			m_Layers.erase(m_Layers.begin() + m_LayerInsertIndex);
			throw;
		}
		return *LayerPointer;
	}

	Layer& LayerStack::PushOverlay(std::unique_ptr<Layer> NewOverlay)
	{
		if (!NewOverlay)
			throw std::invalid_argument("Cannot push a null overlay");

		Layer* LayerPointer = NewOverlay.get();
		m_Layers.emplace_back(std::move(NewOverlay));
		try
		{
			LayerPointer->OnAttach();
		}
		catch (...)
		{
			DetachLayer(*LayerPointer);
			m_Layers.pop_back();
			throw;
		}
		return *LayerPointer;
	}

	bool LayerStack::PopLayer(Layer& Layer)
	{
		auto It = std::find_if(m_Layers.begin(), m_Layers.begin() + m_LayerInsertIndex,
			[&Layer](const auto& OwnedLayer) { return OwnedLayer.get() == &Layer; });
		if (It != m_Layers.begin() + m_LayerInsertIndex)
		{
			DetachLayer(**It);
			m_Layers.erase(It);
			m_LayerInsertIndex--;
			return true;
		}
		return false;
	}

	bool LayerStack::PopOverlay(Layer& Overlay)
	{
		auto It = std::find_if(m_Layers.begin() + m_LayerInsertIndex, m_Layers.end(),
			[&Overlay](const auto& OwnedLayer) { return OwnedLayer.get() == &Overlay; });
		if (It != m_Layers.end())
		{
			DetachLayer(**It);
			m_Layers.erase(It);
			return true;
		}
		return false;
	}
}
