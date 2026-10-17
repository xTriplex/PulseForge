#include "Core/Application.h"
#include "Renderer/RendererBackend.h"

#include <chrono>
#include <functional>
#include <stdexcept>

namespace PulseForge
{
	Application* Application::s_Instance = nullptr;

	Application::Application(RendererAPI API)
		: m_RendererAPI(API)
	{
		if (s_Instance)
			throw std::logic_error("Only one PulseForge Application may exist at a time");

		const EWindowClientAPI WindowClientAPI = API == RendererAPI::OpenGL
			? EWindowClientAPI::OpenGL
			: EWindowClientAPI::None;
		m_Window = Window::Create(WindowProps("PulseForge", 1280, 720, EWindowStyle::Default, WindowClientAPI));
		if (!m_Window)
			throw std::runtime_error("Window creation failed");
		m_Renderer = RendererBackend::Create(API, *m_Window);
		if (!m_Renderer)
			throw std::runtime_error("Renderer backend creation failed");

		s_Instance = this;
		try
		{
			m_Window->SetEventCallback(std::bind(&Application::OnEvent, this, std::placeholders::_1));
		}
		catch (...)
		{
			s_Instance = nullptr;
			throw;
		}
	}

	Application::~Application()
	{
		m_LayerStack.Clear();
		s_Instance = nullptr;
		m_Renderer.reset();
		m_Window.reset();
	}

	OutputColorEncoding Application::GetOutputColorEncoding() const noexcept
	{
		return m_Renderer->GetOutputColorEncoding();
	}

	std::pair<unsigned int, unsigned int> Application::GetOutputFramebufferSize() const noexcept
	{
		return m_Window->GetFramebufferSize();
	}

	std::expected<uint64_t, std::string> Application::CreatePlatformPresentationTarget(void* NativeWindow)
	{
		return m_Renderer->CreatePlatformPresentationTarget(NativeWindow);
	}

	bool Application::BeginPlatformPresentationTarget(uint64_t TargetID)
	{
		return m_Renderer->BeginPlatformPresentationTarget(TargetID);
	}

	void Application::EndPlatformPresentationTarget(uint64_t TargetID)
	{
		m_Renderer->EndPlatformPresentationTarget(TargetID);
	}

	bool Application::DestroyPlatformPresentationTarget(uint64_t TargetID)
	{
		return m_Renderer->DestroyPlatformPresentationTarget(TargetID);
	}

	bool Application::HasPlatformPresentationTargets() const noexcept
	{
		return m_Renderer->HasPlatformPresentationTargets();
	}

	void Application::SetExternalPlatformPresentationTargetCount(uint32_t Count) noexcept
	{
		m_Renderer->SetExternalPlatformPresentationTargetCount(Count);
	}

#if defined(PF_DEBUG)
	bool Application::InjectNextPlatformAcquireOutOfDateForTesting(uint64_t TargetID) noexcept
	{
		return m_Renderer->InjectNextPlatformAcquireOutOfDateForTesting(TargetID);
	}

	bool Application::InjectNextPlatformImageAllocationFailureForTesting() noexcept
	{
		return m_Renderer->InjectNextPlatformImageAllocationFailureForTesting();
	}
#endif

	void Application::Run()
	{
		auto PreviousFrameTime = std::chrono::steady_clock::now();
		while (m_Running)
		{
			m_Window->PollEvents();

			const auto CurrentFrameTime = std::chrono::steady_clock::now();
			const Timestep DeltaTime(CurrentFrameTime - PreviousFrameTime);
			PreviousFrameTime = CurrentFrameTime;

			m_LayerStack.UpdateLayers(DeltaTime);

			// Resource preparation during updates must happen before the renderer opens its frame command list.
			const bool bCanRender = m_Renderer->BeginFrame();
			m_MainFrameActive = bCanRender;

			const bool bHasPlatformPresentationTargets = m_Renderer->HasPlatformPresentationTargets();
			if (bCanRender || bHasPlatformPresentationTargets)
				m_LayerStack.RenderLayers();

			if (bCanRender)
				m_Renderer->EndFrame();
			m_MainFrameActive = false;

			if (bCanRender || bHasPlatformPresentationTargets)
				m_LayerStack.RenderPlatformWindows();
			else
				m_Window->WaitEventsTimeout(0.05);
		}
	}

