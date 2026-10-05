#include "Core/PulseForgePCH.h"
#include "Core/Application.h"
#include "Renderer/RendererBackend.h"
#include <stdexcept>
#include <chrono>

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

			if (API == RendererAPI::OpenGL)
			{
				auto ImGuiOverlay = std::make_unique<ImGuiLayer>();
				m_ImGuiLayer = static_cast<ImGuiLayer*>(
					&m_LayerStack.PushOverlay(std::move(ImGuiOverlay)));
			}
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
		m_ImGuiLayer = nullptr;
		s_Instance = nullptr;
		m_Renderer.reset();
		m_Window.reset();
	}

	void Application::Run()
	{
		auto PreviousFrameTime = std::chrono::steady_clock::now();
		while (m_Running)
		{
			m_Window->PollEvents();
			const bool bCanRender = m_Renderer->BeginFrame();

			const auto CurrentFrameTime = std::chrono::steady_clock::now();
			const Timestep DeltaTime(CurrentFrameTime - PreviousFrameTime);
			PreviousFrameTime = CurrentFrameTime;

			for (const auto& Layer : m_LayerStack)
			{
				Layer->OnUpdate(DeltaTime);
			}

			if (bCanRender)
			{
				for (const auto& Layer : m_LayerStack)
					Layer->OnRender();
			}

			if (bCanRender && m_ImGuiLayer)
			{
				m_ImGuiLayer->Begin();

				for (const auto& Layer : m_LayerStack)
				{
					Layer->OnImGuiRender();
				}

				m_ImGuiLayer->End();
			}

			if (bCanRender)
				m_Renderer->EndFrame();
			else
				m_Window->WaitEventsTimeout(0.05);
		}
	}

	void Application::OnEvent(Event& E)
	{
		m_Input.OnEvent(E);

		EventDispatcher Dispatcher(E);

		Dispatcher.Dispatch<WindowCloseEvent>(std::bind(&Application::OnWindowClose, this, std::placeholders::_1));

		for (auto It = m_LayerStack.rbegin(); It != m_LayerStack.rend(); ++It)
		{
			(*It)->OnEvent(E);
			if (E.bHandled)
			{
				break;
			}
		}
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

	TextureCreateResult Application::CreateTexture(
		const TextureDesc& Description,
		std::span<const std::byte> InitialData)
	{
		const auto Validation = ValidateTextureUpload(Description, InitialData.size());
		if (!Validation)
			return std::unexpected(Validation.error());

		return m_Renderer->CreateTexture(Description, InitialData);
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
