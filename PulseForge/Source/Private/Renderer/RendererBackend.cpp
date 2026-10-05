#include "Core/PulseForgePCH.h"
#include "Renderer/RendererBackend.h"
#include "Window/Window.h"

#include <stdexcept>

namespace PulseForge
{
	std::unique_ptr<RendererBackend> CreateOpenGLRenderer(Window& Window);
	std::unique_ptr<RendererBackend> CreateVulkanRenderer(Window& Window);

	std::unique_ptr<RendererBackend> RendererBackend::Create(RendererAPI API, Window& Window)
	{
		switch (API)
		{
			case RendererAPI::OpenGL:
				return CreateOpenGLRenderer(Window);
			case RendererAPI::Vulkan:
				return CreateVulkanRenderer(Window);
		}

		throw std::invalid_argument("Unsupported PulseForge renderer API");
	}
}
