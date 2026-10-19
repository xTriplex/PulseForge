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
#include "Renderer/RenderDevice.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/DepthCubemap.h"

#include <memory>
#include <expected>
#include <string>
#include <cstdint>

namespace PulseForge
{
	class RendererBackend;

	class PULSEFORGE_API Application : public RenderDevice
	{
	public:
		explicit Application(RendererAPI API = RendererAPI::Vulkan);
		virtual ~Application();

		void Run();
		// Stops the main loop after the current iteration.
		void RequestClose() noexcept { m_Running = false; }
		[[nodiscard]] std::expected<uint64_t, std::string> CreatePlatformPresentationTarget(void* NativeWindow);
		[[nodiscard]] bool BeginPlatformPresentationTarget(uint64_t TargetID);
		void EndPlatformPresentationTarget(uint64_t TargetID);
		[[nodiscard]] bool DestroyPlatformPresentationTarget(uint64_t TargetID);
		[[nodiscard]] bool HasPlatformPresentationTargets() const noexcept;
		void SetExternalPlatformPresentationTargetCount(uint32_t Count) noexcept;
#if defined(PF_DEBUG)
		[[nodiscard]] bool InjectNextPlatformAcquireOutOfDateForTesting(uint64_t TargetID) noexcept;
		[[nodiscard]] bool InjectNextPlatformImageAllocationFailureForTesting() noexcept;
#endif
		[[nodiscard]] bool IsMainFrameActive() const noexcept { return m_MainFrameActive; }
		void OnEvent(Event& E);
		Layer& PushLayer(std::unique_ptr<Layer> Layer);
		Layer& PushOverlay(std::unique_ptr<Layer> Overlay);

		inline Window& GetWindow() { return *m_Window; }
		Input& GetInput() { return m_Input; }
		RendererAPI GetRendererAPI() const { return m_RendererAPI; }
		[[nodiscard]] std::pair<unsigned int, unsigned int> GetOutputFramebufferSize() const noexcept override;
		[[nodiscard]] OutputColorEncoding GetOutputColorEncoding() const noexcept override;
		// Renderer resources should be released before this Application is destroyed.
		BufferCreateResult CreateBuffer(const BufferDesc& Description, std::span<const std::byte> InitialData = {}) override;
		// Records an aligned constant-buffer or dynamic geometry-buffer update on the active renderer frame.
		BufferUpdateResult WriteBuffer(const Buffer& Target, uint64_t DestinationOffset, std::span<const std::byte> Data) override;
		MeshCreateResult CreateMesh(const MeshDesc& Description);
		RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc& Description) override;
		DepthCubemapCreateResult CreateDepthCubemap(uint32_t Resolution, const std::string& DebugName) override;
		// The target may be rendered only during a frame; EndFrame closes an unclosed target with a diagnostic.
		GraphicsResult BeginRenderTarget(const RenderTarget& Target, const RenderTargetClearValue& ClearValue = {}) override;
		GraphicsResult EndRenderTarget() override;
		TextureCreateResult CreateTexture(const TextureDesc& Description, std::span<const std::byte> InitialData);
		TextureCreateResult CreateTexture(const TextureDesc& Description, std::span<const TextureSubresourceData> InitialData) override;
		// Development/test-only blocking readback of one RGBA8, RGBA16F, or D32 slice while the renderer is idle.
		TextureReadbackResult ReadTextureForValidation(const Texture& Source, uint32_t ArraySlice = 0);
		SamplerCreateResult CreateSampler(const SamplerDesc& Description) override;
		BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc& Description) override;
		BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description) override;
		ShaderCreateResult CreateShader(const ShaderDesc& Description, std::span<const std::byte> Bytecode) override;
		GraphicsPipelineCreateResult CreateGraphicsPipeline(const GraphicsPipelineDesc& Description) override;
		GraphicsResult Draw(const GraphicsPipeline& Pipeline, const Buffer& VertexBuffer, const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {}) override;
		GraphicsResult Draw(const GraphicsPipeline& Pipeline, const Mesh& Geometry, const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {}) override;
		GraphicsResult DrawIndexed(const GraphicsPipeline& Pipeline, const Mesh& Geometry, const DrawIndexedArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {}) override;
		GraphicsResult DrawIndexed(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const Buffer& IndexBuffer,
			const DrawIndexedArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {}) override;
		static Application& Get();

	private:
		bool OnWindowClose(WindowCloseEvent& E);

	private:
		std::unique_ptr<Window> m_Window;
		std::unique_ptr<RendererBackend> m_Renderer;
		bool m_Running = true;
		bool m_MainFrameActive = false;
		LayerStack m_LayerStack;
		Input m_Input;
		RendererAPI m_RendererAPI;
		static Application* s_Instance;
	};

	// To be defined in the Client
	std::unique_ptr<Application> CreateApplication();
}
