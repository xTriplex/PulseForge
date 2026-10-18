#include "Core/LayerStack.h"

#include <cstdio>
#include <exception>
#include <stdexcept>

namespace PulseForge
{
	namespace
	{
		class CallbackDepthScope final
		{
		public:
			explicit CallbackDepthScope(std::size_t& Depth) : m_Depth(Depth) { ++m_Depth; }
			~CallbackDepthScope() { --m_Depth; }

		private:
			std::size_t& m_Depth;
		};
	} // namespace

	LayerStack::~LayerStack()
	{
		Clear();
	}

	void LayerStack::EnsureMutationAllowed() const
	{
		if (m_DispatchDepth != 0) throw std::logic_error("LayerStack mutation is not allowed during layer dispatch");
		if (m_LifecycleCallbackDepth != 0) throw std::logic_error("LayerStack mutation is not allowed from a layer lifecycle callback");
	}

	void LayerStack::Clear()
	{
		EnsureMutationAllowed();
		for (auto It = m_Layers.rbegin(); It != m_Layers.rend(); ++It)
			DetachLayer(**It);
		m_Layers.clear();
		m_LayerInsertIndex = 0;
	}

	void LayerStack::UpdateLayers(Timestep DeltaTime)
	{
		CallbackDepthScope DispatchScope(m_DispatchDepth);
		for (const auto& Layer : m_Layers)
			Layer->OnUpdate(DeltaTime);
	}

	void LayerStack::RenderLayers()
	{
		CallbackDepthScope DispatchScope(m_DispatchDepth);
		for (const auto& Layer : m_Layers)
			Layer->OnRender();
	}

	void LayerStack::RenderPlatformWindows()
	{
		CallbackDepthScope DispatchScope(m_DispatchDepth);
		for (const auto& Layer : m_Layers)
			Layer->OnRenderPlatformWindows();
	}

	void LayerStack::DispatchEvent(Event& EventValue)
	{
		CallbackDepthScope DispatchScope(m_DispatchDepth);
		for (auto It = m_Layers.rbegin(); It != m_Layers.rend(); ++It)
		{
			(*It)->OnEvent(EventValue);
			if (EventValue.bHandled) break;
		}
	}

	void LayerStack::AttachLayer(Layer& LayerValue)
	{
		CallbackDepthScope LifecycleScope(m_LifecycleCallbackDepth);
		LayerValue.OnAttach();
	}

	void LayerStack::DetachLayer(Layer& Layer) noexcept
	{
		CallbackDepthScope LifecycleScope(m_LifecycleCallbackDepth);
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
		EnsureMutationAllowed();

		Layer* LayerPointer = NewLayer.get();
		m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, std::move(NewLayer));
		m_LayerInsertIndex++;
		try
		{
			AttachLayer(*LayerPointer);
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
		EnsureMutationAllowed();

		Layer* LayerPointer = NewOverlay.get();
		m_Layers.emplace_back(std::move(NewOverlay));
		try
		{
			AttachLayer(*LayerPointer);
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
		EnsureMutationAllowed();
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
		EnsureMutationAllowed();
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