	void Application::OnEvent(Event& E)
	{
		m_Input.OnEvent(E);

		EventDispatcher Dispatcher(E);

		m_LayerStack.DispatchEvent(E);

		if (!E.bHandled)
			Dispatcher.Dispatch<WindowCloseEvent>(std::bind(&Application::OnWindowClose, this, std::placeholders::_1));
	}

	Layer& Application::PushLayer(std::unique_ptr<Layer> Layer)
	{
		return m_LayerStack.PushLayer(std::move(Layer));
	}

	Layer& Application::PushOverlay(std::unique_ptr<Layer> Overlay)
	{
		return m_LayerStack.PushOverlay(std::move(Overlay));
	}

	BufferCreateResult Application::CreateBuffer(
		const BufferDesc& Description,
		std::span<const std::byte> InitialData)
	{
		auto Validation = ValidateBufferDescription(Description, InitialData.size());
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateBuffer(Description, InitialData);
	}

	BufferUpdateResult Application::WriteBuffer(
		const Buffer& Target,
		uint64_t DestinationOffset,
		std::span<const std::byte> Data)
	{
		const BufferUpdateResult Validation = ValidateBufferUpdate(
			Target.GetDescription(),
			DestinationOffset,
			Data.size());
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->WriteBuffer(Target, DestinationOffset, Data);
	}

	MeshCreateResult Application::CreateMesh(const MeshDesc& Description)
	{
		return RenderDevice::CreateMesh(Description);
	}

	RenderTargetCreateResult Application::CreateRenderTarget(const RenderTargetDesc& Description)
	{
		const auto Validation = ValidateRenderTargetDescription(Description);
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateRenderTarget(Description);
	}

	DepthCubemapCreateResult Application::CreateDepthCubemap(uint32_t Resolution, const std::string& DebugName)
	{
		if (Resolution == 0 || DebugName.empty())
			return std::unexpected(DepthCubemapError{ DepthCubemapErrorCode::InvalidDescription,
				"Depth cubemap resolution and debug name must be valid" });
		return m_Renderer->CreateDepthCubemap(Resolution, DebugName);
	}

