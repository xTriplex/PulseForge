#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/Binding.h"
#include "Renderer/Graphics.h"

#include <memory>
#include "Renderer/RendererAPI.h"

namespace PulseForge
{
	class Window;

	class RendererBackend
	{
	public:
		virtual ~RendererBackend() = default;

		virtual bool BeginFrame() = 0;
		virtual void EndFrame() = 0;
		virtual BufferCreateResult CreateBuffer(
			const BufferDesc& Description,
			std::span<const std::byte> InitialData) = 0;
		virtual TextureCreateResult CreateTexture(
			const TextureDesc& Description,
			std::span<const std::byte> InitialData) = 0;
		virtual SamplerCreateResult CreateSampler(const SamplerDesc& Description) = 0;
		virtual BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc& Description) = 0;
		virtual BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description) = 0;
		virtual ShaderCreateResult CreateShader(
			const ShaderDesc& Description,
			std::span<const std::byte> Bytecode) = 0;
		virtual GraphicsPipelineCreateResult CreateGraphicsPipeline(
			const GraphicsPipelineDesc& Description) = 0;
		virtual GraphicsResult Draw(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets) = 0;

		static std::unique_ptr<RendererBackend> Create(RendererAPI API, Window& Window);
	};
}
