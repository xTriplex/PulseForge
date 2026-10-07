#pragma once

#include "Core/Core.h"
#include "Window/Window.h"
#include "Core/LayerStack.h"
#include "Events/ApplicationEvent.h"
#include "Core/Input.h"
#include "Renderer/RendererAPI.h"
#include "Renderer/Buffer.h"
#include "Renderer/Binding.h"
#include "Renderer/Graphics.h"
#include "Renderer/Mesh.h"
#include "Renderer/RenderTarget.h"

#include <memory>

namespace PulseForge
{
	class RendererBackend;

	class PULSEFORGE_API Application
	{
	public:
		explicit Application(RendererAPI API = RendererAPI::Vulkan);
		virtual ~Application();

		void Run();
		// Stops the main loop after the current iteration.
		void RequestClose() noexcept { m_Running = false; }
		void OnEvent(Event& E);
		Layer& PushLayer(std::unique_ptr<Layer> Layer);
		Layer& PushOverlay(std::unique_ptr<Layer> Overlay);

		inline Window& GetWindow() { return *m_Window; }
		Input& GetInput() { return m_Input; }
		RendererAPI GetRendererAPI() const { return m_RendererAPI; }
		// Renderer resources should be released before this Application is destroyed.
		BufferCreateResult CreateBuffer(const BufferDesc& Description, std::span<const std::byte> InitialData = {});
		// Records an aligned constant-buffer update on the active renderer frame.
		BufferUpdateResult WriteBuffer(
			const Buffer& Target,
			uint64_t DestinationOffset,
			std::span<const std::byte> Data);
		MeshCreateResult CreateMesh(const MeshDesc& Description);
		RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc& Description);
		// The target may be rendered only during a frame; EndFrame closes an unclosed target with a diagnostic.
		GraphicsResult BeginRenderTarget(const RenderTarget& Target, const RenderTargetClearValue& ClearValue = {});
		GraphicsResult EndRenderTarget();
		TextureCreateResult CreateTexture(const TextureDesc& Description, std::span<const std::byte> InitialData);
		SamplerCreateResult CreateSampler(const SamplerDesc& Description);
		BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc& Description);
		BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description);
		ShaderCreateResult CreateShader(const ShaderDesc& Description, std::span<const std::byte> Bytecode);
		GraphicsPipelineCreateResult CreateGraphicsPipeline(const GraphicsPipelineDesc& Description);
		GraphicsResult Draw(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {});
		GraphicsResult Draw(
			const GraphicsPipeline& Pipeline,
			const Mesh& Geometry,
			const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {});
		GraphicsResult DrawIndexed(
			const GraphicsPipeline& Pipeline,
			const Mesh& Geometry,
			const DrawIndexedArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {});
		static Application& Get();

	private:
		bool OnWindowClose(WindowCloseEvent& E);

	private:
		std::unique_ptr<Window> m_Window;
		std::unique_ptr<RendererBackend> m_Renderer;
		bool m_Running = true;
		LayerStack m_LayerStack;
		Input m_Input;
		RendererAPI m_RendererAPI;
		static Application* s_Instance;
	};

	// To be defined in the Client
	std::unique_ptr<Application> CreateApplication();
}