	GraphicsResult Application::BeginRenderTarget(const RenderTarget& Target, const RenderTargetClearValue& ClearValue)
	{
		const auto Validation = ValidateRenderTargetClearValue(ClearValue);
		if (!Validation)
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::InvalidDescription,
				Validation.error().Message
			});
		}

		return m_Renderer->BeginRenderTarget(Target, ClearValue);
	}

	GraphicsResult Application::EndRenderTarget()
	{
		return m_Renderer->EndRenderTarget();
	}

	TextureCreateResult Application::CreateTexture(
		const TextureDesc& Description,
		std::span<const std::byte> InitialData)
	{
		return RenderDevice::CreateTexture(Description, InitialData);
	}

	TextureCreateResult Application::CreateTexture(
		const TextureDesc& Description,
		std::span<const TextureSubresourceData> InitialData)
	{
		const auto Validation = ValidateTextureUpload(Description, InitialData);
		if (!Validation)
			return std::unexpected(Validation.error());
		return m_Renderer->CreateTexture(Description, InitialData);
	}

	TextureReadbackResult Application::ReadTextureForValidation(const Texture& Source, uint32_t ArraySlice)
	{
		return m_Renderer->ReadTextureForValidation(Source, ArraySlice);
	}

	SamplerCreateResult Application::CreateSampler(const SamplerDesc& Description)
	{
		const auto Validation = ValidateSamplerDescription(Description);
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateSampler(Description);
	}

	BindingLayoutCreateResult Application::CreateBindingLayout(const BindingLayoutDesc& Description)
	{
		const auto Validation = ValidateBindingLayout(Description);
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateBindingLayout(Description);
	}

	BindingSetCreateResult Application::CreateBindingSet(const BindingSetDesc& Description)
	{
		const auto Validation = ValidateBindingSet(Description);
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateBindingSet(Description);
	}

	ShaderCreateResult Application::CreateShader(
		const ShaderDesc& Description,
		std::span<const std::byte> Bytecode)
	{
		const GraphicsResult Validation = ValidateShaderBytecode(Description, Bytecode);
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateShader(Description, Bytecode);
	}

	GraphicsPipelineCreateResult Application::CreateGraphicsPipeline(const GraphicsPipelineDesc& Description)
	{
		const GraphicsResult Validation = ValidateGraphicsPipelineDescription(Description);
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateGraphicsPipeline(Description);
	}

	GraphicsResult Application::Draw(
		const GraphicsPipeline& Pipeline,
		const Buffer& VertexBuffer,
		const DrawArguments& Arguments,
		std::span<const BindingSet* const> BindingSets)
	{
		const GraphicsResult Validation = ValidateDrawArguments(
			Arguments,
			Pipeline.GetDescription(),
			VertexBuffer.GetDescription());
		if (!Validation)
			return Validation;
		const GraphicsResult BindingValidation = ValidateDrawBindingSets(
			Pipeline.GetDescription(),
			BindingSets);
		if (!BindingValidation)
			return BindingValidation;

		return m_Renderer->Draw(Pipeline, VertexBuffer, Arguments, BindingSets);
	}

	GraphicsResult Application::Draw(
		const GraphicsPipeline& Pipeline,
		const Mesh& Geometry,
		const DrawArguments& Arguments,
		std::span<const BindingSet* const> BindingSets)
	{
		const GraphicsResult Validation = ValidateMeshDrawArguments(
			Arguments,
			Pipeline.GetDescription(),
			Geometry.GetVertexLayout(),
			Geometry.GetVertexBuffer().GetDescription());
		if (!Validation)
			return Validation;

		const GraphicsResult BindingValidation = ValidateDrawBindingSets(Pipeline.GetDescription(), BindingSets);
		if (!BindingValidation)
			return BindingValidation;

		return m_Renderer->Draw(Pipeline, Geometry.GetVertexBuffer(), Arguments, BindingSets);
	}

	GraphicsResult Application::DrawIndexed(
		const GraphicsPipeline& Pipeline,
		const Mesh& Geometry,
		const DrawIndexedArguments& Arguments,
		std::span<const BindingSet* const> BindingSets)
	{
		const GraphicsResult Validation = ValidateIndexedDrawArguments(
			Arguments,
			Pipeline.GetDescription(),
			Geometry.GetVertexLayout(),
			Geometry.GetIndexCount());
		if (!Validation)
			return Validation;

		const GraphicsResult BindingValidation = ValidateDrawBindingSets(Pipeline.GetDescription(), BindingSets);
		if (!BindingValidation)
			return BindingValidation;

		return m_Renderer->DrawIndexed(
			Pipeline,
			Geometry.GetVertexBuffer(),
			*Geometry.GetIndexBuffer(),
			Arguments,
			BindingSets);
	}

	GraphicsResult Application::DrawIndexed(
		const GraphicsPipeline& Pipeline,
		const Buffer& VertexBuffer,
		const Buffer& IndexBuffer,
		const DrawIndexedArguments& Arguments,
		std::span<const BindingSet* const> BindingSets)
	{
		const GraphicsResult Validation = ValidateIndexedBufferDrawArguments(
			Arguments,
			Pipeline.GetDescription(),
			VertexBuffer.GetDescription(),
			IndexBuffer.GetDescription());
		if (!Validation)
			return Validation;

		const GraphicsResult BindingValidation = ValidateDrawBindingSets(Pipeline.GetDescription(), BindingSets);
		if (!BindingValidation)
			return BindingValidation;

		return m_Renderer->DrawIndexed(Pipeline, VertexBuffer, IndexBuffer, Arguments, BindingSets);
	}

	Application& Application::Get()
	{
		if (!s_Instance)
			throw std::logic_error("PulseForge Application is not initialized");
		return *s_Instance;
	}

	bool Application::OnWindowClose(WindowCloseEvent& E)
	{
		m_Running = false;
		return true;
	}
}
