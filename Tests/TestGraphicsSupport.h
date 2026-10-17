#pragma once

#include "TestRendererSupport.h"

namespace PulseForgeTestSupport
{
	class TestShader final : public PulseForge::Shader
	{
	public:
		explicit TestShader(PulseForge::ShaderStage Stage)
		{
			m_Description.Stage = Stage;
			m_Description.EntryPoint = Stage == PulseForge::ShaderStage::Vertex ? "VSMain" : "PSMain";
		}

		const PulseForge::ShaderDesc& GetDescription() const noexcept override { return m_Description; }

	private:
		PulseForge::ShaderDesc m_Description;
	};

	class TestBuffer final : public PulseForge::Buffer
	{
	public:
		explicit TestBuffer(PulseForge::BufferDesc Description) : m_Description(std::move(Description)) {}

		const PulseForge::BufferDesc& GetDescription() const noexcept override { return m_Description; }

	private:
		PulseForge::BufferDesc m_Description;
	};

	class TestTexture final : public PulseForge::Texture
	{
	public:
		explicit TestTexture(PulseForge::TextureDesc Description) : m_Description(std::move(Description)) {}

		const PulseForge::TextureDesc& GetDescription() const noexcept override { return m_Description; }

	private:
		PulseForge::TextureDesc m_Description;
	};

	class TestSampler final : public PulseForge::Sampler
	{
	public:
		explicit TestSampler(PulseForge::SamplerDesc Description = {}) : m_Description(std::move(Description)) {}

		const PulseForge::SamplerDesc& GetDescription() const noexcept override { return m_Description; }

	private:
		PulseForge::SamplerDesc m_Description;
	};

	class TestBindingLayout final : public PulseForge::BindingLayout
	{
	public:
		explicit TestBindingLayout(PulseForge::BindingLayoutDesc Description) : m_Description(std::move(Description)) {}

		const PulseForge::BindingLayoutDesc& GetDescription() const noexcept override { return m_Description; }

	private:
		PulseForge::BindingLayoutDesc m_Description;
	};

	class TestBindingSet final : public PulseForge::BindingSet
	{
	public:
		explicit TestBindingSet(PulseForge::BindingLayoutHandle Layout) : m_Layout(std::move(Layout)) {}

		const PulseForge::BindingLayout& GetLayout() const noexcept override { return *m_Layout; }

	private:
		PulseForge::BindingLayoutHandle m_Layout;
	};

	inline PulseForge::GraphicsPipelineDesc MakeTestPipelineDescription()
	{
		using namespace PulseForge;
		GraphicsPipelineDesc Description;
		Description.VertexShader = std::make_shared<TestShader>(ShaderStage::Vertex);
		Description.FragmentShader = std::make_shared<TestShader>(ShaderStage::Fragment);
		Description.VertexLayout.Stride = sizeof(float) * 6;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 },
			{ VertexSemantic::Color, VertexFormat::Float3, sizeof(float) * 3 } };
		return Description;
	}
} // namespace PulseForgeTestSupport
